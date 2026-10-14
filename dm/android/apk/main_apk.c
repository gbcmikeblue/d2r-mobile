// 安卓窗口最小实现（M3 的第一块）：一个能装、能跑、**手机上能看到东西**的 APK。
// Android window, minimal (the first piece of M3): an APK that installs, runs and **shows something on the phone**.
//
// 为什么先做它：M2 的验收是"屏幕出画面"，而控制台宿主（dmhost）没有窗口，手机屏上什么都看不到 —— 而且我们量过，
// 就算现在把窗口接上游戏，要上屏的那张纹理也是全白（第 14 步）。所以这一步的目标很窄、也很确定：
// 把 **APK → ANativeWindow → VkSurface → 交换链 → 上屏** 这条链打通，画面先是一张**会动的测试图**：
// 底色缓慢变色 + 一根横向滑动的白条。**它必须动** —— 静止的一张图分不清"上屏通了"与"画面卡住了"。
// Why this first: M2's acceptance is "a picture appears", and the console host (dmhost) has no window, so the phone
// screen shows nothing at all -- and we measured that even wiring the game to a window today would present an all-white
// texture (step 14). So this step aims narrow and certain: get **APK -> ANativeWindow -> VkSurface -> swapchain ->
// present** working, with a **moving test pattern** as the picture: a slowly shifting background plus a white bar
// sliding sideways. **It has to move** -- a still image cannot tell "present works" from "the picture is stuck".
//
// 与游戏的关系：这一版**不跑游戏**，只证明窗口那条链。游戏进 APK 与"贴图查看器"是下一步（那时 `front_run` 会把
// 客体放到工作线程上，本文件的循环留在主线程上报屏）。
// Relation to the game: this version does **not** run the game; it only proves the window chain. Putting the game into
// the APK and turning the window into a texture viewer is the next step (then `front_run` moves the guest onto a worker
// thread and this file's loop stays on the main thread to present).
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <android_native_app_glue.h>
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TAG "dmwin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static VkInstance g_inst;
static VkSurfaceKHR g_surf;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfam;
static VkSwapchainKHR g_sc;
static VkFormat g_fmt;
static VkExtent2D g_ext;
static uint32_t g_nimg;
static VkImage g_img[8];
static VkImageView g_view[8];
static VkCommandPool g_cpool;
static VkCommandBuffer g_cb;
static VkSemaphore g_sem_acq, g_sem_rdy;
static VkFence g_fence;
static int g_built;
// 滑条：一块全是 0xFF 的宿主可见缓冲，按帧拷进画面的一块矩形。白色对字节序不敏感（RGBA/BGRA 都是 0xFF），
// 所以不必按交换链格式分别填。| The bar: a host-visible buffer full of 0xFF, copied into a rectangle of the picture each
// frame. White is byte-order agnostic (0xFF in both RGBA and BGRA), so it needs no per-format filling.
static VkBuffer g_bar_buf;
static VkDeviceMemory g_bar_mem;
static uint32_t g_bar_w, g_bar_h;

// ---------- 权限：什么时候要用户点一下，以及那个按钮 ----------
// ---------- Permissions: when the user has to tap something, and that button ----------
// 整个包只申请两条权限（见 AndroidManifest.xml），其中只有一条是"特殊"的：MANAGE_EXTERNAL_STORAGE（所有文件访问），
// 必须在系统设置里点开 —— 窗口里那块**橙色矩形**就是给它的按钮，而**按钮的有与无本身就是权限状态**（有按钮 = 还没
// 授权），所以这一版不需要画一个字。它买到的是"数据放在用户看得见、随时能用文件管理器替换的地方"（/sdcard/d2rm，
// 入口里已经写好的备用路径）；它**买不到** /data/local/tmp 的写权限 —— 那里是 SELinux 的 shell_data_file，任何
// 权限都到不了（真机实测：授权后写入仍 errno 13）。
// The package requests just two permissions (see AndroidManifest.xml) and only one of them is "special":
// MANAGE_EXTERNAL_STORAGE (all files access), which has to be switched on in system settings. The **orange rectangle**
// in the window is its button, and **the button's presence or absence is itself the permission state** (a button means
// not granted), so nothing has to be drawn in words. What it buys is "data in a place the user can see and replace with
// a file manager at any time" (/sdcard/d2rm, the fallback path the entry already knows); what it does **not** buy is
// write access to /data/local/tmp -- that is SELinux's shell_data_file and no permission reaches it (measured on the
// device: after the grant the write still fails with errno 13).
#include <jni.h>

