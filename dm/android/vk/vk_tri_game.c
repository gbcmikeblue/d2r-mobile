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
#include "vk_spv.h"               // N4-l：`vk_spv_force_set` / `vk_spv_scan`（片段那一份也要有反射）
#include "vk_tri_game_spv.h"      // 内联 SPIR-V（由 .work/n4h_tri_spv.py 生成，自证 .work/logs/tri-ingame-spv.txt）
#include "vk_tri_ladder_spv.h"    // N4-i 的那一份（由 .work/n4i_ladder_spv.py 生成，自证 .work/logs/tri-ladder-spv.txt）
#include "vk_tri_fs_cap_spv.h"    // N4-k 的三个片段着色器变体（由 .work/n4k_fs_variants.py 生成，自证 .work/logs/n4k-fs-variants.txt）
#include "vk_tri_fs_n4l_spv.h"    // N4-l 的四份（只保留一条绑定的四格 + 声明但不读；自证 .work/logs/n4l-fs-variants.txt）
#include "vk_tri_fs_n4l_cap_spv.h" // N4-l 第二刀：只加 capability（游戏那份的清单；自证 .work/logs/n4l-cap-variants.txt）
#include "vk_tri_fs_n4m_spv.h"    // N4-m：`blit` + isnan 守卫 + precise（OpIsNan/OpSelect/NoContraction；自证 .work/logs/n4m-fs-variants.txt）
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

// N4-i 的阶梯（定义在文件末尾；这里先声明，因为心跳那一格与开关自证都要用）
static void ladder_parse(void);
static const char *ladder_name(int m);
static int g_ladder_parsed, g_ladder;
static _Atomic uint64_t g_n4i_ins, g_n4i_nopipe;

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
    // N4-i：阶梯开着时，心跳那一行**多一格**（只在开关打开时出现 ⇒ 缺省关时这一行与上一轮逐字节相同，第 ㉓ 条）
    if (n > 0 && (size_t)n < cap) {
        ladder_parse();
        if (g_ladder) {
            int m = snprintf(buf + n, cap - (size_t)n, " / 阶梯 %s 插入 %llu / 骨架没搭起来 %llu",
                             ladder_name(g_ladder),
                             (unsigned long long)atomic_load(&g_n4i_ins),
                             (unsigned long long)atomic_load(&g_n4i_nopipe));
            if (m > 0) n += m;
        }
        // N4-j：逐项接上的开关那一格 —— **只在真的接了东西时出现**（缺省关时这一行与上一轮逐字节相同，第 ㉓ 条）
        if (n > 0 && (size_t)n < cap) {
            char fb[160];
            if (vk_tri_fields_desc(fb, sizeof fb) > 0) {
                int m = snprintf(buf + n, cap - (size_t)n, " / 逐项接上 %s", fb);
                if (m > 0) n += m;
            }
        }
    }
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

// ============================================================================
// N4-i：**二分阶梯**（`DM_TRI_LADDER=<档>`；缺省关）
// ============================================================================
// 要回答的那句话（写死）：**"游戏那些绘制自己的哪一项输入，让它产不出片元？"**
//
// 为什么这样问（证据链，全部是前几轮的真机读数）：
//   · N4-h：**通道 / 附件 / 布局 / 清 / 视口裁剪**那一层是好的 —— 我们自己的最简管线（全屏三角形 + 纯红）
//     画进游戏的通道里就**落屏**（序号 28 第 0/1 级从整片 152 变成整片 85）；
//   · N4-g：声明 9 个裁剪/剔除距离却从不写**不足以致命**（我们自己的三角形两个变体都落屏）；
//   · N4-e：光栅化侧逐条排除（`DM_RASTER_BRUTE` 全放开仍是整片已知色）；N4-f：顶点数据与常量缓冲都正常；
//   · A1（真机校验层）：16 种 VUID 里**没有一条是 draw 层面的用法错误**。
//   ⇒ 剩下的问题只能是：**游戏自己那一刻的着色器 / 顶点输入那一层**。
//
// 做法：每档只比上一档**多接一样**，其余照旧用我们那条已经证明能落屏的管线：
//   ① `vb`    ：我们自己的管线 + **游戏的顶点缓冲**（顶点位置取 `Location 0`）；
//   ② `state` ：我们自己的管线 + **游戏那一刻的状态**（拓扑 / 写掩码 / 混合 / 剔除 / 深度，全从 `gfx_state_fill` 来）；
//   ③ `vs`    ：**游戏的顶点着色器** + 我们自己的片段着色器，其余（状态 / 描述符 / 顶点缓冲 / 绘制调用）全是游戏的；
//   ④ `both`  ：**游戏的顶点着色器 + 游戏的片段着色器**（= 把游戏那一次绘制原地重放一遍）——
//               它是 ③ 的**对照**：④ 与 ③ 之间**只差"片段着色器模块"这一样东西**。
//               离线那一刀已经给出线索：全库 4556 份片段着色器里 **4189 份带 `OpKill`**（"杀片元"这条路在游戏里是
//               常态），而 `OpKill` 发生在光栅化**之后** —— N4-e 那些"把光栅化侧放开"的开关**碰不到它**。
//
// 判据（与 N4-h 逐字相同，写死）：`DM_PASS_PROBE=28` 的已知色底子上**出现非 152 的值 = 落屏**；
// **哪一档开始不落屏，责任就落在那一项上**。每档都打一行自证：**这一档到底接上了什么**（绑定号 / 格式 /
// 步长 / 偏移 / 地址）+ **游戏那一刻的状态**（掩码 / 混合 / 深度 / 剔除 / 拓扑）+ **这一档真的画了** ——
// 没有这一行，"没落屏"就分不清是"没接上"还是"被状态吃掉"。
//
// 副作用纪律（照 N4-h）：只改**通道内的动态视口 / 裁剪**，插完立刻按游戏原值恢复；不动游戏的静态状态、
// 不碰附件、不改布局、不提交；缺省关时一行都不走（N4-h 那条路径一字未改）。

