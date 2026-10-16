// 新地基（Vulkan 原生的显式实现）N1：**影子内存 + 分块后备资源 + 地址反查**。
//
// 一句话：**客体池永远是普通内存**（宿主 `mmap` 出来的 `PROT_READ|PROT_WRITE`），GPU 用**另一份**按块切分的
// `DEVICE_LOCAL` 后备资源。「池在哪儿」与「GPU 用哪块」**解耦** —— 这是与旧路（`vk_region.c`：分配可导出内存
// → `vkGetMemoryFdKHR` → `mmap(MAP_FIXED)` 把它压回客体地址，池与 GPU 缓冲是**同一份**内存）最本质的区别，
// 也正是「第二台启动就停在内存换轨 / AHB 原子访问 `SIGBUS` 并把内核带 panic / 启动约 25% 崩溃」从根上消失的原因。
//
// 本文件只做三件事（规格：docs/rewrite/ANDROID_REWRITE.zh-CN.md §9.2 / §9.3 / §9.8，判据 G3）：
//   ① `vk2_pool` / `vk2_chunk`：池登记 + 按块建**全功能**后备资源（块 = 960 MB，N0 的真机读数，见 §9.9）；
//   ② `vk2_addr()`：地址反查；**请求跨块时返回 0**，并记一份「跨块请求的长度分布」（块大小的复核依据）；
//   ③ `DM_GFX2` 开关的**可读**（缺省关）+ 快路 PINNED 的**降级规则**（不做默认、不做实现；正确性不依赖它）。
//
// **不做**（本轮明确不做，见本轮交接）：脏记账（N2 的 `vk2_dirty`）、上传/回读窗口（N2 的 `vk2_xfer`）、
// 提交（N3 的 `vk2_sub`）、接管回放层（N4；不改 `nvn_vk.c` / `vk_tex.c` / `vk_region.c`）。
//
// ---------------------------------------------------------------------------------------------------------------
// vk2_pool.h -- N1 of the new foundation (Vulkan native, explicit): shadow memory + chunked backing store +
// address lookup. A guest pool is **always ordinary memory**; the GPU uses a **separate**, chunk-split
// DEVICE_LOCAL backing store. "Where the pool lives" and "which piece the GPU uses" are decoupled -- exactly the
// difference from the old road (vk_region.c allocates exportable memory, takes an fd and mmap(MAP_FIXED)s it back
// onto the guest address, so pool and GPU buffer are the same memory), and the reason "device 2 stops dead in the
// memory overlay / atomic access on an AHB brings the kernel down / ~25% boot crash" disappears at the root.
#ifndef DM_VK2_POOL_H
#define DM_VK2_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "vk2_dirty.h"   // N2：区间级脏记账（§9.4）——池里那个占位字段现在换实了 | N2's dirty accounting

// ---------------------------------------------------------------- 常数（N0 真机读数，不许再估）
// 块大小 = **960 MB**：纸面上限 = min(maxStorageBufferRange 2047 MB, maxMemoryAllocationSize 1024 MB) = 1024 MB，
// 阶梯实测「1088 MB 失败（静默：建/分配/绑定/提交全成功，只有尾页 4 KB 回读对不上）/ 1024 MB 成功」，
// 取 1024 MB 再减一档（一档 = 64 MB）⇒ 960 MB。4 GB 池 ⇒ 5 块（4×960 + 256）。
// Chunk size = 960 MB, measured on the device during N0 (section 9.9): the paper ceiling is
// min(maxStorageBufferRange 2047 MB, maxMemoryAllocationSize 1024 MB) = 1024 MB; the ladder measured "1088 MB
// fails -- silently: create/allocate/bind/submit all succeed, only the last page fails to read back" and
// "1024 MB succeeds", so take 1024 MB minus one notch (64 MB) = 960 MB. A 4 GB pool is 5 chunks.
#define VK2_CHUNK_MB          960u
#define VK2_CHUNK_BYTES       (960ull * 1024ull * 1024ull)
// 一档 = 64 MB（N0 阶梯的档位）；上限缩到"纸面上限取整再减一档"时用它 | one notch = 64 MB, as in N0's ladder
#define VK2_NOTCH_BYTES       (64ull * 1024ull * 1024ull)
// 单个池最多切多少块（4 GB ÷ 960 MB = 5；留够余量给别处更大的池）| max chunks per pool (4 GB / 960 MB = 5)
#define VK2_MAX_CHUNKS        128
// 一个进程最多登记多少个池 | how many pools one process may register
#define VK2_MAX_POOLS         256