static int g_perm_files;              // "所有文件访问"是否已授予 | whether all files access is granted
static VkBuffer g_btn_buf;            // 按钮那块颜色缓冲（橙）| the button's colour buffer (orange)
static VkDeviceMemory g_btn_mem;
static int32_t g_btn_x, g_btn_y, g_btn_w, g_btn_h;   // 按钮的矩形（画面像素坐标，与触摸坐标同一个空间）| the button's rectangle in picture pixels, the same space as touch coordinates
static struct android_app *g_app;      // 输入回调与权限查询要它 | needed by the input callback and the permission query

// 取当前线程的 JNI 环境：app_glue 的主线程本来就附着在 JVM 上，所以通常一次 GetEnv 就够
// Get this thread's JNI environment: the app_glue main thread is already attached to the JVM, so one GetEnv normally
// suffices
static JNIEnv *jnienv(void) {
    if (!g_app || !g_app->activity || !g_app->activity->vm) return NULL;
    JavaVM *vm = g_app->activity->vm;
    JNIEnv *e = NULL;
    if ((*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_6) == JNI_OK) return e;
    if ((*vm)->AttachCurrentThread(vm, &e, NULL) == JNI_OK) return e;
    return NULL;
}

static void jni_swallow(JNIEnv *e) { if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e); }

// 查"所有文件访问"是否已授予（API 30+ 的 Environment.isExternalStorageManager）；-1 = 查不了
// Query whether all files access is granted (Environment.isExternalStorageManager, API 30+); -1 means it cannot be
// queried
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

// 打开"所有文件访问"的授权页：先试**本应用**那一页（在那里能直接授权），打不开再退到全局列表页
// Open the "all files access" page: try **this app's** own page first (where it can be granted directly) and fall back
// to the global list page if that one cannot be opened
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

// 每秒（以及刚返回时）刷一次权限状态：改变了就报一行 —— 手机上那块橙块该出现/该消失也就是这么定的
// Refresh the permission state about once a second (and right after returning): a change logs one line, and that is
// also what decides whether the orange button should appear or disappear on the phone
static void perm_tick(double t) {
    static double next;
    static int said_first;
    if (t < next) return;
    next = t + 1.0;
    int v = perm_query();
    if (v < 0) {
        if (!said_first) { said_first = 1; LOGE("查不到「所有文件访问」的状态（JNI 那一步没走通）"); }
        return;                              // 查不了就别下结论 | do not conclude anything when it cannot be queried
    }
    if (!said_first || v != g_perm_files) {
        // 第一次也报：只报变化的话，"一条都不打"到底是"已授权"还是"查不了"就分不出来（踩过一次）
        // Log the first one as well: with only changes logged, "nothing printed" cannot be told apart from "granted"
        // or "cannot be queried" (learned the hard way)
        said_first = 1;
        g_perm_files = v;
        LOGI("「所有文件访问」%s（%s）", v ? "已授予" : "未授予",
             v ? "数据可以放到 /sdcard/d2rm 让用户自己看得见" : "橙块就是申请按钮，点它打开系统设置页");
    }
}