static const char *ladder_name(int m) {
    switch (m) {
        case VK_TRI_LADDER_VB: return "vb";
        case VK_TRI_LADDER_STATE: return "state";
        case VK_TRI_LADDER_VS: return "vs";
        case VK_TRI_LADDER_BOTH: return "both";
        case VK_TRI_LADDER_VSN: return "vsn";
        case VK_TRI_LADDER_VSO: return "vso";
        case VK_TRI_LADDER_VSF: return "vsf";
        case VK_TRI_LADDER_VSFN: return "vsfn";
        case VK_TRI_LADDER_VSFULL: return "vsfull";
        case VK_TRI_LADDER_VSFULLF: return "vsfullf";
    }
    return "off";
}

const char *vk_tri_ladder_name(int mode) { return ladder_name(mode); }

static void ladder_parse(void) {
    if (g_ladder_parsed) return;
    g_ladder_parsed = 1;
    const char *e = getenv("DM_TRI_LADDER");
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "off")) return;
    if (!strcmp(e, "vb")) g_ladder = VK_TRI_LADDER_VB;
    else if (!strcmp(e, "state")) g_ladder = VK_TRI_LADDER_STATE;
    else if (!strcmp(e, "vs")) g_ladder = VK_TRI_LADDER_VS;
    else if (!strcmp(e, "both")) g_ladder = VK_TRI_LADDER_BOTH;
    else if (!strcmp(e, "vsn")) g_ladder = VK_TRI_LADDER_VSN;
    else if (!strcmp(e, "vso")) g_ladder = VK_TRI_LADDER_VSO;
    else if (!strcmp(e, "vsf")) g_ladder = VK_TRI_LADDER_VSF;
    else if (!strcmp(e, "vsfn")) g_ladder = VK_TRI_LADDER_VSFN;
    else if (!strcmp(e, "vsfull")) g_ladder = VK_TRI_LADDER_VSFULL;
    else if (!strcmp(e, "vsfullf")) g_ladder = VK_TRI_LADDER_VSFULLF;
    else {
        LOG("【黄】DM_TRI_LADDER=%s 认不出来（只认识 vb / state / vs / both / vsn / vso / vsf / vsfn / vsfull / vsfullf）——按关处理", e);
        return;
    }
    LOG("【查】DM_TRI_LADDER=%s：拿 N4-h 那条**已经证明能落屏**的管线当标尺，把游戏那一刻的输入"
        "**一次只接一样**地接上来（① `vb` = 我们自己的管线 + 游戏的顶点缓冲；② `state` = 我们自己的管线 + "
        "游戏那一刻的状态；③ `vs` = 游戏的顶点着色器 + 我们自己的片段着色器；④ `both` = 游戏的顶点着色器 + "
        "游戏的片段着色器 = 对照组；⑤ `vsn` = ③ 但不绑描述符；⑥ `vso` = 游戏的顶点着色器 + 我们自己的状态；"
        "⑦ `vsf` = 游戏的顶点着色器 + **游戏的片段着色器** + 我们自己的状态（N4-k）；⑧ `vsfn` = ⑦ 但不绑描述符；"
        "⑨ `vsfull` = **我们自己的全屏三角形 VS** + 游戏的片段着色器 + 我们自己的状态（与 ⑦ 只差几何））。"
        "判据：`DM_PASS_PROBE=28` 的已知色底子上**出现非 152 的值 = 这一档仍然落屏**；"
        "**哪一档开始不落屏，责任就落在那一项上**。各档都用 `DM_TRI_IN_GAME` 定「哪一次绘制 / 哪一段通道」",
        e);
}

