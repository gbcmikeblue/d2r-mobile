// NVN 贴图 → VkImage（安卓回放层）。设计取舍见 vk_tex.h 的头注释。
// NVN textures -> VkImage (the Android replay layer). See the header comment of vk_tex.h for the design
// trade-offs.
#include "vk_tex.h"
#include "vk_device.h"
#include "vk_region.h"
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
// mtl_addr：池地址 → (池缓冲, 池内偏移)。缓冲区贴图的视图直接建在池缓冲上，所以要它（见 vk_tex_bufview）。
// mtl_addr: pool address -> (pool buffer, in-pool offset). A buffer texture's view is built straight on the pool
// buffer, so it is needed here (see vk_tex_bufview).
#include "nvn_mtl.h"
#include "dm.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic uint64_t g_created, g_uploads, g_upload_bytes, g_reads, g_fmt_miss, g_reads_gw;

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
            dm_waitled_enter("vk：建采样器 VkSampler（客体线程上按需建）");
            VkResult smr = vkCreateSampler(vk_dev(), &ci, NULL, &sv);
            dm_waitled_leave();
            if (smr != VK_SUCCESS) {
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
    VkBufferView bv;          // 缓冲区贴图（target 10）的 texel buffer 视图：它与 img/view 互斥 | the texel-buffer view of a buffer texture (target 10): mutually exclusive with img/view
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
static VkCommandBuffer g_cb_up;   // 上传专用的那条（见下面"两条，各管一段"）| the upload-only one (see "two of them, one per section" below)

// 读回（以及对照清除）**必须串行**：读回会被**两个线程**同时发起 —— 出图/扫描在客体线程、查看器在前端线程 ——
// 而它们共用同一块常驻中转缓冲（`stg_get`，会销毁重建）与同一块命令缓冲（`g_cb`）。早先只锁了"队列提交"那一段，
// 于是"一边在录命令，另一边把中转缓冲销毁重建/映射"直接踩进未定义行为：真机现场是
// `CRASH sig=11 pc=vk_tex_grab_ser+0x7a0`（用到了一个已经销毁或还没映射成的指针）。这把锁覆盖从
// "拿中转缓冲"到"读完解映射"整段。
// **上传不在里面**：上传走**自己那条命令缓冲**（`g_cb_up`），所以它既不需要这把锁、也不会与读回相撞
//（为什么必须这样分，见下面"两条，各管一段"）。曾经把上传也纳进来，结果是锁序反转：调用方持着 `g_mu` 调上传，
// 而这把锁的顺序是 `one_mu → g_mu`，一拿就成环 —— 真机表现是"启动就停住、出图 0 次"（「M3-2n」第三节）。
// Readbacks (and the control clear) **must be serialised**: a readback can be started by **two threads** at once --
// present/scan on the guest thread, the viewer on the front-end thread -- and they share one resident staging buffer
// (`stg_get`, which destroys and recreates it) and one command buffer (`g_cb`). Only the "queue submit" stretch used to
// be locked, so "one side records commands while the other destroys/recreates/maps the staging buffer" went straight
// into undefined behaviour: the device's scene is `CRASH sig=11 pc=vk_tex_grab_ser+0x7a0` (a pointer that had been
// destroyed or never mapped). This lock spans "take the staging buffer" through "unmap after reading".
// **Uploads are not in it**: uploads run on **their own command buffer** (`g_cb_up`), so they neither need this lock nor
// collide with the readback (why the split is mandatory is in "two of them, one per section" below). Folding uploads in
// once produced a lock-order inversion -- the caller holds `g_mu` when it uploads while this lock orders as
// `one_mu -> g_mu`, which forms a cycle -- showing on the device as "a stall at boot with zero presents" (part three of
// "M3-2n").
static pthread_mutex_t g_one_mu = PTHREAD_MUTEX_INITIALIZER;

// ---------- M3-2v：读回路径的锁账本 ----------
// 「M3-2u」把"停住"缩到"查看器 + 逐级扫描同时读回"，并把现场指到"客体线程卡在宿主侧的一把锁 / futex 上"；
// 剩下的问题只有一个：**哪一把**。这里给 g_one_mu 记四样 —— 持有者 tid、持有人在做什么（hphase）、已持多久、
// 门外那位是谁、等了多久 —— 超过阈值各打一行日志：**不看转储、只看日志尾巴**也能读出"谁被谁挡住"。队列锁本来
// 就有这套（vk_queue_lock_state），栅栏那条在 nvn_vk.c 的 inflight_take 里另记（输出在 mtl_wait_state_dump 里）。
// M3-2v: a ledger for the read-back path. "M3-2u" narrowed the stall to "the viewer and the scan reading back at the
// same time" and pointed the scene at "the guest thread blocked on a host-side lock / futex"; the only question left is
// **which** one. This records four things about g_one_mu -- the holder's tid, what it is doing (hphase), how long it has
// held it, and who is outside waiting and for how long -- and logs a line past a threshold, so the tail of the log alone
// names the edge. The queue lock already has this (vk_queue_lock_state); the fence wait is accounted for separately in
// inflight_take (nvn_vk.c) and printed by mtl_wait_state_dump.
#define DM_LED_MS 300
static struct {
    _Atomic int holder; const char *volatile hphase; uint64_t since;          // 持有者及其在做什么 | the holder and what it is doing
    _Atomic int waiter; const char *volatile wphase; uint64_t wsince;         // 等在门外的那位及其目的 | who waits outside and why
} g_led_one;
static int led_tid(void) { return (int)pthread_gettid_np(pthread_self()); }

static void one_lock(const char *phase) {
    uint64_t t0 = now_ns();
    atomic_store(&g_led_one.waiter, led_tid()); g_led_one.wphase = phase; g_led_one.wsince = t0;
    pthread_mutex_lock(&g_one_mu);
    uint64_t waited = now_ns() - t0;
    atomic_store(&g_led_one.waiter, 0);
    atomic_store(&g_led_one.holder, led_tid()); g_led_one.hphase = phase; g_led_one.since = now_ns();
    if (waited > (uint64_t)DM_LED_MS * 1000000ull)
        LOG("[vk] 读回锁：等了 %llu 毫秒才拿到（本次要做：%s）", (unsigned long long)(waited / 1000000), phase ? phase : "?");
}
static void one_unlock(void) {
    uint64_t held = now_ns() - g_led_one.since;
    const char *ph = g_led_one.hphase;
    atomic_store(&g_led_one.holder, 0);
    pthread_mutex_unlock(&g_one_mu);
    if (held > (uint64_t)DM_LED_MS * 1000000ull)
        LOG("[vk] 读回锁：持有了 %llu 毫秒（做的事：%s）", (unsigned long long)(held / 1000000), ph ? ph : "?");
}

// 停住 / 按需转储时打几行：读回锁现在在谁手里、在做什么、多久了；门外是谁、等了多久。
// A few lines for a stall / on-demand dump: who holds the read-back lock, doing what, for how long; and who is outside
// waiting, for how long.
void vk_led_line(void) {
    int h = atomic_load(&g_led_one.holder), w = atomic_load(&g_led_one.waiter);
    uint64_t now = now_ns();
    if (h) LOG("[vk] 读回锁：持有者 tid %d，在做「%s」，已持 %llu 毫秒", h, g_led_one.hphase ? g_led_one.hphase : "?", (unsigned long long)((now - g_led_one.since) / 1000000));
    if (w) LOG("[vk] 读回锁：等它的人 tid %d（要做「%s」），已等 %llu 毫秒", w, g_led_one.wphase ? g_led_one.wphase : "?", (unsigned long long)((now - g_led_one.wsince) / 1000000));
    if (!h && !w) LOG("[vk] 读回锁：无人持有、没人等");
}

static uint64_t hsh(uint64_t k) { k *= 11400714819323198485ull; return k ^ (k >> 29); }

// ---------- 窗口（交换链）贴图的登记（见 vk_tex.h 的说明）----------
// ---------- Registration of window (swapchain) textures (see the note in vk_tex.h) ----------
static uint64_t g_win_ser[64];
static int g_nwin;

void vk_tex_mark_window_ser(uint64_t serial) {
    if (!serial) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nwin; i++) if (g_win_ser[i] == serial) { pthread_mutex_unlock(&g_mu); return; }
    if (g_nwin < 64) g_win_ser[g_nwin++] = serial;
    else { static int said; if (!said++) LOG("【黄】窗口贴图超过 %d 张：多出来的按普通贴图对待", 64); }
    pthread_mutex_unlock(&g_mu);
}

