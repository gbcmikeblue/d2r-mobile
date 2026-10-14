// 见 vk_win.h：窗口/交换链/上屏（M3）。只认 8 位四通道（真机交换链实测 R8G8B8A8_UNORM，格式 37）。
// See vk_win.h: window, swapchain, present (M3). 8-bit four-channel only (the device's swapchain measures as
// R8G8B8A8_UNORM, format 37).
#include "vk_win.h"
#include "vk_device.h"
#include "dm.h"
#include <stdlib.h>
#include <string.h>

#define WIN_MAX_IMG 8

static VkSwapchainKHR g_sc;
static VkFormat g_fmt;
static VkExtent2D g_ext;
static uint32_t g_nimg;
static VkImage g_img[WIN_MAX_IMG];
static VkImageView g_view[WIN_MAX_IMG];
static VkCommandPool g_pool;
static VkCommandBuffer g_cb;
static VkSemaphore g_acq, g_rdy;
static VkFence g_fence;
static int g_ready;

// 主机像素 → 屏幕的中转：一块宿主可见缓冲 + 一张设备本地中转图。**中转图必须是 OPTIMAL**：vkCmdBlitImage 要求
// 两边都是 OPTIMAL 平铺，所以线性图这条捷径走不通（试过，规格不允许）。
// Staging from host pixels to screen: a host-visible buffer plus one device-local intermediate image. **The
// intermediate image must be OPTIMAL**: vkCmdBlitImage requires both sides to be optimally tiled, so the linear-image
// shortcut is closed (tried it; the spec does not allow it).
static VkBuffer g_sbuf;
static VkDeviceMemory g_smem;
static VkImage g_simg;
static VkDeviceMemory g_simgmem;
static uint32_t g_sw, g_sh;
static VkImageLayout g_simg_lay = VK_IMAGE_LAYOUT_UNDEFINED;

static int sc_build(void) {
    VkSurfaceKHR surf = vk_dev_surface();
    // vk_device.c 没有暴露物理设备，这里从实例取一次（真机上只有一个）| vk_device.c does not expose the physical
    // device, so take it from the instance once (there is only one on the device)
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t np = 1;
    if (vkEnumeratePhysicalDevices(vk_instance(), &np, &phys) != VK_SUCCESS || !phys) return 0;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps) != VK_SUCCESS) return 0;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, NULL);
    VkSurfaceFormatKHR fmts[32];
    if (!nf || nf > 32) { LOG("【红】窗口：surface 报的格式数 %u 不可用", nf); return 0; }
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, fmts);
    g_fmt = fmts[0].format;
    VkColorSpaceKHR cs = fmts[0].colorSpace;
    for (uint32_t i = 0; i < nf; i++)
        if ((fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { g_fmt = fmts[i].format; cs = fmts[i].colorSpace; break; }
    if (g_fmt != VK_FORMAT_R8G8B8A8_UNORM && g_fmt != VK_FORMAT_B8G8R8A8_UNORM) {
        LOG("【黄】窗口：交换链格式 %d 不是 8 位四通道，这一版上不了屏", (int)g_fmt);
        return 0;
    }
    g_ext = caps.currentExtent;
    if (g_ext.width == 0xffffffffu) g_ext = caps.minImageExtent;   // 有些实现让应用自己定尺寸 | some let the app choose
    uint32_t ni = caps.minImageCount + 1;
    if (caps.maxImageCount && ni > caps.maxImageCount) ni = caps.maxImageCount;
    if (ni > WIN_MAX_IMG) ni = WIN_MAX_IMG;
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surf, .minImageCount = ni, .imageFormat = g_fmt, .imageColorSpace = cs, .imageExtent = g_ext,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE, .oldSwapchain = VK_NULL_HANDLE };
    if (vkCreateSwapchainKHR(vk_dev(), &ci, NULL, &g_sc) != VK_SUCCESS) { LOG("【红】窗口：建交换链失败"); g_sc = VK_NULL_HANDLE; return 0; }
    g_nimg = 0;
    if (vkGetSwapchainImagesKHR(vk_dev(), g_sc, &g_nimg, NULL) != VK_SUCCESS) return 0;
    if (g_nimg > WIN_MAX_IMG) g_nimg = WIN_MAX_IMG;
    vkGetSwapchainImagesKHR(vk_dev(), g_sc, &g_nimg, g_img);
    for (uint32_t i = 0; i < g_nimg; i++) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_img[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g_fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (vkCreateImageView(vk_dev(), &vi, NULL, &g_view[i]) != VK_SUCCESS) return 0;
    }
    LOG("[vk] 窗口交换链就绪：%ux%u 格式 %d、%u 张、FIFO", g_ext.width, g_ext.height, (int)g_fmt, g_nimg);
    return 1;
}

