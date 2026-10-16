// 安卓窗口工程（M3）：装到手机上、由宿主**那一个** Vulkan 设备上屏。
// The Android window project (M3): installed on the phone, presenting through the host's **single** Vulkan device.
//
// 与第一版的区别（第一版在 app 里自建了一套实例/设备/交换链）：**窗口与游戏必须共用同一个设备**，否则两个设备之间
// 传不了纹理，"窗口显示游戏画出来的那张贴图"就无从谈起。所以现在设备由 `vk_device.c` 建（`vk_device_set_window` 在
// init 之前把窗口告诉它），交换链与上屏由 `vk_win.c` 管，本文件只负责三件事：**画一帧主机内存里的 RGBA8**、
// 报权限状态与那个申请按钮、以及把窗口的生命周期接上。测试图（底色 + 滑条 + 橙块）就是"游戏贴图"的替身 ——
// 它走的正是下一步"贴图查看器"要走的那条路：主机像素 → `vk_win_present_rgba` → 手机屏。
// How this differs from the first version (which built its own instance, device and swapchain inside the app): **the
// window and the game have to share one device**, or textures cannot cross between them and "the window shows the
// texture the game drew" is impossible. So the device now comes from `vk_device.c` (the window is handed to it before
// init), the swapchain and present belong to `vk_win.c`, and this file does three things: **paint one frame of
// host-memory RGBA8**, report the permission state and that request button, and hook up the window's lifecycle. The test
// pattern (background, bar, orange block) stands in for "a game texture" -- and it takes exactly the route the next step's
// texture viewer will take: host pixels -> `vk_win_present_rgba` -> the phone screen.
#include <android_native_app_glue.h>
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "vk_device.h"
#include "vk_png.h"
#include "vk_win.h"

// 贴图查看器只用这两个入口，所以**就地声明**，而不是去包含 vk_tex.h —— 那个头会带进宿主的内部头（nvn_obj.h）与
// 兼容层，而 app 这一侧是干净的安卓代码（见 CMakeLists 里那条说明：**不给**本工程加 dm_android_compat）。
// The texture viewer uses only these two entry points, so they are declared here rather than by including vk_tex.h --
// that header pulls in the host's internal headers (nvn_obj.h) and the compat layer, while the app side is clean
// Android code (see the note in CMakeLists: the dm_android_compat force-include is deliberately **not** added here).
int vk_tex_grab_ser(uint64_t serial, unsigned level, uint8_t *dst, size_t pitch, uint32_t *out_w, uint32_t *out_h);
// `lvs` = 每张贴图的 mip 级数：查看器要按**要读的那一级**准备缓冲与行距（见 viewer_frame 里那段）。
// `lvs` = each texture's mip count: the viewer sizes its buffer and pitch by **the level it reads** (see viewer_frame).
int vk_tex_list(uint64_t *ser, uint32_t *ws, uint32_t *hs, uint32_t *lvs, int cap);
// 心跳/报告用的那两件（同样就地声明）：出图次数、以及一行宿主侧计数
// The two used by the heartbeat / report (also declared locally): the present count and one line of host-side counters
uint64_t vk_present_count(void);
int vk_diag_line(char *buf, size_t cap);
// 宿主的"按需导出现场"（crash.c）：各线程在等什么 + 图形侧等待点，**不退出**。就地声明，理由同上。
// The host's on-demand state dump (crash.c): what each thread waits on plus the graphics wait points, **without
// exiting**. Declared locally for the same reason as the two above.
void crash_dump_now(const char *why);
// M3-2w：给屏上那行数字用的两个宿主读数（同样就地声明）：引擎拍、以及看门狗记的当前阶段。
// M3-2w: the two host readings behind the on-screen line (declared locally as well): the engine tick and the stage the
// watchdog last recorded.
uint32_t dm_cur_tick(void);   // 服务端拍（diag.c；不在局内返回 0） | the server tick (diag.c; 0 when not in a run)
const char *crash_watch_stage(void);

// 宿主那套的环境准备与 dm_main 的交接（dm/android/main_android.c）：**与 APK 必须完全一致**，所以那边只留一个实现，
// 这里直接调它，而不是把环境准备抄一遍（抄一遍就会漂）。
// The host's environment setup and hand-over to dm_main (dm/android/main_android.c): it **has to be identical for the
// APK**, so that file keeps one implementation and this one calls it rather than copying the setup (a copy would drift).
int dm_android_main(int argc, char **argv);

#define TAG "dmwin"

// ---------- 日志：同时进 logcat 与一个文件 ----------
// 为什么必须有文件：进程被系统**直接杀掉**（SIGKILL / ANR / 内存不足）时任何处理器都来不及跑，logcat 也可能被刷掉或
// 截断，只有"每行立刻落盘"能在被杀之前把现场留下 —— 这正是"闪退 / 卡死都看不到原因"要补的那件事。
// 位置：app 自己的外部目录，adb 可直接拉回 Windows 看；每次启动一个新文件，上一轮留成 prev.log。
// 不自己装 SIGSEGV 那类处理器：宿主的 crash.c 已经装了（并且会打回溯），覆盖掉反而丢现场。
// The log goes to both logcat and a file. Why a file is mandatory: when the system kills the process outright
// (SIGKILL / ANR / out of memory) no handler ever runs and logcat can be flooded or truncated, so only "every line lands
// on disk immediately" leaves the scene behind before the kill -- which is exactly the gap behind "a crash or a stall
// with no reason visible". Location: the app's own external directory, which adb can pull straight back to Windows; a
// fresh file per start with the previous run kept as prev.log. No SIGSEGV-class handler is installed here on purpose:
// the host's crash.c already has one (and prints a backtrace), and replacing it would lose that scene rather than add to
// it.
static int g_log_fd = -1;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

// 单行落地（也用于宿主经由日志泵送来的那些行）
// One line to disk (also used for the host's lines arriving through the log pump)
static void log_sink(const char *s, size_t n) {
    pthread_mutex_lock(&g_log_mu);
    if (g_log_fd >= 0 && n) {
        ssize_t w = write(g_log_fd, s, n);
        (void)w;
        if (s[n - 1] != '\n') { ssize_t w2 = write(g_log_fd, "\n", 1); (void)w2; }
    }
    pthread_mutex_unlock(&g_log_mu);
}

static void log_line(int err, const char *fmt, ...) {
    char line[1200];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [%s] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                     (int)(ts.tv_nsec / 1000000), err ? "错" : "信");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    __android_log_print(err ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, TAG, "%s", line);
    log_sink(line, (size_t)n);
}

#define LOGI(...) log_line(0, __VA_ARGS__)
#define LOGE(...) log_line(1, __VA_ARGS__)

// 开日志文件：上一轮的 latest.log 留成 prev.log，然后新建（只保留两轮，免得堆满）
// Open the log file: the previous run's latest.log becomes prev.log, then a fresh one is created (two runs kept, no pile-up)
static void log_files_open(void) {
    static const char *dir = "/sdcard/Android/data/ai.d2rm.dm/files/logs";
    mkdir("/sdcard/Android/data/ai.d2rm.dm/files", 0777);
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "建不了日志目录 %s（errno %d：%s）", dir, errno, strerror(errno));
        return;
    }
    char cur[256], prev[256];
    snprintf(cur, sizeof cur, "%s/latest.log", dir);
    snprintf(prev, sizeof prev, "%s/prev.log", dir);
    rename(cur, prev);
    g_log_fd = open(cur, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
    if (g_log_fd < 0) __android_log_print(ANDROID_LOG_ERROR, TAG, "开不了日志文件 %s（errno %d：%s）", cur, errno, strerror(errno));
}

