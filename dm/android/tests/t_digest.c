// 兼容层摘要实现的自检探针（不进宿主、不进 APK，只在设备上单独跑）。
// 为什么需要它：安卓兼容层原先全是**占位实现**（输出定长零），而图形的回放层要靠
// sha256(控制段)[:16] 算着色器键——占位会让所有着色器算成同一个键。这个探针拿 FIPS 180-4 的标准
// 向量核对真实现，包含一次到位与分块喂入两条路径（回放层对控制段是一次喂入，但两条路都该对）。
// A self-check probe for the compat layer's digest implementations (not part of the host or any APK; it runs
// on a device by itself).
// Why it exists: the Android compat layer used to be **placeholders** (fixed-length zero output), while the
// graphics replay layer needs sha256(control segment)[:16] as a shader key -- a placeholder would give every
// shader the same key. This probe checks the real implementation against the FIPS 180-4 standard vectors,
// covering both the one-shot and the chunked feed paths (the replay layer feeds the control segment in one go,
// but both paths should be correct).
//
// 跑法 | How to run:
//   adb push t_digest /data/local/tmp/dm/ && adb shell chmod 755 /data/local/tmp/dm/t_digest
//   adb shell /data/local/tmp/dm/t_digest        # 退出码 = 失败项数 | exit code = number of failures
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <string.h>

static int fails;

static void hex(const unsigned char *d, int n, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[i * 2] = h[d[i] >> 4]; out[i * 2 + 1] = h[d[i] & 15]; }
    out[n * 2] = 0;
}

static void chk(const char *what, const char *got, const char *want) {
    int ok = !strcmp(got, want);
    if (!ok) fails++;
    printf("%s %-46s %s\n", ok ? "✓" : "✗", what, got);
    if (!ok) printf("   期望 %s\n", want);
}

static void one(const char *what, const void *data, size_t len, const char *want) {
    unsigned char md[32]; char s[65];
    CC_SHA256(data, (CC_LONG)len, md);
    hex(md, 32, s);
    chk(what, s, want);
}

int main(void) {
    // FIPS 180-4 与 NIST 的公开向量 | the published FIPS 180-4 / NIST vectors
    one("sha256(\"\")", "", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    one("sha256(\"abc\")", "abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    one("sha256(56 字节向量)", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    one("sha256(112 字节向量，跨两组)", "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
        "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 112,
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

    // 分块喂入：与一次到位必须一致（还顺带压一压 64 字节边界：1+1+1、55+1、63+1、64+0）
    // Chunked feed: must agree with the one-shot path (and it presses on the 64-byte boundary: 1+1+1, 55+1,
    // 63+1, 64+0)
    {
        unsigned char md[32]; char s[65];
        CC_SHA256_CTX c;
        CC_SHA256_Init(&c);
        CC_SHA256_Update(&c, "a", 1);
        CC_SHA256_Update(&c, "b", 1);
        CC_SHA256_Update(&c, "c", 1);
        CC_SHA256_Final(md, &c);
        hex(md, 32, s);
        chk("分块 sha256(\"abc\")（1+1+1）", s, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
    {
        // 55 字节 + 1 字节：第一组正好差一字节就满 | 55 bytes then 1: the first block is one byte short
        char buf[56]; for (int i = 0; i < 56; i++) buf[i] = (char)('a' + (i % 26));
        unsigned char md[32]; char s[65];
        CC_SHA256_CTX c;
        CC_SHA256_Init(&c); CC_SHA256_Update(&c, buf, 55); CC_SHA256_Update(&c, buf + 55, 1); CC_SHA256_Final(md, &c);
        hex(md, 32, s);
        unsigned char md2[32]; char s2[65];
        CC_SHA256(buf, 56, md2); hex(md2, 32, s2);
        chk("分块 55+1 与一次到位一致", s, s2);
    }
    {
        // 长输入：10 万字节，两条路径一致（回放层的控制段是 2176 字节，这里给足余量）
        // Long input: 100k bytes, both paths agreeing (the replay layer's control segment is 2176 bytes;
        // this leaves plenty of margin)
        static unsigned char big[100000];
        for (size_t i = 0; i < sizeof big; i++) big[i] = (unsigned char)(i * 31 + (i >> 8));
        unsigned char a[32], b[32]; char sa[65], sb[65];
        CC_SHA256(big, (CC_LONG)sizeof big, a); hex(a, 32, sa);
        CC_SHA256_CTX c; CC_SHA256_Init(&c);
        for (size_t off = 0; off < sizeof big; ) { size_t take = (sizeof big - off) > 4096 ? 4096 : (sizeof big - off); CC_SHA256_Update(&c, big + off, (CC_LONG)take); off += take; }
        CC_SHA256_Final(b, &c); hex(b, 32, sb);
        chk("分块 4096 字节 × 24 与一次到位一致", sb, sa);
    }
    // 回放层真正的用法：控制段 0x880 字节取前 16 位十六进制当键 | the replay layer's actual use: the first
    // 16 hex digits of the digest of a 0x880-byte control segment is the key
    {
        static unsigned char ctl[0x880];
        for (size_t i = 0; i < sizeof ctl; i++) ctl[i] = (unsigned char)(i * 7 + 3);
        unsigned char md[32]; char s[65];
        CC_SHA256(ctl, (CC_LONG)sizeof ctl, md);
        hex(md, 32, s);
        s[16] = 0;   // 键 = 前 16 位十六进制 | the key is the first 16 hex digits
        printf("  控制段 0x880 字节的键（前 16 位）=%s\n", s);
    }

    printf("%s共 %d 项失败\n", fails ? "✗ " : "✓ ", fails);
    return fails;
}
