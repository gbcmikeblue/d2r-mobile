// SPIR-V 绑定扫描器的实现，见 vk_spv.h 的文件头（为什么必须以 SPIR-V 为权威）。
// Implementation of the SPIR-V binding scanner; see the header of vk_spv.h for why the SPIR-V must be authoritative.
//
// 只走声明部分：SPIR-V 的布局是「调试信息 / 注解 / 类型 / 变量 / 函数」——所有 OpDecorate、OpType*、OpVariable
// 都在第一个 OpFunction 之前，所以扫到函数体就停。这既够用又避免了走指令流时的任何不同步风险。
// Only the declaration part is walked: a SPIR-V module is laid out as debug info / annotations / types / variables /
// functions, so every OpDecorate, OpType* and OpVariable precedes the first OpFunction -- stopping at the first
// function is both sufficient and free of any desynchronisation risk while walking the instruction stream.
#include "vk_spv.h"
#include <string.h>

#define SPV_MAGIC 0x07230203u
// id 上限：SPIR-V 的 id 稠密、从 1 起，越大的着色器 id 越多。早先取 4096，真机上就吃到了教训——
// 大着色器里 b0 那个块的类型 id 超过 4096，于是被当成"认不出来的种类"（日志里每份着色器正好一条黄）。
// 按模块头里声明的 bound 检查，不足就报错而不是静默丢 id。
// id limit: SPIR-V ids are dense and start at 1, and bigger shaders have more of them. The earlier value of 4096
// taught a lesson on the device -- in large shaders the type id of that b0 block exceeds 4096 and was therefore
// treated as an unrecognised kind (exactly one yellow line per shader in the log). The module header's declared
// bound is checked and a shortfall is an error instead of ids being silently dropped.
#define MAX_IDS 65536

enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28,
       OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32,
       OP_TYPE_FLOAT = 21, OP_TYPE_INT = 22, OP_TYPE_VECTOR = 23, OP_CONSTANT = 43,
       OP_DECORATE = 71, OP_VARIABLE = 59, OP_FUNCTION = 54 };

enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34, DEC_LOCATION = 30 };

// 输入变量（顶点属性）暂存：Location、结果类型 id，以及该类型是标量还是向量（分量类型取自标量声明）
// Staging for input variables (vertex attributes): the Location, the result type id, and whether that type is a
// scalar or a vector (the component type comes from the scalar declaration)
typedef struct { uint32_t loc; uint32_t type; uint8_t has_loc; } spv_in;

// 按 id 索引的几张表：SPIR-V 的 id 是稠密的（从 1 起），用下标数组最省事
// A few tables indexed by id: SPIR-V ids are dense (from 1), so arrays indexed by id are simplest
typedef struct {
    uint32_t ptr_pointee[MAX_IDS];   // OpTypePointer 的结果 id -> 指向的类型
    uint8_t ptr_sc[MAX_IDS];         // OpTypePointer 的结果 id -> 存储类
    uint8_t type_op[MAX_IDS];        // 类型 id -> opcode（只记我们关心的几种）
    uint32_t elem_of[MAX_IDS];       // 数组类型 id -> 元素类型
    uint32_t arr_len[MAX_IDS];       // 数组类型 id -> 元素个数（0 = 不是编译期常量）| array type id -> element count (0 = not a compile-time constant)
    uint32_t const_val[MAX_IDS];     // OpConstant 的值（32 位）| OpConstant value (32-bit)
    uint8_t has_const[MAX_IDS];
    uint8_t image_sampled[MAX_IDS];  // 图像类型 id -> sampled 字段
    uint8_t var_sc[MAX_IDS];         // 变量 id -> 存储类
    uint32_t var_type[MAX_IDS];      // 变量 id -> 结果类型
    uint8_t is_block[MAX_IDS], is_buffer_block[MAX_IDS];
    int32_t set_of[MAX_IDS], binding_of[MAX_IDS];
    uint8_t has_var[MAX_IDS];
    uint8_t scalar_op[MAX_IDS];      // 标量类型 id -> OpTypeFloat / OpTypeInt
    uint8_t int_signed[MAX_IDS];     // OpTypeInt 的有符号位
    uint8_t vec_len[MAX_IDS];        // 向量类型 id -> 分量数
    uint32_t vec_comp[MAX_IDS];      // 向量类型 id -> 分量类型 id
    int32_t in_loc[MAX_IDS];         // 输入变量 id -> Location（-1 = 没有，比如内建）
    uint8_t has_in[MAX_IDS];
} spv_tab;
static spv_tab t;   // 静态（约 80 KB）：扫描器只在装载时用，进程内一次一个 | static (~80 KB): the scanner runs at load time only, one module at a time

