// 多模块装载（扮演 Horizon 的 rtld）：映射、占线程槽、门登记、跨模块重定位、按名接管、段权限。
// 代码区（原 text 与补丁区）一拷进来就设成只读+执行，此后装载器只写 data：iOS 上这两块要作为签名代码段原样内嵌。
// Multi-module loading (playing the role of Horizon's rtld): mapping, claiming thread slots, gate registration, cross-module relocation, take-over by name, segment permissions.
// The code area (original text and patch area) is set to read-only+execute as soon as it is copied in, and from then on the loader only writes data: on iOS these two blocks must be embedded as is as signed code segments.
#include "dm.h"
#include <TargetConditionals.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include "hle.h"

module_t g_mod[MAX_MOD];
int g_nmod;
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
pthread_key_t g_keyres[2], g_keyaspect, g_keyaspectui;   // g_keyaspect：宽高比上限参数位（KASPECT）          // 分辨率参数位（改写器 PARAMS，KRES0 掌机 / KRES1 主机）；产物没有即 0、不占 | g_keyaspect: aspect ratio upper-limit parameter slot (KASPECT)          // resolution parameter slots (rewriter PARAMS, KRES0 handheld / KRES1 docked); 0 and not allocated if the build output lacks them
enum { KEY_K18, KEY_KTLS, KEY_KTLR, KEY_KGATE, KEY_KRES0, KEY_KRES1, KEY_KASPECT, KEY_KASPECTUI, KEY_N };

uint64_t resolve_import(module_t *m, const char *name);
void gate_register(uint64_t ret_addr, const char *kind, int arg, uint64_t orig);