// N4-k：`DM_TRI_FS_VARIANT=<cap|3out|3out_cap>`（缺省关）——把阶梯用的那份**我们自己的**纯红 FS 换成变体。
// 要回答的那句话写死在 vk_tri_game.h 上：**"只多一条 capability 声明会不会让绘制消失？"**
// 全库 4556 份游戏片段着色器**全部**声明 `FragmentShaderPixelInterlockEXT`，而设备上没有
// `VK_EXT_fragment_shader_interlock`（N4-g 的能力探针：165 个扩展里没有它）⇒ 这个 capability 永远无法被满足。
// 这一档就是"我们自己那份能落屏的纯红 FS" + 那一条声明（指令一条没多）⇒ 单变量。
static int g_fsv_parsed, g_fsv;

int vk_tri_fs_variant(void) {
    if (g_fsv_parsed) return g_fsv;
    g_fsv_parsed = 1;
    const char *e = getenv("DM_TRI_FS_VARIANT");
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "off")) return 0;
    if (!strcmp(e, "cap")) g_fsv = 1;
    else if (!strcmp(e, "3out")) g_fsv = 2;
    else if (!strcmp(e, "3out_cap")) g_fsv = 3;
    else if (!strcmp(e, "iface")) g_fsv = 4;
    else if (!strcmp(e, "blit")) g_fsv = 5;
    else if (!strcmp(e, "if_ubo")) g_fsv = 6;
    else if (!strcmp(e, "if_smp")) g_fsv = 7;
    else if (!strcmp(e, "smp1")) g_fsv = 8;      // N4-l：只声明一张 sampler2D（binding 1）
    else if (!strcmp(e, "ubo4")) g_fsv = 9;      // N4-l：只声明池内那一条 UBO（binding 4，有动态偏移）
    else if (!strcmp(e, "ubo0")) g_fsv = 10;     // N4-l：只声明 support_buffer（binding 0，宿主常量段）
    else if (!strcmp(e, "decl")) g_fsv = 11;     // N4-l：五条绑定全声明、一条都不读
    else if (!strcmp(e, "capf64")) g_fsv = 12;   // N4-l 第二刀：只多 OpCapability Float64
    else if (!strcmp(e, "capgather")) g_fsv = 13;
    else if (!strcmp(e, "capimg")) g_fsv = 14;
    else if (!strcmp(e, "capgroup")) g_fsv = 15;
    else if (!strcmp(e, "capall")) g_fsv = 16;   // 以上全加（= 游戏那份的 capability 清单）
    else if (!strcmp(e, "blitn")) g_fsv = 17;    // N4-m：`blit` + isnan 守卫（OpIsNan/OpSelect）+ precise（NoContraction）
    else {
        LOG("【黄】DM_TRI_FS_VARIANT=%s 认不出来（只认识 cap / 3out / 3out_cap / iface / blit / if_ubo / if_smp / "
            "smp1 / ubo4 / ubo0 / decl / blitn）——按关处理", e);
        return 0;
    }
    LOG("【查】DM_TRI_FS_VARIANT=%s：阶梯用的那份**我们自己的**片段着色器换成变体 %d"
        "（1 = 纯红 + `OpCapability FragmentShaderPixelInterlockEXT` + `OpExtension \"SPV_EXT_fragment_shader_interlock\"`、"
        "指令一条没多；2 = 纯红但写 3 个输出 Location 0/1/2；3 = 两者合起来；"
        "4 = **与游戏那份逐项相同的描述符接口**（2 个 UBO + 3 张 sampler2D）+ 极简代码（输出恒纯红）；"
        "5 = 同接口 + **照游戏那份反汇编逐条复刻的算术**；6 = **只声明两个 UBO**；7 = **只声明三张 sampler2D**）"
        "—— 叠在**已知落屏**的 `vso` 档上就是单变量；判据：已知色底盘 152 上出现非 152 = 仍然落屏。"
        "自证见 .work/logs/n4k-fs-variants.txt（`spirv-dis` 的 capability / 输出装饰 / 接口逐项对照）", e, g_fsv);
    return g_fsv;
}