static int win_ser_locked(uint64_t serial) {
    for (int i = 0; i < g_nwin; i++) if (g_win_ser[i] == serial) return 1;
    return 0;
}

int vk_tex_is_window_ser(uint64_t serial) {
    pthread_mutex_lock(&g_mu);
    int v = win_ser_locked(serial);
    pthread_mutex_unlock(&g_mu);
    return v;
}

void vk_tex_dump_size(uint32_t w, uint32_t h) {
    pthread_mutex_lock(&g_mu);
    int shown = 0;
    for (int i = 0; i < g_cap && shown < 8; i++) {
        ent_t *e = &g_tab[i];
        if (!e->serial || !e->img || e->w != w || e->h != h) continue;
        LOG("[vk] 同尺寸（%ux%u）的已知贴图：序号 %llu、格式 %d、%s、%s", w, h, (unsigned long long)e->serial, (int)e->fmt,
            win_ser_locked(e->serial) ? "窗口贴图" : "普通贴图", e->gpu_written ? "GPU 写过" : "没被 GPU 写过");
        shown++;
    }
    if (!shown) LOG("[vk] 同尺寸（%ux%u）的已知贴图：一张都没有", w, h);
    pthread_mutex_unlock(&g_mu);
}

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

// ---------- 一次性命令缓冲：**两条，各管一段** ----------
// ---------- one-off command buffers: **two of them, one per section** ----------
// `g_cb` 给**读回与对照清除**，`g_cb_up` 给**上传**。为什么必须分开：读回会被**两个线程**同时发起（查看器 /
// 出图·扫描），所以那一段有自己的一把锁（`g_one_mu`）；而上传这条路的调用方 `tex_of` 正**持着 `g_mu`**，
// **不能**再拿读回那把锁 —— 那会形成锁序反转，真机表现是"**启动就停住、出图 0 次**"（「M3-2n」第三节）。
// 两条命令缓冲各管一段之后，"谁跟谁共用一块缓冲"这件事就不存在了；剩下要串行的只有"队列提交"，而那一段
// 本来就有队列锁。
// `g_cb` serves the **readback and the control clear**, `g_cb_up` serves **uploads**. Why they must be separate: a
// readback can be started by **two threads** at once (viewer / present-scan) so that section has its own lock
// (`g_one_mu`), while the upload path's caller `tex_of` holds **`g_mu`** and therefore **cannot** take that lock --
// doing so inverts the order, which on the device is "**a stall at boot with zero presents**" (part three of "M3-2n").
// With two command buffers there is no buffer sharing left to reason about, and the only thing that still needs
// serialising -- the queue submit -- has the queue lock already.
static int one_begin_on(VkCommandBuffer *slot) {
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return 0;
    if (!g_pool) {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk_queue_family() };
        if (vkCreateCommandPool(vk_dev(), &pci, NULL, &g_pool) != VK_SUCCESS) return 0;
    }
    if (!*slot) {
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        if (vkAllocateCommandBuffers(vk_dev(), &ai, slot) != VK_SUCCESS) { *slot = VK_NULL_HANDLE; return 0; }
    }
    if (vkResetCommandBuffer(*slot, 0) != VK_SUCCESS) return 0;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    return vkBeginCommandBuffer(*slot, &bi) == VK_SUCCESS;
}
static int one_end_on(VkCommandBuffer cb) {
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return 0;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    // 提交与"等队列空闲"是一件事（一次性命令缓冲的语义），整段拿队列锁（见 vk_device.h）
    // The submit and the queue-idle wait are one thing for a one-off command buffer, so the whole span takes the
    // queue lock (see vk_device.h)
    vk_queue_note("一次性提交 + 等空闲");
    vk_queue_lock();
    dm_waitled_enter("vk：一次性提交（读回 / 对照）");
    VkResult sr = vkQueueSubmit(vk_queue(), 1, &si, VK_NULL_HANDLE);
    dm_waitled_leave();
    if (sr != VK_SUCCESS) { vk_queue_unlock(); return 0; }
    // M3-2w：读回 / 对照走的就是这条"一次性提交 + 等空闲"，而它**可能跑在客体线程上**
    // （「M3-2u」的采样 PC：`vk_tex_view ← mtl_exec ← dm_gfx_suspend`）—— 所以它是"轮询式等待"的头号嫌疑之一，记账。
    // M3-2w: readback / control clear go through this "one-off submit + wait idle", and it **can run on a guest thread**
    // ("M3-2u" sample PC: `vk_tex_view <- mtl_exec <- dm_gfx_suspend`), which makes it a prime suspect for the "polling
    // wait" -- so it is recorded.
    dm_waitled_enter("一次性提交等空闲（读回 / 对照，等驱动，可能跑在客体线程上）");
    VkResult r = vkQueueWaitIdle(vk_queue());
    dm_waitled_leave();
    vk_queue_unlock();
    if (r != VK_SUCCESS) { LOG("【红】一次性提交等待失败 VkResult %d", (int)r); return 0; }
    return 1;
}
static int one_begin(void) { return one_begin_on(&g_cb); }        // 读回 / 对照清除 | readback / control clear
static int one_end(void) { return one_end_on(g_cb); }

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
    // M3-2w：建贴图/视图这几步也记账 —— 它们**跑在客体线程上**（命令流里第一次用到就建），而且真机上
    // "建不出来 / 上传失败"正是停住那一轮同时出现的东西；驱动在这些调用里等内存/等内部线程时，账本要能点名。
    // M3-2w: creating the image/view is recorded too -- it **runs on a guest thread** (built the first time the command
    // stream touches it), and on the device "cannot build / upload failed" is exactly what shows up in the stalled run;
    // if the driver waits for memory or an internal thread inside these calls, the ledger has to name it.
    dm_waitled_enter("vk：建 VkImage（贴图）");
    VkResult ires = vkCreateImage(vk_dev(), &ci, NULL, &e->img);
    dm_waitled_leave();
    if (ires != VK_SUCCESS) { LOG("【红】建 VkImage 失败 %ux%ux%u 格式 %d", e->w, e->h, e->d, (int)e->fmt); e->img = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(vk_dev(), e->img, &mr);
    // 优先要「设备本地 + 宿主可见 + 一致」：零拷贝内存（与内存池同一种），方便回读与调试
    // Prefer device-local + host-visible + coherent, the zero-copy type shared with the memory pools, which
    // keeps readback and debugging simple
    uint32_t mi = vk_memory_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mr.memoryTypeBits);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mi };
    dm_waitled_enter("vk：贴图内存分配");
    VkResult mres = vkAllocateMemory(vk_dev(), &mai, NULL, &e->mem);
    dm_waitled_leave();
    if (mres != VK_SUCCESS) { LOG("【红】贴图内存分配失败 %llu 字节", (unsigned long long)mr.size); return 0; }
    if (vkBindImageMemory(vk_dev(), e->img, e->mem, 0) != VK_SUCCESS) { LOG("【红】贴图内存绑定失败"); return 0; }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = e->img,
        .viewType = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D :
                    (iflags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? (e->layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) :
                    (e->layers > 1 ? (type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY) :
                     (type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D)),
        .format = e->fmt, .subresourceRange = { e->depth_like ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, e->levels, 0, e->layers } };
    if (e->depth_like) { vi.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT; if (!e->dsa) vi.subresourceRange.aspectMask &= ~(VkImageAspectFlags)VK_IMAGE_ASPECT_STENCIL_BIT; }
    dm_waitled_enter("vk：建 VkImageView（贴图视图）");
    VkResult vres = vkCreateImageView(vk_dev(), &vi, NULL, &e->view);
    dm_waitled_leave();
    if (vres != VK_SUCCESS) { LOG("【红】建 VkImageView 失败"); return 0; }
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
    // M3-2w：上传的"建中转缓冲 + 分配 + 映射"也记账 —— 这一段**每次上传都新建对象**，而真机上它正好伴生
    // 上百条「贴图上传失败」；`vkAllocateMemory` / `vkMapMemory` 在驱动里都可能久等（分配器 / 非一致映射）。
    // M3-2w: the upload's "create staging buffer + allocate + map" is recorded too -- this runs **per upload** and on the
    // device it is exactly what accompanies the hundreds of 「贴图上传失败」 lines; `vkAllocateMemory` / `vkMapMemory` can
    // both wait inside the driver (allocator / non-coherent mapping).
    dm_waitled_enter("vk：上传：建中转缓冲 + 分配 + 映射");
    if (vkCreateBuffer(vk_dev(), &bci, NULL, &stg) != VK_SUCCESS) { dm_waitled_leave(); return 0; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vk_dev(), stg, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mr.memoryTypeBits) };
    VkDeviceMemory sm = VK_NULL_HANDLE;
    if (vkAllocateMemory(vk_dev(), &mai, NULL, &sm) != VK_SUCCESS) { dm_waitled_leave(); vkDestroyBuffer(vk_dev(), stg, NULL); return 0; }
    vkBindBufferMemory(vk_dev(), stg, sm, 0);
    void *smap = NULL;
    if (vkMapMemory(vk_dev(), sm, 0, VK_WHOLE_SIZE, 0, &smap) != VK_SUCCESS) { dm_waitled_leave(); vkDestroyBuffer(vk_dev(), stg, NULL); vkFreeMemory(vk_dev(), sm, NULL); return 0; }
    dm_waitled_leave();

    // **上传走自己那条命令缓冲（`g_cb_up`），也不拿 `g_one_mu`**。两件都要：调用方 `tex_of` 正**持着 `g_mu`** 调进来，
    // 而读回那边是 `one_mu → g_mu` —— 上传若去拿读回那把锁就是锁序反转，真机表现是"**启动就停住、出图 0 次**"
    // （「M3-2n」第三节，踩过一次）；而"共用一块命令缓冲"这件事，由"各用各的"直接消掉。于是这里既不需要锁，
    // 也不会与读回那条路相撞。剩下要串行的只有队列提交，那一段本来就有队列锁。
    // **Uploads use their own command buffer (`g_cb_up`) and do not take `g_one_mu`.** Both are needed: the caller
    // `tex_of` holds **`g_mu`** when it calls in, while the readback path is `one_mu -> g_mu`, so an upload taking the
    // readback's lock would invert the order -- on the device, "**a stall at boot with zero presents**" (part three of
    // "M3-2n"; walked into once). And "sharing one command buffer" disappears by construction once each section has its
    // own, so this site needs no lock and cannot collide with the readback path. The only thing left to serialise is the
    // queue submit, which has the queue lock already.
    if (!one_begin_on(&g_cb_up)) { vkUnmapMemory(vk_dev(), sm); vkDestroyBuffer(vk_dev(), stg, NULL); vkFreeMemory(vk_dev(), sm, NULL); return 0; }
    // 拷贝前的布局转换：新建是 UNDEFINED，目标 TRANSFER_DST
    // Layout transition before the copy: newly created is UNDEFINED, the target is TRANSFER_DST
    VkImageMemoryBarrier b0 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = e->img,
        .subresourceRange = { aspect, 0, e->levels, 0, e->layers },
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(g_cb_up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b0);

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
            vkCmdCopyBufferToImage(g_cb_up, stg, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            off += (VkDeviceSize)row_bytes * rows * (slices ? slices : 1);
        }
    }
    if (!ok) { static int said; if (!said++) LOG("【红】解排布失败：格式 %#x 目标 %d（见 nvn_deswizzle.c 的限制）", t->d.format, t->d.target); }
    VkImageMemoryBarrier b1 = b0;
    b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(g_cb_up, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    int done = ok && one_end_on(g_cb_up);

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

// ---------- 贴图 → 贴图拷贝（见 vk_tex.h 的说明）----------
// ---------- Texture-to-texture copy (see the note in vk_tex.h) ----------
static _Atomic uint64_t g_t2t_n;

int vk_tex_copy_t2t(const tex_t *src, const view_t *sv, const NVNcopyRegion *sr,
                    const tex_t *dst, const view_t *dv, const NVNcopyRegion *dr, VkCommandBuffer cb) {
    if (!src || !dst || !cb || !src->serial || !dst->serial) return 0;
    if (src == dst && !sr && !dr) return 0;   // 同图同区：没有意义，也没有副作用 | same image, same region: nothing to do and no side effect
    // 两边都先备好图像。注意目标一侧也要建：拷贝要往里写，没有图像就无处可写——**这正是窗口纹理的处境**
    // （它从没被当渲染目标用过，所以到这一步之前它在 Vulkan 侧根本不存在）。
    // Prepare both images first. The destination needs building too: a copy writes into it and there is nowhere to
    // write without an image -- **which is exactly the window texture's situation** (never used as a render target, so
    // on the Vulkan side it does not exist yet at this point).
    if (!vk_tex_view(src, NULL)) {
        static int said; if (!said++) LOG("【红】T2T 拷贝：源贴图（序号 %llu）的图像建不出来", (unsigned long long)src->serial);
        return 0;
    }
    if (!vk_tex_view(dst, NULL)) {
        static int said; if (!said++) LOG("【红】T2T 拷贝：目标贴图（序号 %llu）的图像建不出来（多半是池里取不到内存）", (unsigned long long)dst->serial);
        return 0;
    }
    VkImage si = VK_NULL_HANDLE, di = VK_NULL_HANDLE;
    VkFormat sf = VK_FORMAT_UNDEFINED, df = VK_FORMAT_UNDEFINED;
    uint32_t sw = 0, sh = 0, sl = 0, dw = 0, dh = 0, dl = 0;
    pthread_mutex_lock(&g_mu);
    ent_t *es = find_slot(src->serial, 0), *ed = find_slot(dst->serial, 0);
    if (es && es->img) { si = es->img; sf = es->fmt; sw = es->w; sh = es->h; sl = es->levels; }
    if (ed && ed->img) { di = ed->img; df = ed->fmt; dw = ed->w; dh = ed->h; dl = ed->levels; }
    pthread_mutex_unlock(&g_mu);
    if (!si || !di) return 0;

    // 视图可以限定 mip：拷的是它限定的那一级（与渲染目标那边同一套读法）
    // A view can limit the mip level: the copy targets the level it names (the same reading as on the render-target side)
    uint32_t slv = (sv && sv->set_levels && sv->base_level > 0) ? (uint32_t)sv->base_level : 0;
    uint32_t dlv = (dv && dv->set_levels && dv->base_level > 0) ? (uint32_t)dv->base_level : 0;
    if (slv >= sl) slv = 0;
    if (dlv >= dl) dlv = 0;
    if (sf != df) {
        static int said;
        if (!said++) LOG("【黄】T2T 拷贝的两边格式不同（源 %d、目标 %d）：这一对拷贝跳过", (int)sf, (int)df);
        return 0;
    }
    // 区域：宽/高/深 <= 0 = 从偏移到边界；再按这一级的实际尺寸钳住（钳了就报，与渲染区那处同一个理由）
    // Region: width/height/depth <= 0 means from the offset to the edge, then clamped to that level's real size
    // (a clamp is reported, for the same reason as the render-area clamp)
    int32_t lw = (int32_t)(slv > 31 ? 1u : sw >> slv), lh = (int32_t)(slv > 31 ? 1u : sh >> slv);
    int32_t x = sr ? sr->xoffset : 0, y = sr ? sr->yoffset : 0, z = sr ? sr->zoffset : 0;
    int32_t w = sr && sr->width > 0 ? sr->width : lw - x, h = sr && sr->height > 0 ? sr->height : lh - y;
    int32_t d = sr && sr->depth > 0 ? sr->depth : 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (z < 0) z = 0;
    if (x >= lw) x = lw - 1;
    if (y >= lh) y = lh - 1;
    if (w > lw - x) w = lw - x;
    if (h > lh - y) h = lh - y;
    if (w <= 0 || h <= 0 || d <= 0) { static int said; if (!said++) LOG("【黄】T2T 拷贝的区域不成立（%d,%d %dx%d 深 %d，源这一级 %dx%d）", x, y, w, h, d, lw, lh); return 0; }
    int32_t dx = dr ? dr->xoffset : 0, dy = dr ? dr->yoffset : 0, dz = dr ? dr->zoffset : 0;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    if (dz < 0) dz = 0;

    // 布局转换由纹理层做（它记着当前布局）；两个调用都不在锁里——它们自己会加锁
    // The layout transitions go through the texture layer (which tracks the current layouts); neither call is made
    // under our lock because they take it themselves
    vk_tex_transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cb);
    vk_tex_transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cb);
    VkImageCopy rg = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, slv, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, dlv, 0, 1 },
        .srcOffset = { x, y, z }, .dstOffset = { dx, dy, dz }, .extent = { (uint32_t)w, (uint32_t)h, (uint32_t)d } };
    vkCmdCopyImage(cb, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, di, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    // 目标从这一刻起"池不是真值"：CPU 再读它（或它被当渲染目标）都得走驱动侧
    // From now on the destination's pool is not the truth: a CPU read of it (or its use as a render target) has to go
    // through the driver side
    vk_tex_gpu_written(dst);
    uint64_t n = atomic_fetch_add(&g_t2t_n, 1) + 1;
    // 序号一起报：**"这次拷贝是不是把画面搬进了出图纹理"**只能靠序号回答（真机上出图纹理是窗口贴图 5/6/7，
    // 而它全白、游戏自己的渲染目标全零——到底谁往谁里搬，看这一行）。每 300 次也报一次，用来判断它是每帧都拷
    // 还是一次性行为——只报前几次会把这两件事混起来。
    // The serials are reported too: **"did this copy move the picture into the present texture"** can only be answered by
    // serial (on the device the present texture is window texture 5/6/7, and it is all white while the game's own render
    // targets are all zero -- which one moves into which is what this line says). It is also reported every 300th time,
    // to tell "copies every frame" from "a one-off" -- reporting only the first few would conflate the two.
    if (n <= 5 || n % 300 == 0)
        LOG("[vk] T2T 拷贝第 %llu 次：序号 %llu → %llu，%ux%u（源级 %u、目标级 %u；目标%s窗口贴图）", (unsigned long long)n,
            (unsigned long long)src->serial, (unsigned long long)dst->serial, (unsigned)w, (unsigned)h, slv, dlv,
            vk_tex_is_window_ser(dst->serial) ? "是" : "不是");
    return 1;
}