// 触摸：点在橙块里就打开授权页（没授权时橙块在，授权后它自己消失，所以不会误触）
// Touch: a tap inside the orange block opens the grant page (the block is there only while it is not granted, so it
// cannot be tapped by accident afterwards)
static int32_t on_input(struct android_app *app, AInputEvent *ev) {
    (void)app;
    if (g_perm_files || !g_btn_buf) return 0;
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;
    int32_t act = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
    if (act != AMOTION_EVENT_ACTION_DOWN) return 0;
    float x = AMotionEvent_getX(ev, 0), y = AMotionEvent_getY(ev, 0);
    if (x >= (float)g_btn_x && x < (float)(g_btn_x + g_btn_w) &&
        y >= (float)g_btn_y && y < (float)(g_btn_y + g_btn_h)) {
        LOGI("点了授权按钮（%.0f,%.0f）", x, y);
        perm_open_settings();
        return 1;
    }
    return 0;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// 建一块"整块一个颜色"的宿主可见缓冲（滑条与按钮都用它）：往里拷就能在画面上画出一块矩形，不需要渲染通道。
// Make a host-visible buffer of one solid colour (used by both the bar and the button): copying from it paints a
// rectangle into the picture without needing a render pass.
static int mk_solid(VkDeviceSize sz, const uint8_t pix[4], VkBuffer *out_b, VkDeviceMemory *out_m) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(g_dev, &bci, NULL, out_b) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_dev, *out_b, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    uint32_t mi = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mi = i; break; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mi };
    if (vkAllocateMemory(g_dev, &mai, NULL, out_m) != VK_SUCCESS || vkBindBufferMemory(g_dev, *out_b, *out_m, 0) != VK_SUCCESS) {
        if (*out_m) { vkFreeMemory(g_dev, *out_m, NULL); *out_m = VK_NULL_HANDLE; }
        vkDestroyBuffer(g_dev, *out_b, NULL);
        *out_b = VK_NULL_HANDLE;
        return 0;
    }
    void *p = NULL;
    if (vkMapMemory(g_dev, *out_m, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 0;
    for (VkDeviceSize i = 0; i < sz; i += 4) memcpy((uint8_t *)p + i, pix, 4);
    vkUnmapMemory(g_dev, *out_m);
    return 1;
}

// 等设备空闲并把窗口那一套拆掉（TERM_WINDOW / 退出时）| drain the device and tear the window chain down (on TERM_WINDOW / exit)
static void win_teardown(void) {
    if (!g_dev) return;
    vkDeviceWaitIdle(g_dev);
    if (g_bar_buf) { vkDestroyBuffer(g_dev, g_bar_buf, NULL); g_bar_buf = VK_NULL_HANDLE; }
    if (g_bar_mem) { vkFreeMemory(g_dev, g_bar_mem, NULL); g_bar_mem = VK_NULL_HANDLE; }
    if (g_btn_buf) { vkDestroyBuffer(g_dev, g_btn_buf, NULL); g_btn_buf = VK_NULL_HANDLE; }
    if (g_btn_mem) { vkFreeMemory(g_dev, g_btn_mem, NULL); g_btn_mem = VK_NULL_HANDLE; }
    g_bar_w = g_bar_h = 0;
    for (uint32_t i = 0; i < g_nimg; i++) if (g_view[i]) vkDestroyImageView(g_dev, g_view[i], NULL);
    g_nimg = 0;
    if (g_sc) { vkDestroySwapchainKHR(g_dev, g_sc, NULL); g_sc = VK_NULL_HANDLE; }
    if (g_sem_acq) { vkDestroySemaphore(g_dev, g_sem_acq, NULL); g_sem_acq = VK_NULL_HANDLE; }
    if (g_sem_rdy) { vkDestroySemaphore(g_dev, g_sem_rdy, NULL); g_sem_rdy = VK_NULL_HANDLE; }
    if (g_fence) { vkDestroyFence(g_dev, g_fence, NULL); g_fence = VK_NULL_HANDLE; }
    if (g_cpool) { vkDestroyCommandPool(g_dev, g_cpool, NULL); g_cpool = VK_NULL_HANDLE; }
    if (g_surf) { vkDestroySurfaceKHR(g_inst, g_surf, NULL); g_surf = VK_NULL_HANDLE; }
    g_built = 0;
    LOGI("窗口那一套已拆（TERM_WINDOW / 退出）");
}

// 交换链：格式优先要 8 位 RGBA（与我们的读回同一种），呈现模式取 FIFO（有节流、不会空转烧机）
// Swapchain: prefer an 8-bit RGBA format (the same kind our readback produces) and FIFO present mode (throttles, so it
// does not spin and burn the battery)
static int sc_build(void) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surf, &caps) != VK_SUCCESS) return 0;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surf, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (!nf || nf > 64) return 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surf, &nf, fmts);
    g_fmt = fmts[0].format;
    VkColorSpaceKHR cs = fmts[0].colorSpace;
    for (uint32_t i = 0; i < nf; i++)
        if ((fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { g_fmt = fmts[i].format; cs = fmts[i].colorSpace; break; }
    g_ext = caps.currentExtent;
    if (g_ext.width == 0xffffffffu) { g_ext = caps.minImageExtent; }   // 有些实现让应用自己定尺寸 | some implementations let the app choose
    uint32_t n = caps.minImageCount + 1;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    if (n > 8) n = 8;
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g_surf, .minImageCount = n, .imageFormat = g_fmt, .imageColorSpace = cs,
        .imageExtent = g_ext, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE, .oldSwapchain = VK_NULL_HANDLE };
    if (vkCreateSwapchainKHR(g_dev, &ci, NULL, &g_sc) != VK_SUCCESS) return 0;
    if (vkGetSwapchainImagesKHR(g_dev, g_sc, &g_nimg, NULL) != VK_SUCCESS) return 0;
    if (g_nimg > 8) g_nimg = 8;
    vkGetSwapchainImagesKHR(g_dev, g_sc, &g_nimg, g_img);
    for (uint32_t i = 0; i < g_nimg; i++) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_img[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g_fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (vkCreateImageView(g_dev, &vi, NULL, &g_view[i]) != VK_SUCCESS) return 0;
    }
    // 滑条与"授权按钮"：都只在 8 位四通道的交换链格式下做（拷贝要求每像素字节数一致；别的格式就只放动画底色，
    // 仍能证明"活着"）。白色对字节序不敏感；橙色不是，所以按格式定字节序。
    // The bar and the permission button exist only for 8-bit four-channel swapchain formats (a copy needs matching texel
    // sizes; other formats fall back to the animated background alone, which still proves the chain is alive). White is
    // byte-order agnostic; orange is not, so the byte order is taken from the format.
    int rgba8 = g_fmt == VK_FORMAT_R8G8B8A8_UNORM || g_fmt == VK_FORMAT_R8G8B8A8_SRGB;
    int bgra8 = g_fmt == VK_FORMAT_B8G8R8A8_UNORM || g_fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (rgba8 || bgra8) {
        const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        const uint8_t orange[4] = { rgba8 ? 0xFF : 0x00, 0x8C, rgba8 ? 0x00 : 0xFF, 0xFF };
        g_bar_w = g_ext.width / 12 ? g_ext.width / 12 : 1;
        g_bar_h = g_ext.height / 5 ? g_ext.height / 5 : 1;
        mk_solid((VkDeviceSize)g_bar_w * g_bar_h * 4, white, &g_bar_buf, &g_bar_mem);
        // 按钮：画面 70% 宽、9% 高，横居中、贴底部 12% 处（够大，手指点得中）
        // Button: 70% of the width and 9% of the height, centred and 12% up from the bottom (big enough to hit with a finger)
        g_btn_w = (int32_t)g_ext.width * 7 / 10;
        g_btn_h = (int32_t)g_ext.height * 9 / 100;
        g_btn_x = ((int32_t)g_ext.width - g_btn_w) / 2;
        g_btn_y = (int32_t)g_ext.height - g_btn_h - (int32_t)g_ext.height * 12 / 100;
        mk_solid((VkDeviceSize)g_btn_w * g_btn_h * 4, orange, &g_btn_buf, &g_btn_mem);
    }
    LOGI("交换链就绪：%ux%u 格式 %d、%u 张、FIFO；滑条 %ux%u；授权按钮 %dx%d@%d,%d", g_ext.width, g_ext.height, (int)g_fmt,
         g_nimg, g_bar_w, g_bar_h, g_btn_w, g_btn_h, g_btn_x, g_btn_y);
    return 1;
}