static pthread_key_t key_value(unsigned i) {
    switch (i) {
    case KEY_K18: return g_key18; case KEY_KTLS: return g_keytls; case KEY_KTLR: return g_keytlr;
    case KEY_KGATE: return g_keygate; case KEY_KRES0: return g_keyres[0]; case KEY_KRES1: return g_keyres[1];
    case KEY_KASPECT: return g_keyaspect; default: return g_keyaspectui;
    }
}
// 客体读线程槽的序列是「mrs xN,<基址寄存器> → 加键页里的偏移 → ldr xN,[xN]」，所以键页里放的偏移必须正好等于
// 「本平台 pthread 实现里该键的值所在字节偏移」。两个平台不一样：
//   苹果：pthread_key_t 就是槽号，值在 基址 + 键×8，基址寄存器用 tpidrro_el0；
//   Linux/安卓：pthread_key_t 不是槽号（bionic 给加了 0x80000000 标志），TSD 数组在 tpidr_el0，每个键还占两个
//     指针位（实测：键 0x80000001 / 0x80000002 / 0x80000003 的值分别在 tpidr_el0+88 / +104 / +120）。bionic 的
//     内部编码不该写死在这儿，所以启动时用哨兵键实测出「首个键的偏移」和「相邻键的字节间距」，再线性外推。
// A guest slot read is "mrs xN,<base register> → add the offset from the key page → ldr xN,[xN]", so the
// offset stored in the key page must be exactly the byte offset at which this platform's pthread
// implementation keeps that key's value. The two platforms differ:
//   Apple: pthread_key_t is the slot number, the value sits at base + key*8, base register tpidrro_el0;
//   Linux/Android: pthread_key_t is not a slot number (bionic sets a 0x80000000 flag), the TSD array is at
//     tpidr_el0, and each key takes two pointer-sized slots (measured: keys 0x80000001 / 2 / 3 keep their
//     values at tpidr_el0+88 / +104 / +120). bionic's internal encoding should not be hard-coded here, so
//     the offset of the first key and the byte distance between adjacent keys are measured with sentinel
//     keys at startup and then extrapolated linearly.
#if defined(__ANDROID__) || defined(__linux__)
static uint64_t g_slot_off0, g_slot_step;      // 首个键的偏移 / 相邻键的字节间距 | offset of the first key / byte distance between adjacent keys
static pthread_key_t g_slot_key0;              // 实测时用的首个哨兵键（不释放，免得键号被复用后线性关系看着变了） | the first sentinel key used (not released, so key reuse cannot make the linear relation look wrong)
static int g_slot_measured;
static void measure_key_slots(void) {
    if (g_slot_measured) return;
    g_slot_measured = 1;
    pthread_key_t b;
    if (pthread_key_create(&g_slot_key0, NULL) || pthread_key_create(&b, NULL)) die("建探测键失败");
    const uint64_t sa = 0x5A5A1234ull, sb = 0x5A5A5678ull;
    pthread_setspecific(g_slot_key0, (void *)sa); pthread_setspecific(b, (void *)sb);
    uint64_t base; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(base));
    if (!base) die("拿不到 tpidr_el0（本平台不是 bionic 那种 TSD 布局？）");
    const uint64_t *t = (const uint64_t *)(base & ~7ull);
    long ia = -1, ib = -1;
    for (long i = 0; i < 512; i++) { if (t[i] == sa) ia = i; if (t[i] == sb) ib = i; }
    if (ia < 0 || ib < 0 || ib <= ia) die("量线程槽失败：tpidr_el0=%#llx 找到 %ld / %ld", (unsigned long long)base, ia, ib);
    g_slot_off0 = (uint64_t)ia * 8;
    g_slot_step = (uint64_t)(ib - ia) * 8 / (uint64_t)(b - g_slot_key0);
    LOG("线程槽量测：首个键 %#x 在 tpidr_el0+%llu，键间距 %llu 字节",
        (unsigned)g_slot_key0, (unsigned long long)g_slot_off0, (unsigned long long)g_slot_step);
}
// 键 → 键页里要放的字节偏移 | key -> the byte offset to store in the key page
static uint64_t key_slot_offset(pthread_key_t k) {
    measure_key_slots();
    return g_slot_off0 + (uint64_t)(k - g_slot_key0) * g_slot_step;
}
#else
static void measure_key_slots(void) { }
static uint64_t key_slot_offset(pthread_key_t k) { return (uint64_t)k * 8; }
#endif
static void create_keys(void) {
    measure_key_slots();   // 必须赶在建真键之前：量测要拿「第一个键」当零点 | must run before the real keys: the measurement uses the first key as its zero
    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||
        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) die("创建宿主键失败");
    // 参数位键一律建（不按产物有没有用到判断：iOS 包里没有 params.tsv，按文件判断会漏建、键页写成 0 → 游戏读到 TSD 0 号槽）
    // Parameter slot keys are always created (not decided by whether the build output uses them: an iOS app has no params.tsv, so deciding by file would miss creating them and the key page would be written as 0 → the game reads TSD slot 0)
    if (pthread_key_create(&g_keyres[0], NULL) || pthread_key_create(&g_keyres[1], NULL) ||
        pthread_key_create(&g_keyaspect, NULL) || pthread_key_create(&g_keyaspectui, NULL)) die("创建参数位键失败");
    // pthread_key_t 在苹果与 bionic 上都是 32 位：按 %lu 传会被读成 8 字节，后面几个数全是垃圾
    // pthread_key_t is 32 bits on both Apple and bionic: passing it to %lu makes it read 8 bytes and every following number comes out as garbage
    LOG("键号 K18=%u KTLS=%u KTLR=%u KGATE=%u KRES0=%u KRES1=%u KASPECT=%u KASPECTUI=%u",
        (unsigned)g_key18, (unsigned)g_keytls, (unsigned)g_keytlr, (unsigned)g_keygate,
        (unsigned)g_keyres[0], (unsigned)g_keyres[1], (unsigned)g_keyaspect, (unsigned)g_keyaspectui);
}

