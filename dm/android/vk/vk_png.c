// 见 vk_png.h 的说明：最小 PNG 写出，无依赖。
// See vk_png.h: a minimal, dependency-free PNG writer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vk_png.h"

// ---------- CRC32（PNG 每个块尾要它）----------
// ---------- CRC32 (every PNG chunk ends with it) ----------
static uint32_t g_crc_tab[256];
static int g_crc_ready;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[n] = c;
    }
    g_crc_ready = 1;
}

static uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t c) {
    c = ~c;
    for (size_t i = 0; i < n; i++) c = g_crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}

// ---------- Adler32（zlib 流尾要它）----------
// ---------- Adler32 (the zlib stream ends with it) ----------
static uint32_t adler32_of(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}

static void be32(uint8_t *o, uint32_t v) { o[0] = (uint8_t)(v >> 24); o[1] = (uint8_t)(v >> 16); o[2] = (uint8_t)(v >> 8); o[3] = (uint8_t)v; }

static int wr(FILE *f, const uint8_t *p, size_t n) { return n == 0 || fwrite(p, 1, n, f) == n; }

// 一块 = 长度 + 类型 + 数据 + CRC（CRC 覆盖类型与数据）
// A chunk is length + type + data + CRC (the CRC covers the type and the data)
static int chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t h[8];
    be32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    if (!wr(f, h, 8) || !wr(f, data, n)) return 0;
    uint32_t c = crc32_of((const uint8_t *)type, 4, 0);
    c = crc32_of(data, n, c);
    uint8_t e[4];
    be32(e, c);
    return wr(f, e, 4);
}

int vk_png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    if (!g_crc_ready) crc_init();
    if (!path || !rgba || !w || !h) return 0;

    // 原始扫描线：每行前面加一个过滤字节 0（不过滤）
    // Raw scanlines: one filter byte of 0 (no filtering) in front of every row
    size_t row = 1 + (size_t)w * 4, raw = (size_t)h * row;
    uint8_t *buf = malloc(raw);
    if (!buf) return 0;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *d = buf + (size_t)y * row;
        d[0] = 0;
        memcpy(d + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }

    // zlib 流：两字节头 + 若干存储块（每个最多 65535 字节）+ 四字节 adler32。
    // 存储块的头是「末块位 + 两字节长度 + 两字节长度的反码」，不是压缩数据——所以这个流只做封装。
    // The zlib stream: a two-byte header, stored blocks (at most 65535 bytes each) and a four-byte adler32.
    // A stored block's header is "last flag + 16-bit length + its complement" with the bytes following verbatim, so
    // this stream only wraps the data.
    size_t nblk = (raw + 65534) / 65535, zcap = 2 + raw + nblk * 5 + 4;
    uint8_t *z = malloc(zcap);
    if (!z) { free(buf); return 0; }
    size_t zn = 0;
    z[zn++] = 0x78;   // CMF：deflate、32K 窗口 | CMF: deflate with a 32K window
    z[zn++] = 0x01;   // FLG：不用预设字典、校验等级 0、头两字节能被 31 整除 | FLG: no preset dictionary, check level 0
    for (size_t off = 0; off < raw;) {
        size_t blk = raw - off > 65535 ? 65535 : raw - off;
        int last = (off + blk >= raw);
        uint16_t len = (uint16_t)blk, nlen = (uint16_t)~len;
        z[zn++] = (uint8_t)(last ? 1 : 0);
        z[zn++] = (uint8_t)(len & 0xff); z[zn++] = (uint8_t)(len >> 8);
        z[zn++] = (uint8_t)(nlen & 0xff); z[zn++] = (uint8_t)(nlen >> 8);
        memcpy(z + zn, buf + off, blk); zn += blk; off += blk;
    }
    uint32_t ad = adler32_of(buf, raw);
    be32(z + zn, ad); zn += 4;
    free(buf);

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return 0; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    uint8_t ihdr[13];
    be32(ihdr, w); be32(ihdr + 4, h);
    ihdr[8] = 8;    // 位深 | bit depth
    ihdr[9] = 6;    // 颜色类型 6 = 真彩 + alpha | colour type 6 = truecolour with alpha
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   // 压缩 / 过滤 / 隔行 都取标准值 | compression, filter and interlace at their standard values
    int ok = wr(f, sig, 8) && chunk(f, "IHDR", ihdr, sizeof ihdr) && chunk(f, "IDAT", z, zn) && chunk(f, "IEND", NULL, 0);
    ok = (fclose(f) == 0) && ok;
    free(z);
    return ok;
}
