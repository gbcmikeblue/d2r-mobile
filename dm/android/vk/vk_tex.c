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
    // ---- 低段：单/双通道系列（0x01..0x1A）。真机实测遇到的缺项正在这一段（0x1 R8、0x6 R16、0xb R32UI、0xd RG8）——
    //      掩码图、查找表、高度图这类贴图都落在这里，游戏里很常见。
    // ---- the low range: single/dual-channel formats (0x01..0x1A). The gaps the device reported live exactly here
    //      (0x1 R8, 0x6 R16, 0xb R32UI, 0xd RG8) -- masks, lookup tables and height maps land here and are common.
    case 0x01: *out = VK_FORMAT_R8_UNORM; return 1;
    case 0x02: *out = VK_FORMAT_R8_SNORM; return 1;
    case 0x03: *out = VK_FORMAT_R8_UINT; return 1;
    case 0x04: *out = VK_FORMAT_R8_SINT; return 1;
    case 0x05: *out = VK_FORMAT_R16_SFLOAT; return 1;
    case 0x06: *out = VK_FORMAT_R16_UNORM; return 1;
    case 0x07: *out = VK_FORMAT_R16_SNORM; return 1;
    case 0x08: *out = VK_FORMAT_R16_UINT; return 1;
    case 0x09: *out = VK_FORMAT_R16_SINT; return 1;
    case 0x0A: *out = VK_FORMAT_R32_SFLOAT; return 1;
    case 0x0B: *out = VK_FORMAT_R32_UINT; return 1;
    case 0x0C: *out = VK_FORMAT_R32_SINT; return 1;
    case 0x0D: *out = VK_FORMAT_R8G8_UNORM; return 1;
    case 0x0E: *out = VK_FORMAT_R8G8_SNORM; return 1;
    case 0x0F: *out = VK_FORMAT_R8G8_UINT; return 1;
    case 0x10: *out = VK_FORMAT_R8G8_SINT; return 1;
    case 0x11: *out = VK_FORMAT_R16G16_SFLOAT; return 1;
    case 0x12: *out = VK_FORMAT_R16G16_UNORM; return 1;
    case 0x13: *out = VK_FORMAT_R16G16_SNORM; return 1;
    case 0x14: *out = VK_FORMAT_R16G16_UINT; return 1;
    case 0x15: *out = VK_FORMAT_R16G16_SINT; return 1;
    case 0x16: *out = VK_FORMAT_R32G32_SFLOAT; return 1;
    case 0x17: *out = VK_FORMAT_R32G32_UINT; return 1;
    case 0x18: *out = VK_FORMAT_R32G32_SINT; return 1;
    case 0x19: *out = VK_FORMAT_R8G8B8_UNORM; return 1;
    case 0x1A: *out = VK_FORMAT_R8G8B8_SNORM; return 1;
    // ---- UI2F / I2F（0x51..0x68）：数据按整数存、取样时转成浮点——Vulkan 的 **_USCALED / _SSCALED 正是这个语义**，
    //      不是近似值，所以照直映射。**但只有 8 / 16 位通道有这两个变体**：32 位的 UI2F / I2F 在 Vulkan 里
    //      没有对应物（映射成 UINT/SINT 会让着色器读到整数而不是浮点，那是在猜），所以照规矩报出来。
    // ---- UI2F / I2F (0x51..0x68): the data is stored as integers and converted to float when sampled -- Vulkan's
    //      **_USCALED / _SSCALED are exactly that semantic**, not an approximation, so they map straight across.
    //      **Those two variants only exist for 8- and 16-bit channels**, though: the 32-bit UI2F / I2F have no
    //      counterpart in Vulkan (mapping them to UINT/SINT would hand the shader integers where it expects floats,
    //      which is guessing), so they are reported instead.
    case 0x51: *out = VK_FORMAT_R8_USCALED; return 1;
    case 0x52: *out = VK_FORMAT_R8_SSCALED; return 1;
    case 0x53: *out = VK_FORMAT_R16_USCALED; return 1;
    case 0x54: *out = VK_FORMAT_R16_SSCALED; return 1;
    case 0x57: *out = VK_FORMAT_R8G8_USCALED; return 1;
    case 0x58: *out = VK_FORMAT_R8G8_SSCALED; return 1;
    case 0x59: *out = VK_FORMAT_R16G16_USCALED; return 1;
    case 0x5A: *out = VK_FORMAT_R16G16_SSCALED; return 1;
    case 0x5D: *out = VK_FORMAT_R8G8B8_USCALED; return 1;
    case 0x5E: *out = VK_FORMAT_R8G8B8_SSCALED; return 1;
    case 0x5F: *out = VK_FORMAT_R16G16B16_USCALED; return 1;
    case 0x60: *out = VK_FORMAT_R16G16B16_SSCALED; return 1;
    case 0x63: *out = VK_FORMAT_R8G8B8A8_USCALED; return 1;
    case 0x64: *out = VK_FORMAT_R8G8B8A8_SSCALED; return 1;
    case 0x65: *out = VK_FORMAT_R16G16B16A16_USCALED; return 1;
    case 0x66: *out = VK_FORMAT_R16G16B16A16_SSCALED; return 1;
    case 0x69: *out = VK_FORMAT_A2B10G10R10_SNORM_PACK32; return 1;
    case 0x6A: *out = VK_FORMAT_A2B10G10R10_SINT_PACK32; return 1;
    // 0x6B / 0x6C（RGB10A2_UI2F / I2F）：Vulkan 的 10:10:10:2 没有 scaled 变体，不猜，留给报出机制
    // 0x6B / 0x6C (RGB10A2_UI2F / I2F): Vulkan's 10:10:10:2 has no scaled variant, so it is not guessed and is left
    // to the reporting mechanism
    // ---- RGBX（0x6D..0x78）：X 通道不参与采样，按 RGBA 承接（与上面 0x37 同一个做法）----
    // ---- RGBX (0x6D..0x78): the X channel is not sampled, so it is taken as RGBA (the same treatment as 0x37 above)
    case 0x6D: *out = VK_FORMAT_R8G8B8A8_UNORM; return 1;
    case 0x6E: *out = VK_FORMAT_R8G8B8A8_SNORM; return 1;
    case 0x6F: *out = VK_FORMAT_R8G8B8A8_UINT; return 1;
    case 0x70: *out = VK_FORMAT_R8G8B8A8_SINT; return 1;
    case 0x71: *out = VK_FORMAT_R16G16B16A16_SFLOAT; return 1;
    case 0x72: *out = VK_FORMAT_R16G16B16A16_UNORM; return 1;
    case 0x73: *out = VK_FORMAT_R16G16B16A16_SNORM; return 1;
    case 0x74: *out = VK_FORMAT_R16G16B16A16_UINT; return 1;
    case 0x75: *out = VK_FORMAT_R16G16B16A16_SINT; return 1;
    case 0x76: *out = VK_FORMAT_R32G32B32A32_SFLOAT; return 1;
    case 0x77: *out = VK_FORMAT_R32G32B32A32_UINT; return 1;
    case 0x78: *out = VK_FORMAT_R32G32B32A32_SINT; return 1;
    // ---- ASTC（0x79..0x94）：Switch 贴图的主力压缩格式，而 Adreno 原生支持 ASTC —— 这几条映射直接决定贴图能不能出。
    //      14 种块尺寸 × (UNORM, SRGB)，一个不多一个不少。----
    // ---- ASTC (0x79..0x94): the Switch's main compressed texture format, and Adreno supports ASTC natively -- these
    //      mappings decide whether textures appear at all. Fourteen block sizes x (UNORM, SRGB), no more and no less.
    case 0x79: *out = VK_FORMAT_ASTC_4x4_UNORM_BLOCK; return 1;
    case 0x7A: *out = VK_FORMAT_ASTC_5x4_UNORM_BLOCK; return 1;
    case 0x7B: *out = VK_FORMAT_ASTC_5x5_UNORM_BLOCK; return 1;
    case 0x7C: *out = VK_FORMAT_ASTC_6x5_UNORM_BLOCK; return 1;
    case 0x7D: *out = VK_FORMAT_ASTC_6x6_UNORM_BLOCK; return 1;
    case 0x7E: *out = VK_FORMAT_ASTC_8x5_UNORM_BLOCK; return 1;
    case 0x7F: *out = VK_FORMAT_ASTC_8x6_UNORM_BLOCK; return 1;
    case 0x80: *out = VK_FORMAT_ASTC_8x8_UNORM_BLOCK; return 1;
    case 0x81: *out = VK_FORMAT_ASTC_10x5_UNORM_BLOCK; return 1;
    case 0x82: *out = VK_FORMAT_ASTC_10x6_UNORM_BLOCK; return 1;
    case 0x83: *out = VK_FORMAT_ASTC_10x8_UNORM_BLOCK; return 1;
    case 0x84: *out = VK_FORMAT_ASTC_10x10_UNORM_BLOCK; return 1;
    case 0x85: *out = VK_FORMAT_ASTC_12x10_UNORM_BLOCK; return 1;
    case 0x86: *out = VK_FORMAT_ASTC_12x12_UNORM_BLOCK; return 1;
    case 0x87: *out = VK_FORMAT_ASTC_4x4_SRGB_BLOCK; return 1;
    case 0x88: *out = VK_FORMAT_ASTC_5x4_SRGB_BLOCK; return 1;
    case 0x89: *out = VK_FORMAT_ASTC_5x5_SRGB_BLOCK; return 1;
    case 0x8A: *out = VK_FORMAT_ASTC_6x5_SRGB_BLOCK; return 1;
    case 0x8B: *out = VK_FORMAT_ASTC_6x6_SRGB_BLOCK; return 1;
    case 0x8C: *out = VK_FORMAT_ASTC_8x5_SRGB_BLOCK; return 1;
    case 0x8D: *out = VK_FORMAT_ASTC_8x6_SRGB_BLOCK; return 1;
    case 0x8E: *out = VK_FORMAT_ASTC_8x8_SRGB_BLOCK; return 1;
    case 0x8F: *out = VK_FORMAT_ASTC_10x5_SRGB_BLOCK; return 1;
    case 0x90: *out = VK_FORMAT_ASTC_10x6_SRGB_BLOCK; return 1;
    case 0x91: *out = VK_FORMAT_ASTC_10x8_SRGB_BLOCK; return 1;
    case 0x92: *out = VK_FORMAT_ASTC_10x10_SRGB_BLOCK; return 1;
    case 0x93: *out = VK_FORMAT_ASTC_12x10_SRGB_BLOCK; return 1;
    case 0x94: *out = VK_FORMAT_ASTC_12x12_SRGB_BLOCK; return 1;
    // ---- BGR 系列（0x95..0x9C）：Vulkan 有 BGR 变体，照直映射；只有 BGR5（0x96）没有对应物 ----
    // ---- the BGR series (0x95..0x9C): Vulkan has BGR variants and they map straight across; only BGR5 (0x96) has
    //      no counterpart
    case 0x95: *out = VK_FORMAT_B5G6R5_UNORM_PACK16; return 1;
    case 0x96: return 0;
    case 0x97: case 0x98: *out = VK_FORMAT_B5G5R5A1_UNORM_PACK16; return 1;
    case 0x99: case 0x9A: *out = VK_FORMAT_B8G8R8A8_UNORM; return 1;
    case 0x9B: case 0x9C: *out = VK_FORMAT_B8G8R8A8_SRGB; return 1;
    // 认不出来就不猜：报出来（设备日志里出现哪个就按那一个补），硬边界不变
    // Unrecognised formats are not guessed but reported (whichever shows up in a device log is added then); the hard
    // boundary is unchanged
    default: return 0;
    }
}