// 测试图的尺寸：按游戏真实画面目标来（真机是 1600×900），这样上屏那套缩放与将来一致
// The test pattern's size follows the game's real render targets (1600x900 on the device), so the present-side scaling
// matches what comes later
#define IMG_W 1600
#define IMG_H 900

static struct android_app *g_app;
static int g_guest_started;             // 游戏只起一次（TERM_WINDOW/INIT_WINDOW 会来回）| the game starts once (TERM_WINDOW/INIT_WINDOW come and go)
static uint8_t *g_img;                  // 主机内存里的一帧（RGBA8）| one frame in host memory (RGBA8)
static int g_perm_files;                // "所有文件访问"是否已授予 | whether all files access is granted
static int32_t g_btn_x, g_btn_y, g_btn_w, g_btn_h;   // 申请按钮（**图像坐标**）| the request button, in image coordinates
// M3-2w：屏幕上那两个"给用户用"的口子的状态（详细说明见 hud_overlay 那一段）——数字行、点角落取现场。
// M3-2w: the state of the two user-facing on-screen affordances (details in the hud_overlay section) -- the readout
// line and the corner tap that takes a state dump.
static int g_hud_text, g_hud_tap;
static volatile int g_tap_dump;   // 点角落 → 交给下一跳心跳（volatile 够用：写一次读一次）| corner tap -> next heartbeat (volatile is enough: one write, one read)
static struct { double t; uint64_t present; unsigned tick; char stage[28]; uint64_t vok, vskip; } g_hud;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// ---------- 把宿主的 stderr 接到 logcat ----------
// 宿主的 LOG() 走 stderr，而 **app 的 stdout/stderr 不进 logcat** —— 不接的话，游戏那一堆诊断在手机上一条都读不到
// （不是推测：合一设备那次真机日志里只有 app 自己的行）。接法取最不侵入的一种：开一条管道、把 1/2 号 fd 换过去、
// 再起一个线程按行读出来转给 `__android_log_print`。好处是连 libc 与第三方库的消息也一并进 logcat，宿主一行都不用改。
// ---------- Routing the host's stderr into logcat ----------
// The host's LOG() writes to stderr and **an app's stdout/stderr do not reach logcat** -- without this, the game's
// diagnostics cannot be read on the phone at all (not a guess: in the merged-device run the device log held only this
// app's own lines). The least invasive route is taken: make a pipe, move fds 1 and 2 onto it, and have a thread read it
// line by line into `__android_log_print`. The dividend is that libc's and third-party libraries' messages come along
// too, with not one line of the host changed.
static void *log_pump(void *arg) {
    // N4-d 第二部分：宿主自己的线程要能在 atrace / ps -T 里一眼认出来（`dm-*` = 宿主、`dm-g*` = 客体，
    // 见 `hle_thread.c` 里那段说明）。这一条是"事后把 tid 认成谁"的唯一凭据。
    // N4-d part 2: name our own threads so atrace / ps -T can tell them apart (dm-* ours, dm-g* the guest's).
    pthread_setname_np(pthread_self(), "dm-log");
    int fd = (int)(intptr_t)arg;
    char buf[1024];
    size_t n = 0;
    for (;;) {
        char tmp[512];
        ssize_t r = read(fd, tmp, sizeof tmp);
        if (r <= 0) break;
        for (ssize_t i = 0; i < r; i++) {
            char c = tmp[i];
            if (c == '\n' || n >= sizeof buf - 1) {
                buf[n] = 0;
                if (n) { __android_log_print(ANDROID_LOG_INFO, "dmhost", "%s", buf); log_sink(buf, n); }
                n = 0;
            } else buf[n++] = c;
        }
    }
    if (n) { buf[n] = 0; __android_log_print(ANDROID_LOG_INFO, "dmhost", "%s", buf); log_sink(buf, n); }
    return NULL;
}

static void log_pipe_start(void) {
    log_files_open();   // 先把文件开好：**连 libc 与第三方库在启动早期的输出也要落盘** | open the file first so even libc's and third-party libraries' early startup output lands on disk
    int fd[2];
    if (pipe(fd) != 0) { LOGE("接日志失败：pipe %s", strerror(errno)); return; }
    dup2(fd[1], 1);
    dup2(fd[1], 2);
    close(fd[1]);
    setvbuf(stdout, NULL, _IOLBF, 0);   // 行缓冲 + 无缓冲：日志要"说到就到"，否则卡在缓冲里看不到
    setvbuf(stderr, NULL, _IONBF, 0);
    pthread_t th;
    if (pthread_create(&th, NULL, log_pump, (void *)(intptr_t)fd[0]) == 0) pthread_detach(th);
    else LOGE("接日志失败：起不了读日志的线程");
}

// ---------- 游戏：跑在工作线程上 ----------
// dm_main 会**阻塞**（把客体一直跑到退出），而主线程要留着做事件与上屏 —— app_glue 的事件循环必须在创建它的那个线程
// 上跑，所以两边不能互换位置。窗口先建好再起游戏：`vk_device_init` 只会跑一次，而窗口必须在它之前交过去（见
// vk_device_set_window），所以顺序不能反。
// ---------- The game: on a worker thread ----------
// dm_main **blocks** (it runs the guest until it exits) while the main thread has to stay free for events and
// presenting -- app_glue's event loop must run on the thread that created it, so the two cannot swap places. The window
// is brought up before the game starts: `vk_device_init` only ever runs once and the window has to be handed over before
// it (see vk_device_set_window), so the order cannot be reversed.
static void *guest_thread(void *arg) {
    (void)arg;
    static char *a[] = { (char *)"dm", (char *)"", (char *)"main", (char *)"sdk" };   // argv[1] 空 = 用 <根>/patched | empty argv[1] means <root>/patched
    int rc = dm_android_main(4, a);
    LOGI("宿主返回 %d（游戏结束）", rc);
    return NULL;
}