// K 槽校验：pthread TSD 布局固定，槽 = tpidrro_el0 + 8*key
// K slot check: the pthread TSD layout is fixed, slot = tpidrro_el0 + 8*key
static void tsd_check(void) {
    pthread_setspecific(g_key18, (void *)0x1234);
    uint64_t base;
#if defined(__ANDROID__) || defined(__linux__)
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(base));     // 与产物同一套约定：安卓的基址在 tpidr_el0 | same convention as the build output: on Android the base is in tpidr_el0
    if (!base) die("tpidr_el0 是 0——线程槽基址取不到");
#else
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
#endif
    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);
    // 按客体那两步照做：基址 + 键页偏移，再取一次 | do exactly what the guest does: base + key page offset, then one load
    if (*(uint64_t *)(base + off) != 0x1234)
        die("TSD 布局不符（基址 %#llx + 键页偏移 %#llu）：产物是按另一种线程槽基址寄存器出的？"
            "本平台的产物要用 DM_TLSREG=%s 重出", (unsigned long long)base, (unsigned long long)off,
#if defined(__ANDROID__) || defined(__linux__)
            "linux"
#else
            "apple"
#endif
            );
    pthread_setspecific(g_key18, 0);
}

static void build_symhash(module_t *m);
static void parse_dynamic(module_t *m) {
    uint32_t mod0 = *(uint32_t *)(m->base + 4);
    if (memcmp(m->base + mod0, "MOD0", 4)) die("%s: no MOD0", m->name);
    m->dyn = (const Elf64_Dyn *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf64_Sym *)(m->base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(m->base + d->d_un.d_ptr); break;
        case DT_INIT: m->init = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: m->init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->d_un.d_val; break;
    }
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym));
    build_symhash(m);
}

static void *module_sym_linear(module_t *m, const char *name) {
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i];
        if (s->st_shndx && !strcmp(m->strtab + s->st_name, name)) return m->base + s->st_value;
    }
    return NULL;
}
// 符号名哈希表：按下标升序插入、同名已在则不覆盖 ⇒ 查询结果与线性扫描（首个已定义同名符号）逐条相同。
// ① 宿主装载 91–99 ms 里重定位 + 强制接管占 77–86 ms，主因是每条按名解析的重定位都线性扫两个模块的符号表
// Symbol name hash table: inserted in ascending index order, not overwritten if the same name is already there ⇒ lookup results are identical, entry by entry, to a linear scan (the first defined symbol with that name).
// Of the 91–99 ms of ① host loading, relocation + forced take-over took 77–86 ms, mainly because every by-name relocation scanned the symbol tables of two modules linearly
static uint32_t sym_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static void build_symhash(module_t *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->symh = calloc(cap, sizeof *m->symh); m->symh_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i]; if (!s->st_shndx) continue;
        const char *nm = m->strtab + s->st_name;
        for (unsigned j = sym_hash(nm) & m->symh_mask;; j = (j + 1) & m->symh_mask) {
            if (!m->symh[j]) { m->symh[j] = i; break; }
            if (!strcmp(m->strtab + m->symtab[m->symh[j]].st_name, nm)) break;   // 同名已有更小下标：保留它 | The same name already exists with a smaller index: keep that one
        }
    }
}
static unsigned long g_symchk_n, g_symchk_bad;
void *module_sym(module_t *m, const char *name) {
    unsigned long nq; (void)nq;
    if (!m->symh) return module_sym_linear(m, name);
    void *r = NULL;
    for (unsigned j = sym_hash(name) & m->symh_mask; m->symh[j]; j = (j + 1) & m->symh_mask) {
        const Elf64_Sym *s = &m->symtab[m->symh[j]];
        if (!strcmp(m->strtab + s->st_name, name)) { r = m->base + s->st_value; break; }
    }
    static int chk = -1; if (chk < 0) chk = getenv("DM_SYMCHECK") != NULL;
    if (chk) { void *l = module_sym_linear(m, name); g_symchk_n++; nq = g_symchk_n;
               if (l != r) { g_symchk_bad++; LOG("【红】DM_SYMCHECK：%s 在 %s 哈希 %p ≠ 线性 %p", name, m->name, r, l); } }
    return r;
}
void *guest_sym(const char *name) {
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return p; }
    return NULL;
}
module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++)
        if ((const uint8_t *)p >= g_mod[i].base && (const uint8_t *)p < g_mod[i].base + g_mod[i].size) return &g_mod[i];
    return NULL;
}