// ---------- 采样器：按**参数内容**缓存（不是按对象）----------
// ---------- samplers: cached by **parameter content**, not by object ----------
// 为什么不能按对象缓存：游戏按值拷贝 NVNsampler（nvn_obj.h 里 tex_t 的注释说得很清楚），同一个参数组合可能以
// 多个对象存在，按指针/序号当键会白建一堆 VkSampler。
// Why not by object: the game copies NVNsampler by value (the comment on tex_t in nvn_obj.h makes that plain), so one
// parameter set can exist as many objects and keying by pointer or serial would create a pile of redundant VkSamplers.
static VkSamplerAddressMode wrap_vk(uint8_t w) {
    switch (w) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;   // NVN 的 CLAMP 就是钳到边缘 | NVN's CLAMP means clamp to edge
        case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        // 2/3/4 = 镜像钳制族；4（镜像 + 到边界色）Vulkan 没有，退成到边缘并记一次
        // 2/3/4 = the mirror-clamp family; 4 (mirror + border colour) has no Vulkan form and degrades to edge, reported once
        case 2: case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case 4: { static int said; if (!said++) LOG("【黄】采样器环绕模式 4（镜像钳制到边界色）在 Vulkan 里没有对应：按镜像钳制到边缘"); return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; }
        case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // 7 = CLAMP_TO_EDGE | 7 = CLAMP_TO_EDGE
    }
}

