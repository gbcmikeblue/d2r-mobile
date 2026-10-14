// Apple CommonCrypto 的摘要接口在安卓上没有：给宿主提供同名替身。
// 用途（dm/host）：loader.c 的 CC_SHA256（DM_LOADHASH 诊断）、hle_tact.c 的 CC_MD5（取样核对）、
// dm_padmode.c 的 CC_SHA1（识别界面原文串）、diag.c 的 CC_MD5（解压基准）。
// 图形的回放层也用 CC_SHA256——**着色器的键就是 sha256(控制段)[:16]**（见 dm/gfx/README：运行时
// nvnProgramSetShaders 拿到控制段指针，回放层算出与映射表里同一个键）。占位实现会让所有着色器算出同一个键、
// 一条管线都建不起来，所以这一条必须是真实现。
// No Apple CommonCrypto digest API on Android: same-named stand-ins for the host.
// Uses in dm/host: CC_SHA256 in loader.c (DM_LOADHASH diagnostics), CC_MD5 in hle_tact.c (sample checks),
// CC_SHA1 in dm_padmode.c (identifying original UI strings), CC_MD5 in diag.c (decompression benchmark).
// The graphics replay layer uses CC_SHA256 too -- **a shader's key is sha256(control segment)[:16]** (see
// dm/gfx/README: at run time nvnProgramSetShaders receives the control-segment pointer and the replay layer
// computes the same key the mapping table uses). A placeholder would give every shader the same key and not a
// single pipeline could be built, so this one has to be real.
//
// 状态：**SHA-256 是真实现**（FIPS 180-4，真机跑标准向量核对，见 dm/android/tests/t_digest.c）；
// CC_MD5 与 CC_SHA1 仍是占位（它们只服务于诊断与界面串识别，不挡任何通路），要补时照 SHA-256 的样子加。
// Status: **SHA-256 is real** (FIPS 180-4, checked against the standard vectors on a device, see
// dm/android/tests/t_digest.c); CC_MD5 and CC_SHA1 are still placeholders (they only serve diagnostics and UI
// string identification and block no path), to be added the same way as SHA-256 when needed.
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint32_t CC_LONG;

#define CC_MD5_DIGEST_LENGTH 16
#define CC_SHA1_DIGEST_LENGTH 20
#define CC_SHA256_DIGEST_LENGTH 32

typedef struct { uint32_t n; uint64_t len; uint8_t buf[64]; } CC_MD5_CTX;
typedef struct { uint32_t n; uint64_t len; uint8_t buf[64]; } CC_SHA1_CTX;
// SHA-256 的上下文：8 个状态字 + 已喂入字节数 + 未满一组的尾巴。苹果那边的结构体布局我们不用照抄
// （这是替身自己的头，没有任何一处依赖它的字段顺序）。
// SHA-256 context: the eight state words, the number of bytes fed in, and the tail that has not filled a
// block yet. The Apple struct layout is not copied (this is the stand-in's own header and nothing depends on
// its field order).
typedef struct { uint32_t h[8]; uint64_t len; uint32_t n; uint8_t buf[64]; } CC_SHA256_CTX;

static inline void dm_digest_placeholder(void *md, size_t out_len) {
    if (md) for (size_t i = 0; i < out_len; i++) ((uint8_t *)md)[i] = 0;
}

static inline void *CC_MD5(const void *data, CC_LONG len, unsigned char *md) {
    (void)data; (void)len; dm_digest_placeholder(md, CC_MD5_DIGEST_LENGTH); return md;
}
static inline void *CC_SHA1(const void *data, CC_LONG len, unsigned char *md) {
    (void)data; (void)len; dm_digest_placeholder(md, CC_SHA1_DIGEST_LENGTH); return md;
}

// ---------- SHA-256（FIPS 180-4）----------
// 按标准写：64 步、K 为前 64 个素数立方根小数部分的前 32 位，初值为前 8 个素数平方根小数部分的前 32 位。
// The standard construction: 64 rounds, K is the first 32 bits of the fractional parts of the cube roots of
// the first 64 primes, and the initial values are the first 32 bits of the fractional parts of the square
// roots of the first 8 primes.
static const uint32_t dm_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};
static inline uint32_t dm_sha256_ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static inline void dm_sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = dm_sha256_ror(w[i - 15], 7) ^ dm_sha256_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = dm_sha256_ror(w[i - 2], 17) ^ dm_sha256_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = dm_sha256_ror(e, 6) ^ dm_sha256_ror(e, 11) ^ dm_sha256_ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + dm_sha256_k[i] + w[i];
        uint32_t S0 = dm_sha256_ror(a, 2) ^ dm_sha256_ror(a, 13) ^ dm_sha256_ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static inline int CC_SHA256_Init(CC_SHA256_CTX *c) {
    if (!c) return 0;
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u; c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu; c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->len = 0; c->n = 0;
    return 1;
}
static inline int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len) {
    if (!c || (!data && len)) return 0;
    const uint8_t *p = (const uint8_t *)data;
    c->len += len;
    while (len) {
        uint32_t take = 64 - c->n;
        if (take > len) take = len;
        for (uint32_t i = 0; i < take; i++) c->buf[c->n + i] = p[i];
        c->n += take; p += take; len -= (CC_LONG)take;
        if (c->n == 64) { dm_sha256_block(c->h, c->buf); c->n = 0; }
    }
    return 1;
}
static inline int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    if (!c || !md) return 0;
    uint64_t bits = c->len * 8;
    uint8_t pad[72];
    uint32_t np = (c->n < 56) ? (56 - c->n) : (120 - c->n);
    pad[0] = 0x80;
    for (uint32_t i = 1; i < np; i++) pad[i] = 0;
    for (int i = 0; i < 8; i++) pad[np + i] = (uint8_t)(bits >> (56 - i * 8));
    CC_SHA256_Update(c, pad, (CC_LONG)(np + 8));
    for (int i = 0; i < 8; i++) {
        md[i * 4] = (uint8_t)(c->h[i] >> 24); md[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        md[i * 4 + 2] = (uint8_t)(c->h[i] >> 8); md[i * 4 + 3] = (uint8_t)c->h[i];
    }
    return 1;
}
static inline void *CC_SHA256(const void *data, CC_LONG len, unsigned char *md) {
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, data, len);
    CC_SHA256_Final(md, &c);
    return md;
}