// 跨模块符号解析：先在已装模块里找定义，找不到再交给 HLE / 桩
// Cross-module symbol resolution: first look for a definition in the loaded modules, and only if not found hand over to HLE / stubs
static uint64_t resolve_sym(module_t *m, const char *name) {
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return (uint64_t)p; }
    return resolve_import(m, name);
}

// 强制接管表（hle.h HLE_FORCE，__DATA,swforce 段）：命中即覆盖模块内定义
// Forced take-over table (hle.h HLE_FORCE, the __DATA,swforce section): a hit overrides the definition inside the module
static const hle_force_ent *g_force; static unsigned g_nforce; static unsigned *g_force_hits;
static void force_init(void) {
    unsigned long sz; DM_SEC_BOUNDS(swforce, "__DATA", hle_force_ent, g_force, sz);
    g_nforce = (unsigned)sz;
    g_force_hits = calloc(g_nforce + 1, sizeof *g_force_hits);
    for (unsigned i = 0; i < g_nforce; i++)
        for (unsigned j = i + 1; j < g_nforce; j++)
            if (!strcmp(g_force[i].name, g_force[j].name)) die("强制接管清单重复：%s", g_force[i].name);
}
static void *force_lookup(const char *name) {
    for (unsigned i = 0; i < g_nforce; i++)
        if (!strcmp(g_force[i].name, name)) {
            if (!g_force_hits[i]++ && g_flag.imports) LOG("force %s  （%s）", name, g_force[i].why);
            void *hlecount_wrap(const char *, const char *, void *); return hlecount_wrap("FORCE", name, g_force[i].fn);   // DM_HLECOUNT 时为计数桩 | A counting stub when DM_HLECOUNT is set
        }
    return NULL;
}
static void force_check(void) {
    int bad = 0;
    for (unsigned i = 0; i < g_nforce; i++)
        if (!g_force_hits[i]) { LOG("强制接管条目无任何引用：%s", g_force[i].name); bad = 1; }
    if (bad) die("强制接管清单有误");
    LOG("force: %u 条全部命中", g_nforce);
}

// 读改写器表头（段边界、槽号）；表头以外有任何行即旧版产物（装载期改代码），拒绝
// Read the rewriter table header (segment boundaries, slot numbers); any row beyond the header means an old-format build output (code modified at load time), which is rejected
static void read_layout(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) die("open %s", path);
    static const char *kn[8] = {"K18", "KTLS", "KTLR", "KGATE", "KRES0", "KRES1", "KASPECT", "KASPECTUI"};
    char line[256], key[32]; unsigned long v; unsigned seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %31s %lx", key, &v) != 2) die("%s: 产物是旧改写器出的，换成当前代号（dm/PRODUCT）的产物", path);
        if (!strcmp(key, "PATCH_OFF")) m->patch_off = v;
        else if (!strcmp(key, "KEY_PAGE_OFF")) m->key_page_off = v;
        else if (!strcmp(key, "KEY_PAGE_SIZE")) m->key_page_size = v;
        else if (!strcmp(key, "TEXT_END")) m->text_end = v;
        else if (!strcmp(key, "RO_OFF")) m->ro_off = v;
        else if (!strcmp(key, "DATA_OFF")) m->data_off = v;
        else for (int k = 0; k < 8; k++) {
            size_t n = strlen(kn[k]);
            if (!strncmp(key, "KEY_", 4) && !strncmp(key + 4, kn[k], n) && !strcmp(key + 4 + n, "_IDX")) {
                m->key_idx[k] = (unsigned)strtoul(strrchr(line, '\t') + 1, 0, 10); seen |= 1u << k;
            }
        }
    }
    fclose(f);
    if (!m->patch_off || m->key_page_off % 0x4000 || m->key_page_size != 0x4000 || seen != 0xff)
        die("%s: 产物是旧改写器出的，换成当前代号（dm/PRODUCT）的产物", path);
}