int vk_tri_ladder_mode(void) {
    if (!vk_tri_game_on()) return VK_TRI_LADDER_OFF;   // 仍由 DM_TRI_IN_GAME 定"哪一次绘制 / 哪一段通道"
    ladder_parse();
    return g_ladder;
}

int vk_tri_ladder_arm(uint64_t draw_index, uint64_t color0_ser) {
    if (!vk_tri_game_on()) return 0;
    ladder_parse();
    if (!g_ladder) return 0;
    if (g_in_pass) return 0;                            // 这一段通道里已经插过一次（与 N4-h 共用同一个标志）
    if (g_draw_no && g_draw_no != draw_index) return 0;
    if (g_color0_ser && color0_ser != g_color0_ser) return 0;
    // **这一档这次真的画了**才把"这一段通道已插过"置上（由回放层在录完之后调 `vk_tri_ladder_done`）：
    // 因为还有一道"这一次绘制适不适合当采样点"的形状判据（三角形族 + 索引/顶点数 ≥ 3）——形状不合适时
    // 要能继续往这一段通道里的**下一次**绘制走，否则一次 3 个索引的线带就把整段通道的采样点占掉
    // （N4-i 第一轮的真机现场：`@28` 打到的正是"索引数 3 的线带"，那一次读数回答不了题目）。
    return 1;
}

void vk_tri_ladder_done(void) { g_in_pass = 1; }

// 我们自己的三份着色器（包成 vk_shader 交给 vk_pipe_graphics；缺省关时一次都不建）
static vk_shader g_lsh[3];
static vk_spv_in g_lsh_in[1];
static int g_lsh_made;