// ---------- 数据从哪读（探针）----------
// 装成 APK 后 uid 不再是 shell：**读得到、写不进** adb 放进 /data/local/tmp 的数据（SELinux 的 shell_data_file）。
// 于是数据原地读（9.1 GB 不用搬），存档与临时走 DM_SAVE / DM_TEMP 指到 app 可写目录。这条探针在每次启动时把
// 结论重说一遍 —— 环境、权限、设备都可能变，结论不该靠记忆。
// Where the data comes from (probe). Once installed as an APK the uid is no longer shell: the data adb pushed into
// /data/local/tmp can be **read but not written** (SELinux's shell_data_file). So the data is read in place (the 9.1 GB
// needs no copying) while saves and temporary files go through DM_SAVE / DM_TEMP pointing at a directory the app may
// write. This probe restates the conclusion on every start -- environment, permissions and devices change, and a
// conclusion should not rest on memory.
static void probe_paths(void) {
    char ctx[256] = "";
    FILE *f = fopen("/proc/self/attr/current", "r");
    if (f) { if (fgets(ctx, sizeof ctx, f)) { char *nl = strchr(ctx, '\n'); if (nl) *nl = 0; } fclose(f); }
    LOGI("探针：uid=%u gid=%u 域 %s", (unsigned)getuid(), (unsigned)getgid(), ctx[0] ? ctx : "?");
    static const char *paths[] = {
        "/data/local/tmp/dm/patched/main.patched.bin",   // 游戏主模块（54 MB）| the game's main module
        "/data/local/tmp/dm/spv",                         // 着色器产物目录 | the shader artifact directory
        "/data/local/tmp/dm/romfs",                       // 8.3 GB 的原版档案 | the 8.3 GB original archives
        "/sdcard/Android/data/ai.d2rm.dm/files",          // 正向对照：app 自己的外部目录 | positive control: the app's own external directory
        "/sdcard/d2rm/romfs",                             // 入口里写好的备用位置 | the fallback the entry already knows
    };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        FILE *g = fopen(paths[i], "rb");
        if (g) { LOGI("  能读 %s", paths[i]); fclose(g); }
        else LOGE("  读不了 %s（errno %d：%s）", paths[i], errno, strerror(errno));
    }
    // /sdcard/d2rm 是「所有文件访问」要用的那个目录，入口的备用路径也正指着它：先试着建它 —— 建成 / 建不成本身
    // 就是这条权限的证据（不建的话，往里写会报 errno 2「没有这个目录」，那是另一回事，看不出权限）
    // /sdcard/d2rm is the directory "all files access" is for; creating it first is itself the evidence (without it a
    // write inside reports errno 2 "no such directory", which says nothing about the permission)
    if (mkdir("/sdcard/d2rm", 0777) != 0 && errno != EEXIST)
        LOGE("  建不了 /sdcard/d2rm（errno %d：%s）", errno, strerror(errno));
    else LOGI("  /sdcard/d2rm 在");
    static const char *wpaths[] = {
        "/data/local/tmp/dm/save/.d2rm_probe",            // 读得到、**写不进**（SELinux）| readable, **not** writable (SELinux)
        "/data/local/tmp/dm/temp/.d2rm_probe",
        "/sdcard/Android/data/ai.d2rm.dm/files/.d2rm_probe",
        "/sdcard/d2rm/.d2rm_probe",                       // 「所有文件访问」真正买到的那块地方 | what "all files access" really buys
    };
    for (unsigned i = 0; i < sizeof wpaths / sizeof wpaths[0]; i++) {
        FILE *g = fopen(wpaths[i], "wb");
        if (g) { fputc('x', g); fclose(g); remove(wpaths[i]); LOGI("  能写 %s", wpaths[i]); }
        else LOGE("  写不了 %s（errno %d：%s）", wpaths[i], errno, strerror(errno));
    }
    // 建目录单独测：**能建文件不等于能建目录**，而客体要的正是建目录（真机上就是这里报 errno=13 让它卡住的）
    // Test mkdir separately: **being able to create a file does not mean being able to create a directory**, and the
    // guest needs exactly that (on the device this is where errno=13 stalled it).
    static const char *dpaths[] = {
        "/sdcard/Android/data/ai.d2rm.dm/files/probe_dir",     // 外部（FUSE，带包作用域）| external (FUSE, package scoped)
        "/data/data/ai.d2rm.dm/files/probe_dir",                // 内部（本 uid 自己的）| internal (this uid's own)
    };
    for (unsigned i = 0; i < sizeof dpaths / sizeof dpaths[0]; i++) {
        if (mkdir(dpaths[i], 0777) == 0) { rmdir(dpaths[i]); LOGI("  能建目录 %s", dpaths[i]); }
        else if (errno == EEXIST) LOGI("  能建目录 %s（已存在）", dpaths[i]);
        else LOGE("  建不了目录 %s（errno %d：%s）", dpaths[i], errno, strerror(errno));
    }
}

// ---------- 权限：什么时候要用户点一下，以及那个按钮 ----------
// 整个包只申请两条权限（见 AndroidManifest.xml），只有 MANAGE_EXTERNAL_STORAGE（所有文件访问）是"特殊"的、要在
// 系统设置里点开 —— 图像里那块**橙色矩形**就是给它的按钮，而**按钮的有与无本身就是权限状态**（有 = 还没授权），
// 所以不必画一个字。它买到的是"数据放在用户看得见、随时能用文件管理器替换的地方"（/sdcard/d2rm）；它**买不到**
// /data/local/tmp 的写权限 —— 那条路是 SELinux 的 shell_data_file，任何权限都到不了（A/B 实测过）。
// Permissions: the package requests just two (see AndroidManifest.xml) and only MANAGE_EXTERNAL_STORAGE is "special",
// needing a switch in system settings -- the **orange rectangle** in the picture is its button, and **the button's
// presence or absence is itself the permission state** (present means not granted), so nothing is drawn in words. What
// it buys is "data in a place the user can see and replace with a file manager" (/sdcard/d2rm); what it does **not** buy
// is write access to /data/local/tmp -- that is SELinux's shell_data_file and no permission reaches it (measured A/B).
static JNIEnv *jnienv(void) {
    if (!g_app || !g_app->activity || !g_app->activity->vm) return NULL;
    JavaVM *vm = g_app->activity->vm;
    JNIEnv *e = NULL;
    if ((*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_6) == JNI_OK) return e;
    if ((*vm)->AttachCurrentThread(vm, &e, NULL) == JNI_OK) return e;
    return NULL;
}

static void jni_swallow(JNIEnv *e) { if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e); }

// 查"所有文件访问"是否已授予（Environment.isExternalStorageManager，API 30+）；-1 = 查不了
// Whether all files access is granted (Environment.isExternalStorageManager, API 30+); -1 = cannot be queried
static int perm_query(void) {
    JNIEnv *e = jnienv();
    if (!e) return -1;
    jclass c = (*e)->FindClass(e, "android/os/Environment");
    if (!c) { jni_swallow(e); return -1; }
    jmethodID m = (*e)->GetStaticMethodID(e, c, "isExternalStorageManager", "()Z");
    if (!m) { jni_swallow(e); (*e)->DeleteLocalRef(e, c); return -1; }
    jboolean r = (*e)->CallStaticBooleanMethod(e, c, m);
    jni_swallow(e);
    (*e)->DeleteLocalRef(e, c);
    return r == JNI_TRUE ? 1 : 0;
}

