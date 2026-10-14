// 安卓入口：把宿主需要的路径环境变量与工作目录备好，再交给 dm_main。
// dm_main 就是 dm/host/main.c 里的 main —— 与 iOS 同一条路（那边以 -Dmain=dm_main 编入，
// 见 dm/ios/Sources/main.m 的声明），这样 main.c 本身不必为安卓多开一个入口分支。
// Android entry point: prepare the path environment variables and working directory the host needs,
// then hand over to dm_main. dm_main is main() from dm/host/main.c -- the same route iOS takes
// (compiled in there with -Dmain=dm_main, see the declaration in dm/ios/Sources/main.m), so main.c
// itself needs no extra entry branch for Android.
//
// 目录约定（全部相对工作区根，用 DM_ROOT 指过去；不设则取可执行文件所在目录）：
//   <根>/patched/  改写器产出的模块（main / sdk / subsdk0）
//   <根>/romfs/    原版 RomFS（data/ 下那批档案）
//   <根>/save/     存档            <根>/temp/  临时文件
// Directory layout (all relative to the workspace root, pointed at by DM_ROOT; if unset, the
// directory holding the executable is used):
//   <root>/patched/  the rewriter's output modules (main / sdk / subsdk0)
//   <root>/romfs/    the original RomFS (the archives under data/)
//   <root>/save/     saves        <root>/temp/  temporary files
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int dm_main(int argc, char **argv);

static void mkdir_if_needed(const char *p) {
    if (mkdir(p, 0755) != 0 && errno != EEXIST) fprintf(stderr, "【红】建目录 %s 失败：%s\n", p, strerror(errno));
}
// 文件/目录在不在 / whether a path exists
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

