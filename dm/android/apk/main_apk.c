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
#include <jni.h>
#include <pthread.h>
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
int vk_tex_list(uint64_t *ser, uint32_t *ws, uint32_t *hs, int cap);

// 宿主那套的环境准备与 dm_main 的交接（dm/android/main_android.c）：**与 APK 必须完全一致**，所以那边只留一个实现，
// 这里直接调它，而不是把环境准备抄一遍（抄一遍就会漂）。
// The host's environment setup and hand-over to dm_main (dm/android/main_android.c): it **has to be identical for the
// APK**, so that file keeps one implementation and this one calls it rather than copying the setup (a copy would drift).
int dm_android_main(int argc, char **argv);

#define TAG "dmwin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

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
                if (n) __android_log_print(ANDROID_LOG_INFO, "dmhost", "%s", buf);
                n = 0;
            } else buf[n++] = c;
        }
    }
    if (n) { buf[n] = 0; __android_log_print(ANDROID_LOG_INFO, "dmhost", "%s", buf); }
    return NULL;
}

static void log_pipe_start(void) {
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
    if (g_perm_files) return 0;
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;
    if ((AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK) != AMOTION_EVENT_ACTION_DOWN) return 0;
    int ix = 0, iy = 0;
    if (!vk_win_window_to_src(AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0), IMG_W, IMG_H, &ix, &iy)) return 0;
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
static uint64_t g_src_ser;
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
        static uint64_t ser[64]; static uint32_t ws[64], hs[64];
        int n = vk_tex_list(ser, ws, hs, 64);
        for (int i = 0; i < n; i++) if (ser[i] == g_src_ser) { need_w = ws[i]; need_h = hs[i]; break; }
        if (!need_w) need_w = IMG_W, need_h = IMG_H;
        g_src_buf = malloc((size_t)need_w * need_h * 4);
        if (!g_src_buf) { LOGE("查看器：%ux%u 的缓冲分配不出", need_w, need_h); g_src_ser = 0; return 0; }
        g_src_w = need_w; g_src_h = need_h;
        LOGI("贴图查看器：源序号 %llu（%ux%u）", (unsigned long long)g_src_ser, g_src_w, g_src_h);
    }
    uint32_t gw = 0, gh = 0;
    if (!vk_tex_grab_ser(g_src_ser, 0, g_src_buf, (size_t)g_src_w * 4, &gw, &gh)) {
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
        LOGI("查看器：序号 %llu %ux%u 第 %llu 次，亮度 均值 %.1f、最小 %u、最大 %u（读成功 %llu、跳过 %llu）",
             (unsigned long long)g_src_ser, gw, gh, (unsigned long long)g_view_ok, np ? (double)sum / (double)np : 0.0,
             lo, hi, (unsigned long long)g_view_ok, (unsigned long long)g_view_skip);
    }
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
            vk_win_shutdown();
            vk_device_drop_surface();
            LOGI("窗口那一套已拆（TERM_WINDOW）");
            break;
        case APP_CMD_DESTROY:
            LOGI("活动结束，退出");
            break;
        default:
            break;
    }
}

void android_main(struct android_app *app) {
    g_app = app;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    log_pipe_start();   // 先接日志：它之后的宿主输出全部进 logcat | route the log first: every host line after this reaches logcat
    LOGI("启动（NativeActivity + app_glue；上屏走宿主那一个设备）");
    probe_paths();
    g_img = malloc((size_t)IMG_W * IMG_H * 4);
    if (!g_img) { LOGE("测试图那块内存分配不出"); return; }
    // 查看器的源：默认序号 28 = 真机上用得最多的那张渲染目标（1600×900、RGBA16F、每帧 5400 次）；
    // DM_WIN_SRC=<序号> 可换，给 0 就关掉、回到测试图。
    // The viewer's source: serial 28 by default = the most-used render target on the device (1600x900, RGBA16F, 5400
    // uses per frame); DM_WIN_SRC=<serial> changes it and 0 turns it off, back to the test pattern.
    const char *src = getenv("DM_WIN_SRC");
    g_src_ser = src ? strtoull(src, NULL, 10) : 28;
    LOGI("贴图查看器的源序号 %llu（DM_WIN_SRC 可改，0 = 关）", (unsigned long long)g_src_ser);
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
                vk_win_shutdown();
                free(g_img);
                LOGI("退出");
                return;
            }
            timeout = vk_win_live() ? 0 : -1;
        }
        perm_tick(now_s());
        if (vk_win_live()) {
            double t = now_s();
            // 先试贴图查看器（游戏自己那张渲染目标）；读不回来才回落到测试图 —— 于是"黑屏"与"坏了"分得开
            // Try the texture viewer first (one of the game's own render targets) and fall back to the test pattern only
            // when it cannot be read -- so "a black screen" and "broken" stay distinguishable
            if (!viewer_frame(t)) { paint(t); vk_win_present_rgba(g_img, IMG_W, IMG_H); }
        }
    }
}
