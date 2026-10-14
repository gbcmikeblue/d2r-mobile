// NVN 贴图 → VkImage（安卓回放层）。设计取舍见 vk_tex.h 的头注释。
// NVN textures -> VkImage (the Android replay layer). See the header comment of vk_tex.h for the design
// trade-offs.
#include "vk_tex.h"
#include "vk_device.h"
#include "vk_region.h"
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include "dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic uint64_t g_created, g_uploads, g_upload_bytes, g_reads, g_fmt_miss;

// ---------- NVN 格式 → VkFormat ----------
// 数值取自 nvn.h 的 NVN_FORMAT_*（权威来源，别猜）。没列出的按"没有对应"处理并报红一次，
// 真机日志里出现哪个再由实测补——不猜。
// ---------- NVN format -> VkFormat ----------
// The values come from NVN_FORMAT_* in nvn.h (the authoritative source; do not guess). Anything not listed is
// treated as "no counterpart" and reported once, and whatever shows up in a device log is added from that
// measurement rather than guessed.
int vk_fmt_to_vk(int f, VkFormat *out) {
    switch (f) {
    case 0x1B: *out = VK_FORMAT_R8G8B8_UINT; return 1;
    case 0x1C: *out = VK_FORMAT_R8G8B8_SINT; return 1;
    case 0x1D: *out = VK_FORMAT_R16G16B16_SFLOAT; return 1;
    case 0x1E: *out = VK_FORMAT_R16G16B16_UNORM; return 1;
    case 0x1F: *out = VK_FORMAT_R16G16B16_SNORM; return 1;
    case 0x20: *out = VK_FORMAT_R16G16B16_UINT; return 1;
    case 0x21: *out = VK_FORMAT_R16G16B16_SINT; return 1;
    case 0x22: *out = VK_FORMAT_R32G32B32_SFLOAT; return 1;
    case 0x23: *out = VK_FORMAT_R32G32B32_UINT; return 1;
    case 0x24: *out = VK_FORMAT_R32G32B32_SINT; return 1;
    case 0x25: *out = VK_FORMAT_R8G8B8A8_UNORM; return 1;
    case 0x26: *out = VK_FORMAT_R8G8B8A8_SNORM; return 1;
    case 0x27: *out = VK_FORMAT_R8G8B8A8_UINT; return 1;
    case 0x28: *out = VK_FORMAT_R8G8B8A8_SINT; return 1;
    case 0x29: *out = VK_FORMAT_R16G16B16A16_SFLOAT; return 1;
    case 0x2A: *out = VK_FORMAT_R16G16B16A16_UNORM; return 1;
    case 0x2B: *out = VK_FORMAT_R16G16B16A16_SNORM; return 1;
    case 0x2C: *out = VK_FORMAT_R16G16B16A16_UINT; return 1;
    case 0x2D: *out = VK_FORMAT_R16G16B16A16_SINT; return 1;
    case 0x2E: *out = VK_FORMAT_R32G32B32A32_SFLOAT; return 1;
    case 0x2F: *out = VK_FORMAT_R32G32B32A32_UINT; return 1;
    case 0x30: *out = VK_FORMAT_R32G32B32A32_SINT; return 1;
    case 0x31: *out = VK_FORMAT_S8_UINT; return 1;
    case 0x32: *out = VK_FORMAT_D16_UNORM; return 1;
    case 0x33: *out = VK_FORMAT_X8_D24_UNORM_PACK32; return 1;
    case 0x34: *out = VK_FORMAT_D32_SFLOAT; return 1;
    case 0x35: *out = VK_FORMAT_D24_UNORM_S8_UINT; return 1;
    case 0x36: *out = VK_FORMAT_D32_SFLOAT_S8_UINT; return 1;
    case 0x37: *out = VK_FORMAT_R8G8B8A8_SRGB; return 1;   // RGBX8_SRGB：X 通道不参与采样，按 RGBA8 承接
    case 0x38: *out = VK_FORMAT_R8G8B8A8_SRGB; return 1;
    case 0x39: *out = VK_FORMAT_R4G4B4A4_UNORM_PACK16; return 1;
    case 0x3A: *out = VK_FORMAT_R5G6B5_UNORM_PACK16; return 1;   // RGB5：Vulkan 无 5-5-5，按 565 承接并记一次
    case 0x3B: *out = VK_FORMAT_R5G5B5A1_UNORM_PACK16; return 1;
    case 0x3C: *out = VK_FORMAT_R5G6B5_UNORM_PACK16; return 1;
    case 0x3D: *out = VK_FORMAT_A2B10G10R10_UNORM_PACK32; return 1;
    case 0x3E: *out = VK_FORMAT_A2B10G10R10_UINT_PACK32; return 1;
    case 0x3F: *out = VK_FORMAT_B10G11R11_UFLOAT_PACK32; return 1;
    case 0x40: *out = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32; return 1;
    case 0x41: *out = VK_FORMAT_BC1_RGB_UNORM_BLOCK; return 1;
    case 0x42: *out = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; return 1;
    case 0x43: *out = VK_FORMAT_BC2_UNORM_BLOCK; return 1;
    case 0x44: *out = VK_FORMAT_BC3_UNORM_BLOCK; return 1;
    case 0x45: *out = VK_FORMAT_BC1_RGB_SRGB_BLOCK; return 1;
    case 0x46: *out = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; return 1;
    case 0x47: *out = VK_FORMAT_BC2_SRGB_BLOCK; return 1;
    case 0x48: *out = VK_FORMAT_BC3_SRGB_BLOCK; return 1;
    case 0x49: *out = VK_FORMAT_BC4_UNORM_BLOCK; return 1;
    case 0x4A: *out = VK_FORMAT_BC4_SNORM_BLOCK; return 1;
    case 0x4B: *out = VK_FORMAT_BC5_UNORM_BLOCK; return 1;
    case 0x4C: *out = VK_FORMAT_BC5_SNORM_BLOCK; return 1;
    case 0x4D: *out = VK_FORMAT_BC7_UNORM_BLOCK; return 1;
    case 0x4E: *out = VK_FORMAT_BC7_SRGB_BLOCK; return 1;
    case 0x4F: *out = VK_FORMAT_BC6H_SFLOAT_BLOCK; return 1;
    case 0x50: *out = VK_FORMAT_BC6H_UFLOAT_BLOCK; return 1;
    default: return 0;   // 其余（UI2F / I2F 那一批 0x51 起、以及更小的 R/RG 系列）待实测补 | the rest (the UI2F / I2F batch from 0x51, and the smaller R/RG series) to be added from measurements
    }
}