uint64_t vk_tex_t2t_count(void) { return atomic_load(&g_t2t_n); }

// 半精度 → 单精度（读回浮点画面时用；标准做法，逐位展开）
// Half precision to single precision (used when reading back a float picture; the standard bit-by-bit expansion)
static float h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) & 1u, e = (uint32_t)(h >> 10) & 0x1fu, m = (uint32_t)h & 0x3ffu;
    if (e == 0) {                       // 非规格化：没有隐含的 1 | subnormal: no implicit 1
        if (!m) return s ? -0.0f : 0.0f;
        float f = (float)m * (1.0f / 1024.0f) * (1.0f / 16777216.0f);
        return s ? -f : f;
    }
    if (e == 31) return m ? 0.0f : (s ? -1.0f / 0.0f : 1.0f / 0.0f);   // 无穷 / NaN：NaN 当 0，免得污染统计 | inf / NaN (NaN as 0 so it does not pollute the statistics)
    float f = (1.0f + (float)m * (1.0f / 1024.0f)) * powf(2.0f, (float)((int)e - 15));
    return s ? -f : f;
}

// 无符号浮点（11/11/10 位那类）→ 单精度：5 位指数、偏置 15、没有符号位也没有隐含的 1。
// 这就是 VK_FORMAT_B10G11R11_UFLOAT_PACK32 的编码（名字里 B 在前，占最高 10 位；R 在最低 11 位）。
// Unsigned float (the 11/11/10-bit kind) to single precision: a 5-bit exponent with a bias of 15 and neither a sign
// bit nor an implicit 1. This is how VK_FORMAT_B10G11R11_UFLOAT_PACK32 is encoded (the name puts B first, occupying
// the top 10 bits, with R in the low 11).
static float uf(uint32_t v, int nbits) {
    int mb = nbits - 5;
    uint32_t e = v >> mb, m = v & ((1u << mb) - 1u);
    if (e == 0) return (float)m * (1.0f / 16777216.0f) * (1.0f / 16384.0f);   // 非规格化 | subnormal
    if (e == 31) return m ? 0.0f : 3.4e38f;                                    // NaN 当 0、无穷给个上限（后面会钳住）| NaN as 0, infinity as a ceiling (clamped later)
    return (1.0f + (float)m * (1.0f / (float)(1u << mb))) * powf(2.0f, (float)((int)e - 15));
}