static void install_key_page(module_t *m) {
    if (m->key_page_off + m->key_page_size > m->size) die("%s: 键页越界", m->name);
    uint64_t *page = (uint64_t *)(m->base + m->key_page_off);
    memset(page, 0, m->key_page_size);
    for (unsigned i = 0; i < KEY_N; i++) page[m->key_idx[i]] = key_slot_offset(key_value(i));
    for (int k = 0; k < 8; k++) if (!page[m->key_idx[k]]) die("%s: 键页第 %d 项为 0（宿主键没建）——游戏会读到 TSD 0 号槽", m->name, k);
    if (mprotect(m->base + m->key_page_off, m->key_page_size, PROT_READ)) die("%s 键页 mprotect", m->name);
}

static void register_gates(module_t *m, const char *dir) {
    char path[1024], line[256]; snprintf(path, sizeof path, "%s/%s.gates.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    int ng = 0;
    if (f) {
        unsigned long ret, orig; char kind[16]; int arg;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%lx %15s %d %lx", &ret, kind, &arg, &orig) == 4) { gate_register((uint64_t)m->base + ret, kind, arg, orig); ng++; }
        fclose(f);
    }
    LOG("%s: gates %d patch_off %#llx keys %lu/%lu/%lu/%lu", m->name, ng, m->patch_off, g_key18, g_keytls, g_keytlr, g_keygate);
}

static void relocate(module_t *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
    }
    int cnt[3] = {0};
    const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
    uint64_t sizes[2] = {relasz, pltsz};
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
            const Elf64_Rela *r = &tabs[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
            if (type == R_AARCH64_RELATIVE) { *where = (uint64_t)m->base + r->r_addend; cnt[0]++; continue; }
            const Elf64_Sym *s = &m->symtab[si];
            uint64_t v = (uint64_t)force_lookup(m->strtab + s->st_name);
            if (!v) v = s->st_shndx ? (uint64_t)m->base + s->st_value : resolve_sym(m, m->strtab + s->st_name);
            if (type == R_AARCH64_JUMP_SLOT) { *where = v; cnt[2]++; }
            else { *where = v + r->r_addend; cnt[1]++; }       // ABS64 / GLOB_DAT
        }
    LOG("%s: reloc relative=%d abs/glob=%d jump=%d", m->name, cnt[0], cnt[1], cnt[2]);
}

