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
static uint32_t g_set_moved;   // 被挪到 set 1 的片元绑定条数（诊断用）| fragment bindings moved to set 1 (for diagnostics)

// ---------- 接口描述表（shader_facts.tsv）：SPIR-V 贴图 binding ↔ NVN 贴图槽 ----------
// ---------- Interface table (shader_facts.tsv): SPIR-V texture binding <-> NVN texture slot ----------
// 为什么需要配对：SPIR-V 里的贴图 binding 与 NVN 的贴图槽**不是一回事**，而 `<键>.res.json` 里**没有**这个对应
// 关系（苹果侧那份反射有 `reg` 字段，安卓这份没有——翻译器只给 binding）。仓库里另有一张接口描述表
// `shader_facts.tsv`：逐键记着"这个着色器用了哪些贴图槽、各是什么维度"，它的生成脚本自己写着
// "NVN slot number = D3D register number"（dmsh_facts.py 的文件头）。
// 于是"binding ↔ 槽"只能靠两边**配对**，规则是**按维度分组、组内按槽号/绑定号升序**。这条规则不是拍脑袋：
// 它在本地用全库 44,135 条贴图绑定验过——自洽 98.4%；并做了**反例对照**（故意弄错一条映射就掉到 65.1%），
// 所以这个一致性指标是有分辨力的。配不上的保持 -1 并在真机上计数，**不猜**。
// 还有两个 type 至今没定下来（1029、1283，共 687 条）：试过把它们当 texel buffer，只能多配上 229 条，说明不是；
// 它们一律不配，等在真机上看到实际影响再定。
// Why pairing is needed: a texture binding in the SPIR-V is **not** an NVN texture slot, and `<key>.res.json` has
// **no** such correspondence (Apple's reflection has a `reg` field and this one does not -- the translator emits only
// the binding). The repository does carry another interface table, `shader_facts.tsv`, recording per key "which
// texture slots this shader uses and what dimension each is", and its own generator states "NVN slot number = D3D
// register number" (the header of dmsh_facts.py).
// So "binding <-> slot" can only be **paired**, by the rule "group by dimension, then ascending slot/binding within a
// group". That rule is not a guess: it was checked locally against all 44,135 texture bindings in the library -- 98.4%
// self-consistent -- and falsified with a control (deliberately corrupting one mapping drops it to 65.1%), so the
// consistency figure does discriminate. Unpaired entries stay at -1 and are counted on the device; **nothing is
// guessed**.
// Two types remain unsettled (1029 and 1283, 687 bindings together): treating them as texel buffers matched only 229
// more, so that is not it. They stay unpaired until their real effect shows up on the device.
#include <stdatomic.h>   // 配对计数是原子的：装载可能来自多个线程 | the pairing counters are atomic: loading may come from several threads

#define FACTS_MAXSLOT 64
static char *g_facts;
static size_t g_facts_len;
static int g_facts_tried;
static _Atomic uint32_t g_pair_ok, g_pair_bad;

static char *slurp(const char *path, size_t *n) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); return NULL; }
    rewind(fp);
    char *b = malloc((size_t)sz + 1);
    if (!b) { fclose(fp); return NULL; }
    size_t got = fread(b, 1, (size_t)sz, fp);
    fclose(fp);
    b[got] = 0;
    *n = got;
    return b;
}

static void facts_load(void) {
    if (g_facts_tried) return;
    g_facts_tried = 1;
    char path[4200];
    const char *env = getenv("DM_NVN_FACTS");
    if (env && *env) g_facts = slurp(env, &g_facts_len);
    if (!g_facts) {
        const char *root = getenv("DM_ROOT");
        if (root && *root) {
            snprintf(path, sizeof path, "%s/shader_facts.tsv", root);
            g_facts = slurp(path, &g_facts_len);
        }
    }
    if (!g_facts) {
        snprintf(path, sizeof path, "%s/../shader_facts.tsv", g_dir);
        g_facts = slurp(path, &g_facts_len);
    }
    if (g_facts) LOG("【绿】接口描述表已载入（%zu KB）：贴图 binding 与 NVN 槽的配对靠它", g_facts_len >> 10);
    else {
        static int said;
        if (!said++) LOG("【黄】没有接口描述表（shader_facts.tsv）：贴图绑定一律配不上，贴图会写成空白");
    }
}

