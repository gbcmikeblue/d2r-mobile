// N4-h：把「我们自己的三角形」画进**游戏的通道里**（缺省关、诊断用）。设计说明见 vk_tri_game.h。
//
// 为什么要有这一步（证据链，全部是前几轮的真机读数）：
//   · N4-e：`DM_PASS_PROBE=28` 的已知色底子上，游戏 3.9 万次绘制**一个像素都没落下**；光栅化侧逐条排除
//     （深度/模板、剔除、混合、写掩码、动态视口/裁剪全放开仍全零）⇒ **片元在光栅化之前就没了**；
//   · N4-f：顶点数据与常量缓冲都是正常的（NaN 0 / Inf 0 / 逐槽 |max| 两臂逐项相同）；
//   · A1（校验层）：**16 种 VUID 里没有一条是 draw 层面的用法错误** ⇒ 我们的 draw 在 API 层面合法；
//   · N4-g：两刀把"顶点输出（裁剪/剔除距离）"那一层排掉，**并且顺带证明我们自己的三角形在离屏通道里能落屏**。
//   ⇒ 剩下来的"是非题"就是这一条：**游戏那一刻的通道 / 状态，到底能不能把片元落下来**。
//
// 做法（只在一个开关下发生，缺省一行都不走）：游戏的某次绘制刚录完，用同一条命令缓冲、同一个通道、
// 同一批附件，绑一条**我们自己的**最简管线（全屏三角形 + 纯红），`vkCmdDraw(3)` 一次，然后按游戏的原值
// 把动态视口/裁剪恢复回去。判据写在头文件里（底盘上出现非 152 的值 = 落屏）。
#include "vk_tri_game.h"
#include "vk_device.h"
#include "vk_pipe.h"
#include "vk_tex.h"
#include "vk_tri_game_spv.h"   // 内联 SPIR-V（由 .work/n4h_tri_spv.py 生成，自证 .work/logs/tri-ingame-spv.txt）
#include "dm.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_parsed;
static int g_on;
static uint64_t g_draw_no;       // 0 = 不限绘制序号
static uint64_t g_color0_ser;    // 0 = 不限 color0 序号
static int g_use_game_vp;        // 1 = 用游戏那一刻的动态视口/裁剪；0 = 铺满附件（缺省）
static int g_in_pass;            // 这一段通道里已经插过（每段通道只插一次）

static _Atomic uint64_t g_inserts, g_pipes, g_nofmt, g_nopipe;

// 管线按「颜色附件格式序列 + 采样数」缓存（游戏一帧里格式通常只有一两种）
#define TRI_PIPE_MAX 8
typedef struct { uint32_t samples; int n; VkFormat fmt[8]; VkPipeline pipe; } tri_ent;
static tri_ent g_tab[TRI_PIPE_MAX];
static int g_ntab;

// ---- 开关：DM_TRI_IN_GAME=<绘制序号>[@<color0序号>][:game]（缺省关；读一次，幂等）----
static void tri_parse(void) {
    if (g_parsed) return;
    g_parsed = 1;
    const char *e = getenv("DM_TRI_IN_GAME");
    if (!e || !*e || !strcmp(e, "0")) return;
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s", e);
    char *s = tmp;
    char *colon = strchr(s, ':');
    if (colon) {
        *colon = 0;
        if (!strcmp(colon + 1, "game")) g_use_game_vp = 1;
        else LOG("【黄】DM_TRI_IN_GAME：后缀 `:%s` 认不出来（只认识 `:game`）——按缺省（铺满附件视口）走", colon + 1);
    }
    char *at = strchr(s, '@');
    if (at) { *at = 0; g_color0_ser = strtoull(at + 1, NULL, 10); }
    if (*s) g_draw_no = strtoull(s, NULL, 10);
    g_on = (g_draw_no || g_color0_ser) ? 1 : 0;
    if (!g_on) {
        LOG("【黄】DM_TRI_IN_GAME=%s 两个字段都是 0：这个开关要至少写一个（绘制序号 或 @color0序号）——按关处理", e);
        return;
    }
    LOG("【查】DM_TRI_IN_GAME=%s：绘制序号 %s、限定 color0 序号 %s、视口来源 %s —— 在游戏的通道里插一次"
        "**我们自己的最简管线绘制**（全屏三角形 + 纯红，无顶点缓冲/描述符/常量；每段通道只插一次）。"
        "判据：`DM_PASS_PROBE=<color0序号>` 的已知色底子上**出现非 152 的值 = 落屏**",
        e, g_draw_no ? "见下" : "（不限）", g_color0_ser ? "见下" : "（不限）",
        g_use_game_vp ? "游戏那一刻的动态视口/裁剪" : "铺满附件（先隔离通道/附件/布局那一层）");
    if (g_draw_no) LOG("【查】DM_TRI_IN_GAME：绘制序号 = 全局第 %llu 次绘制（与判据行里的「第 N 次」同口径）",
                       (unsigned long long)g_draw_no);
    if (g_color0_ser) LOG("【查】DM_TRI_IN_GAME：限定 color0 序号 = %llu（配 `DM_PASS_PROBE=%llu` 用，"
                          "这样底盘一定是已知色）", (unsigned long long)g_color0_ser, (unsigned long long)g_color0_ser);
}

