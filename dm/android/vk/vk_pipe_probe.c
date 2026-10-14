// 管线层的自检探针（不进宿主、不进 APK，只在设备上单独跑）：
//  ① 给产物目录里**每一份**着色器建一次描述符集布局 + 管线布局 —— 这等于把"每个 binding 号是什么类型、
//     属于哪个阶段"这套解读交给驱动判一遍，全库都过就说明解读是对的；
//  ② 给每个**计算**着色器建一条计算管线 —— 计算管线不需要任何渲染状态，是唯一能在补齐影子状态之前
//     闭环验证「反射 ↔ SPIR-V ↔ 驱动」的地方（图形管线要等顶点输入与状态对象补齐，见 vk_pipe.c 的文件头）。
// A self-check probe for the pipeline layer (not part of the host or any APK; it runs on a device by itself):
//   1. build a descriptor set layout plus pipeline layout for **every** shader in the artifact directory -- that
//      hands the reading of "which type is this binding number, which stage does it belong to" to the driver, and
//      a clean sweep over the whole library means the reading is right;
//   2. build a compute pipeline for every **compute** shader -- a compute pipeline needs no render state and is
//      the only place where "reflection <-> SPIR-V <-> driver" can be closed before the shadow state is complete
//      (graphics pipelines wait for vertex input and the state objects; see the header of vk_pipe.c).
//
// 跑法 | How to run:
//   adb push vk_pipe_probe /data/local/tmp/dm/ && adb shell chmod 755 /data/local/tmp/dm/vk_pipe_probe
//   adb shell "cd /data/local/tmp/dm && ./vk_pipe_probe"     # 退出码 = 失败项数 | exit code = number of failures
#include "vk_device.h"
#include "vk_shader.h"
#include "vk_pipe.h"
#include "dm.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static double now_s(void) { struct timeval tv; gettimeofday(&tv, NULL); return (double)tv.tv_sec + tv.tv_usec * 1e-6; }