static void sc_destroy(void) {
    if (!vk_dev()) return;
    for (uint32_t i = 0; i < g_nimg; i++)
        if (g_view[i]) { vkDestroyImageView(vk_dev(), g_view[i], NULL); g_view[i] = VK_NULL_HANDLE; }
    g_nimg = 0;
    if (g_sc) { vkDestroySwapchainKHR(vk_dev(), g_sc, NULL); g_sc = VK_NULL_HANDLE; }
}

static void staging_destroy(void) {
    if (!vk_dev()) return;
    if (g_sbuf) { vkDestroyBuffer(vk_dev(), g_sbuf, NULL); g_sbuf = VK_NULL_HANDLE; }
    if (g_smem) { vkFreeMemory(vk_dev(), g_smem, NULL); g_smem = VK_NULL_HANDLE; }
    if (g_simg) { vkDestroyImage(vk_dev(), g_simg, NULL); g_simg = VK_NULL_HANDLE; }
    if (g_simgmem) { vkFreeMemory(vk_dev(), g_simgmem, NULL); g_simgmem = VK_NULL_HANDLE; }
    g_sw = g_sh = 0;
    g_simg_lay = VK_IMAGE_LAYOUT_UNDEFINED;
}

int vk_win_live(void) { return g_sc != VK_NULL_HANDLE; }

int vk_win_boot(void) {
    if (!vk_dev()) { LOG("【红】窗口：设备还没建起来（要先 vk_device_init）"); return 0; }
    if (!vk_dev_surface()) { LOG("【红】窗口：没有 surface（要先 vk_device_set_window 再 init）"); return 0; }
    if (!sc_build()) return 0;
    if (!g_pool) {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk_queue_family() };
        if (vkCreateCommandPool(vk_dev(), &pci, NULL, &g_pool) != VK_SUCCESS) return 0;
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        if (vkAllocateCommandBuffers(vk_dev(), &ai, &g_cb) != VK_SUCCESS) return 0;
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        if (vkCreateSemaphore(vk_dev(), &si, NULL, &g_acq) != VK_SUCCESS) return 0;
        if (vkCreateSemaphore(vk_dev(), &si, NULL, &g_rdy) != VK_SUCCESS) return 0;
        if (vkCreateFence(vk_dev(), &fi, NULL, &g_fence) != VK_SUCCESS) return 0;
    }
    g_ready = 1;
    return 1;
}

void vk_win_shutdown(void) {
    if (!vk_dev()) return;
    // 拆窗口会等整台设备空闲，而客体线程还在提交：先拿队列锁，把队列上的活挡在门外（设备级的完全串行化还没做，
    // 这条只覆盖队列这一侧；窗口拆除本身很少发生）。
    // Tearing the window down waits for the whole device to go idle while the guest thread is still submitting: take
    // the queue lock first to keep queue work out (full device-level serialisation is not done yet; this covers the
    // queue side, and window teardown is rare).
    vk_queue_lock();
    vkDeviceWaitIdle(vk_dev());
    vk_queue_unlock();
    sc_destroy();
    staging_destroy();
    if (g_cb) { vkFreeCommandBuffers(vk_dev(), g_pool, 1, &g_cb); g_cb = VK_NULL_HANDLE; }
    if (g_pool) { vkDestroyCommandPool(vk_dev(), g_pool, NULL); g_pool = VK_NULL_HANDLE; }
    if (g_acq) { vkDestroySemaphore(vk_dev(), g_acq, NULL); g_acq = VK_NULL_HANDLE; }
    if (g_rdy) { vkDestroySemaphore(vk_dev(), g_rdy, NULL); g_rdy = VK_NULL_HANDLE; }
    if (g_fence) { vkDestroyFence(vk_dev(), g_fence, NULL); g_fence = VK_NULL_HANDLE; }
    g_ready = 0;
}