#define SAMP_TAB 256
typedef struct { samp_par p; VkSampler s; int used; } samp_e;
static samp_e g_samps[SAMP_TAB];

VkSampler vk_samp_of(const samp_t *s) {
    if (!s) return VK_NULL_HANDLE;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    samp_par p = s->p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof p; i++) h = (h ^ ((const uint8_t *)&p)[i]) * 16777619u;
    h &= SAMP_TAB - 1;
    for (uint32_t k = 0; k < SAMP_TAB; k++) {
        samp_e *e = &g_samps[(h + k) & (SAMP_TAB - 1)];
        if (e->used && !memcmp(&e->p, &p, sizeof p)) return e->s;
        if (!e->used) {
            // minf 同时编码 min 过滤与 mip 过滤（0..5：0 最近邻、1 线性、2/3 加最近邻 mip、4/5 加线性 mip），
            // 与苹果侧同一套解读。
            // minf encodes both the min filter and the mip filter (0..5: 0 nearest, 1 linear, 2/3 with nearest mip,
            // 4/5 with linear mip), read the same way as on the Apple side.
            int lin_min = (p.minf == 1 || p.minf == 3 || p.minf == 5);
            int lin_mip = (p.minf == 4 || p.minf == 5);
            int aniso_ok = vk_have_anisotropy() && p.aniso > 1;
            VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter = p.magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                .minFilter = lin_min ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                .mipmapMode = lin_mip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = wrap_vk(p.ws), .addressModeV = wrap_vk(p.wt), .addressModeW = wrap_vk(p.wr),
                .mipLodBias = p.lod_bias,
                .anisotropyEnable = aniso_ok ? VK_TRUE : VK_FALSE,
                .maxAnisotropy = aniso_ok ? (p.aniso > 16 ? 16.0f : p.aniso) : 1.0f,
                .compareEnable = p.cmp_mode ? VK_TRUE : VK_FALSE,
                .compareOp = p.cmp_mode ? (VkCompareOp)(p.cmp_func >= 1 && p.cmp_func <= 8 ? p.cmp_func - 1 : 7) : VK_COMPARE_OP_NEVER,
                .minLod = p.lod_min < 0 ? 0.0f : p.lod_min,
                .maxLod = p.lod_max > 0 ? p.lod_max : VK_LOD_CLAMP_NONE,   // 0 = 没设，别把整条 mip 链钳到第 0 级 | 0 means unset, so do not clamp the whole mip chain to level 0
                .borderColor = p.border[3] > 0.5f ? (p.border[0] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK)
                                                  : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
            if (!vk_have_anisotropy() && p.aniso > 1) {
                static int said; if (!said++) LOG("【黄】设备不支持各向异性过滤：采样器按 1x 建");
            }
            VkSampler sv = VK_NULL_HANDLE;
            if (vkCreateSampler(vk_dev(), &ci, NULL, &sv) != VK_SUCCESS) {
                LOG("【红】建采样器失败（minf=%u magf=%u 环绕 %u/%u/%u 比较 %u）", p.minf, p.magf, p.ws, p.wt, p.wr, p.cmp_mode);
                return VK_NULL_HANDLE;
            }
            e->p = p; e->s = sv; e->used = 1;
            return sv;
        }
    }
    LOG("【红】采样器缓存满了（%d 项）", SAMP_TAB);
    return VK_NULL_HANDLE;
}