// 打开授权页：先试**本应用**那一页（在那里能直接授权），打不开再退到全局列表页
// Open the grant page: try **this app's** own page first (where it can be granted directly), then the global list page
static void perm_open_settings(void) {
    JNIEnv *e = jnienv();
    if (!e) { LOGE("打不开授权页：拿不到 JNI 环境"); return; }
    jclass ac = (*e)->GetObjectClass(e, g_app->activity->clazz);
    jmethodID start = (*e)->GetMethodID(e, ac, "startActivity", "(Landroid/content/Intent;)V");
    jclass ic = (*e)->FindClass(e, "android/content/Intent");
    jclass uc = (*e)->FindClass(e, "android/net/Uri");
    if (!start || !ic || !uc) { jni_swallow(e); LOGE("打不开授权页：JNI 的方法或类没找到"); return; }
    jmethodID ctor = (*e)->GetMethodID(e, ic, "<init>", "(Ljava/lang/String;)V");
    jmethodID parse = (*e)->GetStaticMethodID(e, uc, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jmethodID set_data = (*e)->GetMethodID(e, ic, "setData", "(Landroid/net/Uri;)Landroid/content/Intent;");
    static const char *acts[] = { "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION",
                                  "android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION" };
    for (int i = 0; i < 2; i++) {
        jstring js = (*e)->NewStringUTF(e, acts[i]);
        jobject in = (*e)->NewObject(e, ic, ctor, js);
        if (i == 0) {   // 本应用那一页要带 `package:` 的 data | this app's page needs a `package:` data URI
            jstring ps = (*e)->NewStringUTF(e, "package:ai.d2rm.dm");
            jobject uri = (*e)->CallStaticObjectMethod(e, uc, parse, ps);
            if (uri) (*e)->CallObjectMethod(e, in, set_data, uri);
            if (ps) (*e)->DeleteLocalRef(e, ps);
            if (uri) (*e)->DeleteLocalRef(e, uri);
        }
        (*e)->CallVoidMethod(e, g_app->activity->clazz, start, in);
        jboolean bad = (*e)->ExceptionCheck(e);
        jni_swallow(e);
        if (in) (*e)->DeleteLocalRef(e, in);
        if (js) (*e)->DeleteLocalRef(e, js);
        if (!bad) { LOGI("已打开「所有文件访问」授权页：%s", acts[i]); break; }
        LOGE("授权页 %s 打不开，试下一个", acts[i]);
    }
    (*e)->DeleteLocalRef(e, ac);
    (*e)->DeleteLocalRef(e, ic);
    (*e)->DeleteLocalRef(e, uc);
}

// 约每秒查一次，**第一次也报**：只报变化的话，"一条都不打"分不清是"已授权"还是"查不了"（踩过一次）
// Queried about once a second, and logged on the **first** one too: logging only changes leaves "nothing printed"
// ambiguous between "granted" and "cannot be queried" (learned the hard way)
static void perm_tick(double t) {
    static double next;
    static int said_first;
    if (t < next) return;
    next = t + 1.0;
    int v = perm_query();
    if (v < 0) {
        if (!said_first) { said_first = 1; LOGE("查不到「所有文件访问」的状态（JNI 那一步没走通）"); }
        return;
    }
    if (!said_first || v != g_perm_files) {
        said_first = 1;
        g_perm_files = v;
        LOGI("「所有文件访问」%s（%s）", v ? "已授予" : "未授予",
             v ? "数据可以放到 /sdcard/d2rm 让用户自己看得见" : "橙块就是申请按钮，点它打开系统设置页");
    }
}

// 触摸：点在橙块里就打开授权页。窗口坐标要先换算成图像坐标（上屏是"保持比例居中"，中间有黑边）。
// Touch: a tap inside the orange block opens the grant page. Window coordinates are first mapped to image coordinates
// (presenting is fit-and-centre, so there are black bars in between).
static int32_t on_input(struct android_app *app, AInputEvent *ev) {
    (void)app;
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;
    if ((AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK) != AMOTION_EVENT_ACTION_DOWN) return 0;
    int ix = 0, iy = 0;
    if (!vk_win_window_to_src(AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0), IMG_W, IMG_H, &ix, &iy)) return 0;
    // M3-2w：点屏幕**左下角** = 取一份现场，和 `dump.trigger` 是同一件事（DM_HUD_TAP=1 才开，缺省关）。
    // 为什么要有它：adb 掉线、或者画面已经冻住的时候，用户自己按一下就能留现场，不必等电脑接上；
    // 为什么在权限判断**之前**：授权之后触摸仍然要能用它。为什么放左下角：那一块在游戏里通常没有可点的界面。
    // M3-2w: tapping the **bottom-left corner** takes a state dump -- the same thing dump.trigger does (only with
    // DM_HUD_TAP=1, off by default). Why: when adb is gone or the picture has already frozen, the user can leave a
    // scene behind with one tap instead of waiting for a computer. Why before the permission check: it must keep
    // working after the permission is granted. Why the bottom left: that corner usually has no tappable UI in the game.
    if (g_hud_tap && ix < IMG_W / 5 && iy > IMG_H * 4 / 5) {
        LOGI("点按左下角（图像坐标 %d,%d）：取一份现场", ix, iy);
        g_tap_dump = 1;   // 交给下一跳心跳去做（转储要几秒，别堵住输入线程）| handed to the next heartbeat (a dump takes seconds; do not block the input thread)
        return 1;
    }
    if (g_perm_files) return 0;
    if (ix < g_btn_x || ix >= g_btn_x + g_btn_w || iy < g_btn_y || iy >= g_btn_y + g_btn_h) return 0;
    LOGI("点了授权按钮（图像坐标 %d,%d）", ix, iy);
    perm_open_settings();
    return 1;
}

// ---------- 贴图查看器（M3-2c）：把游戏自己的一张渲染目标搬上手机屏 ----------
// 为什么是"游戏自己的那张"而不是"出图纹理"：真机量过 —— **游戏从没往要上屏的那张纹理里写过**（既没当渲染目标、
// 也没被 T2T 拷过），而它每帧都在往自己那三张 1600×900 里画（序号 28 = RGBA16F、27 = RGBA8、26 = B10G11R11，
// 每帧 5400/5400/4800 次）。所以画面只能从那里取，默认取用得最多的 28，可用 DM_WIN_SRC=<序号> 换。
// 今天它多半是**黑的**——那就说明渲染没落下来；一旦渲染链修好，手机屏上直接就是画面，不再需要把 PNG 拉到电脑上。
// ---------- Texture viewer (M3-2c): put one of the game's own render targets on the phone screen ----------
// Why "one of the game's own" and not "the present texture": it was measured on the device that **the game never writes
// the texture it presents** (neither as a render target nor as a T2T destination) while it renders into its own 1600x900
// trio every frame (serial 28 = RGBA16F, 27 = RGBA8, 26 = B10G11R11, used 5400/5400/4800 times per frame). So the
// picture can only come from there; the default is the most-used one (28), changeable with DM_WIN_SRC=<serial>. Today it
// is most likely **black** -- which means the rendering is not landing; once the render chain is fixed the phone screen
// shows the picture directly, with no more pulling PNGs onto a PC.
// ---------- 心跳方块（画在右下角，不需要字体）----------
// 为什么要有它：查看器显示的那张游戏目标本来就是黑的 ✗ —— "一直黑屏"和"卡死"光看屏幕分不出来 ✗。两个小方块把它分开：
//   上面的：**心跳** —— 每次出图换一次亮/暗（在跑就能看见它在闪 ✓）
//   下面的：**状态** —— 绿 = 一秒内有新出图 ✓；黄 = 卡住 1～8 秒 ✓；红 = 超过 8 秒、或查看器读不回来 ✓
// ---------- The heartbeat squares (bottom right, no font needed) ----------
// Why they exist: the game target the viewer shows is black to begin with, so "a permanent black screen" and "hung" cannot
// be told apart by looking. Two small squares separate them: the upper one is the **heartbeat** (it flips bright/dark on
// every present, so it visibly blinks while running) and the lower one is the **state** -- green when a present happened
// within the last second, yellow when stuck for 1-8 s, red when stuck longer or the viewer cannot read.
static uint64_t g_hb_present;   // 上次心跳时的出图次数 | present count at the last heartbeat
static int g_hb_stall_s;        // 连续多少秒没有新出图 | consecutive seconds without a new present
static int g_hb_blink = 1;      // 每次出图翻转 | flipped on every present