int main(int argc, char **argv) {
    // 工作区根：DM_ROOT 优先；否则取可执行文件所在目录（/proc/self/exe 在安卓上可用）
    // Workspace root: DM_ROOT wins; otherwise the directory holding the executable (/proc/self/exe works on Android)
    char root[4096] = "";
    const char *envroot = getenv("DM_ROOT");
    if (envroot && *envroot) {
        snprintf(root, sizeof root, "%s", envroot);
    } else {
        char exe[4096]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n <= 0) { fprintf(stderr, "【红】取不到自身路径（/proc/self/exe）：%s\n", strerror(errno)); return 2; }
        exe[n] = 0;
        snprintf(root, sizeof root, "%s", exe);
        char *slash = strrchr(root, '/'); if (slash) *slash = 0;
    }
    while (strlen(root) > 1 && root[strlen(root) - 1] == '/') root[strlen(root) - 1] = 0;
    setenv("DM_ROOT", root, 1);

    char buf[4096];
    // 存档与临时目录：宿主 hle_fs.c 的缺省就是 <DM_ROOT>/save 与 <DM_ROOT>/temp，这里只负责把目录建出来
    // Save and temp directories: hle_fs.c already defaults to <DM_ROOT>/save and <DM_ROOT>/temp, so this only creates them
    snprintf(buf, sizeof buf, "%s/save", root); mkdir_if_needed(buf);
    snprintf(buf, sizeof buf, "%s/temp", root); mkdir_if_needed(buf);

    // RomFS：调用方可以自己用 DM_ROMFS 指；否则按候选顺序自动探测，**前面的优先**，并把选中的那个打出来。
    //   ① <根>/romfs、<根>/data/romfs   首选：资源放工作区里（本机上就是 /data/local/tmp/dm/romfs），
    //                                  与工作区同分区、读盘最快
    //   ② /sdcard/d2rm/romfs、/sdcard/Download/d2rm/romfs   后备：重启不丢、手机文件管理器里能直接看到
    //                                  （走 FUSE，加载稍慢）。放一份在这里就不会被系统清理影响
    //   ③ /data/local/d2rm/romfs        需要 root 才能建（/data/local 是 root:root 且不给 shell 写），
    //                                  列在最后只是"有就用"，没有也不提示
    // 顺序反映的是决定本身（2026-10-14：保持工作区里那份为主，重启风险用 /sdcard 那份兜底）；
    // 做成"按顺序找、找到即用"而不是写死，换目录时只管搬文件、宿主不用改。
    // RomFS: the caller may point at it with DM_ROMFS; otherwise candidates are probed in this order (earlier
    // ones win) and the chosen one is printed.
    //   1. <root>/romfs, <root>/data/romfs   preferred: resources live inside the workspace (on this device that
    //                                 is /data/local/tmp/dm/romfs), same partition, fastest reads
    //   2. /sdcard/d2rm/romfs, /sdcard/Download/d2rm/romfs   fallback: survives a reboot and is visible in the
    //                                 phone's file manager (over FUSE, so loading is a bit slower). Keeping a copy
    //                                 here means the system cleaning /data/local/tmp does not matter
    //   3. /data/local/d2rm/romfs      needs root to create (/data/local is root:root and not writable by shell);
    //                                 listed last simply as "use it if it is there", with no warning otherwise
    // The order reflects the decision itself (2026-10-14: keep the workspace copy as the primary and use the
    // /sdcard copy as cover for the reboot risk); "probe in order, use the first hit" rather than hard-coding is
    // what makes a directory change a matter of moving files only, with no host change.
    if (!getenv("DM_ROMFS")) {
        char c_ws[4096], c_wsdata[4096];
        snprintf(c_ws, sizeof c_ws, "%s/romfs", root);
        snprintf(c_wsdata, sizeof c_wsdata, "%s/data/romfs", root);
        const char *cands[] = { c_ws, c_wsdata,
                                "/sdcard/d2rm/romfs", "/sdcard/Download/d2rm/romfs",
                                "/data/local/d2rm/romfs" };
        for (unsigned i = 0; i < sizeof cands / sizeof cands[0]; i++)
            if (exists(cands[i])) { setenv("DM_ROMFS", cands[i], 1); fprintf(stderr, "romfs: %s\n", cands[i]); break; }
        if (!getenv("DM_ROMFS"))
            fprintf(stderr, "romfs: 【黄】候选目录都没有（%s、/sdcard/d2rm/romfs…），"
                            "也没有 DM_ROMFS——资源读不到（先跑通到 UNIMPL 不受影响）\n", c_ws);
    }

    // 存档：宿主不设 DM_SAVE 会报红中止（它必须指向本局可写的那份），这里给 <根>/save 作缺省——
    // 与 DM_ROMFS 同一套做法，目录已经建出来了，空目录起步没问题。
    // Saves: the host reports a failure and aborts when DM_SAVE is unset (it must point at this run's writable
    // copy), so default it to <root>/save -- the same treatment as DM_ROMFS, and the directory already exists,
    // so starting from an empty one is fine.
    if (!getenv("DM_SAVE")) {
        snprintf(buf, sizeof buf, "%s/save", root);
        setenv("DM_SAVE", buf, 1);
        fprintf(stderr, "save: %s\n", buf);
    }

    // 工作目录设成工作区根：环境变量里的相对路径都落在这里
    // Set the working directory to the workspace root: relative paths in environment variables land here
    if (chdir(root)) fprintf(stderr, "【红】chdir %s 失败：%s\n", root, strerror(errno));

    // 模块目录：命令行第一个参数优先，否则 <根>/patched
    // Module directory: the first command-line argument wins, otherwise <root>/patched
    static char moddir[4096];
    if (argc > 1 && argv[1][0]) snprintf(moddir, sizeof moddir, "%s", argv[1]);
    else snprintf(moddir, sizeof moddir, "%s/patched", root);

    // 交给宿主：与 iOS 那边同形的参数（dmhost <模块目录> main sdk）
    // Hand over to the host with the same argument shape iOS uses (dmhost <module dir> main sdk)
    char *host_argv[8]; int host_argc = 0;
    char extra[3][4096]; int nextra = 0;
    for (int i = 2; i < argc && nextra < 3; i++) { snprintf(extra[nextra], sizeof extra[0], "%s", argv[i]); nextra++; }
    host_argv[host_argc++] = (char *)"dmhost";
    host_argv[host_argc++] = moddir;
    for (int i = 0; i < nextra; i++) host_argv[host_argc++] = extra[i];
    if (!nextra) { host_argv[host_argc++] = (char *)"main"; host_argv[host_argc++] = (char *)"sdk"; }

    fprintf(stderr, "安卓宿主：根 %s，模块 %s，参数 %d 个\n", root, moddir, host_argc);
    int rc = dm_main(host_argc, host_argv);
    fprintf(stderr, "dm_main 返回 %d\n", rc);
    return rc;
}