// ---------- serial → 贴图表（开放寻址，键是序号不是指针）----------
// ---------- serial -> texture table (open addressing; the key is the serial, not a pointer) ----------
typedef struct {
    uint64_t serial;
    VkImage img; VkImageView view; VkDeviceMemory mem;
    VkFormat fmt; uint32_t w, h, d, levels, layers;
    int depth_like, dsa;      // dsa：深度模板合并（D24S8 / D32FS8）| dsa: combined depth-stencil
    int dirty;                // 池被 CPU 改过，下次取用前重传 | pool changed on the CPU; re-upload before the next use
    int gpu_written;          // GPU 写过：池不再是真值，CPU 读回要走 VkImage | the GPU has written it: the pool is no longer the truth and a CPU readback must go through the VkImage
    VkImageLayout lay;        // 当前布局（转换只在这一层发生）| current layout (transitions happen only here)
} ent_t;

static ent_t *g_tab; static int g_cap, g_n;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static VkCommandPool g_pool; static VkCommandBuffer g_cb;

static uint64_t hsh(uint64_t k) { k *= 11400714819323198485ull; return k ^ (k >> 29); }

static ent_t *find_slot(uint64_t serial, int create) {
    if (!g_cap) { g_cap = 256; g_tab = calloc((size_t)g_cap, sizeof *g_tab); }
    if (create && g_n * 2 >= g_cap) {
        int ncap = g_cap * 2; ent_t *nt = calloc((size_t)ncap, sizeof *nt);
        for (int i = 0; i < g_cap; i++) if (g_tab[i].serial) {
            uint64_t j = hsh(g_tab[i].serial) % (uint64_t)ncap;
            while (nt[j].serial) j = (j + 1) % (uint64_t)ncap;
            nt[j] = g_tab[i];
        }
        free(g_tab); g_tab = nt; g_cap = ncap;
    }
    uint64_t i = hsh(serial) % (uint64_t)g_cap;
    for (;;) {
        if (g_tab[i].serial == serial) return &g_tab[i];
        if (!g_tab[i].serial) { if (!create) return NULL; g_tab[i].serial = serial; g_n++; return &g_tab[i]; }
        i = (i + 1) % (uint64_t)g_cap;
    }
}

