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

    // RomFS：调用方可以自己用 DM_ROMFS 指；否则看 <根>/romfs 在不在
    // RomFS: the caller may point at it with DM_ROMFS; otherwise look for <root>/romfs
    if (!getenv("DM_ROMFS")) {
        snprintf(buf, sizeof buf, "%s/romfs", root);
        if (exists(buf)) { setenv("DM_ROMFS", buf, 1); fprintf(stderr, "romfs: %s\n", buf); }
        else fprintf(stderr, "romfs: 【黄】没找到 %s，也没有 DM_ROMFS——资源读不到（先跑通到 UNIMPL 不受影响）\n", buf);
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
