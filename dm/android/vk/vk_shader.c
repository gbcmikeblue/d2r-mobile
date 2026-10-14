// 安卓图形后端的着色器装载实现，见 vk_shader.h 的文件头（键的算法、反射从哪来）。
// Implementation of the Android graphics backend's shader loading; see the header of vk_shader.h for the key
// algorithm and where the reflection comes from.
#include "vk_shader.h"
#include "vk_device.h"
#include "dm.h"
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_dir[4096];
static int g_inited;

// 缓存：键（16 个字符）→ 着色器。开放寻址、线性探测——键的长度固定且只有 16 个字符，比较很便宜。
// 表开 8192 个槽（游戏整局用到的着色器是数以千计，装载后常驻）。
// Cache: key (16 characters) -> shader. Open addressing with linear probing -- the key has a fixed length of
// 16 characters, so comparison is cheap. The table has 8192 slots (a whole run of the game uses thousands of
// shaders, and loaded ones stay resident).
#define SH_TAB 8192
static vk_shader *g_tab[SH_TAB];

static uint32_t g_loaded, g_missing, g_bad;
static uint32_t g_red_missing, g_red_bad;   // 报红次数上限：产物缺一大片时别把日志刷爆 | cap on failure logs: do not flood the log when a whole batch is missing

static uint32_t key_hash(const char *k) {
    uint32_t h = 2166136261u;                  // FNV-1a：键只有 16 字节，够散 | FNV-1a: 16 bytes is short enough for this to spread well
    for (int i = 0; i < VK_SHADER_KEY_LEN; i++) h = (h ^ (uint8_t)k[i]) * 16777619u;
    return h;
}

// ---------- 键 ----------
int vk_shader_ctl_key(const void *control, char out[VK_SHADER_KEY_LEN + 1]) {
    if (!control || !out) return 0;
    uint8_t md[32];
    CC_SHA256(control, VK_SHADER_CTL_LEN, md);
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < VK_SHADER_KEY_LEN / 2; i++) {
        out[i * 2] = hx[md[i] >> 4];
        out[i * 2 + 1] = hx[md[i] & 15];
    }
    out[VK_SHADER_KEY_LEN] = 0;
    return 1;
}

// ---------- 目录 ----------
void vk_shader_init(void) {
    if (g_inited) return;
    g_inited = 1;
    const char *e = getenv("DM_NVN_SPV_DIR");
    if (e && *e) snprintf(g_dir, sizeof g_dir, "%s", e);
    else {
        const char *root = getenv("DM_ROOT");
        snprintf(g_dir, sizeof g_dir, "%s/spv", (root && *root) ? root : ".");
    }
    size_t n = strlen(g_dir);
    while (n > 1 && g_dir[n - 1] == '/') g_dir[--n] = 0;
    // 数一下目录里有多少 .spv / .res.json：这一行是"产物到底推上来了没有"的第一个证据
    // Count the .spv / .res.json files in the directory: this line is the first evidence of whether the
    // artifacts were deployed at all
    DIR *d = opendir(g_dir);
    uint32_t nspv = 0, njson = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t l = strlen(de->d_name);
            if (l > 4 && !strcmp(de->d_name + l - 4, ".spv")) nspv++;
            else if (l > 9 && !strcmp(de->d_name + l - 9, ".res.json")) njson++;
        }
        closedir(d);
    }
    if (!d || (!nspv && !njson))
        LOG("【红】着色器产物目录 %s 里没有 .spv/.res.json：管线一条也建不起来（DM_NVN_SPV_DIR 可指定）", g_dir);
    else
        LOG("【绿】着色器产物就绪：%s（%u 个 .spv，%u 个 .res.json）", g_dir, nspv, njson);
}
const char *vk_shader_dir(void) { vk_shader_init(); return g_dir; }

// ---------- 读整个文件 ----------
static uint8_t *read_all(const char *path, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (*out_n = (size_t)ftell(f)) == (size_t)-1 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    uint8_t *b = malloc(*out_n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, *out_n, f);
    fclose(f);
    b[got] = 0;
    *out_n = got;
    return b;
}