// 中转缓冲缓存（读回用）：实测每张读回约 0.45 秒，大半花在分配/销毁上——而读回只发生在调试路径（出图截图、
// 扫描），所以留着反复用是划算的，尺寸不够时换一块大的。
// Cached staging buffer (for readbacks): a readback measures at about 0.45 s per texture, most of it in allocation and
// destruction, and readbacks only happen on debugging paths (present screenshots, scans), so keeping one around and
// reusing it pays; when it is too small it is replaced with a larger one.
static VkBuffer g_stg_buf;
static VkDeviceMemory g_stg_mem;
static VkDeviceSize g_stg_sz;

static int stg_get(VkDeviceSize need, VkBuffer *out_buf, VkDeviceMemory *out_mem) {
    if (!g_stg_buf || g_stg_sz < need) {
        if (g_stg_buf) {
            vkDestroyBuffer(vk_dev(), g_stg_buf, NULL);
            vkFreeMemory(vk_dev(), g_stg_mem, NULL);
            g_stg_buf = VK_NULL_HANDLE; g_stg_mem = VK_NULL_HANDLE; g_stg_sz = 0;
        }
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = need,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        if (vkCreateBuffer(vk_dev(), &bci, NULL, &g_stg_buf) != VK_SUCCESS) { g_stg_buf = VK_NULL_HANDLE; return 0; }
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk_dev(), g_stg_buf, &mr);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
            .memoryTypeIndex = vk_memory_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mr.memoryTypeBits) };
        if (vkAllocateMemory(vk_dev(), &mai, NULL, &g_stg_mem) != VK_SUCCESS || vkBindBufferMemory(vk_dev(), g_stg_buf, g_stg_mem, 0) != VK_SUCCESS) {
            if (g_stg_mem) { vkFreeMemory(vk_dev(), g_stg_mem, NULL); g_stg_mem = VK_NULL_HANDLE; }
            vkDestroyBuffer(vk_dev(), g_stg_buf, NULL);
            g_stg_buf = VK_NULL_HANDLE;
            return 0;
        }
        g_stg_sz = need;
    }
    *out_buf = g_stg_buf;
    *out_mem = g_stg_mem;
    return 1;
}

