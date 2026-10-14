// 着色器产物的自检探针（不进宿主、不进 APK，只在设备上单独跑）：
// 把产物目录里**每一个** <键>.spv 真的建成 VkShaderModule，并解析同名的 <键>.res.json。
// Why it is worth a separate run: whether this driver accepts the SPIR-V and whether the reflection file parses
// are the only two things that can be verified independently before pipelines exist. Once both pass here, a
// failing pipeline can only be a state-machinery problem.
// A self-check probe for the shader artifacts (not part of the host or any APK; it runs on a device by itself):
// it really creates a VkShaderModule for **every** <key>.spv in the artifact directory and parses the
// matching <key>.res.json.
//
// 跑法 | How to run:
//   adb push vk_shader_probe /data/local/tmp/dm/ && adb shell chmod 755 /data/local/tmp/dm/vk_shader_probe
//   adb shell "cd /data/local/tmp/dm && ./vk_shader_probe"     # 退出码 0 = 全部好 | exit code 0 means all good
#include "vk_device.h"
#include "vk_shader.h"
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
    printf("=== 着色器产物自检 ===\n目录 %s\n", dir);

    DIR *d = opendir(dir);
    if (!d) { printf("【红】打不开目录\n"); return 2; }
    uint32_t nvs = 0, nps = 0, ncs = 0, nother = 0, nsample = 0;
    uint64_t cbuf = 0, sbuf = 0, tex = 0;
    char first_bad[32] = "";
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l != VK_SHADER_KEY_LEN + 4 || strcmp(de->d_name + VK_SHADER_KEY_LEN, ".spv")) continue;
        char key[VK_SHADER_KEY_LEN + 1];
        memcpy(key, de->d_name, VK_SHADER_KEY_LEN);
        key[VK_SHADER_KEY_LEN] = 0;
        const vk_shader *sh = vk_shader_by_key(key, VK_STAGE_NONE);
        if (!sh) { if (!first_bad[0]) snprintf(first_bad, sizeof first_bad, "%s", key); continue; }
        switch (sh->stage) {
            case VK_STAGE_VS: nvs++; break;
            case VK_STAGE_PS: nps++; break;
            case VK_STAGE_CS: ncs++; break;
            default: nother++; break;
        }
        cbuf += sh->ncbuf; sbuf += sh->nsbuf; tex += sh->ntex;
        if (nsample < 3 && sh->ntex) {   // 打几条样例便于人眼核对 | print a few samples for eyeballing
            nsample++;
            printf("  样例 %s 阶段 %d：cbuf %u（binding→槽 ",
                   sh->key, sh->stage, sh->ncbuf);
            for (uint32_t i = 0; i < sh->ncbuf && i < 3; i++) printf("%u→%d ", sh->cbuf[i].binding, sh->cbuf[i].slot);
            printf("）sbuf %u（", sh->nsbuf);
            for (uint32_t i = 0; i < sh->nsbuf && i < 3; i++)
                printf("%u→c%d+%d ", sh->sbuf[i].binding, sh->sbuf[i].cb_slot, sh->sbuf[i].cb_offset);
            printf("）tex %u（", sh->ntex);
            for (uint32_t i = 0; i < sh->ntex && i < 3; i++)
                printf("%u:type%d ", sh->tex[i].binding, sh->tex[i].type);
            printf("）\n");
        }
    }
    closedir(d);

    uint32_t loaded, missing, bad;
    vk_shader_stats(&loaded, &missing, &bad);
    printf("阶段分布：顶点 %u、像素 %u、计算 %u、其它 %u\n", nvs, nps, ncs, nother);
    printf("绑定合计：uniform %llu、存储 %llu、贴图/图像 %llu（%u 个着色器）\n",
           (unsigned long long)cbuf, (unsigned long long)sbuf, (unsigned long long)tex, loaded);
    printf("装载：成功 %u、缺产物 %u、坏产物 %u%s%s\n", loaded, missing, bad,
           bad ? "（首个坏键 " : "", bad ? first_bad : "");
    printf("耗时 %.1f 秒\n", now_s() - t0);
    printf("%s\n", bad ? "✗ 有坏产物：建管线前要先解决" : "✓ 全部 SPIR-V 都能被这台驱动收下，反射都能解析");
    return bad ? 1 : 0;
}
