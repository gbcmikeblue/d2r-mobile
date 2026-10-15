// M3-2w：「等待站点账本」在探针里的空实现。
// 为什么需要：账本的真身在宿主侧（`dm/host/crash.c` —— 它必须能被任何链接组合取到），而 `vk_pipe_probe` 这类探针
// 只链图形层的几个源文件（vk_pipe.c 里有建管线的记账调用），**有意不链宿主**：探针量的是驱动的能力，
// 与"谁在等什么"无关。于是给这三个入口一份空实现，探针照旧独立可跑。
// M3-2w: empty "wait-site ledger" for the probes. Why: the real ledger lives on the host side (`dm/host/crash.c` -- it
// has to be reachable from every link combination), while probes such as `vk_pipe_probe` link only a few graphics-layer
// sources (vk_pipe.c carries the pipeline-creation accounting) and deliberately **do not** link the host: a probe
// measures what the driver can do, not who waits for what. These three empty definitions keep the probes standalone.
void dm_waitled_enter(const char *site) { (void)site; }
void dm_waitled_leave(void) {}
void dm_waitled_dump(void) {}