const vk_shader *vk_tri_ladder_shader(int which) {
    if (which < 0 || which > 2) return NULL;
    if (!g_lsh_made) {
        g_lsh_made = 1;
        VkDevice dev = vk_dev();
        if (dev == VK_NULL_HANDLE) return NULL;
        VkShaderModuleCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = (size_t)VK_TRI_GAME_VS_WORDS * 4; ci.pCode = VK_TRI_GAME_VS;
        VkResult r0 = vkCreateShaderModule(dev, &ci, NULL, &g_lsh[0].mod);
        snprintf(g_lsh[0].key, sizeof g_lsh[0].key, "n4i-tri-vs");
        g_lsh[0].stage = VK_STAGE_VS;
        ci.codeSize = (size_t)VK_TRI_LADDER_VB_VS_WORDS * 4; ci.pCode = VK_TRI_LADDER_VB_VS;
        VkResult r1 = vkCreateShaderModule(dev, &ci, NULL, &g_lsh[1].mod);
        snprintf(g_lsh[1].key, sizeof g_lsh[1].key, "n4i-tri-vbvs");
        g_lsh[1].stage = VK_STAGE_VS;
        // 与 ladder.vert.spv 的 `layout(location = 0) in vec4 a_p` 逐项对上（自证见 .work/logs/tri-ladder-spv.txt 第二节）
        g_lsh_in[0].loc = 0; g_lsh_in[0].comps = 4; g_lsh_in[0].ctype = VK_SPV_F32;
        g_lsh[1].in = g_lsh_in; g_lsh[1].nin = 1;
        // N4-k：片段那一份按 `DM_TRI_FS_VARIANT` 选（缺省 = N4-h 那份纯红，一字不改）
        int fsv = vk_tri_fs_variant();
        const uint32_t *fsw = VK_TRI_GAME_FS; size_t fsn = (size_t)VK_TRI_GAME_FS_WORDS;
        if (fsv == 1) { fsw = VK_TRI_FS_CAP; fsn = (size_t)VK_TRI_FS_CAP_WORDS; }
        else if (fsv == 2) { fsw = VK_TRI_FS_3OUT; fsn = (size_t)VK_TRI_FS_3OUT_WORDS; }
        else if (fsv == 3) { fsw = VK_TRI_FS_3OUT_CAP; fsn = (size_t)VK_TRI_FS_3OUT_CAP_WORDS; }
        else if (fsv == 4) { fsw = VK_TRI_FS_IFACE; fsn = (size_t)VK_TRI_FS_IFACE_WORDS; }
        else if (fsv == 5) { fsw = VK_TRI_FS_BLIT; fsn = (size_t)VK_TRI_FS_BLIT_WORDS; }
        else if (fsv == 6) { fsw = VK_TRI_FS_IF_UBO; fsn = (size_t)VK_TRI_FS_IF_UBO_WORDS; }
        else if (fsv == 7) { fsw = VK_TRI_FS_IF_SMP; fsn = (size_t)VK_TRI_FS_IF_SMP_WORDS; }
        else if (fsv == 8) { fsw = VK_TRI_FS_SMP1; fsn = (size_t)VK_TRI_FS_SMP1_WORDS; }
        else if (fsv == 9) { fsw = VK_TRI_FS_UBO4; fsn = (size_t)VK_TRI_FS_UBO4_WORDS; }
        else if (fsv == 10) { fsw = VK_TRI_FS_UBO0; fsn = (size_t)VK_TRI_FS_UBO0_WORDS; }
        else if (fsv == 11) { fsw = VK_TRI_FS_DECL; fsn = (size_t)VK_TRI_FS_DECL_WORDS; }
        else if (fsv == 12) { fsw = VK_TRI_FS_CAPF64; fsn = (size_t)VK_TRI_FS_CAPF64_WORDS; }
        else if (fsv == 13) { fsw = VK_TRI_FS_CAPGATHER; fsn = (size_t)VK_TRI_FS_CAPGATHER_WORDS; }
        else if (fsv == 14) { fsw = VK_TRI_FS_CAPIMG; fsn = (size_t)VK_TRI_FS_CAPIMG_WORDS; }
        else if (fsv == 15) { fsw = VK_TRI_FS_CAPGROUP; fsn = (size_t)VK_TRI_FS_CAPGROUP_WORDS; }
        else if (fsv == 16) { fsw = VK_TRI_FS_CAPALL; fsn = (size_t)VK_TRI_FS_CAPALL_WORDS; }
        else if (fsv == 17) { fsw = VK_TRI_FS_BLITN; fsn = (size_t)VK_TRI_FS_BLITN_WORDS; }
        // -------------------------------------------------------------------
        // N4-l：**这一份 FS 也要走"按阶段分空间"与"反射"**（以前没有）。
        // 游戏那几份是在 `vk_shader.c` 的 `load()` 里做的（PS 的绑定整体挪到 set 1，然后把 SPIR-V 声明的绑定
        // 抄进 `sh->spv`，那是**描述符集布局的唯一权威来源**）。而这三份是我们直接在 `vk_tri_game.c` 里建模块的：
        // `spv` 一直是空的 ⇒ 布局里一条绑定都没有。以前没有变体声明描述符，所以从来没露出来；N4-l 一开始要
        // "只声明一条绑定"，这个窟窿就变成主变量：**着色器声明了 set1/binding N，而管线布局里没有这一条**
        // ⇒ 这次绘制在 API 层面就是错的（Adreno 上同样可能"不产片元"，而且它会把"读描述符"这个题目污染掉）。
        // 所以这里照 `load()` 的顺序做一遍：copy → `vk_spv_force_set(…, 1)` → 建模块 → `vk_spv_scan`。
        // -------------------------------------------------------------------
        uint32_t *fsbuf = malloc(fsn * 4);
        VkResult r2 = VK_ERROR_OUT_OF_HOST_MEMORY;
        int nforced = -1, nref = -1, nfin = 0;
        if (fsbuf) {
            memcpy(fsbuf, fsw, fsn * 4);
            nforced = vk_spv_force_set(fsbuf, fsn * 4, 1);
            if (nforced >= 0) {
                ci.codeSize = fsn * 4; ci.pCode = fsbuf;
                r2 = vkCreateShaderModule(dev, &ci, NULL, &g_lsh[2].mod);
                if (r2 == VK_SUCCESS) {
                    g_lsh[2].spv = calloc(VK_SPV_MAXB, sizeof *g_lsh[2].spv);
                    g_lsh[2].in = calloc(VK_SPV_MAXIN, sizeof *g_lsh[2].in);
                    if (g_lsh[2].spv && g_lsh[2].in) {
                        nref = vk_spv_scan(fsbuf, fsn * 4, g_lsh[2].spv, VK_SPV_MAXB, g_lsh[2].in, VK_SPV_MAXIN, &nfin);
                        if (nref >= 0) { g_lsh[2].nspv = (uint32_t)nref; g_lsh[2].nin = (uint32_t)nfin; }
                    }
                }
            }
            free(fsbuf);
        }
        snprintf(g_lsh[2].key, sizeof g_lsh[2].key, "n4i-tri-fs%s",
                 fsv == 1 ? "-cap" : fsv == 2 ? "-3out" : fsv == 3 ? "-3outcap"
               : fsv == 4 ? "-iface" : fsv == 5 ? "-blit"
               : fsv == 6 ? "-ifubo" : fsv == 7 ? "-ifsmp"
               : fsv == 8 ? "-smp1" : fsv == 9 ? "-ubo4" : fsv == 10 ? "-ubo0" : fsv == 11 ? "-decl"
               : fsv == 12 ? "-capf64" : fsv == 13 ? "-capgather" : fsv == 14 ? "-capimg"
               : fsv == 15 ? "-capgroup" : fsv == 16 ? "-capall" : fsv == 17 ? "-blitn" : "");
        g_lsh[2].stage = VK_STAGE_PS;
        if (r0 != VK_SUCCESS) g_lsh[0].mod = VK_NULL_HANDLE;
        if (r1 != VK_SUCCESS) g_lsh[1].mod = VK_NULL_HANDLE;
        if (r2 != VK_SUCCESS) g_lsh[2].mod = VK_NULL_HANDLE;
        LOG("【查】DM_TRI_LADDER：我们自己的三份着色器模块（全屏三角形 VS / 读顶点缓冲 VS / 纯红 FS）建出来了没"
            " —— VkResult %d / %d / %d（0 = 建出来了）；读顶点缓冲那份声明的输入 = Location 0、4 分量 float"
            "（与 .work/logs/tri-ladder-spv.txt 的自证逐项对上）；**片段那一份的变体 = %d**"
            "（0 = 纯红缺省 / 1 = +interlock capability / 2 = 3 个输出 / 3 = 两者 / "
            "4 = 游戏的描述符接口 + 极简代码 / 5 = 同接口 + 复刻游戏算术 / 8 = 一条 sampler2D / "
            "9 = 一条池内 UBO(binding 4) / 10 = 一条 support_buffer(binding 0) / 11 = 五条都声明但一条不读；"
            "见 DM_TRI_FS_VARIANT）——⚠ 这一份只在 `use_ps` 取到它的档里生效（`vso` / `vs` / `vb` / `vsfullf`），"
            "而 `vsf` / `vsfull` 写死用**游戏那份** FS（N4-l 量到的仪器错误：N4-k 的 iface/blit/if_ubo/if_smp 就是跑在 vsfull 上）",
            (int)r0, (int)r1, (int)r2, fsv);
        LOG("【查】DM_TRI_LADDER：片段那一份的**反射**（N4-l 新增）—— 挪到 set 1 的绑定 %d 条、`vk_spv_scan` 读到 %d 条"
            "（读不到 = -1）、声明了 %d 个顶点输入；**布局里能看到的绑定就是这几条**"
            "（以前这里一条都没有 ⇒ 变体明明声明了描述符、布局却是空的）",
            nforced, nref, nfin);
    }
    return g_lsh[which].mod ? &g_lsh[which] : NULL;
}

