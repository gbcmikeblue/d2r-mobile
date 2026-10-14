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
    uint32_t fail_dsl = 0, fail_pipe = 0;
    char first_dsl_bad[32] = "", first_pipe_bad[32] = "";
    uint64_t binds = 0;
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
        const vk_shader *one[1] = { sh };
        if (vk_pipe_layout(one, 1) == VK_NULL_HANDLE) {
            fail_dsl++;
            if (!first_dsl_bad[0]) snprintf(first_dsl_bad, sizeof first_dsl_bad, "%s", key);
        } else nlay++;
        binds += sh->ncbuf + sh->nsbuf + sh->ntex;
        if (sh->stage == VK_STAGE_CS) {
            ncs++;
            if (vk_pipe_compute(sh) != VK_NULL_HANDLE) npipe++;
            else { fail_pipe++; if (!first_pipe_bad[0]) snprintf(first_pipe_bad, sizeof first_pipe_bad, "%s", key); }
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
    printf("耗时 %.1f 秒（布局按键去重，所以句柄数少于着色器数）\n", now_s() - t0);
    printf("%s\n", (fail_dsl || fail_pipe) ? "✗ 有失败：先看上面的键与 VkResult" : "✓ 全库布局与全部计算管线都被驱动收下");
    return (int)(fail_dsl + fail_pipe);
}