// 块的「全功能」用法位：SSBO / 上传 / 回读 / 顶点 / 索引 / 间接 / 缓冲区贴图（texel）。
// **故意不含 `UNIFORM_BUFFER`**：规范对 VkBufferCreateInfo 有一条 VU 要求「带 UNIFORM_BUFFER 的缓冲，
// size ≤ maxUniformBufferRange（本机 64 KB）」，而我们的块是 960 MB ⇒ 带上它等于明知故犯地走在规范外
// （旧路把 UBO 位给了整池，走的正是这条路）。块内当 UBO 用的那些小段要怎么处理属 N4 的绑定问题；
// 探针另有一条子问专门量"960 MB 带 UBO 位建不建得出来"，只为把旧路的宽容度记成读数。
// The "full-featured" usage set, deliberately without UNIFORM_BUFFER: a spec VU requires a buffer with
// UNIFORM_BUFFER to be <= maxUniformBufferRange (64 KB here) while our chunks are 960 MB, so giving it the bit
// would knowingly step outside the spec (which is exactly what the old road did by handing the bit to a whole
// pool). How UBO-sized ranges inside a chunk are bound is an N4 question; the probe has a sub-question that
// measures whether a 960 MB buffer with the UBO bit is even creatable, purely to record that tolerance.
#define VK2_FULL_USAGE (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | \
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | \
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | \
                        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)
// 建不出全功能时退到的那一档（**要计数**：这就是旧路的 `g_pool_small`）| the fallback tier, which must be counted
#define VK2_MIN_USAGE  (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)

// 池的两种模式（§9.8）：SHADOW = 影子 + 分块后备（**正确性路径，唯一默认**）；
// PINNED = 把池钉到 GPU 内存上省掉搬运的**快路**（本轮只落降级规则，见 vk2_pool_pin）。
// The two pool modes: SHADOW is the correctness path and the only default; PINNED is the fast path.
#define VK2_MODE_SHADOW 0
#define VK2_MODE_PINNED 1

// 跨块请求的长度分布档位边界（字节）：<4 KiB / 4~64 KiB / 64~1024 KiB / 1~16 MiB / ≥16 MiB
// Bucket edges (bytes) of the cross-chunk length distribution
#define VK2_XLEN_BUCKETS 5
extern const uint64_t vk2_xlen_edge[VK2_XLEN_BUCKETS];   // 每档的上界（最后一档 = UINT64_MAX）

// 地址掩码：**这台机上的数据指针带高位标记，掩掉的是最高那个字节**。
// 实测（N1 探针，`VK2_POOL_MALLOC_TAG_BITS=0xb400000000000000`）：`malloc` 返回的指针高位是 `0xb4…`，
// 而 `mmap` 与 `/proc/self/maps` 给的都是**不带标记**的地址 ⇒ 「池在不在本进程的可读写映射里」必须先掩掉
// 标记再比，否则真实池子会被判成"不是普通内存"。
// 掩法是**去掉最高一个字节**（`0x00ffffffffffffff`，即 aarch64 的 TBI：硬件只忽略位 56~63），而不是按宿主的
// `crash.c` 那样掩到 48 位 —— 后者是给**带 PAC 的返回地址**用的（签名占高 16 位），而数据指针的标记只占高 8 位，
// 且 52 位 VA 的设备上位 48~51 是**真地址**。这台机的用户空间地址实测都在 2^40 上下，两种掩法结果一样，
// 但"去掉硬件忽略的那一个字节"才是语义正确的那个。
// Address mask: strip exactly the top byte. Measured in the N1 probe (VK2_POOL_MALLOC_TAG_BITS=
// 0xb400000000000000): malloc returns tagged pointers while mmap and /proc/self/maps are untagged, so the tag has
// to be masked away before the "is this inside our rw mappings" test. Stripping one byte (what aarch64 TBI makes
// the hardware ignore: bits 56..63) is the semantically correct mask -- not the 48-bit mask crash.c uses for
// PAC-signed return addresses (a 16-bit signature), since on a 52-bit-VA device bits 48..51 are real address bits.
static inline uintptr_t vk2_ptr_mask(uintptr_t p) { return p & (uintptr_t)0x00ffffffffffffffull; }

// ---------------------------------------------------------------- 数据结构（§9.2）
// 池内分块的后备资源：切块是为了绕开 maxStorageBufferRange（4 GB 缓冲带 STORAGE 建不出来的那个坑）
// A chunk of the pool's backing store; the split exists to dodge maxStorageBufferRange
typedef struct {
    uint64_t       off;     // 该块在池内的偏移 | offset of this chunk inside the pool
    size_t         size;    // 该块的字节数（最后一块可能更小）| chunk bytes (the last one may be smaller)
    VkBuffer       buf;     // DEVICE_LOCAL 后备资源 | the DEVICE_LOCAL backing buffer
    VkDeviceMemory mem;
    int            small;   // 1 = 只能按传输用法建出来（必须为 0）| 1 = transfer-only fallback (must stay 0)
} vk2_chunk;