void vk_tri_ladder_stats(uint64_t *inserts, uint64_t *nopipe) {
    if (inserts) *inserts = atomic_load(&g_n4i_ins);
    if (nopipe) *nopipe = atomic_load(&g_n4i_nopipe);
}

void vk_tri_ladder_note(void) { atomic_fetch_add(&g_n4i_ins, 1); }

// `DM_TRI_DECL_DS=1`（**缺省关**）：阶梯自己的状态也**声明游戏那份深度/模板附件格式**。
// 为什么要这一档：`vb`（我们的状态，**不声明**深度格式）**两轮落屏**，而 `vs`/`vsn`（游戏的状态，
// `vk_pipe_graphics` 会把 `prc.depthAttachmentFormat = st->depth_fmt` 声明上）**三轮不落屏** ——
// 两者之间还差着"**深度/模板附件格式声明**"这一样（Vulkan 的规矩：管线如果不做深度/模板测试，
// 声明成 UNDEFINED 是合法的；一旦声明了就必须与当前通道的附件格式一致，否则这条绘制是未定义的）。
// 用法：把它叠在**已知落屏**的 `vb` 档上 ⇒ 单变量（声明/不声明）。
// `DM_TRI_MASK_ALL=1`（**缺省关**）：阶梯自己的状态把**每个颜色附件**的写掩码都开成 0xf。
// 为什么要这一档（本轮最重要的一条待证）：`vso`（游戏的顶点着色器 + 我们自己的状态、掩码**只开附件 0**）
// **落屏**（整片 85），而 `vsn`（同一对管线，但状态用游戏那一刻的 + `DM_RASTER_BRUTE=1`）
// **不落屏** —— 两者在我方管线上的差别只剩"**没被片段着色器写的附件 1/2 也声明了 `0xf`**"（brute 强制的）、
// 以及 `frontFace`（剔除关时不起作用）。这一档就是把它变成单变量：
//   · `vso` + 本开关**不落屏** ⇒ **"对没有片段输出的附件声明非 0 写掩码"这条绘制会整个丢掉**（这台驱动上的行为）；
//   · 落屏 ⇒ 那个差别不是它，得回到 `frontFace` / 其它声明逐一排（`vs`/`vsn` 那两档另找解释）。
// 这条如果成立，**对 N4-e 的 `DM_RASTER_BRUTE` 也是个大前提**：brute 把**每个**附件的掩码都开成 0xf ⇒
// 对"片段着色器只写一部分附件"的管线，brute 会把能落屏的绘制**弄丢**（那种轮里的"仍无内容"因此不能全算数）。
int vk_tri_mask_all(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_TRI_MASK_ALL");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v) LOG("【查】DM_TRI_MASK_ALL=1：阶梯自己的管线把**每个**颜色附件的写掩码都开成 0xf"
                   "（叠在 `vso` 档上就是单变量：只开附件 0 vs 全开）");
    }
    return v;
}

