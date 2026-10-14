// M1 还没有图形后端：这里给出图形层留给宿主的那几个符号的空实现，让宿主能链接起来。
// 它就是 nvn_mtl.m 在安卓这边的替身——M2 的 Vulkan 回放层落地后，这个文件应当删掉。
// M1 has no graphics backend yet: this supplies empty implementations of the few symbols the
// graphics layer offers the host, so that the host links. It is the Android stand-in for nvn_mtl.m
// and should be deleted once the M2 Vulkan replay layer lands.

// nvn_mtl.m：在当前出图帧之后第 k 帧截图；没有出图，请求直接丢弃。
// nvn_mtl.m: screenshot of the kth frame after the current presented frame; there is no
// presentation, so the request is dropped.
// 注意：丢掉它意味着 diag.c 的 DM_TICK_HOLD=E:SHOT 永远不会收到 dm_shot_written 而解冻。
// Note: dropping it means diag.c's DM_TICK_HOLD=E:SHOT never sees dm_shot_written and so never
// unfreezes.
void dm_gfx_shot_at(long k, const char *path) { (void)k; (void)path; }

// nvn_mtl.m：清空在途 GPU 工作（进后面前调）；这里没有 GPU 工作可清。
// nvn_mtl.m: drain in-flight GPU work (called before going to the background); there is none here.
void mtl_drain(void) { }