typedef struct {
    uintptr_t    lo, hi;    // 客体池的地址区间（就是宿主给的那份普通内存）| the pool's address range
    int          nchunk;
    vk2_chunk   *chunk;
    vk2_dirty    dirty;     // N2 填实：区间级脏位图（N1 时这里只是个占位字段）| N2's interval dirty bitmap
    int          mode;      // VK2_MODE_SHADOW / VK2_MODE_PINNED
    uint64_t     seq;       // 登记序号（日志里好认）| registration sequence number (for the log)
    char         tag[24];   // 调用方给的名字（可空）| caller-supplied name (may be empty)
} vk2_pool;

// 本模块需要的那几件设备事实：显式传进来，**不**依赖旧路的 `vk_device.c`（新目录与旧路并存、探针能独立跑；
// N4 接管时由回放层把那一个设备传进来即可）。
// The handful of device facts this module needs, passed in explicitly: no dependency on the old road's
// vk_device.c, which keeps the module usable on its own (the probe builds its own device; N4 hands in the game's).
typedef struct {
    VkDevice                                 dev;
    VkPhysicalDevice                         phys;
    VkQueue                                  q;      // N2 起要提交（搬运窗口 / 回读拷贝）；N1 用不到，留空即可
    uint32_t                                 qfi;    // 承载提交的队列族（量能力/建缓冲时用）| queue family index
    VkPhysicalDeviceMemoryProperties         mp;
    VkPhysicalDeviceProperties               props;
    VkPhysicalDeviceMaintenance3Properties   m3;
} vk2_dev;

// 计数：G3 的两个判据（`small` / `no_region` 必须都为 0）+ 地址反查与快路的读数
// Counters: G3's two criteria (small / no_region must both be 0) plus address-lookup and fast-path readings
typedef struct {
    uint64_t pools;         // 当前登记着的池数 | pools currently registered
    uint64_t pool_adds;     // 累计登记次数（= 旧路的 pool_adds）| cumulative registrations
    uint64_t pool_fail;     // 一块都没建出来（非 no_region 原因，如 OOM）的池数 | pools with no chunk built at all
    uint64_t chunk_try, chunk_ok;
    uint64_t small;         // **G3 判据**：退成只带传输位的**块**数（旧路 g_pool_small）| must be 0
    uint64_t no_region;     // **G3 判据**：因"池落在 GPU 不可达区间"而一块没建出的池数（旧路 g_pool_no_region）
    uint64_t overlap;       // 登记时发现地址区间重叠的池数（调用方 bug，只报计数）| overlapping registrations
    uint64_t tagged;        // 池地址带高位标记（TBI/PAC）的登记次数——掩掉最高字节之后照样成立 | tagged pool addresses
    uint64_t dirty_fail;    // N2：脏位图建不出来 ⇒ 整个池登记失败的次数（应当一直是 0）| dirty bitmap alloc failures
    uint64_t bytes;         // 当前所有池覆盖的总字节 | total bytes covered by the pools
    uint64_t lookups, hits, misses, cross;   // 地址反查：总次数 / 命中 / 没落在池内 / 跨块
    uint64_t cross_hist[VK2_XLEN_BUCKETS];   // 跨块请求的**长度分布**（§9.3 要求的那份日志）
    uint64_t pin_try, pin_ok, pin_fail;      // 快路（§9.8）：请求次数 / 成功 / 回落 SHADOW
} vk2_pool_stats;

// ---------------------------------------------------------------- 生命周期
// 建池模块：`d` 里的字段会被拷走（调用方不必保活），chunk_bytes = 0 时用 VK2_CHUNK_BYTES。
// 返回 0 表示失败（日志里会说明为什么）。chunk_bytes 会被夹到"纸面上限取整再减一档"以内。
// Set the module up; d is copied (no lifetime requirement), chunk_bytes = 0 means VK2_CHUNK_BYTES.
int  vk2_pool_init(const vk2_dev *d, uint64_t chunk_bytes);
void vk2_pool_fini(void);                  // 删掉所有池并清状态（单线程收尾用）| drop every pool and reset
uint64_t vk2_pool_chunk_bytes(void);       // 实际生效的块大小（夹过之后）| the chunk size actually in use
int  vk2_pool_ready(void);