int vk_tri_decl_ds(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_TRI_DECL_DS");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        if (v) LOG("【查】DM_TRI_DECL_DS=1：阶梯自己的管线也**声明游戏那份深度/模板附件格式**"
                   "（叠在 `vb` 档上就是单变量：声明 vs 不声明）");
    }
    return v;
}

// ============================================================================
// N4-j：**把"游戏那一刻的状态"逐项接上**（`DM_TRI_*_GAME=1` / `DM_TRI_FRONT=<vk 值>`；缺省关）
// ============================================================================
// 要回答的那句话写死在 vk_tri_game.h 里那张表的上方：**"游戏那一刻的状态 / 那条管线的声明细节里，
// 哪一项让片元消失？"** N4-i 的四档阶梯已经把着色器、顶点数据、顶点流、索引、计数、描述符、
// 写掩码全开、光栅化侧全放开逐条排除或修掉，剩下的就只有这一格。做法与纪律（照 N4-i 的形状）：
//   · 起点 = **已知落屏**的 `vso` 档（游戏的顶点着色器 + 我们自己的状态；两轮实测整片 85）；
//   · 每个开关只改**一个字段**（形状照 `DM_TRI_MASK_ALL`）；
//   · 判据写死：`DM_PASS_PROBE=28` 的已知色底子（152）上**出现非 152 的值 = 这一档仍然落屏**；
//     **哪一项一接上就从"整片 85"变成"整片 152"，责任就落在那一项上**；
//   · `DM_TRI_GAME_ALL=1` = 一次接全部，做**对照**：它应当复现 `vsn` 的"不落屏"；若它仍然落屏 ⇒
//     这张字段表**不覆盖机制**（这本身是读数，不是失败），得去别处找。
// 为什么方向是"从落屏那一档往上加"而不是"从不落屏那一档往下减"：加法的每一步都以一条**能落屏**的
// 管线为基准，答案直接是"这一项"，而减法只能知道"少了它也不够"（坑 ⑨：判据要能回答题目）。
static uint32_t g_field_mask;
static int g_field_parsed;
static int g_field_front = -1;      // -1 = 没给（`DM_TRI_FRONT` 没设）