// ---------- M3-2w：屏上那行可读数字（缺省关，DM_HUD_TEXT=1 开）----------
// 方块只说得清"在跑 / 卡住"，说不清"卡在哪一步"。这一行把眼下最要紧的五个数摆上屏：T+秒、出图（F）、拍（P）、
// 阶段（S）、查看器读成/跳过（V）。**自带 5×7 点阵字模**：安卓的系统字体不在 app 里，为了几个数去拖一个字体文件
// 不值当。冻住时**定格**：连续 8 秒没有新出图就不再更新这份快照（屏上留的就是冻住那一刻的数），并转成红字。
// 为什么缺省关：它在**前端线程**里逐像素画，量帧耗时的时候不该混进来。
// 另一个取现场的口：`DM_HUD_TAP=1` 时点屏幕**左下角** = 取一份现场（见 on_input），adb 掉线或画面冻住时用户自己就能取。
// ---------- M3-2w: the on-screen readout line (off by default, DM_HUD_TEXT=1 turns it on) ----------
// The squares say "running / stuck" but not "stuck at which step". This line puts the five numbers that matter up on
// screen: T+seconds, presents (F), tick (P), stage (S), viewer read/skipped (V). It carries its **own 5x7 bitmap font**
// (Android's system fonts are not inside the app, and dragging one in for a few numbers is not worth it). When it
// freezes it **holds**: after 8 seconds with no new present the snapshot stops updating (the screen keeps the numbers
// from the moment it froze) and turns red. Off by default because it paints pixel by pixel **on the front-end thread**,
// which must not be mixed into frame-budget measurements. The other way to take a state dump: with `DM_HUD_TAP=1`, tap
// the **bottom-left corner** (see on_input), so the user can take one when adb is gone or the picture has frozen.
// （两个开关与那份快照的状态声明在文件上部，靠近按钮坐标 —— on_input 也要用它们。）
// (The two switches and the snapshot are declared near the top with the button geometry -- on_input needs them too.)
// 5×7 点阵字模：每字符 7 行、每行低 5 位（bit4 = 最左）。只做 HUD 那一行用得到的字符（0-9、A-Z、+ - : . / = 与空格），
// 小写按大写画 —— **不是**字库，别当字体用。
// 5x7 bitmap font: 7 rows per glyph, low 5 bits each (bit4 = leftmost). Only what the HUD line needs (0-9, A-Z,
// + - : . / = and space), with lowercase drawn as uppercase -- **not** a font, do not use it as one.
static const struct { char c; uint8_t g[7]; } g_hud_glyphs[] = {
    {'0', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}}, {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'%', {0x11, 0x02, 0x02, 0x04, 0x08, 0x08, 0x11}},
};
static const uint8_t *hud_glyph(char c) {
    static const uint8_t blank[7] = { 0, 0, 0, 0, 0, 0, 0 };
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (unsigned i = 0; i < sizeof g_hud_glyphs / sizeof g_hud_glyphs[0]; i++)
        if (g_hud_glyphs[i].c == c) return g_hud_glyphs[i].g;
    return blank;
}
// 画一行字：每个亮像素画成 sc×sc 的方块，字符间留一列。越界自动裁掉（不写出去）。
// Draw one line of text: every lit pixel becomes an sc x sc square, one column between glyphs. Out-of-range pixels are
// dropped (nothing written outside the buffer).
static void hud_text(uint8_t *b, uint32_t w, uint32_t h, int x, int y, int sc, const char *s, uint8_t r, uint8_t g, uint8_t bl) {
    for (; *s; s++) {
        const uint8_t *gl = hud_glyph(*s);
        for (int row = 0; row < 7; row++) for (int col = 0; col < 5; col++) {
            if (!(gl[row] & (1u << (4 - col)))) continue;
            for (int dy = 0; dy < sc; dy++) for (int dx = 0; dx < sc; dx++) {
                int px = x + col * sc + dx, py = y + row * sc + dy;
                if (px < 0 || py < 0 || px >= (int)w || py >= (int)h) continue;
                uint8_t *p = b + ((size_t)py * w + (unsigned)px) * 4;
                p[0] = r; p[1] = g; p[2] = bl; p[3] = 255;
            }
        }
        x += 6 * sc;
    }
}

static void hud_overlay(uint8_t *b, uint32_t w, uint32_t h) {
    if (!b || w < 64 || h < 96) return;
    uint32_t s = w / 22;
    if (s < 10) s = 10;
    uint32_t x0 = w - s - s / 2, y0 = h - s - s * 3;   // 右下角，两块竖排 | bottom right, two stacked squares
    uint8_t st[3];
    if (g_hb_stall_s == 0) { st[0] = 40; st[1] = 220; st[2] = 60; }
    else if (g_hb_stall_s < 8) { st[0] = 240; st[1] = 200; st[2] = 40; }
    else { st[0] = 255; st[1] = 40; st[2] = 40; }
    uint8_t hb = g_hb_blink ? 250 : 30;
    for (uint32_t y = 0; y < s; y++) for (uint32_t x = 0; x < s; x++) {
        uint8_t *p1 = b + ((size_t)(y0 + y) * w + x0 + x) * 4;
        p1[0] = p1[1] = p1[2] = hb; p1[3] = 255;
        uint8_t *p2 = b + ((size_t)(y0 + s * 2 + y) * w + x0 + x) * 4;
        p2[0] = st[0]; p2[1] = st[1]; p2[2] = st[2]; p2[3] = 255;
    }
    // M3-2w：那一行可读数字（缺省关）—— 左上角，与右下角的方块、屏幕下方那个按钮都不打架。
    // M3-2w: the readout line (off by default) -- top left, clear of the squares at bottom right and the button near
    // the bottom of the screen.
    if (g_hud_text) {
        char line[160];
        snprintf(line, sizeof line, "T+%.0fS F=%llu P=%u S=%.20s V=%llu/%llu",
                 g_hud.t, (unsigned long long)g_hud.present, g_hud.tick, g_hud.stage,
                 (unsigned long long)g_hud.vok, (unsigned long long)g_hud.vskip);
        int sc = (int)(w / 800);   // 1600 宽 → 2（一行约 600 像素）| 1600 wide -> 2 (a line is about 600 px)
        if (sc < 1) sc = 1;
        if (sc > 3) sc = 3;
        uint8_t r = 250, g = 250, bl = 250;
        if (g_hb_stall_s >= 8) { r = 255; g = 60; bl = 60; }        // 冻住：定格 + 红字 | frozen: held, and red
        else if (g_hb_stall_s) { r = 250; g = 210; bl = 60; }
        hud_text(b, w, h, (int)(w / 64), (int)(h / 64), sc, line, r, g, bl);
    }
}

static uint64_t g_src_ser;
static unsigned g_src_lv;   // 读第几级 mip：真机的渲染区告警"1600×900 对 800×450"说明游戏是**通过 view 往某一级**画 | which mip level to read: the device's render-area warning "1600x900 vs 800x450" says the game draws **into a level** through a view
static uint8_t *g_src_buf;
static uint32_t g_src_w, g_src_h;
static uint64_t g_view_n, g_view_ok, g_view_skip;
static int g_view_said_fail;