// ---------- 一次性的命令缓冲（上传用）----------
// ---------- one-off command buffer (for uploads) ----------
static int one_begin(void) {
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return 0;
    if (!g_pool) {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk_queue_family() };
        if (vkCreateCommandPool(vk_dev(), &pci, NULL, &g_pool) != VK_SUCCESS) return 0;
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        if (vkAllocateCommandBuffers(vk_dev(), &ai, &g_cb) != VK_SUCCESS) return 0;
    }
    if (vkResetCommandBuffer(g_cb, 0) != VK_SUCCESS) return 0;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    return vkBeginCommandBuffer(g_cb, &bi) == VK_SUCCESS;
}
static int one_end(void) {
    if (vkEndCommandBuffer(g_cb) != VK_SUCCESS) return 0;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_cb };
    if (vkQueueSubmit(vk_queue(), 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return 0;
    VkResult r = vkQueueWaitIdle(vk_queue());
    if (r != VK_SUCCESS) { LOG("【红】上传命令等待失败 VkResult %d", (int)r); return 0; }
    return 1;
}

// ---------- 建 / 上传 ----------
static int ent_build(const tex_t *t, ent_t *e) {
    const nvn_texdesc *d = &t->d;
    if (d->target == 5 || d->target == 6 || d->target == 10 || (d->flags & 0x10) || d->samples > 1) {
        static int said; if (!said++) LOG("【黄】贴图 target %d / flags %#x / 采样 %d 暂不支持（缓冲区 / 多采样 / 线性布局）", d->target, d->flags, d->samples);
        return 0;
    }
    if (!vk_fmt_to_vk(d->format, &e->fmt)) {
        uint64_t k = atomic_fetch_add(&g_fmt_miss, 1);
        if (k < 8) LOG("【红】贴图格式 %#x 在 Vulkan 这边没有对应（第 %llu 例，见 vk_tex.c 的格式表）", d->format, (unsigned long long)(k + 1));
        return 0;
    }
    e->depth_like = (d->format >= 0x31 && d->format <= 0x36) ? 1 : 0;
    e->dsa = (d->format == 0x35 || d->format == 0x36) ? 1 : 0;
    e->w = d->width ? d->width : 1; e->h = d->height ? d->height : 1; e->d = 1;
    e->levels = d->levels ? d->levels : 1; e->layers = 1;

    VkImageType type = VK_IMAGE_TYPE_2D;
    VkImageCreateFlags iflags = 0;
    switch (d->target) {
    case 0: type = VK_IMAGE_TYPE_1D; e->h = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D; e->d = d->depth ? d->depth : 1; break;
    case 3: type = VK_IMAGE_TYPE_1D; e->layers = d->height; e->h = 1; break;         // 1D_ARRAY
    case 4: e->layers = d->depth; break;                                             // 2D_ARRAY
    case 8: e->layers = 6; iflags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;      // CUBE
    case 9: e->layers = (d->depth ? d->depth : 1) * 6; iflags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;   // CUBE_ARRAY
    default: break;                                                                   // 1 = 2D
    }
    VkImageUsageFlags use = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!e->depth_like) use |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (e->depth_like) use |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = iflags, .imageType = type,
        .format = e->fmt, .extent = { e->w, e->h, e->d }, .mipLevels = e->levels, .arrayLayers = e->layers,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(vk_dev(), &ci, NULL, &e->img) != VK_SUCCESS) { LOG("【红】建 VkImage 失败 %ux%ux%u 格式 %d", e->w, e->h, e->d, (int)e->fmt); e->img = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(vk_dev(), e->img, &mr);
    // 优先要「设备本地 + 宿主可见 + 一致」：零拷贝内存（与内存池同一种），方便回读与调试
    // Prefer device-local + host-visible + coherent, the zero-copy type shared with the memory pools, which
    // keeps readback and debugging simple
    uint32_t mi = vk_memory_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mr.memoryTypeBits);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mi };
    if (vkAllocateMemory(vk_dev(), &mai, NULL, &e->mem) != VK_SUCCESS) { LOG("【红】贴图内存分配失败 %llu 字节", (unsigned long long)mr.size); return 0; }
    if (vkBindImageMemory(vk_dev(), e->img, e->mem, 0) != VK_SUCCESS) { LOG("【红】贴图内存绑定失败"); return 0; }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = e->img,
        .viewType = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D :
                    (iflags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? (e->layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) :
                    (e->layers > 1 ? (type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY) :
                     (type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D)),
        .format = e->fmt, .subresourceRange = { e->depth_like ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, e->levels, 0, e->layers } };
    if (e->depth_like) { vi.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT; if (!e->dsa) vi.subresourceRange.aspectMask &= ~(VkImageAspectFlags)VK_IMAGE_ASPECT_STENCIL_BIT; }
    if (vkCreateImageView(vk_dev(), &vi, NULL, &e->view) != VK_SUCCESS) { LOG("【红】建 VkImageView 失败"); return 0; }
    atomic_fetch_add(&g_created, 1);
    return 1;
}

