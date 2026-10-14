// Apple CommonCrypto 的摘要接口在安卓上没有：给宿主提供同名替身。
// 用途（dm/host）：loader.c 的 CC_SHA256（DM_LOADHASH 诊断）、hle_tact.c 的 CC_MD5（取样核对）、
// dm_padmode.c 的 CC_SHA1（识别界面原文串）、diag.c 的 CC_MD5（解压基准）。
// No Apple CommonCrypto digest API on Android: same-named stand-ins for the host.
// Uses in dm/host: CC_SHA256 in loader.c (DM_LOADHASH diagnostics), CC_MD5 in hle_tact.c (sample checks),
// CC_SHA1 in dm_padmode.c (identifying original UI strings), CC_MD5 in diag.c (decompression benchmark).
//
// 注意：这里目前是**占位实现**（输出定长零），只保证能编过、能跑起来；真实摘要算法待补（见文档待办）。
// NOTE: these are currently PLACEHOLDERS (fixed-length zero output) so that the build and the run work;
// the real digest algorithms are still to be implemented (tracked in the plan document).
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint32_t CC_LONG;

#define CC_MD5_DIGEST_LENGTH 16
#define CC_SHA1_DIGEST_LENGTH 20
#define CC_SHA256_DIGEST_LENGTH 32

typedef struct { uint32_t n; uint64_t len; uint8_t buf[64]; } CC_MD5_CTX;
typedef struct { uint32_t n; uint64_t len; uint8_t buf[64]; } CC_SHA1_CTX;
typedef struct { uint32_t n; uint64_t len; uint8_t buf[64]; } CC_SHA256_CTX;

static inline void dm_digest_placeholder(void *md, size_t out_len) {
    if (md) for (size_t i = 0; i < out_len; i++) ((uint8_t *)md)[i] = 0;
}

static inline void *CC_MD5(const void *data, CC_LONG len, unsigned char *md) {
    (void)data; (void)len; dm_digest_placeholder(md, CC_MD5_DIGEST_LENGTH); return md;
}
static inline void *CC_SHA1(const void *data, CC_LONG len, unsigned char *md) {
    (void)data; (void)len; dm_digest_placeholder(md, CC_SHA1_DIGEST_LENGTH); return md;
}
static inline void *CC_SHA256(const void *data, CC_LONG len, unsigned char *md) {
    (void)data; (void)len; dm_digest_placeholder(md, CC_SHA256_DIGEST_LENGTH); return md;
}

static inline int CC_SHA256_Init(CC_SHA256_CTX *c) { if (c) { c->n = 0; c->len = 0; } return 1; }
static inline int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len) {
    (void)data; if (c) { c->len += len; c->n += len; } return 1;
}
static inline int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    (void)c; dm_digest_placeholder(md, CC_SHA256_DIGEST_LENGTH); return 1;
}
