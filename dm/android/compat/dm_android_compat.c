// 安卓兼容层里"必须是真符号"的那几个函数（汇编要直接调用，内联不行；递归实现也不该塞进强制包含头）。
// 其余兼容物都是头文件里的宏与 static inline，见 dm_android_compat.h。
// The few compat-layer functions that must be real symbols: the assembly calls some of them directly so an
// inline will not do, and recursive implementations do not belong in a force-included header. Everything
// else is a macro or a static inline in dm_android_compat.h.
#include "dm_android_compat.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

// 与 Apple 的语义一致：单调、纳秒、不随系统时间调整而跳变。
// Matches Apple's semantics: monotonic, nanoseconds, unaffected by wall-clock adjustments.
uint64_t mach_absolute_time(void) {
    return clock_gettime_nsec_np(CLOCK_MONOTONIC);
}

// ---------- clonefile：递归拷贝替身 ----------
// ---------- clonefile: the recursive-copy stand-in ----------

// xattr 一起带走：宿主把客体原名存在里面（hle_fs.c 的 FS_GUEST_NAME），存档事务必须把它复制过去，
// 否则克隆出来的工作目录会丢掉「物理名 → 客体名」的映射。文件系统不支持 xattr 就放过——那只是映射降级，
// 不该让整个存档事务失败。
// xattrs come along: the host stores guest names in them (FS_GUEST_NAME in hle_fs.c) and a save transaction
// must copy them, or the cloned working directory loses its physical-name-to-guest-name mapping. A filesystem
// without xattr support is let through -- that only degrades the mapping and must not fail the whole
// transaction.
static void clone_xattrs(const char *src, const char *dst) {
    ssize_t n = listxattr(src, NULL, 0);
    if (n <= 0) return;
    char *names = malloc((size_t)n);
    if (!names) return;
    n = listxattr(src, names, (size_t)n);
    if (n <= 0) { free(names); return; }
    for (char *p = names; p < names + n; p += strlen(p) + 1) {
        ssize_t v = getxattr(src, p, NULL, 0);
        if (v < 0) continue;
        void *val = malloc(v ? (size_t)v : 1);
        if (!val) break;
        if (getxattr(src, p, val, (size_t)v) == v) setxattr(dst, p, val, (size_t)v, 0);
        free(val);
    }
    free(names);
}

static int clone_tree(const char *src, const char *dst, int flags) {
    struct stat st;
    if (lstat(src, &st) != 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 07777) != 0 && errno != EEXIST) return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *e;
        int rc = 0;
        while (!rc && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char s[4096], t[4096];
            if (snprintf(s, sizeof s, "%s/%s", src, e->d_name) >= (int)sizeof s ||
                snprintf(t, sizeof t, "%s/%s", dst, e->d_name) >= (int)sizeof t) { errno = ENAMETOOLONG; rc = -1; break; }
            rc = clone_tree(s, t, flags);
        }
        closedir(d);
        if (!rc) clone_xattrs(src, dst);
        return rc;
    }

    if (S_ISLNK(st.st_mode)) {
        char buf[4096];
        ssize_t n = readlink(src, buf, sizeof buf - 1);
        if (n < 0) return -1;
        buf[n] = 0;
        if (unlink(dst) != 0 && errno != ENOENT) return -1;
        return symlink(buf, dst);
    }

    if (!S_ISREG(st.st_mode)) { errno = EINVAL; return -1; }

    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) { close(in); return -1; }
    char buf[1 << 16];
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; close(in); close(out); return -1; }
        if (n == 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) { if (errno == EINTR) continue; close(in); close(out); return -1; }
            off += w;
        }
    }
    if (close(in) != 0) { close(out); return -1; }
    if (close(out) != 0) return -1;
    clone_xattrs(src, dst);
    return 0;
}

int clonefile(const char *src, const char *dst, int flags) { return clone_tree(src, dst, flags); }

// clonefileat 的两个 dirfd：宿主只按绝对路径用，这里忽略 dirfd（相对路径的场景用不到）。
// The two dirfds of clonefileat: the host only ever uses absolute paths, so they are ignored here (the
// relative-path case does not arise).
int clonefileat(int src_dirfd, const char *src, int dst_dirfd, const char *dst, int flags) {
    (void)src_dirfd; (void)dst_dirfd;
    return clone_tree(src, dst, flags);
}