// DM_LOADHASH=1（诊断）：装载完逐模块出地址无关 sha256，证「提前起跑 / 不提前」装载结果逐字节相同。
// 模块基址与宿主 ASLR 每局不同，只有重定位槽的值随之变：复制映像，按同一张重定位表把每个槽换成与地址无关的记号——
// RELATIVE → (0, 加数)；符号目标落在某模块 → (1 + 模块号, 模块内偏移)；否则（宿主强制接管 / 宿主符号）→ (名字 FNV, 加数)；其余字节原样。
// DM_LOADHASH=1 (diagnostics): after loading, output an address-independent sha256 per module, to prove that the load results of "early start / no early start" are byte-identical.
// Module base addresses and host ASLR differ every run, and only the values of the relocation slots change with them: copy the image and, using the same relocation table, replace every slot with an address-independent token --
// RELATIVE → (0, addend); symbol target falls in some module → (1 + module number, offset within the module); otherwise (host forced take-over / host symbol) → (name FNV, addend); all other bytes as is.
#include <CommonCrypto/CommonDigest.h>
void loader_hash_log(void) {
    if (!getenv("DM_LOADHASH")) return;
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        uint8_t *cp = malloc(m->size); memcpy(cp, m->base, m->size);
        uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
        for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
            case DT_RELA: rela = d->d_un.d_ptr; break; case DT_RELASZ: relasz = d->d_un.d_val; break;
            case DT_JMPREL: jmprel = d->d_un.d_ptr; break; case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        }
        const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
        uint64_t sizes[2] = {relasz, pltsz}; unsigned nslot = 0;
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tok[1] = {0};
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset); int hit = 0;
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tok[0] = 1 + j; hit = 1; break; }
                    if (!hit) { const char *nm = m->strtab + m->symtab[ELF64_R_SYM(r->r_info)].st_name; uint64_t h = 0xcbf29ce484222325ull;
                                for (; *nm; nm++) h = (h ^ (uint8_t)*nm) * 0x100000001b3ull; tok[0] = h | 1ull << 63; }
                }
                memcpy(cp + r->r_offset, &tok[0], 8);                // 槽 8 字节换成记号头；记号尾在下面按槽序追加 | The 8 slot bytes are replaced by the token head; the token tail is appended below in slot order
                nslot++;
            }
        CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, cp, (CC_LONG)m->size);
        for (int t = 0; t < 2; t++)                                // 记号尾（加数 / 偏移）按槽序追加，与槽头一起唯一确定槽值 | Token tails (addend / offset) are appended in slot order, and together with the slot head uniquely determine the slot value
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tail = (uint64_t)r->r_addend;
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset);
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tail = v - (uint64_t)g_mod[j].base; break; }
                }
                CC_SHA256_Update(&c, &tail, 8);
            }
        uint8_t d[32]; CC_SHA256_Final(d, &c); free(cp);
        char hx[65]; for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        LOG("DM_LOADHASH %s 大小 %#zx 重定位槽 %u sha256 %s", m->name, m->size, nslot, hx);
    }
}