// ---------- 极小的 JSON 扫描器 ----------
// 只服务 <键>.res.json（翻译器机器生成的规整格式：一个顶层对象，里面有若干数组，数组元素是扁平对象，
// 值只有数字、字符串、布尔）。不做通用解析、不建对象树：一次遍历按需取字段。
// Serves only <key>.res.json (a machine-generated, regular format from the translator: one top-level object
// holding arrays whose elements are flat objects with numbers, strings and booleans). No general parsing and no
// object tree: a single pass picks fields out on demand.
static const char *js_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}
static const char *js_skip_str(const char *p) {          // p 在开引号 | p is at the opening quote
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
    return (*p == '"') ? p + 1 : NULL;
}
static const char *js_skip_val(const char *p) {
    p = js_ws(p);
    if (*p == '"') return js_skip_str(p);
    if (*p == '{' || *p == '[') {
        int d = 0;
        for (; *p; p++) {
            if (*p == '"') { p = js_skip_str(p); if (!p) return NULL; p--; continue; }
            if (*p == '{' || *p == '[') d++;
            else if ((*p == '}' || *p == ']') && --d == 0) return p + 1;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}
static const char *js_find(const char *p, const char *key) {   // p 在 '{'；返回该键对应值的起点 | p is at '{'; returns the value start for that key
    size_t kl = strlen(key);
    p = js_ws(p);
    if (*p != '{') return NULL;
    for (p = js_ws(p + 1); *p && *p != '}'; ) {
        if (*p != '"') return NULL;
        const char *ks = p + 1;
        const char *ke = js_skip_str(p);
        if (!ke) return NULL;
        size_t got = (size_t)(ke - 1 - ks);
        int hit = (got == kl) && !memcmp(ks, key, kl);
        p = js_ws(ke);
        if (*p != ':') return NULL;
        p = js_ws(p + 1);
        if (hit) return p;
        p = js_skip_val(p);
        if (!p) return NULL;
        p = js_ws(p);
        if (*p != ',') return NULL;
        p = js_ws(p + 1);
    }
    return NULL;
}
static const char *js_first(const char *p) {              // p 在 '['；空数组返回 NULL | p is at '['; NULL for an empty array
    p = js_ws(p);
    if (*p != '[') return NULL;
    p = js_ws(p + 1);
    return (*p == ']') ? NULL : p;
}
static const char *js_next(const char *p) {               // p 在某个元素；返回下一个元素，到末尾返回 NULL | p is at an element; returns the next one, NULL past the end
    p = js_skip_val(p);
    if (!p) return NULL;
    p = js_ws(p);
    if (*p != ',') return NULL;
    p = js_ws(p + 1);
    return (*p == ']') ? NULL : p;
}
static int js_int(const char *p, int32_t *out) {
    p = js_ws(p);
    char *e; long v = strtol(p, &e, 10);
    if (e == p) return 0;
    *out = (int32_t)v;
    return 1;
}
static int js_u32(const char *p, uint32_t *out) {
    p = js_ws(p);
    char *e; unsigned long v = strtoul(p, &e, 10);
    if (e == p) return 0;
    *out = (uint32_t)v;
    return 1;
}
static int js_bool(const char *p, uint32_t *out) {
    p = js_ws(p);
    if (!strncmp(p, "true", 4)) { *out = 1; return 1; }
    if (!strncmp(p, "false", 5)) { *out = 0; return 1; }
    return 0;
}
static int js_str(const char *p, char *out, size_t n) {
    p = js_ws(p);
    if (*p != '"' || !n) return 0;
    size_t i = 0;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\' && p[1]) { p++; c = *p; if (c == 'n') c = '\n'; else if (c == 't') c = '\t'; }
        if (i + 1 < n) out[i++] = c;
    }
    if (*p != '"') return 0;
    out[i] = 0;
    return 1;
}

// stage 列（vs/ps/cs）→ NVN 阶段号；与 nvn_mtl.m 的 stage_of 指向同一组阶段号 | stage field (vs/ps/cs) -> NVN stage number, the same set nvn_mtl.m's stage_of names
static int stage_of(const char *s) {
    if (!strcmp(s, "vs")) return VK_STAGE_VS;
    if (!strcmp(s, "ps")) return VK_STAGE_PS;
    if (!strcmp(s, "cs")) return VK_STAGE_CS;
    if (!strcmp(s, "hs")) return VK_STAGE_HS;
    if (!strcmp(s, "ds")) return VK_STAGE_DS;
    return VK_STAGE_NONE;
}

// 数一个数组有多少个元素 | count the elements of an array
static uint32_t js_count(const char *a) {
    uint32_t n = 0;
    for (const char *e = js_first(a); e; e = js_next(e)) n++;
    return n;
}

// ---------- 解析 res.json ----------
// 返回 0 表示坏（缺字段、类型不对）；此时调用方按"产物坏了"处理（报红计次），不建管线
// Returns 0 for a broken file (missing field, wrong type); the caller then treats it as a broken artifact
// (logged and counted) and builds no pipeline
static int parse_ref(const char *txt, vk_shader *sh) {
    const char *v;
    if (!(v = js_find(txt, "key")) || !js_str(v, sh->key, sizeof sh->key)) return 0;
    if (strlen(sh->key) != VK_SHADER_KEY_LEN) return 0;
    char st[8];
    if (!(v = js_find(txt, "stage")) || !js_str(v, st, sizeof st)) return 0;
    int sg = stage_of(st);
    if (sg == VK_STAGE_NONE) return 0;
    if (sh->stage == VK_STAGE_NONE) sh->stage = sg;      // 调用方指定了阶段就以它为准（两者本该一致）| a caller-supplied stage wins (the two should agree anyway)

    // cbufs：binding ↔ 客体常量段槽号 | cbufs: binding <-> guest constant slot
    if ((v = js_find(txt, "cbufs"))) {
        uint32_t n = js_count(v);
        if (n) {
            sh->cbuf = calloc(n, sizeof *sh->cbuf);
            if (!sh->cbuf) return 0;
            for (const char *e = js_first(v); e && sh->ncbuf < n; e = js_next(e)) {
                vk_cbuf_ref *r = &sh->cbuf[sh->ncbuf];
                const char *b;
                if (!(b = js_find(e, "binding")) || !js_u32(b, &r->binding)) continue;
                if ((b = js_find(e, "slot"))) js_int(b, &r->slot);
                sh->ncbuf++;
            }
        }
    }
    // sbufs：存储缓冲——地址在 c[cb_slot][cb_offset] | sbufs: storage buffers, whose address lives at c[cb_slot][cb_offset]
    if ((v = js_find(txt, "sbufs"))) {
        uint32_t n = js_count(v);
        if (n) {
            sh->sbuf = calloc(n, sizeof *sh->sbuf);
            if (!sh->sbuf) return 0;
            for (const char *e = js_first(v); e && sh->nsbuf < n; e = js_next(e)) {
                vk_sbuf_ref *r = &sh->sbuf[sh->nsbuf];
                const char *b;
                if (!(b = js_find(e, "binding")) || !js_u32(b, &r->binding)) continue;
                if ((b = js_find(e, "slot"))) js_int(b, &r->slot);
                if ((b = js_find(e, "cb_slot"))) js_int(b, &r->cb_slot);
                if ((b = js_find(e, "cb_offset"))) js_int(b, &r->cb_offset);
                if ((b = js_find(e, "flags"))) js_u32(b, &r->flags);
                sh->nsbuf++;
            }
        }
    }
    // textures 与 images 同一套字段（images 是可写贴图）| textures and images share one set of fields (images are the writable ones)
    static const char *akeys[] = { "textures", "images" };
    uint32_t total = 0;
    for (unsigned i = 0; i < 2; i++) if ((v = js_find(txt, akeys[i]))) total += js_count(v);
    if (total) {
        sh->tex = calloc(total, sizeof *sh->tex);
        if (!sh->tex) return 0;
        for (unsigned i = 0; i < 2; i++) {
            if (!(v = js_find(txt, akeys[i]))) continue;
            for (const char *e = js_first(v); e && sh->ntex < total; e = js_next(e)) {
                vk_tex_ref *r = &sh->tex[sh->ntex];
                const char *b;
                if (!(b = js_find(e, "binding")) || !js_u32(b, &r->binding)) continue;
                if ((b = js_find(e, "type"))) js_u32(b, &r->type);
                if ((b = js_find(e, "array"))) js_u32(b, &r->array);
                if ((b = js_find(e, "separate"))) js_bool(b, &r->separate);
                if ((b = js_find(e, "flags"))) js_u32(b, &r->flags);
                if ((b = js_find(e, "cbuf_slot"))) js_int(b, &r->cbuf_slot);
                if ((b = js_find(e, "handle"))) js_int(b, &r->handle);
                if ((b = js_find(e, "format"))) js_str(b, r->format, sizeof r->format);
                r->writable = (i == 1);   // i==1 即 images 数组：可写图像 | i==1 means the images array: a writable image
                sh->ntex++;
            }
        }
    }
    return 1;
}

static void free_ref(vk_shader *sh) {
    free(sh->cbuf); free(sh->sbuf); free(sh->tex);
    sh->cbuf = NULL; sh->sbuf = NULL; sh->tex = NULL;
    sh->ncbuf = sh->nsbuf = sh->ntex = 0;
}

// ---------- 装载 ----------
static vk_shader *load(const char *key, int stage) {
    vk_shader_init();
    char path[4200];
    snprintf(path, sizeof path, "%s/%s.spv", g_dir, key);
    size_t n = 0;
    uint8_t *code = read_all(path, &n);
    if (!code) {
        if (g_red_missing++ < 5) LOG("【红】着色器 %s 的产物不在 %s 里：这一条建不成管线（缺键要计数）", key, g_dir);
        g_missing++;
        return NULL;
    }
    vk_shader *sh = calloc(1, sizeof *sh);
    if (!sh) { free(code); return NULL; }
    memcpy(sh->key, key, VK_SHADER_KEY_LEN + 1);
    sh->stage = stage;

    // 反射先读：读不动就没必要建模块了 | read the reflection first: without it there is no point creating the module
    snprintf(path, sizeof path, "%s/%s.res.json", g_dir, key);
    size_t jn = 0;
    char *js = (char *)read_all(path, &jn);
    if (!js || !parse_ref(js, sh)) {
        if (g_red_bad++ < 5) LOG("【红】着色器 %s 的 res.json 读不动或缺字段（%s%s）", key, path, js ? "" : "：打不开");
        free(js); free(code); free_ref(sh); free(sh);
        g_bad++;
        return NULL;
    }
    free(js);

    // json 自己的 key 必须与文件名一致：不一致说明产物混了版本，缓存键会被写歪（缓存按 sh->key 查）
    // The key inside the json must match the file name: a mismatch means the artifacts are mixed across
    // versions and the cache key (looked up by sh->key) would be written wrong
    if (memcmp(sh->key, key, VK_SHADER_KEY_LEN)) {
        if (g_red_bad++ < 5) LOG("【红】着色器 %s 的 res.json 里写的是 %s（与文件名不符）", key, sh->key);
        free(code); free_ref(sh); free(sh);
        g_bad++;
        return NULL;
    }

    if (n < 4 || (n & 3)) {   // SPIR-V 是 32 位字的流 | SPIR-V is a stream of 32-bit words
        if (g_red_bad++ < 5) LOG("【红】着色器 %s 的 .spv 长度不对（%zu 字节）", key, n);
        free(code); free_ref(sh); free(sh);
        g_bad++;
        return NULL;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = (const uint32_t *)code };
    VkResult r = vkCreateShaderModule(vk_dev(), &ci, NULL, &sh->mod);
    free(code);
    if (r != VK_SUCCESS) {
        if (g_red_bad++ < 5) LOG("【红】着色器 %s 的 SPIR-V 建不出模块：VkResult %d（%zu 字节）", key, (int)r, n);
        free_ref(sh); free(sh);
        g_bad++;
        return NULL;
    }
    g_loaded++;
    return sh;
}

static vk_shader *cache_get(const char *key) {
    uint32_t i = key_hash(key) & (SH_TAB - 1);
    for (uint32_t k = 0; k < SH_TAB; k++) {
        vk_shader *sh = g_tab[(i + k) & (SH_TAB - 1)];
        if (!sh) return NULL;
        if (!memcmp(sh->key, key, VK_SHADER_KEY_LEN)) return sh;
    }
    return NULL;
}
static void cache_put(vk_shader *sh) {
    uint32_t i = key_hash(sh->key) & (SH_TAB - 1);
    for (uint32_t k = 0; k < SH_TAB; k++) {
        uint32_t j = (i + k) & (SH_TAB - 1);
        if (!g_tab[j] || !memcmp(g_tab[j]->key, sh->key, VK_SHADER_KEY_LEN)) { g_tab[j] = sh; return; }
    }
}

const vk_shader *vk_shader_by_key(const char *key, int stage) {
    if (!key || strlen(key) != VK_SHADER_KEY_LEN) return NULL;
    vk_shader *have = cache_get(key);
    if (have) return have;
    vk_shader *sh = load(key, stage);
    if (!sh) return NULL;
    cache_put(sh);
    return sh;
}

const vk_shader *vk_shader_get(const void *control) {
    char key[VK_SHADER_KEY_LEN + 1];
    if (!vk_shader_ctl_key(control, key)) return NULL;
    return vk_shader_by_key(key, VK_STAGE_NONE);
}

void vk_shader_stats(uint32_t *loaded, uint32_t *missing, uint32_t *bad) {
    if (loaded) *loaded = g_loaded;
    if (missing) *missing = g_missing;
    if (bad) *bad = g_bad;
}