static int kind_of(uint32_t type_id, uint32_t *count_out) {
    uint32_t t0 = type_id, count = 1;
    int sc = -1;
    for (int guard = 0; guard < 8; guard++) {
        if (t0 >= MAX_IDS) return VK_SPV_OTHER;
        if (t.ptr_pointee[t0] || t.ptr_sc[t0]) { sc = t.ptr_sc[t0]; t0 = t.ptr_pointee[t0]; continue; }
        uint8_t op = t.type_op[t0];
        // 指针与最终类型之间的数组就是**描述符数组**（结构体内部的运行期数组不在这条链上），它的长度就是 descriptorCount
        // An array between the pointer and the final type is a **descriptor array** (a runtime array inside a struct is
        // not on this chain); its length is the descriptorCount
        if (op == OP_TYPE_ARRAY || op == OP_TYPE_RUNTIME_ARRAY) {
            uint32_t len = t.arr_len[t0];
            if (op != OP_TYPE_RUNTIME_ARRAY && len) count *= len;   // 不定长（或长度不是常量）按 1，调用方另作处理 | unbounded (or a non-constant length) stays 1; the caller handles it
            t0 = t.elem_of[t0];
            continue;
        }
        if (count_out) *count_out = count;
        if (op == OP_TYPE_STRUCT) {
            if (sc == 12 || t.is_buffer_block[t0]) return VK_SPV_SSBO;
            if (t.is_block[t0]) return VK_SPV_UBO;
            return VK_SPV_OTHER;
        }
        if (op == OP_TYPE_IMAGE) return t.image_sampled[t0] == 2 ? VK_SPV_STORAGE_IMAGE : VK_SPV_IMAGE;
        if (op == OP_TYPE_SAMPLED_IMAGE) return VK_SPV_SAMPLED_IMAGE;
        if (op == OP_TYPE_SAMPLER) return VK_SPV_SAMPLER;
        return VK_SPV_OTHER;
    }
    if (count_out) *count_out = count;
    return VK_SPV_OTHER;
}

const char *vk_spv_kind_name(int kind) {
    switch (kind) {
        case VK_SPV_UBO: return "uniform buffer";
        case VK_SPV_SSBO: return "storage buffer";
        case VK_SPV_SAMPLED_IMAGE: return "combined image sampler";
        case VK_SPV_STORAGE_IMAGE: return "storage image";
        case VK_SPV_IMAGE: return "sampled image";
        case VK_SPV_SAMPLER: return "sampler";
        case VK_SPV_TEXEL: return "texel buffer";
        default: return "other";
    }
}