#if TARGET_OS_IPHONE
// iOS 不许运行时装入的代码执行，且苹果只许可执行文件有一个带执行权限的段（ITMS-90999）：映像由 dm/ios/tools/gen_embed.py 编进本可执行文件——
// 代码两块（原 text、补丁区）是 __TEXT 里的节，W（ro 余部 + data + bss）是单独的 RW 段。三块在文件里不相邻；这里保留一块连续地址，
// 按原相对位置 vm_remap 回去：代码页 copy=FALSE（与 __TEXT 共享同一批已签名页，r-x），W copy=TRUE（写时复制，用到才分页）。
// 运行时布局与原映像逐字节相同。iOS 上把已签名页 remap 到新地址再执行，libffi 的跳板表（FFI_EXEC_TRAMPOLINE_TABLE）就是这么做的
// iOS does not allow code loaded at run time to execute, and Apple allows an executable only one segment with execute permission (ITMS-90999): the image is compiled into this executable by dm/ios/tools/gen_embed.py --
// the two code blocks (original text, patch area) are sections in __TEXT, and W (the rest of ro + data + bss) is a separate RW segment. The three blocks are not adjacent in the file; here one contiguous address range is reserved,
// and they are vm_remap'ed back at their original relative positions: code pages with copy=FALSE (sharing the same signed pages with __TEXT, r-x), W with copy=TRUE (copy-on-write, paged only when used).
// The run-time layout is byte-identical to the original image. Remapping signed pages to a new address and then executing them on iOS is exactly what libffi's trampoline table (FFI_EXEC_TRAMPOLINE_TABLE) does
#include <mach/mach.h>
static void remap_at(module_t *m, vm_address_t to, const void *from, unsigned long size, boolean_t copy, vm_prot_t need) {
    vm_address_t a = to; vm_prot_t cur = 0, max = 0;
    kern_return_t kr = vm_remap(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)from, copy, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || a != to) die("%s vm_remap %p → %#lx（%#lx 字节）失败：%d", m->name, from, (unsigned long)to, size, kr);
    if ((cur & need) != need) die("%s vm_remap 后权限 %#x，缺 %#x", m->name, cur, need);
}
#ifdef DM_SHELL
// 壳（ios/tools/shell_build.sh 出的、不含任何游戏内容的 App）：模块映像不在主可执行文件里，在使用者自己用游戏文件做出来的
// Frameworks/Guest.framework/Guest 里——一个只装段的动态库（dm/tools/guest_dylib.py 写出），节名与内嵌做法相同，下面只换一个映像头去取。
// Shell (the App built by ios/tools/shell_build.sh, containing no game content at all): the module images are not in the main executable but in the
// Frameworks/Guest.framework/Guest that users build themselves from their game files -- a dynamic library that holds only segments (written by dm/tools/guest_dylib.py), with the same section names as the embedded approach; below only the image header to fetch from is swapped.
#include <dlfcn.h>
#include <mach-o/dyld.h>
static const struct mach_header_64 *image_mh(void) {
    static const struct mach_header_64 *mh; if (mh) return mh;
    char exe[4096]; uint32_t n = sizeof exe; if (_NSGetExecutablePath(exe, &n)) die("取不到可执行文件路径");
    char *sl = strrchr(exe, '/'); if (!sl || (size_t)(sl - exe) + 40 > sizeof exe) die("可执行文件路径不对：%s", exe); strcpy(sl, "/Frameworks/Guest.framework/Guest");
    if (!dlopen(exe, RTLD_NOW)) die("这个安装包里还没有游戏程序（%s）：先用 dm/tools/d2rm_pack.py 把你自己的游戏文件做进安装包再装。系统的说法：%s", exe, dlerror());
    for (uint32_t i = 0; i < _dyld_image_count(); i++) if (strstr(_dyld_get_image_name(i), "/Guest.framework/Guest")) mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
    if (!mh) die("Guest 动态库已加载但找不到它的映像头");
    fprintf(stderr, "boot: 模块映像取自 Guest 动态库 %s\n", exe); return mh;
}
#define IMAGE_MH image_mh()
#else
#define IMAGE_MH (&_mh_execute_header)
#endif
static void map_image(module_t *m, const char *dir) {
    (void)dir;
    char sx0[17], sx1[17], dm[17]; snprintf(sx0, sizeof sx0, "__dm%sx0", m->name); snprintf(sx1, sizeof sx1, "__dm%sx1", m->name); snprintf(dm, sizeof dm, "__DM%sW", m->name);
    unsigned long z0, zw, z1;
    const uint8_t *x0 = getsectiondata(IMAGE_MH, "__TEXT", sx0, &z0), *w = getsectiondata(IMAGE_MH, dm, "__img", &zw), *x1 = getsectiondata(IMAGE_MH, "__TEXT", sx1, &z1);
    if (!x0 || !w || !x1) die("内嵌节 %s / %s / %s 缺失", sx0, dm, sx1);
    if (z0 + zw != m->patch_off || (z0 & 0x3FFF) || (zw & 0x3FFF) || (z1 & 0x3FFF) || ((uintptr_t)x0 & 0x3FFF) || ((uintptr_t)w & 0x3FFF) || ((uintptr_t)x1 & 0x3FFF))
        die("%s 内嵌节布局不符（x0 %#lx w %#lx x1 %#lx，PATCH_OFF %#llx）", m->name, z0, zw, z1, (unsigned long long)m->patch_off);
    m->size = m->patch_off + z1;
    vm_address_t base = 0;
    if (vm_allocate(mach_task_self(), &base, m->size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) die("%s 保留 %#zx 字节地址失败", m->name, m->size);
    remap_at(m, base, x0, z0, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    remap_at(m, base + z0, w, zw, TRUE, VM_PROT_READ | VM_PROT_WRITE);
    remap_at(m, base + m->patch_off, x1, z1, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    m->base = (uint8_t *)base; install_key_page(m);
}
#else
static uint8_t *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open %s", path);
    struct stat st; fstat(fd, &st);
    uint8_t *p = malloc(st.st_size);
    if (read(fd, p, st.st_size) != st.st_size) die("read %s", path);
    close(fd); *n = st.st_size; return p;
}

// 拷入匿名页后立即封代码区（页 16 KB）：[0, text_end) 与补丁区只读+执行，此后对代码区的任何写入都会当场崩；
// 之间（ro 与 data 同页只能一起）可写
// Seal the code area immediately after copying into anonymous pages (16 KB pages): [0, text_end) and the patch area are read-only+execute, and from then on any write to the code area crashes on the spot;
// what lies between (ro and data on the same page can only go together) is writable
static void map_image(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    size_t sz; uint8_t *raw = slurp(path, &sz);
    m->size = (sz + 0x3FFF) & ~0x3FFFull;
    m->base = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m->base == MAP_FAILED) die("%s mmap", m->name);
    memcpy(m->base, raw, sz); free(raw); install_key_page(m);
    uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
    if (mprotect(m->base, rx, PROT_READ | PROT_EXEC)) die("%s mprotect text", m->name);
    uint64_t po = m->patch_off & ~0x3FFFull;
    if (mprotect(m->base + po, m->size - po, PROT_READ | PROT_EXEC)) die("%s mprotect patch", m->name);
}
#endif

void load_modules(const char *dir, const char **names, int n) {
    // 先全部映射并解析符号表，再统一重定位（跨模块符号要先都在位）
    // DM_BOOTPROF：① 宿主装载细分（毫秒），定 ③ 预重定位 / mmap 的上限
    // Map everything and parse the symbol tables first, then relocate all at once (cross-module symbols must all be in place first)
    // DM_BOOTPROF: breakdown of ① host loading (milliseconds), to set the upper bound for ③ pre-relocation / mmap
    uint64_t t0 = now_ns(), tm = 0, tp = 0, tg = 0, tr = 0, t;
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        strlcpy(m->name, names[i], sizeof m->name);
        read_layout(m, dir);
    }
    create_keys();
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        t = now_ns(); map_image(m, dir); tm += now_ns() - t;   // 返回时代码区已是只读+执行，此后装载器只写 data | On return the code area is already read-only+execute; from here on the loader only writes data
        if (!i) tsd_check();
        t = now_ns(); parse_dynamic(m); tp += now_ns() - t;
        t = now_ns(); register_gates(m, dir); tg += now_ns() - t;
        LOG("%s: base %p size %#zx", names[i], m->base, m->size);
        g_nmod++;
    }
    force_init();
    t = now_ns(); for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]); tr = now_ns() - t;
    force_check();
    { void hlecount_seal(void); hlecount_seal(); }   // DM_HLECOUNT：重定位完成，计数桩页封成只读可执行 | DM_HLECOUNT: relocation done, seal the counting stub pages as read-only executable
    if (getenv("DM_SYMCHECK")) LOG("DM_SYMCHECK：装载期共核 %lu 次符号查询，哈希与线性不一致 %lu", g_symchk_n, g_symchk_bad);
    if (g_flag.bootprof) LOG("① 细分：映射（读 + 拷 + 封页）%.1f ms、解析动态段 %.1f ms、门注册 %.1f ms、重定位 + 强制接管 %.1f ms，合计 %.1f ms",
                             tm / 1e6, tp / 1e6, tg / 1e6, tr / 1e6, (now_ns() - t0) / 1e6);
}

// 扮演 rtld 的 InitModules（rtld+0x830）：按装载链表正序调各模块 DT_INIT。
// 时机由 nn::init::Start 决定：nninitStartup 与 _init_libc2 之后、nnMain 之前（Start 的第 4 个参数回调）。
// Plays the role of rtld's InitModules (rtld+0x830): call each module's DT_INIT in forward order of the load list.
// The timing is decided by nn::init::Start: after nninitStartup and _init_libc2, before nnMain (the 4th argument callback of Start).
void run_module_inits(void) {
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!m->init) continue;
        LOG("%s: DT_INIT +%#llx（init_array %llu 项）", m->name, m->init, m->init_arraysz / 8);
        ((void (*)(void))(m->base + m->init))();
    }
}