// 建实例 / surface / 设备 / 同步对象，然后建交换链（只做一次；TERM_WINDOW 后整段重来）
// Create instance / surface / device / sync objects, then the swapchain (once; after TERM_WINDOW the whole thing runs
// again from the top)
static int win_boot(struct android_app *app) {
    const char *iext[] = { "VK_KHR_surface", "VK_KHR_android_surface" };
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "d2r-port",
        .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext };
    if (!g_inst && vkCreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) { LOGE("建 Vulkan 实例失败"); return 0; }
    VkAndroidSurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = app->window };
    if (vkCreateAndroidSurfaceKHR(g_inst, &sci, NULL, &g_surf) != VK_SUCCESS) { LOGE("由 ANativeWindow 建 surface 失败"); return 0; }

    if (!g_phys) {
        uint32_t n = 1;
        vkEnumeratePhysicalDevices(g_inst, &n, &g_phys);
        if (!g_phys) { LOGE("没有 Vulkan 物理设备"); return 0; }
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g_phys, &p);
        LOGI("设备：%s（Vulkan %u.%u.%u）", p.deviceName, VK_VERSION_MAJOR(p.apiVersion),
             VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));

        // 队列族：要一个既能画又能上屏的（Adreno 上是同一个，取不到就退成"两个族共享"那条路，这里先只做前者）
        // Queue family: one that can both draw and present (the same one on Adreno; if none, the two-family sharing path
        // would be needed, which this version deliberately does not do yet)
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, NULL);
        VkQueueFamilyProperties qf[16];
        if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, qf);
        g_qfam = 0xffffffffu;
        for (uint32_t i = 0; i < nq; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g_phys, i, g_surf, &present);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { g_qfam = i; break; }
        }
        if (g_qfam == 0xffffffffu) { LOGE("找不到既能画又能上屏的队列族"); return 0; }
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_qfam,
            .queueCount = 1, .pQueuePriorities = &prio };
        const char *dext[] = { "VK_KHR_swapchain" };
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
        if (vkCreateDevice(g_phys, &dci, NULL, &g_dev) != VK_SUCCESS) { LOGE("建设备失败"); return 0; }
        vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    }
    if (!g_built) {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qfam };
        if (vkCreateCommandPool(g_dev, &pci, NULL, &g_cpool) != VK_SUCCESS) return 0;
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g_cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        if (vkAllocateCommandBuffers(g_dev, &cai, &g_cb) != VK_SUCCESS) return 0;
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        if (vkCreateSemaphore(g_dev, &si, NULL, &g_sem_acq) != VK_SUCCESS) return 0;
        if (vkCreateSemaphore(g_dev, &si, NULL, &g_sem_rdy) != VK_SUCCESS) return 0;
        if (vkCreateFence(g_dev, &fi, NULL, &g_fence) != VK_SUCCESS) return 0;
        g_built = 1;
    }
    if (!g_sc && !sc_build()) return 0;
    return 1;
}