int vk_spv_scan(const uint32_t *code, size_t bytes, vk_spv_bind *out, int cap,
                vk_spv_in *ins, int incap, int *nins_out) {
    if (!code || bytes < 20 || !out || cap <= 0) return -1;
    const uint32_t *w = code;
    size_t nw = bytes / 4;
    if (w[0] != SPV_MAGIC) return -1;
    if (nw < 5 || w[3] == 0 || w[3] > MAX_IDS) return -1;   // 模块头声明的 id 上界超过我们的表：报错，别静默丢 id | the header's declared id bound exceeds our tables: fail rather than drop ids silently
    memset(&t, 0, sizeof t);
    for (int i = 0; i < MAX_IDS; i++) { t.set_of[i] = -1; t.binding_of[i] = -1; t.in_loc[i] = -1; }

    size_t i = 5;
    while (i < nw) {
        uint32_t word0 = w[i];
        uint32_t op = word0 & 0xFFFFu, wc = word0 >> 16;
        if (wc == 0 || i + wc > nw) break;
        if (op == OP_FUNCTION) break;        // 声明到此为止 | declarations end here
        const uint32_t *o = w + i;
        switch (op) {
        case OP_DECORATE:
            // 注意字长：Binding / DescriptorSet 带一个操作数（4 个字），而 **Block / BufferBlock 不带**（3 个字）。
            // 早先统一按 4 个字判，于是这两种装饰被整条跳过——真机上表现为"所有结构体类都判成认不出来"
            // （UBO / SSBO → other），而图像类全对（它们的种类来自类型声明、不靠装饰），正是这个组合暴露了它。
            // Mind the word count: Binding / DescriptorSet carry one operand (4 words) while **Block / BufferBlock
            // do not** (3 words). Requiring 4 everywhere silently skipped both decorations -- on the device that
            // showed up as "every struct kind unrecognised" (UBO / SSBO -> other) while image kinds were all
            // correct (they come from the type declarations, not from decorations), and that combination is what
            // exposed it.
            if (wc >= 3 && o[1] < MAX_IDS) {
                uint32_t target = o[1], dec = o[2], val = wc >= 4 ? o[3] : 0;
                if (dec == DEC_BINDING && wc >= 4) t.binding_of[target] = (int32_t)val;
                else if (dec == DEC_SET && wc >= 4) t.set_of[target] = (int32_t)val;
                else if (dec == DEC_BLOCK) t.is_block[target] = 1;
                else if (dec == DEC_BUFFER_BLOCK) t.is_buffer_block[target] = 1;
                else if (dec == DEC_LOCATION && wc >= 4) t.in_loc[target] = (int32_t)val;
            }
            break;
        case OP_TYPE_FLOAT:   // 结果 id、位宽 | result id, width
            if (wc >= 3 && o[1] < MAX_IDS) t.scalar_op[o[1]] = OP_TYPE_FLOAT;
            break;
        case OP_TYPE_INT:     // 结果 id、位宽、有符号位 | result id, width, signedness
            if (wc >= 4 && o[1] < MAX_IDS) { t.scalar_op[o[1]] = OP_TYPE_INT; t.int_signed[o[1]] = (uint8_t)(o[3] ? 1 : 0); }
            break;
        case OP_TYPE_VECTOR:  // 结果 id、分量类型、分量数 | result id, component type, component count
            if (wc >= 4 && o[1] < MAX_IDS) { t.vec_comp[o[1]] = o[2]; t.vec_len[o[1]] = (uint8_t)o[3]; }
            break;
        case OP_TYPE_POINTER:
            if (wc >= 4 && o[1] < MAX_IDS) { t.ptr_sc[o[1]] = (uint8_t)o[2]; t.ptr_pointee[o[1]] = o[3]; }
            break;
        case OP_TYPE_ARRAY:
            // 结果 id、元素类型、长度（一个 OpConstant 的 id）| result id, element type, length (the id of an OpConstant)
            if (wc >= 4 && o[1] < MAX_IDS) {
                t.type_op[o[1]] = (uint8_t)op; t.elem_of[o[1]] = o[2];
                if (o[3] < MAX_IDS && t.has_const[o[3]]) t.arr_len[o[1]] = t.const_val[o[3]];
            }
            break;
        case OP_TYPE_RUNTIME_ARRAY:
            if (wc >= 3 && o[1] < MAX_IDS) { t.type_op[o[1]] = (uint8_t)op; t.elem_of[o[1]] = o[2]; }
            break;
        case OP_CONSTANT:   // 结果类型、结果 id、值（64 位宽的会有两个值字，这里只取低位，够表达数组长度）| result type, result id, value (64-bit ones carry two value words; the low one is enough for array lengths)
            if (wc >= 4 && o[2] < MAX_IDS) { t.const_val[o[2]] = o[3]; t.has_const[o[2]] = 1; }
            break;
        case OP_TYPE_STRUCT:
            if (wc >= 2 && o[1] < MAX_IDS) t.type_op[o[1]] = OP_TYPE_STRUCT;
            break;
        case OP_TYPE_IMAGE:
            // 结果 id、取样类型、维度、深度、数组、多重采样、sampled、格式 | result, sampled type, dim, depth, arrayed, ms, sampled, format
            if (wc >= 8 && o[1] < MAX_IDS) { t.type_op[o[1]] = OP_TYPE_IMAGE; t.image_sampled[o[1]] = (uint8_t)o[7]; }
            break;
        case OP_TYPE_SAMPLED_IMAGE:
            if (wc >= 2 && o[1] < MAX_IDS) t.type_op[o[1]] = OP_TYPE_SAMPLED_IMAGE;
            break;
        case OP_TYPE_SAMPLER:
            if (wc >= 2 && o[1] < MAX_IDS) t.type_op[o[1]] = OP_TYPE_SAMPLER;
            break;
        case OP_VARIABLE:
            if (wc >= 4 && o[2] < MAX_IDS) {
                uint32_t sc = o[3];
                if (sc == 0 || sc == 2) { t.var_sc[o[2]] = (uint8_t)sc; t.var_type[o[2]] = o[1]; t.has_var[o[2]] = 1; }
                else if (sc == 1) { t.has_in[o[2]] = 1; t.var_type[o[2]] = o[1]; }   // Input：顶点属性 | Input: a vertex attribute
            }
            break;
        default: break;
        }
        i += wc;
    }

    int n = 0;
    for (uint32_t id = 0; id < MAX_IDS; id++) {
        if (!t.has_var[id]) continue;
        if (t.set_of[id] < 0 || t.binding_of[id] < 0) continue;   // 不是描述符（没有 set/binding 装饰）| not a descriptor (no set/binding decoration)
        if (n >= cap) return -1;
        out[n].set = (uint32_t)t.set_of[id];
        out[n].binding = (uint32_t)t.binding_of[id];
        out[n].count = 1;
        out[n].kind = kind_of(t.var_type[id], &out[n].count);
        n++;
    }
    // 按 set、binding 升序（插入排序：条数很少）| sort by set then binding (insertion sort: few entries)
    for (int a = 1; a < n; a++) {
        vk_spv_bind k = out[a];
        int b = a - 1;
        while (b >= 0 && (out[b].set > k.set || (out[b].set == k.set && out[b].binding > k.binding))) { out[b + 1] = out[b]; b--; }
        out[b + 1] = k;
    }

    // 输入变量（顶点属性）：只收带 Location 的（内建输入没有 Location，顶点管线不管它们）
    // Input variables (vertex attributes): only those with a Location (built-in inputs have none and no vertex
    // pipeline cares about them)
    if (ins && incap > 0) {
        int ni = 0;
        for (uint32_t id = 0; id < MAX_IDS && ni < incap; id++) {
            if (!t.has_in[id] || t.in_loc[id] < 0) continue;
            uint32_t ty = t.var_type[id], comps = 1, ct = ty;
            if (ty < MAX_IDS && t.vec_len[ty]) { comps = t.vec_len[ty]; ct = t.vec_comp[ty]; }
            int ctype = VK_SPV_F32;
            if (ct < MAX_IDS) {
                if (t.scalar_op[ct] == OP_TYPE_INT) ctype = t.int_signed[ct] ? VK_SPV_I32 : VK_SPV_U32;
            }
            ins[ni].loc = (uint32_t)t.in_loc[id];
            ins[ni].comps = comps > 4 ? 4 : comps;
            ins[ni].ctype = (uint32_t)ctype;
            ni++;
        }
        for (int a = 1; a < ni; a++) {   // 按 Location 升序 | sort by Location
            vk_spv_in k = ins[a];
            int b = a - 1;
            while (b >= 0 && ins[b].loc > k.loc) { ins[b + 1] = ins[b]; b--; }
            ins[b + 1] = k;
        }
        if (nins_out) *nins_out = ni;
    }
    return n;
}
