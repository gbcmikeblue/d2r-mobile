// M1 还没有图形后端：这里给出图形层留给宿主的那批符号的空实现，让宿主能链接起来。
// 它就是 nvn_mtl.m 在安卓这边的替身——M2 的 Vulkan 回放层落地后，这个文件应当删掉。
// M1 has no graphics backend yet: this supplies empty implementations for the symbols the graphics
// layer offers the host, so that the host links. It is the Android stand-in for nvn_mtl.m and should
// be deleted once the M2 Vulkan replay layer lands.
//
// 取值约定：查询类一律「没有 / 没有窗口 / 没准备好」（0 或失败），等待类立刻返回，统计类的出参写 0，
// 释放与登记类空操作。这样宿主里那些统计、诊断与生命周期调用点都拿到一个合理的空值。
// Value conventions: queries always answer "none / no window / not ready" (0 or failure), waits return
// immediately, statistics out-parameters are written 0, and release/registration calls do nothing.
// Host call sites that gather statistics or diagnostics, and the lifecycle ones, therefore get a
// sensible empty answer.
//
// 注意：因为 M1 的链接关掉了 --gc-sections（保登记段，见 CMakeLists.txt），这些空实现必须把宿主
// 引用到的每一个都补上，不能靠链接器丢弃未用代码来兜底。
// Note: because M1 links with --no-gc-sections (to keep the registry sections, see CMakeLists.txt),
// every one these stubs cover must really be provided -- the linker can no longer quietly discard the
// unused callers.
#include "../gfx/nvn/nvn_obj.h"
#include "../gfx/nvn/nvn_mtl.h"

// ---- 帧率与窗口 | Frame rate and window ----

// 目标帧率（省电模式 30 / 普通 60）：M1 没有出图，恒 60（= 关省电模式）
// Target frame rate (30 in power-saving mode, 60 otherwise): with no presentation in M1, always 60
int mtl_fps_target(void) { return 60; }
int mtl_has_window(void) { return 0; }
int mtl_display_hz(void) { return 0; }                                     // 无窗口 | no window
uint32_t mtl_present_qos(void) { return 0; }

// ---- 出图 | Presentation ----

// 出图入队并返回票号；没有出图，恒返回 0
// Enqueue a present and return its ticket; there is no presentation, so always 0
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) { (void)t; (void)period_s; (void)crop; return 0; }
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
void mtl_present_flush(void) { }
double mtl_presented_time(uint64_t ticket) { (void)ticket; return 0; }
void mtl_ub_present(double interval_ms) { (void)interval_ms; }
void mtl_set_frame(uint64_t f) { (void)f; }
uint64_t mtl_drawable_ns_take(void) { return 0; }

// 按请求截图（DM_NVN_SHOT_UI 用）：在当前出图帧之后第 k 帧截图；没有出图，请求直接丢弃。
// Screenshot on request (for DM_NVN_SHOT_UI): the kth frame after the current presented frame; there
// is no presentation, so the request is dropped.
// 注意：丢掉它意味着 diag.c 的 DM_TICK_HOLD=E:SHOT 永远等不到 dm_shot_written 而解冻。
// Note: dropping it means diag.c's DM_TICK_HOLD=E:SHOT never sees dm_shot_written and never unfreezes.
void dm_gfx_shot_at(long k, const char *path) { (void)k; (void)path; }

// ---- 显示节拍与等待 | Display link and waits ----

int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(uint64_t timeout_ns) { (void)timeout_ns; return 0; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 0; }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 1; }          // 没什么要等的 | nothing to wait for
uint64_t mtl_last_fence(void) { return 0; }                                // 从未提交 | never submitted
void mtl_drain(void) { }
void mtl_finish(void) { }

// ---- 提交（Metal 命令缓冲 / 内存池 / 内存池登记） | Submission (Metal command buffers / memory pools) ----

void mtl_pool_add(const void *mem, uint64_t size) { (void)mem; (void)size; }
void mtl_pool_del(const void *mem) { (void)mem; }
void mtl_sub_begin(void) { }
uint64_t mtl_sub_commit(void) { return 0; }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec(const uint8_t *handle) { (void)handle; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }

// ---- 资源登记与释放 | Resource registration and release ----

void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0; (void)r; (void)dst; (void)row; (void)img;
}

// ---- 外推与冻结的渲染侧握手 | Render-side handshake for extrapolation and freezing ----

void mtl_world_frozen(int on) { (void)on; }
void mtl_rt_force_full(int frames) { (void)frames; }
int dm_nvn_ruler_truth(void) { return 0; }                                 // 不在视觉尺子的真值阶段 | not in the visual ruler's ground-truth phase

// ---- 统计读数（宿主只用来打日志） | Statistics read by the host for logging only ----

void mtl_stats(uint64_t *passes, uint64_t *draws) { if (passes) *passes = 0; if (draws) *draws = 0; }
void mtl_args_stats(uint64_t *encoded, uint64_t *reused) { if (encoded) *encoded = 0; if (reused) *reused = 0; }
void mtl_use_stats(uint64_t *calls, uint64_t *skipped) { if (calls) *calls = 0; if (skipped) *skipped = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes) { if (load) *load = 0; if (store) *store = 0; if (full_rw_passes) *full_rw_passes = 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { if (lib_ns) *lib_ns = 0; if (pso_ns) *pso_ns = 0; }
void mtl_create_stats(uint64_t *libs, uint64_t *psos) { if (libs) *libs = 0; if (psos) *psos = 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { if (uploads) *uploads = 0; if (reuploads) *reuploads = 0; }
void mtl_alloc_stats(uint64_t *waits, uint64_t *gpu_waits, int *pools) { if (waits) *waits = 0; if (gpu_waits) *gpu_waits = 0; if (pools) *pools = 0; }
void mtl_mem_stat(unsigned long long frame, unsigned long long mb, int ok, unsigned long long a, unsigned long long b, unsigned long long c) {
    (void)frame; (void)mb; (void)ok; (void)a; (void)b; (void)c;
}
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
double mtl_gpu_ms(uint64_t frame) { (void)frame; return 0; }
int mtl_gpu_span(uint64_t frame, uint64_t *t0, uint64_t *t1) { (void)frame; if (t0) *t0 = 0; if (t1) *t1 = 0; return 0; }

// ---- 苹果私有接口：本线程累计 {指令, 周期}（libSystem；Mac / iOS 都有，安卓没有对应物）----
// ---- Apple private interface: this thread's cumulative {instructions, cycles} (libSystem; present on Mac and iOS, no Android counterpart) ----
// hle_nvn.c 用它读「翻译细项」里的指令数 / 周期数；安卓这边给 0，那几项就报 0（只是统计，不影响行为）。
// hle_nvn.c reads the instruction / cycle counts of the translation breakdown through it; returning 0
// here makes those fields report 0, which is statistics only and does not affect behaviour.
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; if (buf && nbytes) memset(buf, 0, nbytes); return 0; }