// ---------- serial → 贴图表（开放寻址，键是序号不是指针）----------
// ---------- serial -> texture table (open addressing; the key is the serial, not a pointer) ----------
#define ENT_VIEWS 4   // 每张贴图最多缓存几张子视图（按 view 内容区分）| at most this many sub-views cached per texture
// view 生效的证据：分别数"用底视图"、"命中已建子视图"、"新建子视图"——三者一起看才知道 view 这条路真的在跑。
// Evidence that views are in effect: count "base view", "sub-view cache hit" and "sub-view created" separately; only
// together do they show the view path is really running.
static _Atomic uint64_t g_vw_base, g_vw_hit, g_vw_made;
typedef struct {
    uint64_t serial;
    VkImage img; VkImageView view; VkDeviceMemory mem;
    VkFormat fmt; uint32_t w, h, d, levels, layers;
    int depth_like, dsa;      // dsa：深度模板合并（D24S8 / D32FS8）| dsa: combined depth-stencil
    int dirty;                // 池被 CPU 改过，下次取用前重传 | pool changed on the CPU; re-upload before the next use
    int gpu_written;          // GPU 写过：池不再是真值，CPU 读回要走 VkImage | the GPU has written it: the pool is no longer the truth and a CPU readback must go through the VkImage
    VkImageLayout lay;        // 当前布局（转换只在这一层发生）| current layout (transitions happen only here)
    // 按 view 缓存的多张视图（见 vk_tex.h 的说明）：`view` 是底视图，这里是它的"子视图"。
    // 一个贴图被注册成多少个 view 由游戏决定，实测很少（多数 0~2 个），所以用定长小数组，不做哈希表。
    // Several views cached per view (see the note in vk_tex.h): `view` is the base view and these are its sub-views.
    // How many views one texture is registered with is up to the game and measured to be few (0-2 usually), so a
    // small fixed array beats a hash table here.
    struct { uint8_t used; view_t v; VkImageView iv; } vw[ENT_VIEWS];
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
    // MUTABLE_FORMAT 是 view 重解释格式的前提（view_for 会按 set_format 建子视图）；3D 贴图还要
    // 2D_ARRAY_COMPATIBLE，否则"3D 当 2D 数组看"这种 view 建不出来。这两个位必须在建图时就给，因为
    // view 是后来才注册的。
    // MUTABLE_FORMAT is the precondition for a view reinterpreting the format (view_for builds sub-views from
    // set_format); a 3D texture also needs 2D_ARRAY_COMPATIBLE, without which a "3D seen as a 2D array" view cannot
    // be created. Both bits must be given at image creation because views are registered later.
    if (type == VK_IMAGE_TYPE_3D) iflags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    iflags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
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

// 按 view 取（必要时建）视图。view 为空、或它一个"限定/重解释"位都没设，就是底视图。
// target 的取值与 ent_build 的 switch 同一套（0=1D、1=2D、2=3D、3=1D 数组、4=2D 数组、8=Cube、9=Cube 数组）。
// Get (creating if needed) a view. An empty view, or one that sets none of the "limit/reinterpret" bits, is the base
// view. `target` uses the same values as ent_build's switch (0 = 1D, 1 = 2D, 2 = 3D, 3 = 1D array, 4 = 2D array,
// 8 = cube, 9 = cube array).
static VkImageView view_for(ent_t *e, const view_t *v) {
    if (!v || !(v->set_levels || v->set_layers || v->set_format || v->set_target)) { atomic_fetch_add(&g_vw_base, 1); return e->view; }
    for (int i = 0; i < ENT_VIEWS; i++)
        if (e->vw[i].used && !memcmp(&e->vw[i].v, v, sizeof *v)) { atomic_fetch_add(&g_vw_hit, 1); return e->vw[i].iv; }
    int slot = -1;
    for (int i = 0; i < ENT_VIEWS && slot < 0; i++) if (!e->vw[i].used) slot = i;
    if (slot < 0) {
        static int said;
        if (!said++) LOG("【黄】贴图 serial=%llu 的子视图超过 %d 个：多出来的按底视图用", (unsigned long long)e->serial, ENT_VIEWS);
        return e->view;
    }
    VkFormat fmt = e->fmt;
    if (v->set_format) {
        VkFormat f2 = VK_FORMAT_UNDEFINED;
        if (vk_fmt_to_vk(v->format, &f2) && f2 != VK_FORMAT_UNDEFINED) fmt = f2;
        else {
            static int said;
            if (!said++) LOG("【黄】view 的格式 %d 在 Vulkan 这边没有对应：这张子视图按底图格式建", v->format);
        }
    }
    VkImageViewType vt = VK_IMAGE_VIEW_TYPE_2D;
    switch (v->set_target ? v->target : -1) {
        case 0: vt = VK_IMAGE_VIEW_TYPE_1D; break;
        case 1: vt = VK_IMAGE_VIEW_TYPE_2D; break;
        case 2: vt = VK_IMAGE_VIEW_TYPE_3D; break;
        case 3: vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
        case 4: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case 8: vt = VK_IMAGE_VIEW_TYPE_CUBE; break;
        case 9: vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
        default: break;   // 没设 target 就用 2D（下面按底图类型兜底）| no target means 2D (the base type backs it up below)
    }
    if (!v->set_target) vt = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t base = (v->set_levels && v->base_level > 0) ? (uint32_t)v->base_level : 0;
    if (base >= e->levels) base = 0;
    uint32_t nl = (v->set_levels && v->nlevels > 0) ? (uint32_t)v->nlevels : e->levels - base;
    if (base + nl > e->levels) nl = e->levels - base;
    uint32_t lb = (v->set_layers && v->min_layer > 0) ? (uint32_t)v->min_layer : 0;
    if (lb >= e->layers) lb = 0;
    uint32_t ln = (v->set_layers && v->nlayers > 0) ? (uint32_t)v->nlayers : e->layers - lb;
    if (lb + ln > e->layers) ln = e->layers - lb;
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = e->img,
        .viewType = vt, .format = fmt,
        .subresourceRange = { e->depth_like ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, base, nl, lb, ln } };
    if (e->depth_like) {
        if (e->dsa) vi.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        else vi.subresourceRange.aspectMask &= ~(VkImageAspectFlags)VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    VkImageView iv = VK_NULL_HANDLE;
    if (vkCreateImageView(vk_dev(), &vi, NULL, &iv) != VK_SUCCESS) {
        static int said;
        if (!said++) LOG("【红】建子视图失败（serial=%llu、格式 %d、viewType %d、级别 %u+%u、层 %u+%u）",
                         (unsigned long long)e->serial, (int)fmt, (int)vt, base, nl, lb, ln);
        return e->view;
    }
    e->vw[slot].used = 1;
    e->vw[slot].v = *v;
    e->vw[slot].iv = iv;
    atomic_fetch_add(&g_vw_made, 1);
    return iv;
}

static VkImage tex_of(const tex_t *t, VkImageView *view, VkImageAspectFlags *aspect, const view_t *vw) {
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
    if (view) *view = view_for(e, vw);
    VkImage img = e->img;
    pthread_mutex_unlock(&g_mu);
    return img;
}

VkImage vk_tex_image(const tex_t *t) { return tex_of(t, NULL, NULL, NULL); }
VkImageView vk_tex_view(const tex_t *t, const view_t *v) { VkImageView view; return tex_of(t, &view, NULL, v) ? view : VK_NULL_HANDLE; }

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
        for (int i = 0; i < ENT_VIEWS; i++)
            if (e->vw[i].used && e->vw[i].iv) { vkDestroyImageView(vk_dev(), e->vw[i].iv, NULL); e->vw[i].used = 0; e->vw[i].iv = VK_NULL_HANDLE; }
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

void vk_tex_view_stats(uint64_t *base, uint64_t *hit, uint64_t *made) {
    if (base) *base = atomic_load(&g_vw_base);
    if (hit) *hit = atomic_load(&g_vw_hit);
    if (made) *made = atomic_load(&g_vw_made);
}

void vk_tex_stats(uint64_t *created, uint64_t *uploads, uint64_t *bytes, uint64_t *reads, uint64_t *fmt_miss) {
    if (created) *created = atomic_load(&g_created);
    if (uploads) *uploads = atomic_load(&g_uploads);
    if (bytes) *bytes = atomic_load(&g_upload_bytes);
    if (reads) *reads = atomic_load(&g_reads);
    if (fmt_miss) *fmt_miss = atomic_load(&g_fmt_miss);
}