// 抓一帧并上屏；成功返回 1。失败时回落到测试图，手机屏上仍然有东西（不会变成"全黑所以看不出是坏了还是没东西"）。
// Grab one frame and present it, returning 1 on success. On failure it falls back to the test pattern so the screen
// still shows something (rather than going all black, which cannot be told apart from "broken" or "empty").
static int viewer_frame(double t) {
    if (!g_src_ser) return 0;
    g_view_n++;
    // 尺寸先问一次（贴图层的入口里有），再按它准备缓冲：源尺寸变了就换一块
    // Ask the size first (the texture layer's entry has it) and size the buffer to it: replaced when the source changes
    uint32_t need_w = 0, need_h = 0;
    if (!g_src_buf) {
        static uint64_t ser[64]; static uint32_t ws[64], hs[64], lvs[64];
        int n = vk_tex_list(ser, ws, hs, lvs, 64);
        for (int i = 0; i < n; i++) if (ser[i] == g_src_ser) { need_w = ws[i]; need_h = hs[i]; break; }
        if (!need_w) need_w = IMG_W, need_h = IMG_H;
        // 缓冲与行距都按**要读的那一级**的尺寸。读第 1 级却按底图给行距，读回来的行距与实际不符 ——
        // 亮度统计与上屏都把错位的行当成连续像素，看上去像画面被斜切（"读到了内容"那条读数因此也不可信）。
        // Buffer and pitch both follow **the level being read**. Handing over a base-sized pitch while reading level 1
        // makes the rows come back with a stride that does not match -- the luminance loop and the present both then
        // treat shifted rows as contiguous pixels, which looks like a sheared picture (and makes "it read content" an
        // untrustworthy reading too).
        if (g_src_lv < 31) { need_w >>= g_src_lv; need_h >>= g_src_lv; }
        if (!need_w) need_w = 1;
        if (!need_h) need_h = 1;
        g_src_buf = malloc((size_t)need_w * need_h * 4);
        if (!g_src_buf) { LOGE("查看器：%ux%u（第 %u 级）的缓冲分配不出", need_w, need_h, g_src_lv); g_src_ser = 0; return 0; }
        g_src_w = need_w; g_src_h = need_h;
        LOGI("贴图查看器：源序号 %llu 第 %u 级（%ux%u）", (unsigned long long)g_src_ser, g_src_lv, g_src_w, g_src_h);
    }
    uint32_t gw = 0, gh = 0;
    if (!vk_tex_grab_ser(g_src_ser, g_src_lv, g_src_buf, (size_t)g_src_w * 4, &gw, &gh)) {
        g_view_skip++;
        if (!g_view_said_fail++) LOGE("查看器：序号 %llu 读不回来（先回落到测试图）", (unsigned long long)g_src_ser);
        return 0;
    }
    g_view_ok++;
    // 每约 2 秒报一行亮度统计：白 / 黑 / 有内容从此是日志里的数（不必把图拉下来才知道）
    // About once every two seconds, one luminance line: white / black / has content become numbers in the log (no need
    // to pull the file down to find out)
    if (g_view_ok % 60 == 1) {
        uint64_t sum = 0; uint8_t lo = 255, hi = 0;
        size_t np = (size_t)gw * gh;
        for (size_t i = 0; i < np; i++) {
            uint8_t l = (uint8_t)(((unsigned)g_src_buf[i * 4] + g_src_buf[i * 4 + 1] + g_src_buf[i * 4 + 2]) / 3);
            sum += l;
            if (l < lo) lo = l;
            if (l > hi) hi = l;
        }
        LOGI("查看器：序号 %llu 第 %u 级 %ux%u 第 %llu 次，亮度 均值 %.1f、最小 %u、最大 %u（读成功 %llu、跳过 %llu）",
             (unsigned long long)g_src_ser, g_src_lv, gw, gh, (unsigned long long)g_view_ok,
             np ? (double)sum / (double)np : 0.0, lo, hi, (unsigned long long)g_view_ok, (unsigned long long)g_view_skip);
    }
    g_hb_blink ^= 1;
    hud_overlay(g_src_buf, gw, gh);
    return vk_win_present_rgba(g_src_buf, gw, gh);
}

// 画一帧：底色缓慢变色 + 一根横向滑动的白条 + （未授权时）橙块。全在主机内存里画，然后交给 vk_win 上屏。
// One frame: a slowly shifting background, a white bar sliding sideways and (while not granted) the orange block. All
// painted in host memory and then handed to vk_win for presentation.
static void paint(double t) {
    float hue = (float)(t * 0.15 - (long)(t * 0.15));
    uint8_t br = (uint8_t)(40 + 70 * hue), bg_ = (uint8_t)(34 - 8 * hue), bb = (uint8_t)(64 - 30 * hue);
    for (size_t i = 0; i < (size_t)IMG_W * IMG_H; i++) {
        g_img[i * 4 + 0] = br; g_img[i * 4 + 1] = bg_; g_img[i * 4 + 2] = bb; g_img[i * 4 + 3] = 0xFF;
    }
    int32_t bw = IMG_W / 12, bh = IMG_H / 5, span = IMG_W - bw * 2;
    int32_t ph = span > 0 ? (int32_t)(t * 220.0) % (span * 2) : 0;
    if (ph > span) ph = span * 2 - ph;
    int32_t bx = bw + (ph > 0 ? ph : 0), by = IMG_H * 3 / 10;
    for (int32_t y = by; y < by + bh && y < IMG_H; y++)
        for (int32_t x = bx; x < bx + bw && x < IMG_W; x++) {
            uint8_t *p = g_img + ((size_t)y * IMG_W + x) * 4;
            p[0] = p[1] = p[2] = 0xFF; p[3] = 0xFF;
        }
    if (!g_perm_files) {
        for (int32_t y = g_btn_y; y < g_btn_y + g_btn_h && y < IMG_H; y++)
            for (int32_t x = g_btn_x; x < g_btn_x + g_btn_w && x < IMG_W; x++) {
                uint8_t *p = g_img + ((size_t)y * IMG_W + x) * 4;
                p[0] = 0xFF; p[1] = 0x8C; p[2] = 0x00; p[3] = 0xFF;   // 橙 | orange
            }
    }
}

