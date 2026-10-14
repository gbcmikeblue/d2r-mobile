// APFS 的 clonefile 在安卓上没有：用递归拷贝替身（语义一致，只是慢一些）。
// 宿主用途（dm/host/hle_fs.c）：存档事务的快照 / 提交，语义是「把 src 这棵**树**完整复制成 dst」。
// 注意真的是树：宿主克隆的是存档目录，只拷单个文件的替身会在第一个存档事务上以 EISDIR 失败
//（真机上就是这样：FATAL 存档事务：clonefile …/save → …/save.work 失败 errno=21）。
// APFS's clonefile does not exist on Android: a recursive copy stands in for it (same semantics, just
// slower). Host use (dm/host/hle_fs.c): save-transaction snapshot / commit, whose semantics are "copy the
// whole **tree** from src to dst". It really is a tree: the host clones save directories, and a single-file
// stand-in fails with EISDIR on the very first save transaction (which is what happened on the real device:
// FATAL save transaction: clonefile .../save -> .../save.work failed errno=21).
//
// 实现在 dm_android_compat.c：递归函数不该塞进每个编译单元都会吃的强制包含头（兼容层是用 -include 强加的）。
// The implementation lives in dm_android_compat.c: a recursive function does not belong in a force-included
// header (the compat layer is imposed onto every translation unit with -include).
#pragma once

#define CLONE_NOFOLLOW 0x0001
#define CLONE_NOOWNERCOPY 0x0002

// 与 macOS 同签名：成功 0，失败 -1 并置 errno | Same signature as macOS: 0 on success, -1 with errno set
int clonefile(const char *src, const char *dst, int flags);
int clonefileat(int src_dirfd, const char *src, int dst_dirfd, const char *dst, int flags);