// ---------- 从驱动侧读回（见 vk_tex.h 的说明）----------
// ---------- Read back from the driver side (see the note in vk_tex.h) ----------
// 按序号读回：截图与诊断的工具面。不需要 tex_t —— 贴图层的入口里本来就有 img/fmt/w/h/levels/lay，而"游戏把画面
// 画进了哪一张"这种问题只能按序号问（那些贴图的 tex_t 不在回放层手上）。
// Read back by serial: the tool surface for screenshots and diagnostics. No tex_t is needed -- the texture layer's
// entry already holds img/fmt/w/h/levels/lay -- and a question like "which texture did the game draw the picture into"
// can only be asked by serial (the replay layer does not hold those textures' tex_t).
// 函数名带 `_locked`：**调用方必须已经持有 g_one_mu**（公开入口见文件下方那个薄封装）。
// The `_locked` suffix means **the caller must already hold g_one_mu** (the public entry is the thin wrapper below).
static int grab_ser_locked(uint64_t serial, unsigned level, uint8_t *dst, size_t pitch, uint32_t *out_w, uint32_t *out_h) {
    // 读不回就得说清为什么：这一层早退的路很多，静默早退过一次（真机上表现为"截图 0x0 失败"配一条黄红都没有），
    // 所以每条早退都留一句话。
    // A failed readback has to say why: this layer has many early exits and one of them was silent (on the device that
    // looked like "screenshot 0x0 failed" with no failure line anywhere), so every exit leaves a line behind.
    if (!serial || !dst) {
        LOG("【黄】读回：参数不成立（序号 %llu、目标 %p）——序号为 0 说明这个对象还没在宿主这边登记",
            (unsigned long long)serial, (const void *)dst);
        return 0;
    }
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) { LOG("【红】读回：设备还没建起来"); return 0; }

    VkImage img = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t iw = 0, ih = 0, levels = 0;
    VkImageLayout lay = VK_IMAGE_LAYOUT_UNDEFINED;
    int depth_like = 0;
    pthread_mutex_lock(&g_mu);
    ent_t *e = find_slot(serial, 0);
    if (e && e->img) {
        img = e->img; fmt = e->fmt; iw = e->w; ih = e->h; levels = e->levels;
        lay = e->lay; depth_like = e->depth_like || e->dsa;
    }
    pthread_mutex_unlock(&g_mu);
    if (!img) {
        // 入口不存在：这张贴图还没被 GPU 用过，真值在池里（直读那条路），不该走这里
        // No entry: the GPU has never touched this texture and the pool holds the truth (the direct path), so this is
        // the wrong way to read it
        LOG("【黄】从驱动侧读回一张还没被 GPU 用过的贴图（序号 %llu）：它是直读池的那一类", (unsigned long long)serial);
        return 0;
    }
    if (depth_like) {
        static int said;
        if (!said++) LOG("【黄】读回的是深度/模板贴图（序号 %llu）：截图只取颜色，跳过", (unsigned long long)serial);
        return 0;
    }
    // 能读的格式：8 位四通道（两种字节序）与几种"十六位/三十二位浮点或归一化"四通道。
    // 后者是**游戏真正的画面**所在——真机上 1600×900 的同尺寸贴图里有 8 张是浮点格式，而能直读的两张 8 位贴图
    // 全是白的；只支持 8 位就等于看不见画面。浮点的读回来要**压到可看的范围**（钳到 [0,1] 再取伽马），
    // 那是**为了看**的变换，不是数据本身，所以日志里会写明，并把原始峰值一起报出来。
    // Usable formats: 8-bit four-channel (both byte orders) and a few 16/32-bit float or normalised four-channel
    // ones. The latter are where **the game's real picture** lives: on the device eight of the eleven same-size
    // 1600x900 textures are float formats while the two 8-bit ones that can be read directly are pure white, so
    // supporting only 8-bit amounts to not seeing the picture at all. A float readback has to be **squeezed into a
    // viewable range** (clamp to [0,1] then gamma), which is a transform **for looking at it**, not the data itself,
    // so the log says as much and reports the raw peak alongside.
    int bgra = 0, src = 0;   // src: 0 = 8 位归一化、1 = 16 位浮点、2 = 16 位归一化、3 = 32 位浮点 | src: 0 = 8-bit normalised, 1 = 16-bit float, 2 = 16-bit normalised, 3 = 32-bit float
    switch (fmt) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SNORM: bgra = 0; src = 0; break;
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SNORM: bgra = 1; src = 0; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: src = 1; break;
        case VK_FORMAT_R16G16B16A16_UNORM: src = 2; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: src = 3; break;
        // 真机上 1600×900 的场景色就是这一个（编号 122）——查过 NDK 头文件再对的名字，不是猜的
        // On the device the 1600x900 scene colour is this one (number 122), a name checked against the NDK header
        // rather than guessed
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: src = 4; break;
        default: {
            static int said;
            if (!said++) LOG("【黄】读回不支持格式 %d：截图跳过（要看得见就得先支持它）", (int)fmt);
            return 0;
        }
    }
    uint32_t bpp = src == 0 || src == 4 ? 4u : src == 3 ? 16u : 8u;
    uint32_t lv = level < levels ? level : 0;
    uint32_t w = lv > 31 ? 1u : iw >> lv, h = lv > 31 ? 1u : ih >> lv;
    if (!w) w = 1;
    if (!h) h = 1;

    // 中转缓冲：宿主可见 + 一致，拷完直接映射读
    // Staging buffer: host-visible and coherent, mapped and read straight after the copy
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (!stg_get((VkDeviceSize)w * h * bpp, &buf, &mem)) { LOG("【红】读回：建中转缓冲失败（%llu 字节）", (unsigned long long)((VkDeviceSize)w * h * bpp)); return 0; }

    // 拷之前先把队列排空：要读的是 GPU 已经写完的画面（与苹果侧截图前那次 mtl_finish_gpu 同一个意思）
    // Drain the queue before copying: the point is to read a picture the GPU has finished (the same meaning as the
    // mtl_finish_gpu before Apple's screenshot)
    vk_queue_note("读回：等队列空闲");
    vk_queue_lock();
    dm_waitled_enter("读回：等队列空闲（等驱动做完在飞提交）");
    vkQueueWaitIdle(vk_queue());
    dm_waitled_leave();
    vk_queue_unlock();
    int ok = 0;
    if (one_begin()) {
        VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = lay, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img, .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
        vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy rg = { .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1 },
            .imageOffset = { 0, 0, 0 }, .imageExtent = { w, h, 1 } };
        vkCmdCopyImageToBuffer(g_cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rg);
        ok = one_end();
    }
    if (ok) {
        void *p = NULL;
        dm_waitled_enter("vk：映射中转缓冲（读回）");
        VkResult mrp = vkMapMemory(vk_dev(), mem, 0, VK_WHOLE_SIZE, 0, &p);
        dm_waitled_leave();
        if (mrp == VK_SUCCESS) {
            const uint8_t *s = (const uint8_t *)p;
            float peak = 0.0f;
            for (uint32_t y = 0; y < h; y++) {
                uint8_t *d = dst + (size_t)y * pitch;
                const uint8_t *r = s + (size_t)y * w * bpp;
                if (src == 0) {
                    if (bgra) for (uint32_t x = 0; x < w; x++) { d[x * 4 + 0] = r[x * 4 + 2]; d[x * 4 + 1] = r[x * 4 + 1]; d[x * 4 + 2] = r[x * 4 + 0]; d[x * 4 + 3] = r[x * 4 + 3]; }
                    else memcpy(d, r, (size_t)w * 4);
                } else for (uint32_t x = 0; x < w; x++) {
                    // 浮点/归一化 → 可看的 8 位：钳到 [0,1]、取 1/2.2 伽马（为了看，不是为了改数据），峰值单独记
                    // Float/normalised -> viewable 8-bit: clamp to [0,1] and apply a 1/2.2 gamma (for looking, not for
                    // changing the data), with the peak recorded separately
                    for (int c = 0; c < 3; c++) {
                        float v;
                        if (src == 1) v = h2f(*(const uint16_t *)(r + x * 8 + c * 2));
                        else if (src == 2) v = *(const uint16_t *)(r + x * 8 + c * 2) / 65535.0f;
                        else if (src == 4) {
                            // 一个 32 位字里三个无符号浮点：R 在最低 11 位、G 中间 11 位、B 最高 10 位
                            // Three unsigned floats in one 32-bit word: R in the low 11 bits, G the middle 11, B the
                            // top 10
                            uint32_t w32 = ((const uint32_t *)r)[x];
                            v = c == 0 ? uf(w32 & 0x7ffu, 11) : c == 1 ? uf((w32 >> 11) & 0x7ffu, 11) : uf((w32 >> 22) & 0x3ffu, 10);
                        }
                        else v = ((const float *)r)[x * 4 + c];
                        if (v > peak) peak = v;
                        if (!(v > 0.0f)) v = 0.0f;                 // NaN / 负数都当 0 | NaN and negatives count as zero
                        if (v > 1.0f) v = 1.0f;
                        d[x * 4 + c] = (uint8_t)(powf(v, 1.0f / 2.2f) * 255.0f + 0.5f);
                    }
                    d[x * 4 + 3] = 255;
                }
            }
            if (src) LOG("[vk] 读回格式 %d（浮点/归一化，%u 字节每像素）：按「钳到 [0,1] + 1/2.2 伽马」转成可看的图，原始峰值 %.3f",
                         (int)fmt, bpp, (double)peak);
            vkUnmapMemory(vk_dev(), mem);
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
        } else { ok = 0; LOG("【红】读回：中转缓冲映射不出（尺寸 %ux%u、%llu 字节）", w, h, (unsigned long long)((VkDeviceSize)w * h * bpp)); }
    } else {
        static int said;
        if (!said++) LOG("【红】读回：命令缓冲那一套没走通（格式 %d 尺寸 %ux%u）", (int)fmt, w, h);
    }
    // 拷完图像停在 TRANSFER_SRC：把布局记准，下一次转换才知道从哪来
    // The image stays in TRANSFER_SRC after the copy: record that layout so the next transition knows where from
    if (ok) {
        pthread_mutex_lock(&g_mu);
        ent_t *e2 = find_slot(serial, 0);
        if (e2 && e2->img == img) e2->lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        pthread_mutex_unlock(&g_mu);
    }
    return ok;   // 中转缓冲留在缓存里（见 stg_get）：下一个读回接着用 | the staging buffer stays cached (see stg_get) for the next readback
}