static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (!app->window) break;
            // 顺序不能动：窗口先告诉设备（它决定实例/设备的扩展与队列族），设备建好，再建交换链
            // The order matters: the window goes to the device first (it decides the instance and device extensions and
            // the queue family), then the device exists, then the swapchain
            vk_device_set_window(app->window);
            vk_device_init();
            vk_device_make_surface();
            if (!vk_win_boot()) LOGE("窗口那一套建不起来（见上面的日志）");
            else LOGI("窗口就绪：手机上应该能看到底色在变色、白条在滑动");
            // 窗口好之后才起游戏（顺序见上面 guest_thread 的说明），而且只起一次
            // The game starts only after the window is up (the order is explained above guest_thread) and only once
            if (!g_guest_started) {
                g_guest_started = 1;
                // 环境按量出来的结论设：根**原地读**（9.1 GB 不用搬）；存档与临时用 app 的**内部**目录。
                // 为什么不用 /sdcard/Android/data/<包>/files：真机上客体在那里 **mkdir 报 errno=13（EACCES）**
                // （`fs mkdir .../temp/Diablo II 失败 errno=13`），随后就卡在客体初始化里；内部目录是这个 uid
                // 自己的，native 侧读写一律放行（外部那个是 FUSE，还多一层包作用域的门）。
                // The environment follows the measurements: the root is **read in place** (no 9.1 GB copy) and saves and
                // temporary files use the app's **internal** directory. Why not /sdcard/Android/data/<pkg>/files: on the
                // device the guest's **mkdir there reports errno=13 (EACCES)** (`fs mkdir .../temp/Diablo II failed
                // errno=13`) and it then stalls inside guest initialisation; the internal directory belongs to this uid,
                // so native reads and writes are allowed outright (the external one is FUSE with a package-scope door on
                // top).
                const char *in = (app->activity && app->activity->internalDataPath) ? app->activity->internalDataPath
                                                                                   : "/data/data/ai.d2rm.dm/files";
                static char p_save[512], p_temp[512];
                snprintf(p_save, sizeof p_save, "%s/save", in);
                snprintf(p_temp, sizeof p_temp, "%s/temp", in);
                int m1 = mkdir(p_save, 0777), m2 = mkdir(p_temp, 0777);
                LOGI("存档与临时：%s（mkdir %d/%s）、%s（mkdir %d/%s）", p_save, m1, m1 ? strerror(errno) : "好",
                     p_temp, m2, m2 ? strerror(errno) : "好");
                setenv("DM_ROOT", "/data/local/tmp/dm", 1);
                setenv("DM_SAVE", p_save, 1);
                setenv("DM_TEMP", p_temp, 1);
                pthread_t th;
                if (pthread_create(&th, NULL, guest_thread, NULL) == 0) pthread_detach(th);
                else LOGE("起不了游戏线程：%s", strerror(errno));
            }
            break;
        case APP_CMD_TERM_WINDOW:
            // 切后台：趁设备还在，把管线缓存写回（下次启动直接复用；不写就得等下一次切后台或退出）
            // Backgrounded: write the pipeline cache back while the device is still alive (the next start reuses it;
            // otherwise it waits for the next background or exit)
            vk_device_pcache_save("切后台 TERM_WINDOW");
            vk_win_shutdown();
            vk_device_drop_surface();
            LOGI("窗口那一套已拆（TERM_WINDOW）");
            break;
        case APP_CMD_DESTROY:
            vk_device_pcache_save("活动结束 DESTROY");
            LOGI("活动结束，退出");
            break;
        default:
            break;
    }
}

// 配置文件：`/sdcard/Android/data/<包>/files/dm.conf`，每行 `key=value`（`#` 开头当注释）。
// 为什么要有它：Android 上没法给 app 按次启动设环境变量（`am start` 不传 env），而"你观察、我改一个参数"这条回路
// 不该每次都要重新装机 —— 把参数放进一个 adb 就能推上去的文件里，推完重启 app 就生效。
// The configuration file: `/sdcard/Android/data/<pkg>/files/dm.conf`, one `key=value` per line (`#` starts a comment).
// Why it exists: on Android an app cannot be given environment variables per launch (`am start` does not pass env), and
// the "you watch, I change one parameter" loop should not need a reinstall every time -- so the parameters live in a file
// that adb can push, and restarting the app picks them up.
static void conf_load(void) {
    const char *p = "/sdcard/Android/data/ai.d2rm.dm/files/dm.conf";
    FILE *f = fopen(p, "rb");
    if (!f) { LOGI("没有配置文件 %s（用默认值）", p); return; }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        for (char *q = v; *q; q++) if (*q == '\n' || *q == '\r') { *q = 0; break; }
        setenv(s, v, 1);
        LOGI("配置：%s=%s", s, v);
        n++;
    }
    fclose(f);
    LOGI("配置文件 %s：%d 项已生效", p, n);
}

