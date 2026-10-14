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
#include <time.h>
#include <string.h>
#include <stdlib.h>

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

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// 等设备空闲并把窗口那一套拆掉（TERM_WINDOW / 退出时）| drain the device and tear the window chain down (on TERM_WINDOW / exit)
static void win_teardown(void) {
    if (!g_dev) return;
    vkDeviceWaitIdle(g_dev);
    if (g_bar_buf) { vkDestroyBuffer(g_dev, g_bar_buf, NULL); g_bar_buf = VK_NULL_HANDLE; }
    if (g_bar_mem) { vkFreeMemory(g_dev, g_bar_mem, NULL); g_bar_mem = VK_NULL_HANDLE; }
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
    // 滑条缓冲：只在交换链格式是 8 位四通道时才做（拷贝要求每像素字节数一致；别的格式就只放动画底色，仍能证明"活着"）
    // The bar buffer only exists for 8-bit four-channel swapchain formats (a copy needs matching texel sizes; other
    // formats fall back to the animated background alone, which still proves the chain is alive)
    if (g_fmt == VK_FORMAT_R8G8B8A8_UNORM || g_fmt == VK_FORMAT_B8G8R8A8_UNORM ||
        g_fmt == VK_FORMAT_R8G8B8A8_SRGB || g_fmt == VK_FORMAT_B8G8R8A8_SRGB) {
        g_bar_w = g_ext.width / 12 ? g_ext.width / 12 : 1;
        g_bar_h = g_ext.height / 5 ? g_ext.height / 5 : 1;
        VkDeviceSize sz = (VkDeviceSize)g_bar_w * g_bar_h * 4;
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        if (vkCreateBuffer(g_dev, &bci, NULL, &g_bar_buf) == VK_SUCCESS) {
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(g_dev, g_bar_buf, &mr);
            uint32_t mi = 0;
            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
            for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
                if ((mr.memoryTypeBits & (1u << i)) &&
                    (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mi = i; break; }
            VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                .memoryTypeIndex = mi };
            if (vkAllocateMemory(g_dev, &mai, NULL, &g_bar_mem) == VK_SUCCESS &&
                vkBindBufferMemory(g_dev, g_bar_buf, g_bar_mem, 0) == VK_SUCCESS) {
                void *p = NULL;
                if (vkMapMemory(g_dev, g_bar_mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
                    memset(p, 0xFF, (size_t)sz);
                    vkUnmapMemory(g_dev, g_bar_mem);
                }
            } else {
                vkDestroyBuffer(g_dev, g_bar_buf, NULL);
                g_bar_buf = VK_NULL_HANDLE;
            }
        }
    }
    LOGI("交换链就绪：%ux%u 格式 %d、%u 张、FIFO；滑条 %ux%u", g_ext.width, g_ext.height, (int)g_fmt, g_nimg, g_bar_w, g_bar_h);
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

void android_main(struct android_app *app) {
    app->onAppCmd = on_cmd;
    LOGI("启动（NativeActivity + app_glue）");
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
        if (g_built && g_sc) frame();
    }
}