// 公开入口：整段读回**串行**（见 g_one_mu 的说明）。读回会被两个线程同时发起（查看器在前端线程、出图/扫描在
// 客体线程），而它们共用同一块命令缓冲与同一块常驻中转缓冲 —— 不串行就是上面那条崩溃。
// The public entry: the whole readback is **serialised** (see the note on g_one_mu). A readback can be started by two
// threads at once (the viewer on the front-end thread, present/scan on the guest thread) and they share one command
// buffer and one resident staging buffer -- not serialising them is exactly the crash described there.
int vk_tex_grab_ser(uint64_t serial, unsigned level, uint8_t *dst, size_t pitch, uint32_t *out_w, uint32_t *out_h) {
    one_lock("读回整段（抓中转缓冲 → 录制提交 → 等空闲 → 解映射）");
    int r = grab_ser_locked(serial, level, dst, pitch, out_w, out_h);
    one_unlock();
    return r;
}

// 带 tex_t 的旧入口：就是按序号那一支的薄封装（诊断分得更细一点，因为这里能报出对象与序号的关系）
// The older tex_t entry point: a thin wrapper over the serial version (with a slightly richer diagnostic, since here
// the relationship between the object and the serial can be reported)
int vk_tex_grab(const tex_t *t, unsigned level, uint8_t *dst, size_t pitch, uint32_t *out_w, uint32_t *out_h) {
    if (!t || !t->serial) {
        LOG("【黄】读回：这个贴图对象没有序号（对象 %p）：说明它还没在宿主这边登记过", (const void *)t);
        return 0;
    }
    return vk_tex_grab_ser(t->serial, level, dst, pitch, out_w, out_h);
}