void android_main(struct android_app *app) {
    g_app = app;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    log_pipe_start();   // 先接日志：它之后的宿主输出全部进 logcat | route the log first: every host line after this reaches logcat
    LOGI("启动（NativeActivity + app_glue；上屏走宿主那一个设备）");
    conf_load();
    // 管线缓存落盘的位置：app 的**内部**目录（native 侧可写）。不指的话 vk_device.c 会退回 <DM_ROOT>/pipecache.bin，
    // 而 /data/local/tmp 对 APK 只读（SELinux：壳数据是 shell_data_file）——写在那儿只会静默失败，缓存等于没有。
    // 放在这里（而不是 on_cmd 里那段 setenv）是因为设备在 INIT_WINDOW 时就建了，那会儿缓存路径已经要定下来；
    // dm.conf 里若也写了 DM_PIPE_CACHE，以它为准（配置先加载）。
    // Where the pipeline cache blob goes: the app's **internal** directory (writable from native code). Without this
    // vk_device.c falls back to <DM_ROOT>/pipecache.bin, and /data/local/tmp is read-only for the APK (SELinux: shell
    // data is shell_data_file) -- writing there fails silently and there is effectively no cache. It is done here rather
    // than in the setenv block of on_cmd because the device is created at INIT_WINDOW, by which time the cache path must
    // already be settled; a DM_PIPE_CACHE in dm.conf wins (the config is loaded first).
    if (!getenv("DM_PIPE_CACHE")) {
        const char *in = (app->activity && app->activity->internalDataPath) ? app->activity->internalDataPath
                                                                           : "/data/data/ai.d2rm.dm/files";
        char pc[512];
        snprintf(pc, sizeof pc, "%s/pipecache.bin", in);
        setenv("DM_PIPE_CACHE", pc, 1);
        LOGI("管线缓存：%s（切后台与退出时写回）", pc);
    } else {
        LOGI("管线缓存：DM_PIPE_CACHE=%s（配置给的）", getenv("DM_PIPE_CACHE"));
    }
    // 启动头：时间、进程号、构建时间、以及真正生效的开关（"结论不该靠记忆"，这一行让每次日志自带参数）
    // The startup header: time, pid, build time and the switches actually in effect (a conclusion should not rest on
    // memory, so every log carries its own parameters)
    LOGI("=== 本次启动 pid %d，构建 %s %s ===", (int)getpid(), __DATE__, __TIME__);
    LOGI("关键开关：DM_WIN_SRC=%s DM_WIN_LEVEL=%s DM_GFX_PROBE=%s DM_NO_DEPTH_TEST=%s",
         getenv("DM_WIN_SRC") ? getenv("DM_WIN_SRC") : "(缺省 28)",
         getenv("DM_WIN_LEVEL") ? getenv("DM_WIN_LEVEL") : "(缺省 0)",
         getenv("DM_GFX_PROBE") ? getenv("DM_GFX_PROBE") : "(关)",
         getenv("DM_NO_DEPTH_TEST") ? getenv("DM_NO_DEPTH_TEST") : "(关)");
    probe_paths();
    g_img = malloc((size_t)IMG_W * IMG_H * 4);
    if (!g_img) { LOGE("测试图那块内存分配不出"); return; }
    // 查看器的源：默认序号 28 = 真机上用得最多的那张渲染目标（1600×900、RGBA16F、每帧 5400 次）；
    // DM_WIN_SRC=<序号> 可换，给 0 就关掉、回到测试图。
    // The viewer's source: serial 28 by default = the most-used render target on the device (1600x900, RGBA16F, 5400
    // uses per frame); DM_WIN_SRC=<serial> changes it and 0 turns it off, back to the test pattern.
    const char *src = getenv("DM_WIN_SRC");
    g_src_ser = src ? strtoull(src, NULL, 10) : 28;
    // 读哪一级：真机上两条"渲染区超出附件（1600×900 对 800×450）"的告警说明游戏是**通过 view 往某一级 mip 里画**，
    // 而查看器一直读第 0 级 —— 若画在第 1 级上，屏幕就永远是"合法的黑"，读错一级足以解释"目标全零"。
    // Which level: the device's two "render area exceeds the attachment (1600x900 vs 800x450)" warnings say the game
    // draws **into a mip level through a view**, while the viewer has always read level 0 -- if it draws into level 1 the
    // screen is a legitimate black forever, and reading the wrong level is enough to explain "the targets are all zero".
    const char *lv = getenv("DM_WIN_LEVEL");
    g_src_lv = lv ? (unsigned)strtoul(lv, NULL, 10) : 0;
    LOGI("贴图查看器的源序号 %llu、第 %u 级（DM_WIN_SRC / DM_WIN_LEVEL 可改，序号 0 = 关）",
         (unsigned long long)g_src_ser, g_src_lv);
    // M3-2w：给用户用的那两个口子，**缺省都关**（独立开关，不抢主线）：屏上一行可读数字、点左下角取现场。
    // M3-2w: the two user-facing affordances, **both off by default** (separate switches, they do not take over the main
    // line): the on-screen readout line and the bottom-left tap that takes a state dump.
    { const char *a = getenv("DM_HUD_TEXT"), *b = getenv("DM_HUD_TAP");
      g_hud_text = a ? atoi(a) != 0 : 0;
      g_hud_tap = b ? atoi(b) != 0 : 0;
      if (g_hud_text || g_hud_tap)
          LOGI("HUD：数字行 %d、点左下角取现场 %d（DM_HUD_TEXT / DM_HUD_TAP，缺省都关；量帧预算时别开）",
               g_hud_text, g_hud_tap); }
    // 按钮：画面 70% 宽、9% 高，横居中、贴底部 12% 处（**图像坐标**，触摸会换算到这里）
    // The button: 70% of the width and 9% of the height, centred, 12% up from the bottom (in **image** coordinates; a
    // touch is mapped into that space)
    g_btn_w = IMG_W * 7 / 10;
    g_btn_h = IMG_H * 9 / 100;
    g_btn_x = (IMG_W - g_btn_w) / 2;
    g_btn_y = IMG_H - g_btn_h - IMG_H * 12 / 100;

    for (;;) {
        int events = 0;
        struct android_poll_source *src = NULL;
        int timeout = vk_win_live() ? 0 : -1;   // 有交换链就非阻塞转圈出图，没有就阻塞等事件 | spin to present with a swapchain, otherwise block on events
        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) {
                vk_device_pcache_save("退出 destroyRequested");
                vk_win_shutdown();
                free(g_img);
                LOGI("退出");
                return;
            }
            timeout = vk_win_live() ? 0 : -1;
        }
        perm_tick(now_s());
        // 每秒一条心跳 + 卡死判定：进程被系统 SIGKILL 时，日志文件里这些行就是**唯一现场**（处理器来不及跑）
        // One heartbeat per second plus the stall judgement: when the system SIGKILLs the process these lines in the log
        // file are the **only** scene left (no handler ever runs).
        {
            double now = now_s();
            static double last_hb;
            if (now - last_hb >= 1.0) {
                last_hb = now;
                // 管线缓存定期写回（限频 10 秒、只在新建过管线时写）。**这条是主力**：MIUI 把本包当游戏，
                // 退到后台直接 SIGKILL，TERM_WINDOW / DESTROY 都送不到，只有前台这一刻能落盘。
                // Periodic write-back of the pipeline cache (at most once every 10 s, only when new pipelines
                // appeared). **This is the main path**: MIUI treats this package as a game and SIGKILLs it on
                // backgrounding, so TERM_WINDOW / DESTROY never arrive and only the foreground can reach disk.
                vk_device_pcache_tick();
                uint64_t p = vk_present_count();
                static char dl[640];
                vk_diag_line(dl, sizeof dl);
                if (p == g_hb_present) g_hb_stall_s++;
                else { g_hb_stall_s = 0; g_hb_present = p; }
                // M3-2w：屏上那行数字的快照（**冻住时不再更新 = 定格**，屏上留的就是冻住那一刻的数）。
                // 阶段串可能很长（`CharacterCreate:Hover:Barbarian`），HUD 那行只放得下前 20 个字符，截断即可。
                // M3-2w: the snapshot behind the on-screen line (**not updated while frozen = held**, so the screen keeps
                // the numbers from the moment it froze). The stage string can be long (CharacterCreate:Hover:Barbarian)
                // and the HUD line only fits 20 characters, so it is truncated there.
                if (g_hud_text && g_hb_stall_s < 8) {
                    const char *stg = crash_watch_stage();
                    g_hud.t = now; g_hud.present = p; g_hud.tick = dm_cur_tick();
                    snprintf(g_hud.stage, sizeof g_hud.stage, "%s", stg ? stg : "?");
                    g_hud.vok = g_view_ok; g_hud.vskip = g_view_skip;
                }
                LOGI("心跳：%s；查看器 读成 %llu、跳过 %llu；%s", dl, (unsigned long long)g_view_ok,
                     (unsigned long long)g_view_skip, g_hb_stall_s ? "出图没有推进" : "出图在推进");
                if (g_hb_stall_s == 8)
                    LOGE("卡死报告：连续 8 秒没有新出图（上面最后一条心跳即现场；客体线程现场见宿主的看门狗转储）");
                // 按需现场导出（**不退出**）：安卓侧原来没有触发口 —— iOS 有 DM_LOG_PORT，这里只有看门狗那条"导完就 _exit"
                // 的路，而"停住"是间歇的、要盯很久才出现，一跳就自杀会把观察打断。所以给一个文件触发口，每秒查一次：
                //   adb shell touch /sdcard/Android/data/ai.d2rm.dm/files/dump.trigger
                // 见到就删掉它、导一份（与看门狗同一份内容），进程继续跑；要连导几次就再建一次。
                // On-demand state dump (**without exiting**): Android had no trigger -- iOS has DM_LOG_PORT, and here the
                // only route was the watchdog's "dump then _exit", which cuts an observation short exactly when the
                // intermittent stall finally shows up. So: a file trigger, checked once a second (command above); when
                // seen it is removed and one dump is taken (the same content the watchdog prints) while the process
                // keeps running. Create the file again for another dump.
                { static const char *tp = "/sdcard/Android/data/ai.d2rm.dm/files/dump.trigger";
                  if (access(tp, F_OK) == 0) { remove(tp); crash_dump_now("按需触发 dump.trigger"); } }
                // M3-2w：屏幕左下角那一下是**同一个口**（见 on_input）——adb 掉线或画面冻住时，用户自己按一下就有现场。
                // 放在这里而不是 on_input 里：转储要几秒，别堵住输入线程。
                // M3-2w: the bottom-left tap is **the same port** (see on_input) -- when adb is gone or the picture has
                // frozen, one tap from the user leaves a scene behind. Handled here rather than inside on_input: a dump
                // takes seconds and must not block the input thread.
                if (g_hud_tap && g_tap_dump) { g_tap_dump = 0; crash_dump_now("点按屏幕左下角（HUD 取现场口）"); }
            }
        }
        if (vk_win_live()) {
            double t = now_s();
            // 先试贴图查看器（游戏自己那张渲染目标）；读不回来才回落到测试图 —— 于是"黑屏"与"坏了"分得开
            // Try the texture viewer first (one of the game's own render targets) and fall back to the test pattern only
            // when it cannot be read -- so "a black screen" and "broken" stay distinguishable
            if (!viewer_frame(t)) {
                paint(t);
                g_hb_blink ^= 1;
                hud_overlay(g_img, IMG_W, IMG_H);
                vk_win_present_rgba(g_img, IMG_W, IMG_H);
            }
        }
    }
}