// ---------------------------------------------------------------- 池登记 / 反查（§9.2、§9.3）
// 登记一个客体池：`mem` 是宿主给的普通内存（页对齐不是前提），`size` 是字节数。
// 返回 1 = 池在册（可能带着若干"只带传输位"的块，那要计到 small 上，探针会看见）；0 = 一块都没建出来。
// Register a guest pool: mem is ordinary host memory (page alignment is NOT a precondition). 1 = registered
// (possibly with transfer-only chunks, which are counted in `small` and visible to the probe), 0 = nothing built.
int  vk2_pool_add(void *mem, uint64_t size, const char *tag);
void vk2_pool_del(void *mem);

// 地址反查：成功 1 / 失败 0。成功时 `*buf` 是覆盖该段的**块**缓冲，`*off` 是在**那块里**的偏移
// （= addr − 池首 − 块在池内的偏移）。**请求跨块即返回 0**，并记 count + 长度分布 + 一条日志（前几条报明细）。
// `len == 0` 按 1 字节处理（保守）。
// Address lookup: 1 on success, 0 on failure. On success *buf is the covering **chunk** buffer and *off is the
// offset **inside that chunk**. A request crossing a chunk boundary returns 0, counts, lands in the length
// histogram and logs (details for the first few). len == 0 is treated as one byte.
int  vk2_addr(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off);

// 探针/诊断用：看池的内部形状 | for the probe and diagnostics: the pool's internal shape
int             vk2_pool_count(void);
const vk2_pool *vk2_pool_at(int i);
// N2：池的脏位图（可写）。上传窗口按它扫 CPU_DIRTY 的 run，回读之后由调用方置 CLEAN。
// N2: the pool's dirty bitmap (mutable). The upload window scans it for CPU_DIRTY runs; the caller marks CLEAN
// after a readback.
vk2_dirty      *vk2_pool_dirty(int i);
// 池的宿主基地址（= 影子内存的地址）/ 池大小：搬运窗口要按它算"池内偏移"。
// The pool's host base (the shadow's address) and size: the transfer windows derive pool-relative offsets.
void           *vk2_pool_base(int i);
uint64_t        vk2_pool_bytes(int i);

// ---------------------------------------------------------------- 快路（§9.8）：只做降级规则
// 请求把某个池"钉"到 GPU 内存上（省掉搬运）。**任何一步失败 ⇒ 该池回落 SHADOW，报计数不报红**；
// 正确性路径不依赖它。本轮**没有实现真正的钉住**（那是快路，等正确性闭环之后再谈），所以它今天一定回落——
// 探针要能看见这个回落，而且回落之后 `vk2_addr` 照旧正确。
// Ask to pin a pool onto GPU memory (saving the copies). Any step failing degrades the pool back to SHADOW and
// counts (yellow, never red); correctness never depends on it. Real pinning is not implemented yet, so today it
// always degrades -- the probe is expected to see that, and vk2_addr stays correct afterwards.
int  vk2_pool_pin(void *mem);
// 快路的前提在**这台设备上**成不成立（可导出不透明 fd）：1 = 有，0 = 没有。理由写进 `why`（可空）。
// Whether the fast path's precondition holds on the device (exportable opaque fd); 1 = yes, 0 = no.
int  vk2_pin_capable(const char **why);

// ---------------------------------------------------------------- 读数与日志
void vk2_pool_stats_get(vk2_pool_stats *out);
void vk2_pool_stats_reset(void);
// 跨块请求的长度分布印成一行（§9.3 要的那份"带长度分布"的账）| the cross-chunk length distribution, one line
void vk2_cross_hist_text(char *out, size_t cap);

typedef void (*vk2_log_fn)(const char *fmt, ...);
void vk2_pool_set_log(vk2_log_fn fn);      // 不给就用 stderr | defaults to stderr
void vk2_pool_shutdown_log(void);          // 交回给 stderr | hand logging back to stderr

// ---------------------------------------------------------------- 开关 DM_GFX2（缺省关）
// 本模块只回答"**这次走哪条路**"这一个可读事实：真正的选路点与接管回放层是 **N4**（本轮**不**改回放层——
// 不为让开关"看起来有作用"就提前接管）。启动/探针日志里打 vk2_route_line() 即自证。
// This module only answers the readable question "which road is this run taking"; the real selection point and
// the replay-layer take-over are N4 (this round deliberately does not touch the replay layer). Printing
// vk2_route_line() in the start-up or probe log is the self-proof.
// 认得的值：1 / on / yes / true（大小写不敏感）；其余（含未设）都是旧路。
// Recognised values: 1 / on / yes / true (case-insensitive); anything else (including unset) means the old road.
int         vk2_gfx2_enabled(void);
const char *vk2_route_line(void);

#endif // DM_VK2_POOL_H
