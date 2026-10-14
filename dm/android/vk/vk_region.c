// 内存换轨：把客体地址空间的一段换成 Vulkan 零拷贝内存（分配可导出内存 → 取 fd → mmap 回原地址）。
// Memory overlay: turning a piece of the guest address space into Vulkan zero-copy memory (allocate
// exportable memory, take its fd, mmap it back at the same address).
#include "vk_region.h"
#include "vk_device.h"
#include "dm.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// 真机上量出来的硬边界（docs/ANDROID_PLAN 的 M2 第 3 步）：单次分配 3968 MB 成、4096 MB 起 OOM。
// 顶到上限时按 128 MB 往下退，直到成功。
// The hard boundary measured on the real device (M2 step 3 in docs/ANDROID_PLAN): 3968 MB succeeds for a
// single allocation and 4096 MB and up fail with OOM. When it hits the limit, step down by 128 MB until it
// succeeds.
#define STEP_DOWN (128ull << 20)
#define MIN_USEFUL (256ull << 20)
#define MAX_REGION 8

static struct {
    uint8_t *base; size_t size; VkDeviceMemory mem; VkBuffer owner;
} g_reg[MAX_REGION];
static int g_nreg;
static uint64_t g_bytes;

// 这片「专属」缓冲只是导出句柄的载体：这种句柄标了 DEDICATED_ONLY，规范要求一份分配专属一块缓冲，
// 而不链它的话 vkGetMemoryFdKHR 会直接失败（探针里实测过）。池子的真身是后面按偏移绑进来的那些缓冲。
// 用法只给传输位：这载体不会被真的读写，而带上 STORAGE_BUFFER 会撞上驱动的 maxStorageBufferRange 上限
// （真机上实测：4 GB 的载体缓冲建不出来，见下面 vkCreateBuffer 失败时要往下退）。
// This "owner" buffer only exists to carry the export handle: the handle is marked DEDICATED_ONLY, the spec
// wants one allocation dedicated to one buffer, and vkGetMemoryFdKHR simply fails without it (measured in
// the probe). The pools themselves are the buffers bound at offsets into this allocation later on.
// Its usage is transfer-only: the carrier is never really read or written, and asking for STORAGE_BUFFER
// runs into the driver's maxStorageBufferRange limit (measured on the real device: a 4 GB carrier buffer
// cannot be created, hence the step-down on vkCreateBuffer failure below).
static VkBuffer make_owner(uint64_t size, uint32_t *type_bits) {
    *type_bits = 0;
    VkExternalMemoryBufferCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                             .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk_dev(), &bci, NULL, &b) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk_dev(), b, &mr);
    *type_bits = mr.memoryTypeBits;
    return b;
}

size_t vk_region_overlay(void *at, size_t size, const char *what) {
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) { LOG("【红】%s 内存换轨：Vulkan 设备没起来", what); return 0; }
    if (g_nreg >= MAX_REGION) { LOG("【红】%s 内存换轨：段数已满（%d）", what, MAX_REGION); return 0; }
    if (!vk_have_ext("VK_KHR_external_memory_fd")) {
        LOG("【红】%s 内存换轨：设备没有 VK_KHR_external_memory_fd，无法把 Vulkan 内存摆到客体地址上", what);
        return 0;
    }
    if (vk_zero_copy_type() == UINT32_MAX) { LOG("【红】%s 内存换轨：没有零拷贝内存类型", what); return 0; }

    PFN_vkGetMemoryFdKHR getfd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(vk_dev(), "vkGetMemoryFdKHR");
    if (!getfd) { LOG("【红】%s 内存换轨：取不到 vkGetMemoryFdKHR", what); return 0; }

    const size_t pg = (size_t)getpagesize();
    uint64_t try = size & ~(uint64_t)(pg - 1);          // 映射长度要整页 | the mapping length must be whole pages
    for (; try >= MIN_USEFUL; try -= STEP_DOWN) {
        uint32_t bits = 0;
        VkBuffer owner = make_owner(try, &bits);
        if (owner == VK_NULL_HANDLE) {
            // 建不出这么大的缓冲（真机上 4 GB 撞 maxStorageBufferRange）：退一档再试
            // The buffer cannot be created this large (4 GB hits maxStorageBufferRange on the real device):
            // step down and retry
            if (try > MIN_USEFUL) continue;
            LOG("【红】%s 内存换轨：建载体缓冲失败（%llu MB）", what, (unsigned long long)(try >> 20));
            return 0;
        }
        uint32_t mi = vk_memory_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, bits);
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vk_dev(), owner, &mr);

        VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = owner };
        VkExportMemoryAllocateInfo exp = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                           .pNext = &ded, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &exp,
                                     .allocationSize = mr.size, .memoryTypeIndex = mi };
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkResult r = vkAllocateMemory(vk_dev(), &mai, NULL, &mem);
        if (r != VK_SUCCESS) {
            vkDestroyBuffer(vk_dev(), owner, NULL);
            if (try > MIN_USEFUL) continue;             // 顶到上限：退 128 MB 再试 | hit the limit: step down and retry
            LOG("【红】%s 内存换轨：分配失败 VkResult %d", what, (int)r);
            return 0;
        }

        VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem,
                                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        int fd = -1;
        r = getfd(vk_dev(), &gi, &fd);
        if (r != VK_SUCCESS) {
            vkFreeMemory(vk_dev(), mem, NULL); vkDestroyBuffer(vk_dev(), owner, NULL);
            LOG("【红】%s 内存换轨：取 fd 失败 VkResult %d", what, (int)r);
            return 0;
        }
        void *p = mmap(at, try, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        close(fd);                                      // 映射已经拿着引用，fd 可以关 | the mapping holds its own reference, the fd can go
        if (p != at) {
            if (p != MAP_FAILED) munmap(p, try);
            vkFreeMemory(vk_dev(), mem, NULL); vkDestroyBuffer(vk_dev(), owner, NULL);
            LOG("【红】%s 内存换轨：mmap 到 %p 失败（要 %p，errno=%d）", what, p, at, errno);
            return 0;
        }
        g_reg[g_nreg].base = at; g_reg[g_nreg].size = try;
        g_reg[g_nreg].mem = mem; g_reg[g_nreg].owner = owner;
        g_nreg++; g_bytes += try;
        if (try < size)
            LOG("【黄】%s 内存换轨：只换成 %llu MB（要 %llu MB）——顶到单次分配上限，报给客体的上限已相应下调",
                what, (unsigned long long)(try >> 20), (unsigned long long)(size >> 20));
        else
            LOG("【绿】%s 内存换轨：%llu MB 已是 GPU 直接可见（%p，类型 %u）",
                what, (unsigned long long)(try >> 20), at, mi);
        return (size_t)try;
    }
    LOG("【红】%s 内存换轨：连 %llu MB 都要不到", what, (unsigned long long)(MIN_USEFUL >> 20));
    return 0;
}

size_t vk_region_use(void *at, size_t size, const char *what) { return vk_region_overlay(at, size, what); }

int vk_region_find(uint64_t addr, uint64_t len, VkDeviceMemory *mem, uint64_t *off) {
    for (int i = 0; i < g_nreg; i++) {
        uint64_t b = (uint64_t)(uintptr_t)g_reg[i].base;
        if (addr >= b && addr + len <= b + g_reg[i].size) {
            if (mem) *mem = g_reg[i].mem;
            if (off) *off = addr - b;
            return 1;
        }
    }
    return 0;
}

void vk_region_stats(uint64_t *bytes, int *regions) {
    if (bytes) *bytes = g_bytes;
    if (regions) *regions = g_nreg;
}