int vk_tri_game_on(void) { tri_parse(); return g_on; }

void vk_tri_game_pass_begin(void) { g_in_pass = 0; }

void vk_tri_game_stats(uint64_t *inserts, uint64_t *pipes) {
    if (inserts) *inserts = atomic_load(&g_inserts);
    if (pipes) *pipes = atomic_load(&g_pipes);
}

int vk_tri_game_desc(char *buf, size_t cap) {
    tri_parse();
    if (!g_on || !buf || cap < 8) return 0;
    int n = snprintf(buf, cap, "自绘 插入 %llu 次 / 管线 %llu 条 / 没有对应格式 %llu / 建管线失败 %llu",
                     (unsigned long long)atomic_load(&g_inserts), (unsigned long long)atomic_load(&g_pipes),
                     (unsigned long long)atomic_load(&g_nofmt), (unsigned long long)atomic_load(&g_nopipe));
    return n > 0 ? n : 0;
}

// ---- 附件格式：与 vk_tex.c 的 view_for 同口径（view 的 set_format 能重解释格式，必须优先）----
static VkFormat att_fmt(const tex_t *t, const view_t *v) {
    if (!t) return VK_FORMAT_UNDEFINED;
    int nf = t->d.format;
    if (v && v->set_format) {
        VkFormat f2 = VK_FORMAT_UNDEFINED;
        if (vk_fmt_to_vk(v->format, &f2) && f2 != VK_FORMAT_UNDEFINED) nf = v->format;
    }
    VkFormat f = VK_FORMAT_UNDEFINED;
    if (!vk_fmt_to_vk(nf, &f)) return VK_FORMAT_UNDEFINED;
    return f;
}