// 一帧：底色缓慢变色 + 一根横向滑动的白条。两次 clear（整幅 + 一条）省掉渲染通道，够用且没有管线要建。
// One frame: a slowly shifting background plus a white bar sliding sideways. Two clears (the whole image, then a bar)
// avoid a render pass, which is enough and needs no pipeline.
static void frame(void) {
    vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(g_dev, g_sc, UINT64_MAX, g_sem_acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { win_teardown(); return; }
    if (r != VK_SUCCESS) return;

    double t = now_s();
    float hue = (float)(t * 0.15 - (long)(t * 0.15));          // 0..1 循环，慢 | a slow 0..1 cycle
    float bg[4] = { 0.10f + 0.25f * hue, 0.12f - 0.06f * hue, 0.25f - 0.15f * hue, 1.0f };
    // 滑条位置：来回扫（两端各留一个条宽），速度约 220 px/s —— 肉眼一眼能看出"在动"
    // Bar position: sweeps back and forth (one bar width of margin at each end) at about 220 px/s, so that "it is
    // moving" is obvious at a glance
    int32_t span = (int32_t)g_ext.width - (int32_t)g_bar_w * 2;
    int32_t phase = span > 0 ? (int32_t)(t * 220.0) % (span * 2) : 0;
    if (phase > span) phase = span * 2 - phase;
    int32_t bar_x = g_bar_w + (phase > 0 ? phase : 0);
    int32_t bar_y = (int32_t)g_ext.height * 3 / 10;

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(g_cb, 0);
    vkBeginCommandBuffer(g_cb, &bi);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g_img[idx], .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue cc = { .float32 = { bg[0], bg[1], bg[2], bg[3] } };
    VkImageSubresourceRange full = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(g_cb, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &full);
    // 滑条：把那块全白缓冲拷进画面的一块矩形。vkCmdClearColorImage 只能清整幅，而"清一块"要么走渲染通道里的
    // vkCmdClearAttachments、要么就这样拷一次 —— 拷贝这条路不需要渲染通道，也不需要在同一命令缓冲里断开再开。
    // The bar: copy the all-white buffer into a rectangle of the picture. vkCmdClearColorImage can only clear the whole
    // image, and clearing just a rectangle means either vkCmdClearAttachments inside a render pass or a copy like this
    // one -- the copy needs no render pass and does not split the command buffer.
    if (g_bar_buf) {
        VkBufferImageCopy rg = { .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { bar_x, bar_y, 0 },
            .imageExtent = { g_bar_w, g_bar_h, 1 } };
        vkCmdCopyBufferToImage(g_cb, g_bar_buf, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    }
    // 没授权"所有文件访问"就把橙块画上：它既是提示也是按钮（授权后自己消失，所以不会误触）
    // Without all files access granted, paint the orange block: it is both the hint and the button (it disappears once
    // granted, so it cannot be tapped by accident afterwards)
    if (!g_perm_files && g_btn_buf) {
        VkBufferImageCopy bg = { .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { g_btn_x, g_btn_y, 0 },
            .imageExtent = { (uint32_t)g_btn_w, (uint32_t)g_btn_h, 1 } };
        vkCmdCopyBufferToImage(g_cb, g_btn_buf, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bg);
    }
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(g_cb);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_sem_acq,
        .pWaitDstStageMask = &wait, .commandBufferCount = 1, .pCommandBuffers = &g_cb,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &g_sem_rdy };
    vkResetFences(g_dev, 1, &g_fence);
    if (vkQueueSubmit(g_queue, 1, &si, g_fence) != VK_SUCCESS) return;
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g_sem_rdy, .swapchainCount = 1, .pSwapchains = &g_sc, .pImageIndices = &idx };
    VkResult pr = vkQueuePresentKHR(g_queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) win_teardown();
}