// 某尺寸下的全部已知贴图序号（诊断用：把"画面进了哪一张"从猜变成问）
// Every known texture serial of a given size (diagnostics: turn "which one did the picture go into" from a guess into
// a question)
// 列出所有"已经有图像"的贴图（扫描用：不按尺寸筛，一次看全，省得漏掉真画面所在的那个尺寸）
// List every texture that already has an image (for a scan: no size filter, so the size the picture actually lives at
// cannot be missed)
int vk_tex_list(uint64_t *ser, uint32_t *ws, uint32_t *hs, uint32_t *lvs, int cap) {
    pthread_mutex_lock(&g_mu);
    int n = 0;
    for (int i = 0; i < g_cap && n < cap; i++) {
        ent_t *e = &g_tab[i];
        if (!e->serial || !e->img) continue;
        ser[n] = e->serial;
        ws[n] = e->w;
        hs[n] = e->h;
        lvs[n] = e->levels ? e->levels : 1;   // 级数：扫描逐级读要靠它（0 当 1，别把"还不知道"当成"没有") | the level count, which the level-by-level scan needs (0 counts as 1 rather than "unknown means none")
        n++;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

// ---------- 对照实验（见 vk_tex.h 的说明）----------
// ---------- The control experiment (see the note in vk_tex.h) ----------
int vk_tex_probe_clear(uint64_t serial, float r, float g, float b, float a) {
    if (!serial) return 0;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return 0;
    VkImage img = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    VkImageLayout lay = VK_IMAGE_LAYOUT_UNDEFINED;
    int depth_like = 0;
    pthread_mutex_lock(&g_mu);
    ent_t *e = find_slot(serial, 0);
    if (e && e->img) { img = e->img; w = e->w; h = e->h; lay = e->lay; depth_like = e->depth_like || e->dsa; }
    pthread_mutex_unlock(&g_mu);
    if (!img) {
        static int said;
        if (!said++) LOG("【黄】对照实验：序号 %llu 没有图像，清不了（这张贴图还没被用过）", (unsigned long long)serial);
        return 0;
    }
    if (depth_like) {
        static int said;
        if (!said++) LOG("【黄】对照实验：序号 %llu 是深度/模板贴图，vkCmdClearColorImage 用不得，跳过", (unsigned long long)serial);
        return 0;
    }
    vk_queue_note("对照实验：等队列空闲");
    vk_queue_lock();
    dm_waitled_enter("对照实验：等队列空闲（等驱动）");
    vkQueueWaitIdle(vk_queue());
    dm_waitled_leave();
    vk_queue_unlock();
    int ok = 0;
    one_lock("对照清除（等队列空闲 + 转布局）");   // 与读回 / 上传串行（见 g_one_mu）| serialised against readback / upload (see g_one_mu)
    if (one_begin()) {
        VkImageMemoryBarrier bar = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = lay, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img, .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
        vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
        VkClearColorValue c;
        c.float32[0] = r; c.float32[1] = g; c.float32[2] = b; c.float32[3] = a;
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        vkCmdClearColorImage(g_cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rng);
        ok = one_end();
    }
    one_unlock();
    if (ok) {
        pthread_mutex_lock(&g_mu);
        ent_t *e2 = find_slot(serial, 0);
        if (e2 && e2->img == img) { e2->lay = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; e2->gpu_written = 1; }
        pthread_mutex_unlock(&g_mu);
        LOG("[vk] 对照实验：序号 %llu（%ux%u）已清成 (%.2f, %.2f, %.2f, %.2f)", (unsigned long long)serial, w, h,
            (double)r, (double)g, (double)b, (double)a);
    } else {
        static int said;
        if (!said++) LOG("【红】对照实验：序号 %llu 的清除没走通（命令缓冲那一套）", (unsigned long long)serial);
    }
    return ok;
}

void vk_tex_dump_ser(uint64_t serial) {
    pthread_mutex_lock(&g_mu);
    ent_t *e = find_slot(serial, 0);
    if (!e || !e->img) {
        LOG("[vk] 序号 %llu：没有图像（没建过或已回收）", (unsigned long long)serial);
    } else {
        LOG("[vk] 序号 %llu：%ux%u 格式 %d、%u 级、%s、%s", (unsigned long long)serial, e->w, e->h, (int)e->fmt, e->levels,
            win_ser_locked(serial) ? "窗口贴图" : "普通贴图", e->gpu_written ? "GPU 写过" : "没被 GPU 写过");
    }
    pthread_mutex_unlock(&g_mu);
}

int vk_tex_same_size_serials(uint32_t w, uint32_t h, uint64_t *out, int cap) {
    pthread_mutex_lock(&g_mu);
    int n = 0;
    for (int i = 0; i < g_cap && n < cap; i++) {
        ent_t *e = &g_tab[i];
        if (!e->serial || !e->img || e->w != w || e->h != h) continue;
        out[n++] = e->serial;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
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
    dm_waitled_enter("vk：建子视图 VkImageView（view 重解释）");
    VkResult ivr = vkCreateImageView(vk_dev(), &vi, NULL, &iv);
    dm_waitled_leave();
    if (ivr != VK_SUCCESS) {
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
    // 窗口（交换链）贴图没有客体内存：存储由驱动持有，内容是渲染或拷贝进去的——所以它**照样要能建出图像**。
    // 不区分的话，渲染目标那一侧会静默跳过它（截图失败查出来的就是这条），画面永远到不了出图纹理上。
    // A window (swapchain) texture has no guest memory: the driver owns its storage and its content arrives by being
    // rendered or copied into it -- so it **must still get an image**. Without this distinction the render-target path
    // skips it in silence (which is what the failed screenshot uncovered) and the picture never reaches the present
    // texture.
    if (!t || !t->serial) return VK_NULL_HANDLE;
    if (!t->mem && !win_ser_locked(t->serial)) {
        static int said;
        if (!said++) LOG("【黄】贴图（序号 %llu）既没有客体内存、也没登记成窗口贴图：建不出图像", (unsigned long long)t->serial);
        return VK_NULL_HANDLE;
    }
    if (view) *view = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu);
    ent_t *e = find_slot(t->serial, 1);
    int need = !e->img || e->dirty;
    if (need) {
        if (!e->img) { if (!ent_build(t, e)) { e->serial = 0; g_n--; pthread_mutex_unlock(&g_mu); return VK_NULL_HANDLE; } }
        VkImageAspectFlags a = e->depth_like ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | (e->dsa ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
        if (t->mem) {
            if (!ent_upload(t, e, a)) LOG("【红】贴图上传失败 serial=%llu 格式 %#x", (unsigned long long)t->serial, t->d.format);
        } else {
            // 窗口贴图：池里没有真值（本来就没有池），内容只来自 GPU，所以进图就是"GPU 写过"那一档
            // A window texture: the pool holds no truth (there is no pool) and the content only ever comes from the
            // GPU, so it enters as "the GPU has written it"
            e->gpu_written = 1;
        }
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

// ---------- 缓冲区贴图（TARGET_BUFFER）→ VkBufferView ----------
// NVN 的缓冲区贴图把一段缓冲当"一维元素数组"采样：`width` 是元素个数、格式给每元素字节数。
// Vulkan 的对应物是 texel buffer：`VkBufferView` + `VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER`。
// 与苹果侧**同源**（nvn_mtl.m 的 tex_mtl 里那一支：直接在内存池缓冲上建贴图缓冲视图，零拷贝，
// GPU 执行时读池里的当前内容），但有两处不一样，都是 Vulkan 的规矩而不是选择：
//   ① 没有 bytesPerRow 的概念（texel buffer 本来就是一维元素数组），但**偏移有对齐硬要求**
//      （minTexelBufferOffsetAlignment），不满足时 vkCreateBufferView 直接失败（真机没有校验层，失败只看返回值）；
//   ② 可用格式受 VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT 约束，压缩格式与深度/模板格式一概不行。
// 为什么它必须单独一条路：`ent_build` 会拒掉 target 10（没有 VkImage 可言），所以这类贴图走图像那一侧
// 永远是空指针 —— 真机上的表现就是"贴图写空白"，而查找链其实是好的。
// ---------- buffer texture (TARGET_BUFFER) -> VkBufferView ----------
// An NVN buffer texture samples a stretch of a buffer as a one-dimensional array of elements: `width` is the element
// count and the format gives the bytes per element. Vulkan's counterpart is a texel buffer: a `VkBufferView` plus
// `VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER`. It is **the same idea** as the Apple side (the tex_mtl branch in
// nvn_mtl.m: build a texture-buffer view straight on the memory-pool buffer, zero copy, and the GPU reads the pool's
// current content at execution time), with two differences that are Vulkan's rules rather than choices:
//   1. there is no bytesPerRow (a texel buffer is a linear array of elements to begin with), but the **offset has a
//      hard alignment requirement** (minTexelBufferOffsetAlignment) and vkCreateBufferView simply fails otherwise
//      (the device has no validation layers, so the return code is all we get);
//   2. the format must carry VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT; compressed and depth/stencil formats cannot.
// Why it needs a path of its own: `ent_build` rejects target 10 (there is no VkImage to speak of), so going through
// the image side always yields a null handle -- which on the device shows up as a "texture blank" even though the
// lookup chain is perfectly sound.
static _Atomic uint64_t g_texel_made;
VkBufferView vk_tex_bufview(const tex_t *t) {
    if (!t || !t->serial || t->d.target != 10) return VK_NULL_HANDLE;
    vk_device_init();
    if (vk_dev() == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu);
    ent_t *e = find_slot(t->serial, 1);
    if (e->bv) { VkBufferView v = e->bv; pthread_mutex_unlock(&g_mu); return v; }

    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t elems = t->d.width ? t->d.width : 1;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    void *pbuf = NULL; uint64_t poff = 0;
    static int said_fmt, said_mem, said_align, said_feat, said_create;
    if (bpb <= 0 || bw != 1 || bh != 1) {
        if (!said_fmt++) LOG("【红】缓冲区贴图：格式 %#x 不能当线性元素数组（每块字节 %d、块 %dx%d）", t->d.format, bpb, bw, bh);
        goto fail;
    }
    if (!vk_fmt_to_vk(t->d.format, &fmt)) {
        if (!said_fmt++) LOG("【红】缓冲区贴图：格式 %#x 在 Vulkan 这边没有对应", t->d.format);
        goto fail;
    }
    if (fmt == VK_FORMAT_UNDEFINED || (fmt >= VK_FORMAT_D16_UNORM && fmt <= VK_FORMAT_D32_SFLOAT_S8_UINT)) {
        if (!said_fmt++) LOG("【红】缓冲区贴图：格式 %#x 是不透明的深度/模板格式", t->d.format);
        goto fail;
    }
    if (vk_phys()) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(vk_phys(), fmt, &fp);
        if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)) {
            if (!said_feat++) LOG("【红】缓冲区贴图：本设备的 %d 格式不支持 texel buffer（bufferFeatures %#x）", (int)fmt, (unsigned)fp.bufferFeatures);
            goto fail;
        }
    }
    // 存储必须落在某个内存池里——缓冲区贴图**没有副本**，视图指的就是池里的那份（与 NVN 语义一致）
    // The storage has to sit in one of the memory pools: a buffer texture has **no copy**, the view points straight at
    // the pool's data (which is what NVN means by it)
    if (!t->mem || !mtl_addr((uint64_t)(uintptr_t)t->mem, (uint64_t)bpb * elems, &pbuf, &poff) || !pbuf) {
        if (!said_mem++) LOG("【红】缓冲区贴图：存储 %p +%llu 字节不在任何内存池里（序号 %llu）",
                             t->mem, (unsigned long long)((uint64_t)bpb * elems), (unsigned long long)t->serial);
        goto fail;
    }
    {
        VkPhysicalDeviceProperties pp;
        uint64_t al = 0;
        if (vk_phys()) { vkGetPhysicalDeviceProperties(vk_phys(), &pp); al = pp.limits.minTexelBufferOffsetAlignment; }
        if (al && (poff % al)) {
            if (!said_align++) LOG("【黄】缓冲区贴图：池内偏移 %#llx 不是 %llu 的整数倍（序号 %llu）：本张建不出视图，按空白回退",
                                   (unsigned long long)poff, (unsigned long long)al, (unsigned long long)t->serial);
            goto fail;
        }
    }
    {
        VkBufferViewCreateInfo bvi = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .buffer = (VkBuffer)pbuf, .format = fmt, .offset = poff, .range = (uint64_t)bpb * elems };
        VkBufferView v = VK_NULL_HANDLE;
        dm_waitled_enter("vk：建缓冲区贴图视图 VkBufferView");
        VkResult r = vkCreateBufferView(vk_dev(), &bvi, NULL, &v);
        dm_waitled_leave();
        if (r != VK_SUCCESS) {
            // 最常见的两种原因是池缓冲没带 UNIFORM_TEXEL_BUFFER 用法位、或 range 超了 maxTexelBufferElements
            // The two usual causes are a pool buffer without the UNIFORM_TEXEL_BUFFER usage bit, or a range beyond
            // maxTexelBufferElements
            if (!said_create++) LOG("【红】建缓冲区贴图视图失败（VkResult %d）：格式 %d、偏移 %#llx、范围 %llu 字节、%u 个元素",
                                    (int)r, (int)fmt, (unsigned long long)poff, (unsigned long long)((uint64_t)bpb * elems), elems);
            goto fail;
        }
        e->bv = v;
        pthread_mutex_unlock(&g_mu);
        if (atomic_fetch_add(&g_texel_made, 1) < 4)
            LOG("【绿】缓冲区贴图落地（序号 %llu）：%u 个元素 × %d 字节、格式 %#x→%d、池偏移 %#llx（直接建在池缓冲上，零拷贝）",
                (unsigned long long)t->serial, elems, bpb, t->d.format, (int)fmt, (unsigned long long)poff);
        return v;
    }
fail:
    // 与 ent_build 失败同一套收尾：只清"本来就是为它开的"那个槽，别动已有图像/视图的条目
    // The same cleanup as an ent_build failure: clear only a slot opened for this texture, never one that already
    // holds an image or a view
    if (!e->img && !e->bv) { e->serial = 0; g_n--; }
    pthread_mutex_unlock(&g_mu);
    return VK_NULL_HANDLE;
}

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
        // M3-2w：拆贴图这一串（销毁视图/图像 + 释放内存）也记账 —— 驱动的**延迟销毁 / 内存回收**是少数会在
        // "看起来只是 CPU 操作"的调用里等 GPU 的地方（内存吃紧时 vkFreeMemory 会等），而它跑在客体线程上。
        // M3-2w: tearing a texture down (destroy views/image + free memory) is recorded too -- the driver's **deferred
        // destroy / memory reclamation** is one of the few places that waits for the GPU inside what looks like a pure
        // CPU call (vkFreeMemory waits when memory is tight), and it runs on guest threads.
        dm_waitled_enter("vk：拆贴图（销毁视图/图像 + 释放内存）");
        for (int i = 0; i < ENT_VIEWS; i++)
            if (e->vw[i].used && e->vw[i].iv) { vkDestroyImageView(vk_dev(), e->vw[i].iv, NULL); e->vw[i].used = 0; e->vw[i].iv = VK_NULL_HANDLE; }
        if (e->view) vkDestroyImageView(vk_dev(), e->view, NULL);
        if (e->bv) vkDestroyBufferView(vk_dev(), e->bv, NULL);
        if (e->img) vkDestroyImage(vk_dev(), e->img, NULL);
        if (e->mem) vkFreeMemory(vk_dev(), e->mem, NULL);
        dm_waitled_leave();
        e->img = VK_NULL_HANDLE; e->view = VK_NULL_HANDLE; e->bv = VK_NULL_HANDLE; e->mem = VK_NULL_HANDLE;
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
    // 量一个判据：游戏到底会不会读回**被 GPU 写过**的贴图？这一层现在对这类贴图仍走"池内存是真值"的直读路径，
    // 而 GPU 写过的贴图真值在 VkImage 里——那是"从 VkImage 读回"这套机制什么时候**必须**实现的开关（现在它只是
    // 调试工具）。苹果侧靠"是否预排布"隐式区分，安卓侧已有显式的 gpu_written 位（`vk_tex_gpu_written`，渲染目标
    // 处会置位），只是从没人查过；这里查一下并计数。
    // Measuring a judgment: does the game ever read back a **GPU-written** texture? This layer still takes the "pool
    // memory is the truth" direct path for those, while a GPU-written texture's truth lives in the VkImage -- that is
    // the switch for when "read back through the VkImage" **must** be implemented (right now it is only a debugging
    // tool). Apple distinguishes implicitly via "is it pre-laid-out"; Android already has an explicit gpu_written bit
    // (`vk_tex_gpu_written`, set on render targets) that simply nobody ever queried -- so query it and count.
    if (vk_tex_is_gpu_written(t)) atomic_fetch_add(&g_reads_gw, 1);
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

uint64_t vk_tex_reads_gpu_written(void) { return atomic_load(&g_reads_gw); }

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