int main(void) {
    double t0 = now_s();
    vk_device_init();
    if (!vk_dev()) { printf("【红】Vulkan 设备没起来\n"); return 2; }
    vk_shader_init();
    const char *dir = vk_shader_dir();
    printf("=== 管线层自检 ===\n目录 %s\n", dir);

    DIR *d = opendir(dir);
    if (!d) { printf("【红】打不开目录\n"); return 2; }
    uint32_t nsh = 0, ncs = 0, nlay = 0, npipe = 0;
    uint32_t miss_total = 0, miss_shown = 0;   // 交叉核对：SPIR-V 有、res.json 没有的绑定数 | cross-check: bindings in the SPIR-V but not in res.json
    uint32_t fail_dsl = 0, fail_pipe = 0;
    char first_dsl_bad[32] = "", first_pipe_bad[32] = "";
    uint64_t binds = 0;
    uint32_t ngfx_try = 0, ngfx_ok = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l != VK_SHADER_KEY_LEN + 4 || strcmp(de->d_name + VK_SHADER_KEY_LEN, ".spv")) continue;
        char key[VK_SHADER_KEY_LEN + 1];
        memcpy(key, de->d_name, VK_SHADER_KEY_LEN);
        key[VK_SHADER_KEY_LEN] = 0;
        const vk_shader *sh = vk_shader_by_key(key, VK_STAGE_NONE);
        if (!sh) continue;
        nsh++;
        // 头两份把 C 版扫出来的**完整**绑定清单打出来（与 PowerShell 的真值对照，定位种类判错的根因）
        // Print the **full** binding list the C scanner produced for the first two shaders (to compare against the
        // PowerShell ground truth and pin down why a kind is misjudged)
        if (nsh <= 2) {
            printf("  C 版扫描 %s（阶段 %d）：", sh->key, sh->stage);
            for (uint32_t j = 0; j < sh->nspv; j++)
                printf("b%u=%s ", sh->spv[j].binding, vk_spv_kind_name(sh->spv[j].kind));
            printf("\n");
        }
        const vk_shader *one[1] = { sh };
        if (vk_pipe_layout(one, 1) == VK_NULL_HANDLE) {
            fail_dsl++;
            if (!first_dsl_bad[0]) snprintf(first_dsl_bad, sizeof first_dsl_bad, "%s", key);
        } else nlay++;
        binds += sh->ncbuf + sh->nsbuf + sh->ntex;

        // 交叉核对：SPIR-V 里声明了、而 res.json 里没有对应条目的绑定。真机上就是这类绑定把管线挡掉的
        //（res.json 只说数据从哪来，不保证列全 SPIR-V 的绑定，见 vk_spv.h）。
        // Cross-check: bindings the SPIR-V declares that res.json has no entry for. These are exactly what blocked
        // pipeline creation on the device (res.json says only where the data comes from and need not list every
        // binding, see vk_spv.h).
        for (uint32_t k = 0; k < sh->nspv; k++) {
            const vk_spv_bind *b = &sh->spv[k];
            int have = 0;
            for (uint32_t j = 0; j < sh->ncbuf && !have; j++) if (sh->cbuf[j].binding == b->binding) have = 1;
            for (uint32_t j = 0; j < sh->nsbuf && !have; j++) if (sh->sbuf[j].binding == b->binding) have = 1;
            for (uint32_t j = 0; j < sh->ntex && !have; j++) if (sh->tex[j].binding == b->binding) have = 1;
            if (have) continue;
            miss_total++;
            if (miss_shown < 4) {
                miss_shown++;
                printf("  交叉核对：%s 的 set%u/b%u（%s）在 res.json 里没有条目\n",
                       sh->key, b->set, b->binding, vk_spv_kind_name(b->kind));
            }
            (void)0;
        }

        // 图形管线自检：给顶点着色器建一条（不带片元阶段 → 光栅化丢弃，规范如此要求）。顶点输入取自
        // **着色器声明的输入**，影子属性表故意留空 → 每条属性都走常量回退。这样验的是管线构造本身：
        // 属性/绑定/格式、混合与写掩码、深度模板、动态状态、以及动态渲染的附件格式。
        // Graphics pipeline self-check: build one for a vertex shader (no fragment stage -> rasterizer discard, as
        // the spec requires). The vertex input comes from the **inputs the shader declares** and the shadow
        // attribute table is deliberately left empty, so every attribute takes the constant fallback. What this
        // verifies is the construction itself: attributes/bindings/formats, blend and write masks, depth-stencil,
        // dynamic state, and the dynamic-rendering attachment formats.
        if (sh->stage == VK_STAGE_VS && ngfx_try < 60) {
            ngfx_try++;
            vk_gfx_state g;
            memset(&g, 0, sizeof g);
            memcpy(g.vs_key, sh->key, VK_SHADER_KEY_LEN);
            g.topo = vk_topo_of(4);
            g.color_count = 1; g.color[0] = VK_FORMAT_R8G8B8A8_UNORM;
            g.write_mask[0] = 0xF;
            g.poly = VK_POLYGON_MODE_FILL; g.cull = VK_CULL_MODE_NONE; g.front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            g.samples = 1;
            if (vk_pipe_graphics(sh, NULL, &g) != VK_NULL_HANDLE) ngfx_ok++;
        }
        if (sh->stage == VK_STAGE_CS) {
            ncs++;
            if (vk_pipe_compute(sh) != VK_NULL_HANDLE) npipe++;
            else {
                fail_pipe++;
                if (!first_pipe_bad[0]) snprintf(first_pipe_bad, sizeof first_pipe_bad, "%s", key);
                // 失败时把四个诊断变体各试一次：哪个建得成，就说明差的是哪一样（见 vk_pipe.h）
                // On failure try each of the four diagnostic variants: whichever succeeds says what was missing
                static const char *vn[4] = { "加 push constant", "补缺失号(uniform)", "补缺失号(存储)", "补缺失号(贴图)" };
                char tried[128] = "";
                for (int v = 1; v <= 4; v++) {
                    if (vk_pipe_compute_variant(sh, v) == VK_NULL_HANDLE) continue;
                    size_t l = strlen(tried);
                    snprintf(tried + l, sizeof tried - l, "%s%s", l ? "、" : "", vn[v - 1]);
                }
                printf("  诊断：%s 被哪种变体救活 → %s\n", key, tried[0] ? tried : "（四种都不行）");
            }
        }
    }
    closedir(d);

    uint64_t dsl = 0, lay = 0, cpso = 0, gpso = 0, fail = 0;
    vk_pipe_stats(&dsl, &lay, &cpso, &gpso, &fail);
    printf("着色器 %u 份（其中计算 %u），绑定合计 %llu\n",
           nsh, ncs, (unsigned long long)binds);
    printf("描述符集布局 / 管线布局：成功 %u、失败 %u%s%s\n", nlay, fail_dsl,
           fail_dsl ? "（首个失败 " : "", fail_dsl ? first_dsl_bad : "");
    printf("计算管线：成功 %u、失败 %u%s%s\n", npipe, fail_pipe,
           fail_pipe ? "（首个失败 " : "", fail_pipe ? first_pipe_bad : "");
    printf("计数：描述符集布局 %llu、管线布局 %llu、计算管线 %llu、图形管线 %llu、失败 %llu\n",
           (unsigned long long)dsl, (unsigned long long)lay, (unsigned long long)cpso,
           (unsigned long long)gpso, (unsigned long long)fail);
    printf("交叉核对：SPIR-V 有而 res.json 没有的绑定共 %u 条（布局以 SPIR-V 为准，所以这不影响建管线）\n", miss_total);
    printf("图形管线（顶点着色器各建一条、属性走常量回退）：试 %u、成功 %u%s\n", ngfx_try, ngfx_ok,
           ngfx_try && ngfx_ok != ngfx_try ? "  ← 有失败，看上面的【红】" : "");
    printf("耗时 %.1f 秒（布局按键去重，所以句柄数少于着色器数）\n", now_s() - t0);
    printf("%s\n", (fail_dsl || fail_pipe) ? "✗ 有失败：先看上面的键与 VkResult" : "✓ 全库布局与全部计算管线都被驱动收下");
    return (int)(fail_dsl + fail_pipe);
}