static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) {
                if (!win_boot(app)) LOGE("窗口那一套建不起来（见上面的日志）");
                else LOGI("窗口就绪：手机上应该能看到底色在变色、白条在滑动");
            }
            break;
        case APP_CMD_TERM_WINDOW:
            win_teardown();
            break;
        case APP_CMD_DESTROY:
            LOGI("活动结束，退出");
            break;
        default:
            break;
    }
}

// 数据从哪读：这个探针只回答一个问题 —— 装成 APK 之后（uid 不再是 shell），app 还能不能读到 adb 放进
// /data/local/tmp 的游戏数据？**权限位是放得开的**（/data/local/tmp 是 771，"其他"可穿越；/data/local/tmp/dm 是
// 777；文件是 -rw-rw-rw-），所以剩下的只有 **SELinux**：adb shell 建的这类文件标签通常是 shell_data_file，
// 而 app 域（untrusted_app）读它一般是被拒的。能读就省掉 9.1 GB 的搬运，读不到就得把数据放到 app 自己可读的
// 目录里 —— 这个结论不该靠猜，所以让 app 自己说出来（连域与 errno 一起）。
// Where the data comes from: this probe answers exactly one question -- now that we are an installed APK (not the shell
// uid), can the app still read the game data adb pushed into /data/local/tmp? **The permission bits are wide open**
// (/data/local/tmp is 771 so "other" may traverse, /data/local/tmp/dm is 777 and the files are -rw-rw-rw-), so all that
// remains is **SELinux**: files of this kind created by adb shell usually carry the shell_data_file label, which the app
// domain (untrusted_app) is normally denied. If it can read them, 9.1 GB of copying is avoided; if not, the data has to
// move somewhere the app can read -- and that conclusion is not something to guess at, so the app says it itself
// (together with its domain and errno).
static void probe_paths(void) {
    char ctx[256] = "";
    FILE *f = fopen("/proc/self/attr/current", "r");
    if (f) { if (fgets(ctx, sizeof ctx, f)) { char *nl = strchr(ctx, '\n'); if (nl) *nl = 0; } fclose(f); }
    LOGI("探针：uid=%u gid=%u 域 %s", (unsigned)getuid(), (unsigned)getgid(), ctx[0] ? ctx : "?");
    static const char *paths[] = {
        "/data/local/tmp/dm/patched/main.patched.bin",          // 游戏主模块（54 MB）| the game's main module
        "/data/local/tmp/dm/spv",                                // 着色器产物目录 | the shader artifact directory
        "/data/local/tmp/dm/romfs",                              // 8.3 GB 的原版档案 | the 8.3 GB original archives
        "/sdcard/Android/data/ai.d2rm.dm/files",                 // 正向对照：app 自己的外部目录，必然可读
                                                                 // positive control: the app's own external directory, readable by construction
        "/sdcard/d2rm/romfs",                                    // 入口里写好的备用位置（还没建）| the fallback the entry already knows (not created yet)
    };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        FILE *g = fopen(paths[i], "rb");
        if (g) { LOGI("  能读 %s", paths[i]); fclose(g); }
        else LOGE("  读不了 %s（errno %d：%s）", paths[i], errno, strerror(errno));
    }
    // 写探针：游戏要写存档（save）与临时文件（temp），而**读得到不等于写得进** —— SELinux 对 shell_data_file 的写
    // 通常比读严得多。写得进，数据就完全不用搬；写不进，就让 shell 把 save/temp 做成指向 app 自己目录的软链
    // （软链的**目标**是 app_data_file，app 对它可写 —— 而游戏看到的仍是 <DM_ROOT>/save 这个路径）。
    // Write probe: the game writes saves (save) and temporary files (temp), and **being able to read does not imply
    // being able to write** -- SELinux is usually much stricter about writing shell_data_file. If writing works the data
    // needs no moving at all; if not, have the shell symlink save/temp to the app's own directory (the symlink's
    // **target** is app_data_file, which the app may write, while the game still sees the <DM_ROOT>/save path).
    // /sdcard/d2rm 是「所有文件访问」要用的那个目录，入口的备用路径也正指着它：先试着建它 —— 建成 / 建不成本身
    // 就是这条权限的证据（不建的话，往里写会报 errno 2「没有这个目录」，那是另一回事，看不出权限）
    // /sdcard/d2rm is the directory "all files access" is for, and the entry's fallback path points at it: try to create
    // it first -- whether that works is itself the evidence for the permission (without creating it, writing inside
    // reports errno 2 "no such directory", which is a different thing and says nothing about the permission)
    if (mkdir("/sdcard/d2rm", 0777) != 0 && errno != EEXIST)
        LOGE("  建不了 /sdcard/d2rm（errno %d：%s）", errno, strerror(errno));
    else LOGI("  /sdcard/d2rm 在");
    static const char *wpaths[] = {
        "/data/local/tmp/dm/save/.d2rm_probe",
        "/data/local/tmp/dm/temp/.d2rm_probe",
        "/sdcard/Android/data/ai.d2rm.dm/files/.d2rm_probe",
        "/sdcard/d2rm/.d2rm_probe",           // 「所有文件访问」真正买到的那块地方（用户看得见的那个目录）
                                              // what "all files access" really buys: the directory the user can see
    };
    for (unsigned i = 0; i < sizeof wpaths / sizeof wpaths[0]; i++) {
        FILE *g = fopen(wpaths[i], "wb");
        if (g) { fputc('x', g); fclose(g); remove(wpaths[i]); LOGI("  能写 %s", wpaths[i]); }
        else LOGE("  写不了 %s（errno %d：%s）", wpaths[i], errno, strerror(errno));
    }
}

void android_main(struct android_app *app) {
    g_app = app;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    LOGI("启动（NativeActivity + app_glue）");
    probe_paths();
    for (;;) {
        int events = 0;
        struct android_poll_source *src = NULL;
        // 有窗口就非阻塞地转圈出图；没窗口就阻塞等事件（省电）
        // With a window, spin non-blocking to present; without one, block on events (saves power)
        int timeout = (g_built && g_sc) ? 0 : -1;
        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) { win_teardown(); if (g_dev) { vkDeviceWaitIdle(g_dev); vkDestroyDevice(g_dev, NULL); } if (g_inst) vkDestroyInstance(g_inst, NULL); LOGI("退出"); return; }
            timeout = (g_built && g_sc) ? 0 : -1;
        }
        perm_tick(now_s());   // 约每秒查一次权限（从设置页回来时橙块该消失）| query the permission about once a second (the block should vanish on return from settings)
        if (g_built && g_sc) frame();
    }
}
