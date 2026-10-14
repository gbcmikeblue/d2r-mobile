// APFS 的 clonefile 在安卓上没有：用普通文件拷贝替身（语义一致，只是慢一些）。
// 宿主用途（dm/host/hle_fs.c）：存档事务的快照 / 提交，语义是"把 src 完整复制成 dst"。
// APFS clonefile does not exist on Android: a plain file-copy stand-in (same semantics, just slower).
// Host use (dm/host/hle_fs.c): save-transaction snapshot / commit; the semantics are "copy src to dst in full".
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define CLONE_NOFOLLOW 0x0001
#define CLONE_NOOWNERCOPY 0x0002

static inline int dm_clone_copy(const char *src, const char *dst) {
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    struct stat st;
    if (fstat(in, &st) != 0) { close(in); return -1; }
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
    return 0;
}

static inline int clonefile(const char *src, const char *dst, int flags) {
    (void)flags;
    return dm_clone_copy(src, dst);
}
static inline int clonefileat(int src_dirfd, const char *src, int dst_dirfd, const char *dst, int flags) {
    (void)src_dirfd; (void)dst_dirfd; (void)flags;
    return dm_clone_copy(src, dst);
}