// 找某个键的那一行，返回 '\t' 之后的值起点
// Find the line for a key and return the value start after its tab
static const char *facts_line(const char *key) {
    facts_load();
    if (!g_facts) return NULL;
    size_t kl = strlen(key);
    for (const char *p = g_facts; *p;) {
        if ((p == g_facts || p[-1] == '\n') && !strncmp(p, key, kl) && p[kl] == '\t') return p + kl + 1;
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return NULL;
}

// res.json 的 type → 接口描述表里的维度名。前五条是**本地用强制配对推出来并验过**的（见上面那一节）；
// 1029 / 1283 不在这里（未定，不猜）。
// res.json's type -> the dimension name used by the interface table. The first five were **derived from forced
// pairings and validated locally** (see the section above); 1029 / 1283 are absent on purpose (unsettled, not guessed).
static const char *kind_of_type(int type) {
    switch (type) {
        case 3: return "2d";
        case 4: return "3d";
        case 2: return "cube";
        case 5: return "cube";
        case 259: return "2darray";
        default: return NULL;
    }
}

typedef struct { int slot; const char *kind; size_t klen; uint8_t used; } fslot;

// 把这张着色器的贴图绑定与接口描述表里的槽配起来（结果写进 vk_tex_ref.nvn_slot）
// Pair this shader's texture bindings with the table's slots (the result goes into vk_tex_ref.nvn_slot)
static void pair_tex_slots(vk_shader *sh) {
    if (!sh->tex) return;
    for (uint32_t i = 0; i < sh->ntex; i++) sh->tex[i].nvn_slot = -1;
    if (!sh->ntex) return;
    const char *v = facts_line(sh->key);
    if (!v) { g_pair_bad += sh->ntex; return; }
    const char *t = strstr(v, "\"tex\"");
    if (!t) t = v;
    t = strchr(t, '{');
    if (!t) { g_pair_bad += sh->ntex; return; }
    fslot fs[FACTS_MAXSLOT];
    int n = 0;
    for (const char *p = t + 1; *p && *p != '}' && n < FACTS_MAXSLOT;) {
        if (*p != '"') { p++; continue; }
        p++;
        int s = 0;
        if (*p < '0' || *p > '9') break;
        while (*p >= '0' && *p <= '9') s = s * 10 + (*p++ - '0');
        if (*p != '"') break;
        p++;
        while (*p == ' ') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ') p++;
        if (*p != '"') continue;
        p++;
        const char *k0 = p;
        while (*p && *p != '"') p++;
        fs[n].slot = s; fs[n].kind = k0; fs[n].klen = (size_t)(p - k0); fs[n].used = 0;
        n++;
        if (*p == '"') p++;
    }
    if (!n) { g_pair_bad += sh->ntex; return; }
    // 槽按槽号升序（表里的键序不是数字序，"8" 会排在 "48" 之后）——组内顺序就靠这个
    // Sort slots by slot number (the table's key order is not numeric: "8" comes after "48") -- the within-group
    // order depends on it
    for (int i = 1; i < n; i++) {
        fslot x = fs[i];
        int j = i - 1;
        while (j >= 0 && fs[j].slot > x.slot) { fs[j + 1] = fs[j]; j--; }
        fs[j + 1] = x;
    }
    // 绑定按 binding 升序处理（res.json 里本来就有序，但别依赖它）
    // Bindings are processed in ascending binding order (res.json is ordered already, but do not rely on it)
    uint32_t idx[VK_SPV_MAXB];
    uint32_t m = sh->ntex < VK_SPV_MAXB ? sh->ntex : VK_SPV_MAXB;
    for (uint32_t i = 0; i < m; i++) idx[i] = i;
    for (uint32_t i = 1; i < m; i++) {
        uint32_t x = idx[i];
        uint32_t j = i;
        while (j > 0 && sh->tex[idx[j - 1]].binding > sh->tex[x].binding) { idx[j] = idx[j - 1]; j--; }
        idx[j] = x;
    }
    for (uint32_t i = 0; i < m; i++) {
        const char *want = kind_of_type((int)sh->tex[idx[i]].type);
        if (!want) { g_pair_bad++; continue; }
        size_t wl = strlen(want);
        for (int k = 0; k < n; k++) {
            if (fs[k].used || fs[k].klen != wl || strncmp(fs[k].kind, want, wl)) continue;
            fs[k].used = 1;
            sh->tex[idx[i]].nvn_slot = fs[k].slot;
            g_pair_ok++;
            break;
        }
        if (sh->tex[idx[i]].nvn_slot < 0) g_pair_bad++;
    }
}

void vk_shader_texpair_stats(uint32_t *paired, uint32_t *unpaired) {
    if (paired) *paired = atomic_load(&g_pair_ok);
    if (unpaired) *unpaired = atomic_load(&g_pair_bad);
}

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
static void free_spv(vk_shader *sh) { free(sh->spv); sh->spv = NULL; sh->nspv = 0; free(sh->in); sh->in = NULL; sh->nin = 0; }

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

    pair_tex_slots(sh);   // 装载时就把"贴图 binding ↔ NVN 槽"配出来（规则与验证见上面那一节）

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
    // ---- 阶段分空间：片元着色器的描述符绑定整体挪到 set 1（顶点留在 set 0）----
    // 为什么必须挪：真机上出现过"顶点说 binding 1 是 uniform 缓冲、片元说它是组合图像采样器"——Vulkan 里一条
    // binding 只能有一个类型，这种事无解；而根子在 NVN 那边：常量段与贴图是两个互不相干的空间，翻译器给不同
    // 阶段复用同一个号（Metal 那边按阶段分开的参数缓冲不会撞，所以那边没这个问题）。按**阶段**分 set 是最小的
    // 正确修法：阶段在装载时就已知，于是每个片元着色器只有一个变体，不必按程序组合生成变体。
    // 改写发生在建模块**之前**，所以扫描器（下面）读到的就是改写后的 set。
    // ---- a space per stage: the fragment stage's descriptor bindings all move to set 1 (the vertex stage stays on
    // set 0). Why it has to move: the device showed "the vertex shader calls binding 1 a uniform buffer while the
    // fragment shader calls it a combined image sampler" -- in Vulkan one binding can have only one type and that has
    // no answer; the root is on the NVN side, where the constant bank and the textures are two unrelated spaces and
    // the translator reuses one number across stages (Metal's per-stage argument buffers do not collide, which is why
    // this never came up there). Splitting sets by **stage** is the smallest correct fix: the stage is known at load
    // time, so each fragment shader has a single variant and no per-program combinations are needed.
    // The rewrite happens **before** module creation, so the scanner below sees the rewritten set.
    if (sh->stage == VK_STAGE_PS) {
        int np = vk_spv_force_set((uint32_t *)code, n, 1);
        if (np < 0) {
            if (g_red_bad++ < 5) LOG("【红】着色器 %s 的 SPIR-V 结构不对，没法把片元绑定挪到 set 1（%zu 字节）", key, n);
            free(code); free_ref(sh); free(sh);
            g_bad++;
            return NULL;
        }
        g_set_moved += (uint32_t)np;
    }

    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = (const uint32_t *)code };
    VkResult r = vkCreateShaderModule(vk_dev(), &ci, NULL, &sh->mod);

    // 趁字节还在：把 SPIR-V 声明的绑定抄下来（描述符集布局的唯一权威来源，见 vk_spv.h）。
    // res.json 只讲"数据从哪来"，不保证列全 SPIR-V 里的绑定——真机上 3 条计算管线就是这么失败的。
    // While the bytes are still here, copy out the bindings the SPIR-V declares (the only authoritative source for
    // descriptor set layouts, see vk_spv.h). res.json says only where the data comes from and does not necessarily
    // list every binding in the SPIR-V -- which is exactly how 3 compute pipelines failed on the device.
    if (r == VK_SUCCESS) {
        sh->spv = calloc(VK_SPV_MAXB, sizeof *sh->spv);
        sh->in = calloc(VK_SPV_MAXIN, sizeof *sh->in);
        int ni = 0;
        int nb = (sh->spv && sh->in) ? vk_spv_scan((const uint32_t *)code, n, sh->spv, VK_SPV_MAXB, sh->in, VK_SPV_MAXIN, &ni) : -1;
        if (nb < 0) {
            if (g_red_bad++ < 5) LOG("【红】着色器 %s 的 SPIR-V 绑定读不出来（%zu 字节）", key, n);
            free(code); free_ref(sh); free_spv(sh); free(sh);
            g_bad++;
            return NULL;
        }
        sh->nspv = (uint32_t)nb;
        sh->nin = (uint32_t)ni;
    }
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

uint32_t vk_shader_set_moved(void) { return g_set_moved; }

void vk_shader_stats(uint32_t *loaded, uint32_t *missing, uint32_t *bad) {
    if (loaded) *loaded = g_loaded;
    if (missing) *missing = g_missing;
    if (bad) *bad = g_bad;
}