// 中转那两样按源尺寸建；源尺寸在会话里通常固定，所以只建一次，变了就换
// The two staging objects are built for the source size; that is fixed for a session in practice, so once, replaced when it changes
static int staging_ensure(uint32_t w, uint32_t h) {
    if (g_sbuf && g_sw == w && g_sh == h) return 1;
    staging_destroy();
    VkDeviceSize sz = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(vk_dev(), &bci, NULL, &g_sbuf) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk_dev(), g_sbuf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mr.memoryTypeBits) };
    if (vkAllocateMemory(vk_dev(), &mai, NULL, &g_smem) != VK_SUCCESS || vkBindBufferMemory(vk_dev(), g_sbuf, g_smem, 0) != VK_SUCCESS) return 0;
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(vk_dev(), &ici, NULL, &g_simg) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(vk_dev(), g_simg, &mr);
    VkMemoryAllocateInfo mai2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mr.memoryTypeBits) };
    if (vkAllocateMemory(vk_dev(), &mai2, NULL, &g_simgmem) != VK_SUCCESS || vkBindImageMemory(vk_dev(), g_simg, g_simgmem, 0) != VK_SUCCESS) return 0;
    g_sw = w;
    g_sh = h;
    return 1;
}

static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                        VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags sstage, VkPipelineStageFlags dstage) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .srcAccessMask = src, .dstAccessMask = dst,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, sstage, dstage, 0, 0, NULL, 0, NULL, 1, &b);
}

int vk_win_window_to_src(float wx, float wy, uint32_t w, uint32_t h, int *ix, int *iy) {
    if (!g_ext.width || !w || !h) return 0;
    float sx = (float)g_ext.width / (float)w, sy = (float)g_ext.height / (float)h;
    float s = sx < sy ? sx : sy;
    float dw = (float)w * s, dh = (float)h * s;
    float ox = ((float)g_ext.width - dw) * 0.5f, oy = ((float)g_ext.height - dh) * 0.5f;
    float x = (wx - ox) / s, y = (wy - oy) / s;
    if (x < 0.f || y < 0.f || x >= (float)w || y >= (float)h) return 0;
    if (ix) *ix = (int)x;
    if (iy) *iy = (int)y;
    return 1;
}