// ---- 我们自己的那条最简管线：无顶点输入、无描述符、无深度/模板、剔除关、混合关、写掩码只开附件 0 ----
static VkPipeline tri_make(const VkFormat *cf, int n, uint32_t samples) {
    VkDevice dev = vk_dev();
    static VkShaderModule vs, fs;
    if (!vs) {
        VkShaderModuleCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = (size_t)VK_TRI_GAME_VS_WORDS * 4; ci.pCode = VK_TRI_GAME_VS;
        VkResult r = vkCreateShaderModule(dev, &ci, NULL, &vs);
        if (r != VK_SUCCESS) { LOG("【红】DM_TRI_IN_GAME：建顶点着色器模块失败 VkResult %d", (int)r); vs = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
        ci.codeSize = (size_t)VK_TRI_GAME_FS_WORDS * 4; ci.pCode = VK_TRI_GAME_FS;
        r = vkCreateShaderModule(dev, &ci, NULL, &fs);
        if (r != VK_SUCCESS) { LOG("【红】DM_TRI_IN_GAME：建片段着色器模块失败 VkResult %d", (int)r); fs = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    }
    VkPipelineLayoutCreateInfo plc;
    memset(&plc, 0, sizeof plc);
    plc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout lay = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &plc, NULL, &lay) != VK_SUCCESS) {
        LOG("【红】DM_TRI_IN_GAME：建管线布局失败");
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;   // 无顶点输入：顶点全由 gl_VertexIndex 生成
    VkPipelineInputAssemblyStateCreateInfo ia;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps;
    memset(&vps, 0, sizeof vps);
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;                 // 剔除关：三角形朝哪边都画
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = samples ? (VkSampleCountFlagBits)samples : VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba[8];
    memset(cba, 0, sizeof cba);
    cba[0].colorWriteMask = 0xf;                     // 只写附件 0；其余附件 mask 0（片段着色器不写它们）
    VkPipelineColorBlendStateCreateInfo cbd;
    memset(&cbd, 0, sizeof cbd);
    cbd.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbd.attachmentCount = (uint32_t)n; cbd.pAttachments = cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo prc;
    memset(&prc, 0, sizeof prc);
    prc.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    prc.colorAttachmentCount = (uint32_t)n; prc.pColorAttachmentFormats = cf;
    // 深度/模板格式**不声明**：Vulkan 允许（管线不用那两张附件），前提是深度/模板测试都关（下面 memset 全是 0）
    VkGraphicsPipelineCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &prc;
    ci.stageCount = 2; ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cbd;
    ci.pDynamicState = &ds;
    ci.layout = lay;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe);
    vkDestroyPipelineLayout(dev, lay, NULL);
    if (r != VK_SUCCESS) {
        LOG("【红】DM_TRI_IN_GAME：建我们自己的图形管线失败 VkResult %d（%d 个颜色附件、格式 %d/%d、采样 %u）",
            (int)r, n, n > 0 ? (int)cf[0] : -1, n > 1 ? (int)cf[1] : -1, samples);
        atomic_fetch_add(&g_nopipe, 1);
        return VK_NULL_HANDLE;
    }
    atomic_fetch_add(&g_pipes, 1);
    // 顺手让既有那一格也看得见（"图形管线 计数 +1"这条自证）：vk_pipe 的计数按"游戏路径建的管线"记着，
    // 我们这条是回放层以外的模块建的，单独立个入口，免得把 N0~N4-g 的读数口径搅混。
    vk_pipe_note_external_gfx();
    LOG("【查】DM_TRI_IN_GAME：我们自己的图形管线已建（VkResult 0）—— %d 个颜色附件（格式 %s）、采样 %u；"
        "本模块累计 %llu 条", n, n > 0 ? "见下" : "无", samples, (unsigned long long)atomic_load(&g_pipes));
    for (int i = 0; i < n; i++) LOG("【查】DM_TRI_IN_GAME：附件 %d 格式 = VkFormat %d", i, (int)cf[i]);
    return pipe;
}

static VkPipeline tri_pipe(const VkFormat *cf, int n, uint32_t samples) {
    for (int i = 0; i < g_ntab; i++) {
        if (g_tab[i].n == n && g_tab[i].samples == samples && !memcmp(g_tab[i].fmt, cf, sizeof(VkFormat) * (size_t)n))
            return g_tab[i].pipe;
    }
    VkPipeline p = tri_make(cf, n, samples);
    if (!p) return VK_NULL_HANDLE;
    if (g_ntab < TRI_PIPE_MAX) {
        g_tab[g_ntab].n = n; g_tab[g_ntab].samples = samples;
        memcpy(g_tab[g_ntab].fmt, cf, sizeof(VkFormat) * (size_t)n);
        g_tab[g_ntab].pipe = p;
        g_ntab++;
    } else {
        static int said;
        if (!said++) LOG("【黄】DM_TRI_IN_GAME：管线缓存满了（%d 项）—— 这条管线不进缓存（每帧会重建）", TRI_PIPE_MAX);
    }
    return p;
}