static int env_flag(const char *name) {
    const char *e = getenv(name);
    return (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
}

static int field_names(uint32_t m, char *buf, size_t cap) {
    if (!buf || cap < 8) return 0;
    int n = 0;
    static const struct { uint32_t bit; const char *name; } tab[] = {
        { VK_TRI_F_TOPO, "拓扑" }, { VK_TRI_F_CULL, "剔除+前向面" }, { VK_TRI_F_FRONT, "前向面" },
        { VK_TRI_F_BLEND, "混合" }, { VK_TRI_F_DEPTH, "深度/模板" }, { VK_TRI_F_A2C, "a2c" },
        { VK_TRI_F_BIAS, "偏置" }, { VK_TRI_F_POLY, "多边形" }, { VK_TRI_F_MASK, "掩码" },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (!(m & tab[i].bit)) continue;
        int w = snprintf(buf + n, cap - (size_t)n, "%s%s", n ? "+" : "", tab[i].name);
        if (w <= 0 || (size_t)(n + w) >= cap) break;
        n += w;
    }
    if ((m & VK_TRI_F_FRONT) && g_field_front >= 0) {
        int w = snprintf(buf + n, cap - (size_t)n, "(=%d)", g_field_front);
        if (w > 0 && (size_t)(n + w) < cap) n += w;
    }
    return n;
}

uint32_t vk_tri_field_mask(void) {
    if (g_field_parsed) return g_field_mask;
    g_field_parsed = 1;
    if (env_flag("DM_TRI_GAME_ALL")) g_field_mask = 0x1ffu;
    if (env_flag("DM_TRI_TOPO_GAME")) g_field_mask |= VK_TRI_F_TOPO;
    if (env_flag("DM_TRI_CULL_GAME")) g_field_mask |= VK_TRI_F_CULL;
    if (env_flag("DM_TRI_BLEND_GAME")) g_field_mask |= VK_TRI_F_BLEND;
    if (env_flag("DM_TRI_DEPTH_GAME")) g_field_mask |= VK_TRI_F_DEPTH;
    if (env_flag("DM_TRI_A2C_GAME")) g_field_mask |= VK_TRI_F_A2C;
    if (env_flag("DM_TRI_BIAS_GAME")) g_field_mask |= VK_TRI_F_BIAS;
    if (env_flag("DM_TRI_POLY_GAME")) g_field_mask |= VK_TRI_F_POLY;
    if (env_flag("DM_TRI_MASK_GAME")) g_field_mask |= VK_TRI_F_MASK;
    {
        const char *e = getenv("DM_TRI_FRONT");
        if (e && *e) { g_field_front = (int)strtol(e, NULL, 0) ? 1 : 0; g_field_mask |= VK_TRI_F_FRONT; }
    }
    if (g_field_mask) {
        char b[256]; int n = field_names(g_field_mask, b, sizeof b);
        LOG("【查】DM_TRI_ 逐项接上：%s —— 叠在**已知落屏**的 `vso` 档上，一次只改一样；"
            "判据：`DM_PASS_PROBE=28` 的已知色底子（152）上**出现非 152 的值 = 这一档仍然落屏**",
            n ? b : "（空）");
        if (g_field_mask == 0x1ffu)
            LOG("【查】DM_TRI_GAME_ALL=1：**一次接全部**（对照）—— 它应当复现 `vsn` 的「不落屏」；"
                "若它仍然落屏，说明这张字段表不覆盖机制（读数，不是失败）");
    }
    return g_field_mask;
}

int vk_tri_field_front(int *out) {
    vk_tri_field_mask();                 // 保证解析过
    if (g_field_front < 0) return 0;
    if (out) *out = g_field_front;
    return 1;
}

int vk_tri_fields_desc(char *buf, size_t cap) {
    uint32_t m = vk_tri_field_mask();
    if (!m) return 0;
    return field_names(m, buf, cap);
}

void vk_tri_apply_fields(vk_gfx_state *o, const vk_gfx_state *g) {
    if (!o || !g) return;
    uint32_t m = vk_tri_field_mask();
    if (!m) return;
    if (m & VK_TRI_F_TOPO) o->topo = g->topo;
    // 剔除面与前向面是一对：剔除关着时前向面无意义，只看剔除面会得出"不是它"的假结论
    if (m & VK_TRI_F_CULL) { o->cull = g->cull; o->front = g->front; }
    {
        int fv = -1;
        if ((m & VK_TRI_F_FRONT) && vk_tri_field_front(&fv)) o->front = (VkFrontFace)fv;
    }
    if (m & VK_TRI_F_BLEND) {
        for (int i = 0; i < 8; i++) {
            o->blend_en[i] = g->blend_en[i];
            o->src_rgb[i] = g->src_rgb[i]; o->dst_rgb[i] = g->dst_rgb[i];
            o->src_a[i] = g->src_a[i]; o->dst_a[i] = g->dst_a[i];
            o->eq_rgb[i] = g->eq_rgb[i]; o->eq_a[i] = g->eq_a[i];
        }
    }
    if (m & VK_TRI_F_DEPTH) {
        o->depth_test = g->depth_test; o->depth_write = g->depth_write; o->depth_op = g->depth_op;
        o->stencil_en = g->stencil_en;
        for (int i = 0; i < 2; i++) {
            o->stencil_func[i] = g->stencil_func[i];
            o->stencil_vmask[i] = g->stencil_vmask[i]; o->stencil_wmask[i] = g->stencil_wmask[i];
            o->stencil_fail[i] = g->stencil_fail[i];
            o->stencil_zfail[i] = g->stencil_zfail[i];
            o->stencil_zpass[i] = g->stencil_zpass[i];
        }
        // 格式声明**不在这里**（那是 `DM_TRI_DECL_DS` 的一条独立单变量，N4-i 已在 `vb` 档上量过）
    }
    if (m & VK_TRI_F_A2C) o->a2c = g->a2c;
    if (m & VK_TRI_F_BIAS) {
        o->bias_en = g->bias_en;
        o->bias_factor = g->bias_factor; o->bias_units = g->bias_units; o->bias_clamp = g->bias_clamp;
    }
    if (m & VK_TRI_F_POLY) o->poly = g->poly;
    if (m & VK_TRI_F_MASK) for (int i = 0; i < 8; i++) o->write_mask[i] = g->write_mask[i];
}