// 上传：池里的块线性数据 → 解排布进暂存缓冲 → 拷进 VkImage
// Upload: the block-linear data in the pool -> deswizzle into a staging buffer -> copy into the VkImage
static int ent_upload(const tex_t *t, ent_t *e, VkImageAspectFlags aspect) {
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { static int said; if (!said++) LOG("【红】上传：格式 %#x 每块字节未知", t->d.format); return 0; }
    VkDeviceSize total = 0;
    for (uint32_t l = 0; l < e->levels; l++) {
        uint32_t w = e->w >> l, h = e->h >> l; if (!w) w = 1; if (!h) h = 1;
        total += (VkDeviceSize)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * e->layers * (e->d > 1 ? e->d : 1);
    }
    if (!total) return 0;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = total,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer stg = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk_dev(), &bci, NULL, &stg) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vk_dev(), stg, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mr.memoryTypeBits) };
    VkDeviceMemory sm = VK_NULL_HANDLE;
    if (vkAllocateMemory(vk_dev(), &mai, NULL, &sm) != VK_SUCCESS) { vkDestroyBuffer(vk_dev(), stg, NULL); return 0; }
    vkBindBufferMemory(vk_dev(), stg, sm, 0);
    void *smap = NULL;
    if (vkMapMemory(vk_dev(), sm, 0, VK_WHOLE_SIZE, 0, &smap) != VK_SUCCESS) { vkDestroyBuffer(vk_dev(), stg, NULL); vkFreeMemory(vk_dev(), sm, NULL); return 0; }

    if (!one_begin()) { vkUnmapMemory(vk_dev(), sm); vkDestroyBuffer(vk_dev(), stg, NULL); vkFreeMemory(vk_dev(), sm, NULL); return 0; }
    // 拷贝前的布局转换：新建是 UNDEFINED，目标 TRANSFER_DST
    // Layout transition before the copy: newly created is UNDEFINED, the target is TRANSFER_DST
    VkImageMemoryBarrier b0 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = e->img,
        .subresourceRange = { aspect, 0, e->levels, 0, e->layers },
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b0);

    VkDeviceSize off = 0; int ok = 1;
    for (uint32_t l = 0; l < e->levels && ok; l++) {
        uint32_t row_bytes = 0, rows = 0, slices = 0; uint64_t base = 0; unsigned th = 0, td = 0, bp = 0;
        if (nvn_deswizzle_geom(&t->d, l, 0, &row_bytes, &rows, &slices, &base, &th, &td, &bp)) { ok = 0; break; }
        for (uint32_t layer = 0; layer < e->layers && ok; layer++) {
            // 每层单独解排布：nvn_deswizzle 一次解一层一级 | deswizzle one layer of one level at a time
            if (nvn_deswizzle(&t->d, l, layer, t->mem, (uint8_t *)smap + off, row_bytes)) { ok = 0; break; }
            uint32_t w = e->w >> l, h = e->h >> l; if (!w) w = 1; if (!h) h = 1;
            VkBufferImageCopy r = { .bufferOffset = off, .bufferRowLength = 0, .bufferImageHeight = 0,
                .imageSubresource = { aspect, l, layer, 1 },
                .imageOffset = { 0, 0, 0 }, .imageExtent = { w, h, 1 } };
            vkCmdCopyBufferToImage(g_cb, stg, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            off += (VkDeviceSize)row_bytes * rows * (slices ? slices : 1);
        }
    }
    if (!ok) { static int said; if (!said++) LOG("【红】解排布失败：格式 %#x 目标 %d（见 nvn_deswizzle.c 的限制）", t->d.format, t->d.target); }
    VkImageMemoryBarrier b1 = b0;
    b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    int done = ok && one_end();

    vkUnmapMemory(vk_dev(), sm);
    vkDestroyBuffer(vk_dev(), stg, NULL);
    vkFreeMemory(vk_dev(), sm, NULL);
    if (done) { atomic_fetch_add(&g_uploads, 1); atomic_fetch_add(&g_upload_bytes, (uint64_t)total); e->lay = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    return done;
}

// 布局转换：只在真的变了时插屏障。回放层开通道前转成附件布局、收通道后转回可采样布局。
// Layout transition, inserting a barrier only when the layout really changes. The replay layer turns a target
// into an attachment layout before opening a pass and back into a sampleable layout after closing it.
void vk_tex_transition(const tex_t *t, VkImageLayout want, VkCommandBuffer cb) {
    if (!t || !cb || !t->serial) return;
    if (vk_tex_image(t) == VK_NULL_HANDLE) return;   // 先保证建好并上传（内部自己加锁，这里不能再持锁）| make sure it exists and is uploaded first (that path locks on its own, so no lock may be held here)
    pthread_mutex_lock(&g_mu);
    ent_t *e = find_slot(t->serial, 0);
    if (!e || !e->img || e->lay == want) { pthread_mutex_unlock(&g_mu); return; }
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = e->lay, .newLayout = want,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = e->img,
        .subresourceRange = { e->depth_like ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | (e->dsa ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT,
                              0, e->levels, 0, e->layers },
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, NULL, 0, NULL, 1, &b);
    e->lay = want;
    pthread_mutex_unlock(&g_mu);
}

void vk_tex_gpu_written(const tex_t *t) {
    if (!t || !t->serial) return;
    pthread_mutex_lock(&g_mu);
    ent_t *e = find_slot(t->serial, 1);
    // GPU 写过之后池就不再是真值：清掉重传标记，免得下一次取用把它又按池内容覆盖回去
    // Once the GPU has written it the pool is no longer the truth: clear the re-upload flag so the next use does
    // not overwrite it from the pool again
    if (e) { if (!e->img) e->serial = 0, g_n--; else { e->gpu_written = 1; e->dirty = 0; } }
    pthread_mutex_unlock(&g_mu);
}

int vk_tex_is_gpu_written(const tex_t *t) {
    int v = 0;
    if (!t || !t->serial) return 0;
    pthread_mutex_lock(&g_mu);
    ent_t *e = find_slot(t->serial, 0);
    if (e) v = e->gpu_written;
    pthread_mutex_unlock(&g_mu);
    return v;
}

static VkImage tex_of(const tex_t *t, VkImageView *view, VkImageAspectFlags *aspect) {
    if (!t || !t->serial || !t->mem) return VK_NULL_HANDLE;
    if (view) *view = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu);
    ent_t *e = find_slot(t->serial, 1);
    int need = !e->img || e->dirty;
    if (need) {
        if (!e->img) { if (!ent_build(t, e)) { e->serial = 0; g_n--; pthread_mutex_unlock(&g_mu); return VK_NULL_HANDLE; } }
        VkImageAspectFlags a = e->depth_like ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | (e->dsa ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
        if (!ent_upload(t, e, a)) LOG("【红】贴图上传失败 serial=%llu 格式 %#x", (unsigned long long)t->serial, t->d.format);
        e->dirty = 0;
    }
    if (aspect) *aspect = e->depth_like ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | (e->dsa ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
    if (view) *view = e->view;
    VkImage img = e->img;
    pthread_mutex_unlock(&g_mu);
    return img;
}

VkImage vk_tex_image(const tex_t *t) { return tex_of(t, NULL, NULL); }
VkImageView vk_tex_view(const tex_t *t) { VkImageView v; return tex_of(t, &v, NULL) ? v : VK_NULL_HANDLE; }

void vk_tex_invalidate(uint64_t serial) {
    pthread_mutex_lock(&g_mu);
    ent_t *e = find_slot(serial, 0);
    if (e) e->dirty = 1;
    pthread_mutex_unlock(&g_mu);
}

void vk_tex_drop(uint64_t serial) {
    pthread_mutex_lock(&g_mu);
    ent_t *e = find_slot(serial, 0);
    if (e) {
        if (e->view) vkDestroyImageView(vk_dev(), e->view, NULL);
        if (e->img) vkDestroyImage(vk_dev(), e->img, NULL);
        if (e->mem) vkFreeMemory(vk_dev(), e->mem, NULL);
        e->img = VK_NULL_HANDLE; e->view = VK_NULL_HANDLE; e->mem = VK_NULL_HANDLE;
        e->serial = 0;   // 开放寻址的删除：直接清键，查找会在下一个空位停住（同键后又插入时会重建表）| clearing the key is enough for open addressing here; lookups stop at the next empty slot
        g_n--;
    }
    pthread_mutex_unlock(&g_mu);
}

// CPU 读贴图：池内存是真值，解排布直读（与 hle_nvn.c 的注释一致）。区域 r 给出起点与范围（0 = 整级）。
// CPU texture read: pool memory is the truth, so deswizzle straight out of it (as hle_nvn.c's comment says).
// The region r gives the origin and extent (0 = the whole level).
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    if (!t || !t->mem || !dst) return;
    uint32_t row_bytes = 0, rows = 0, slices = 0; uint64_t base = 0; unsigned th = 0, td = 0, bpb = 0;
    if (nvn_deswizzle_geom(&t->d, level, layer0, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) {
        static int said; if (!said++) LOG("【红】ReadTexels：解排布几何不支持 格式 %#x 目标 %d 级 %u", t->d.format, t->d.target, level);
        return;
    }
    uint8_t *lin = malloc(row_bytes * (rows ? rows : 1));
    if (!lin) return;
    int bw = 1, bh = 1; nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t w = t->d.width >> level, h = t->d.height >> level; if (!w) w = 1; if (!h) h = 1;
    uint32_t x0 = r ? (uint32_t)r->xoffset : 0, y0 = r ? (uint32_t)r->yoffset : 0;
    uint32_t cw = r && r->width ? (uint32_t)r->width : w, ch = r && r->height ? (uint32_t)r->height : h;
    for (uint32_t layer = 0; layer < (r && r->depth ? (uint32_t)r->depth : 1); layer++) {
        if (nvn_deswizzle(&t->d, level, layer0 + layer, t->mem, lin, row_bytes)) break;
        uint32_t rb = (w + bw - 1) / bw * bpb;                    // 解排布后一行的字节 | bytes per deswizzled row
        uint32_t xb = x0 / bw * bpb, nb = (cw + bw - 1) / bw * bpb;
        for (uint32_t y = 0; y < ch && y0 + y < rows; y++)
            memcpy(dst + (size_t)layer * img + (size_t)y * row, lin + (size_t)(y0 + y) * rb + xb, nb);
    }
    free(lin);
    atomic_fetch_add(&g_reads, 1);
}

void vk_tex_stats(uint64_t *created, uint64_t *uploads, uint64_t *bytes, uint64_t *reads, uint64_t *fmt_miss) {
    if (created) *created = atomic_load(&g_created);
    if (uploads) *uploads = atomic_load(&g_uploads);
    if (bytes) *bytes = atomic_load(&g_upload_bytes);
    if (reads) *reads = atomic_load(&g_reads);
    if (fmt_miss) *fmt_miss = atomic_load(&g_fmt_miss);
}