int vk_win_present_rgba(const uint8_t *rgba, uint32_t w, uint32_t h) {
    if (!rgba || !w || !h) return 0;
    if (!vk_win_live() && !vk_win_boot()) return 0;
    if (!staging_ensure(w, h)) { LOG("【红】窗口：中转那两样建不出来（%ux%u）", w, h); return 0; }

    void *p = NULL;
    if (vkMapMemory(vk_dev(), g_smem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 0;
    memcpy(p, rgba, (size_t)w * h * 4);
    vkUnmapMemory(vk_dev(), g_smem);

    vkWaitForFences(vk_dev(), 1, &g_fence, VK_TRUE, UINT64_MAX);
    // 取图像与上屏都要与客体线程的提交串行（见 vk_device.h）：这条队列是共用的。
    // 交换链那张图只有我们碰，所以"锁 + 一次取图"不会与客体的活互相等。
    // Acquiring an image and presenting must be serialised against the guest thread's submits (see vk_device.h): the
    // queue is shared. Only this side touches the swapchain images, so "lock plus one acquisition" cannot make the
    // guest's work wait on us in turn.
    vk_queue_lock();
    uint32_t idx = 0;
    // 取图像**给超时**，绝不长占队列锁：屏幕关掉 / 合成器忙的时候交换链可能长时间没有可用图像，
    // 而锁一旦被它占住，客体线程的提交就全堵在那里 —— 实测过这一幕：两端一起停住，等待点显示
    // 客体卡在"正在录制、通道开着"，前端的 1 秒心跳也一起没了。超时就放掉这一帧，下一帧再来。
    // Acquire **with a timeout** and never hold the queue lock through it: with the screen off, or the compositor
    // busy, the swapchain can have no image available for a long time, and while that holds the lock every guest
    // submit piles up behind it -- measured exactly so: both sides stopped, and the wait-point dump showed the guest
    // stuck "recording, pass open" while the front end's one-second heartbeat was gone too. On timeout the frame is
    // simply skipped and the next one retries.
    VkResult r = vkAcquireNextImageKHR(vk_dev(), g_sc, 100000000ull, g_acq, VK_NULL_HANDLE, &idx);   // 100 ms
    // 交换链过期（窗口尺寸变了之类）：拆掉重建，这一帧就算了，下一帧自然重来
    // Out-of-date swapchain (the window was resized and such): tear down and rebuild; this frame is skipped and the next
    // one simply retries
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vk_queue_unlock(); sc_destroy(); return 0; }
    if (r != VK_SUCCESS) { vk_queue_unlock(); return 0; }

    // 保持比例缩放并居中，多出来的地方留黑（所以先整幅清黑）
    // Scaled to fit with the aspect preserved and centred, black around it (hence the full clear first)
    float sx = (float)g_ext.width / (float)w, sy = (float)g_ext.height / (float)h;
    float s = sx < sy ? sx : sy;
    int32_t dw = (int32_t)((float)w * s + 0.5f), dh = (int32_t)((float)h * s + 0.5f);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > (int32_t)g_ext.width) dw = (int32_t)g_ext.width;
    if (dh > (int32_t)g_ext.height) dh = (int32_t)g_ext.height;
    int32_t ox = ((int32_t)g_ext.width - dw) / 2, oy = ((int32_t)g_ext.height - dh) / 2;

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(g_cb, 0);
    vkBeginCommandBuffer(g_cb, &bi);

    // ① 主机像素 → 中转图（TRANSFER_DST），再让它可被 blit 读（TRANSFER_SRC）
    // (1) host pixels into the intermediate image (TRANSFER_DST), then make it blit-readable (TRANSFER_SRC)
    img_barrier(g_cb, g_simg, g_simg_lay, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy rg = { .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageOffset = { 0, 0, 0 },
        .imageExtent = { w, h, 1 } };
    vkCmdCopyBufferToImage(g_cb, g_sbuf, g_simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    img_barrier(g_cb, g_simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    g_simg_lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // ② 交换链那张：UNDEFINED → TRANSFER_DST（反正要整幅清黑，旧内容不要了）→ 清黑 → 缩放 blit → PRESENT_SRC
    // (2) the swapchain image: UNDEFINED -> TRANSFER_DST (it gets cleared black anyway, so the old contents do not
    // matter) -> clear -> scaled blit -> PRESENT_SRC
    img_barrier(g_cb, g_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = { .float32 = { 0.f, 0.f, 0.f, 1.f } };
    VkImageSubresourceRange full = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(g_cb, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &full);
    VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                       .srcOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } },
                       .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                       .dstOffsets = { { ox, oy, 0 }, { ox + dw, oy + dh, 1 } } };
    vkCmdBlitImage(g_cb, g_simg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &bl, VK_FILTER_LINEAR);
    img_barrier(g_cb, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(g_cb);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_acq,
        .pWaitDstStageMask = &wait, .commandBufferCount = 1, .pCommandBuffers = &g_cb,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &g_rdy };
    vkResetFences(vk_dev(), 1, &g_fence);
    if (vkQueueSubmit(vk_queue(), 1, &si, g_fence) != VK_SUCCESS) { vk_queue_unlock(); return 0; }
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g_rdy, .swapchainCount = 1, .pSwapchains = &g_sc, .pImageIndices = &idx };
    VkResult pr = vkQueuePresentKHR(vk_queue(), &pi);
    vk_queue_unlock();   // 取图像 → 提交 → 上屏这一段都在锁里 | acquire -> submit -> present is the whole critical section
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) sc_destroy();
    return 1;
}