int vk_tri_game_try_draw(VkCommandBuffer cb, uint64_t draw_index,
                         const tex_t *const *colors, const view_t *cviews, const uint8_t *has_cview,
                         int ncolor, uint32_t att_w, uint32_t att_h, uint32_t samples,
                         const VkViewport *vp, int nvp, const VkRect2D *sc, int nsc) {
    if (!vk_tri_game_on()) return 0;
    if (g_in_pass) return 0;                                  // 这一段通道里已经插过一次
    if (g_draw_no && g_draw_no != draw_index) return 0;
    if (ncolor <= 0 || !colors || !colors[0]) return 0;
    if (g_color0_ser && colors[0]->serial != g_color0_ser) return 0;
    if (!att_w || !att_h) return 0;
    if (vk_dev() == VK_NULL_HANDLE) return 0;
    VkFormat cf[8];
    int n = 0;
    for (int i = 0; i < ncolor && i < 8; i++) {
        VkFormat f = att_fmt(colors[i], (has_cview && has_cview[i]) ? &cviews[i] : NULL);
        if (f == VK_FORMAT_UNDEFINED) {
            uint64_t k = atomic_fetch_add(&g_nofmt, 1) + 1;
            if (k <= 4) LOG("【黄】DM_TRI_IN_GAME：颜色附件 %d（序号 %llu、NVN 格式 %#x）没有 Vulkan 对应物 ⇒ 这次不插",
                            i, (unsigned long long)(colors[i] ? colors[i]->serial : 0),
                            colors[i] ? (unsigned)colors[i]->d.format : 0u);
            return 0;
        }
        cf[n++] = f;
    }
    VkPipeline pl = tri_pipe(cf, n, samples);
    if (!pl) return 0;
    // 视口/裁剪：缺省铺满附件（隔离"通道/附件/布局"这一层）；`:game` 档用游戏那一刻的
    VkViewport full = { 0.0f, 0.0f, (float)att_w, (float)att_h, 0.0f, 1.0f };
    VkRect2D fullsc = { { 0, 0 }, { att_w, att_h } };
    int use_game = (g_use_game_vp && nvp > 0);
    if (use_game) vkCmdSetViewport(cb, 0, (uint32_t)nvp, vp);
    else vkCmdSetViewport(cb, 0, 1, &full);
    if (g_use_game_vp && nsc > 0) vkCmdSetScissor(cb, 0, (uint32_t)nsc, sc);
    else vkCmdSetScissor(cb, 0, 1, &fullsc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    vkCmdDraw(cb, 3, 1, 0, 0);
    // 恢复游戏的动态状态（与 pass_begin 同口径：视口 nvp 个、裁剪只补第 0 个）——我们改的只该活这一次绘制
    if (nvp > 0) vkCmdSetViewport(cb, 0, (uint32_t)nvp, vp);
    if (nsc > 0) vkCmdSetScissor(cb, 0, 1, sc);
    g_in_pass = 1;
    uint64_t k = atomic_fetch_add(&g_inserts, 1) + 1;
    if (k == 1 || (k & 4095) == 0) {
        uint64_t nlay = 0, nl2 = 0, ncpso = 0, gpso = 0, nfail = 0;
        vk_pipe_stats(&nlay, &nl2, &ncpso, &gpso, &nfail);
        float vx = (nvp > 0) ? vp[0].x : 0.0f, vy = (nvp > 0) ? vp[0].y : 0.0f;
        float vw = (nvp > 0) ? vp[0].width : 0.0f, vh = (nvp > 0) ? vp[0].height : 0.0f;
        LOG("【查】DM_TRI_IN_GAME：第 %llu 次插入 —— 游戏第 %llu 次绘制；通道 color0 = 序号 %llu"
            "（附件 %ux%u、%d 个颜色附件、VkFormat %d/%d、采样 %u）；本轮绘制视口 = %s；游戏那一刻的动态视口 %d 个"
            "（%.0fx%.0f@(%.0f,%.0f)）、裁剪 %d 个（%ux%u@(%d,%d)）；图形管线累计 %llu 条（失败 %llu）"
            "—— 其中**我们这条**已计入，本模块累计 %llu 条；`vkCmdDraw(3)` 已录进这条命令缓冲",
            (unsigned long long)k, (unsigned long long)draw_index, (unsigned long long)colors[0]->serial,
            att_w, att_h, n, (int)cf[0], n > 1 ? (int)cf[1] : -1, samples,
            use_game ? "游戏那一刻的（`:game`）" : "铺满附件（缺省）",
            nvp, (double)vw, (double)vh, (double)vx, (double)vy,
            nsc, nsc > 0 ? sc[0].extent.width : 0u, nsc > 0 ? sc[0].extent.height : 0u,
            nsc > 0 ? (int)sc[0].offset.x : 0, nsc > 0 ? (int)sc[0].offset.y : 0,
            (unsigned long long)gpso, (unsigned long long)nfail, (unsigned long long)atomic_load(&g_pipes));
    }
    return 1;
}
