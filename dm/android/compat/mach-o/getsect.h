// Apple 的 mach-o 段表查询在安卓上没有：给最小替身。
// 宿主用途：在自己（Mac 宿主）的 Mach-O 映像里按名字找段——安卓上是 ELF，
// 后续应按实际用途改成 ELF 段查找（记为待办）；当前先返回未找到，保证能编过。
// Apple's mach-o section lookup does not exist on Android: minimal stand-in.
// Host use: look up a section by name inside the Mac host's own Mach-O image. Android is ELF,
// so this should be replaced by an ELF section lookup later (tracked as a follow-up); for now it
// reports "not found" so that the build proceeds.
#pragma once

#include <stdint.h>
#include <stddef.h>

struct mach_header { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags; };
struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
typedef struct mach_header mach_header_t;

#define MH_MAGIC_64 0xfeedfacfu

static inline const uint8_t *getsectiondata(const struct mach_header *mhp, const char *segname, const char *sectname,
                                            unsigned long *size) {
    (void)mhp; (void)segname; (void)sectname;
    if (size) *size = 0;
    return NULL;
}
static inline const uint8_t *getsegmentdata(const struct mach_header *mhp, const char *segname, unsigned long *size) {
    (void)mhp; (void)segname;
    if (size) *size = 0;
    return NULL;
}
static inline const struct mach_header *dm_main_mach_header(void) { return NULL; }
