// N4-h：把「我们自己的三角形」画进**游戏的通道里**（缺省关、诊断用）。
//
// 要回答的那一句话（写死）：
//   **「游戏那一刻的通道 / 状态，到底能不能把片元落下来」** ——
//   在 `DM_PASS_PROBE=28` 的**已知色底子**（序号 28 扫出来是亮度 152）之上，用游戏那一刻的通道
//   （附件 / 渲染区 / 布局都是游戏建立的，我们一根手指都没碰），插一次**我们自己的最简管线绘制**：
//   顶点 = 全屏三角形（只用 `gl_VertexIndex`）、片段 = 纯红 `(1,0,0,1)`、无顶点缓冲 / 无描述符 / 无常量。
// 判读（先写下来再让读数回答）：
//   · **底盘上出现非 152 的值 = 落屏** ⇒ 「**通道与状态在那一刻是好的、只有游戏的绘制产不出片元**」
//     成立（与 A1 的"draw 在 API 层面合法"、N4-e 的"片元在光栅化之前就没了"合起来，责任就落在
//     "**游戏的着色器 / 管线状态**"这一侧）；
//   · **仍是整片 152** ⇒ **那一层（通道 / 视口 / 裁剪 / 附件）本身有问题**，先修那里（别去动游戏的着色器）。
//
// 开关：`DM_TRI_IN_GAME=<绘制序号>[@<color0序号>][:game]`（**缺省关**）
//   · `<绘制序号>`：游戏命令流里的全局第 N 次绘制（1 起；与判据行里的"第 N 次"同口径）。**省略 = 不限**；
//   · `[@<color0序号>]`：只在"这一刻通道的 color0 就是该序号"时插（与 `DM_PASS_PROBE=<序号>` 天然对齐
//     —— 探针把那一段通道的底板清成了已知色，判据才成立）。**省略 = 不限通道**；
//   · `[:game]`：用**游戏那一刻的动态视口 / 裁剪**（缺省：铺满附件——先隔离"通道/附件/布局"那一层，
//     视口/裁剪这个变量留到下一步再单独量）；
//   · 形状示例：`@28`（在序号 28 的通道里、第一次绘制处插）、`1234@28`、`1234`、`@28:game`。
// **每段通道只插一次**（`vk_tri_game_pass_begin()` 在开通道时清标志），所以 `@28` 的效果是"每一帧在
// 序号 28 的通道里插一次" —— 截图与扫描在同一轮里都能看到它（底板每帧都会被重新清成已知色）。
//
// 副作用纪律：我们改的是**通道内的动态视口 / 裁剪**，插完立刻按游戏的原值恢复（与 `pass_begin` 同口径），
// 不动游戏的任何静态状态、不碰附件、不改布局、不提交 —— 一次只改一个变量。
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vulkan/vulkan.h>
#include "nvn_obj.h"        // tex_t / view_t
#include "vk_shader.h"      // vk_shader（阶梯要把我们自己的三份着色器包成 vk_shader 交给 vk_pipe_graphics）
#include "vk_pipe.h"        // vk_gfx_state（N4-j 的"逐项接上"要按字段改我们这条管线的状态）

// 开关开没开（读一次环境变量，幂等）。开的时候会打一行【查】自证。
int vk_tri_game_on(void);

// 开通道时调一次：把"这一段通道里还没插过"清掉（缺省关时什么都不做）。
void vk_tri_game_pass_begin(void);

// 游戏某次绘制录完之后调。返回 1 = 真的插了这一次绘制。
//   colors/cviews/has_cview/ncolor：这一刻通道的颜色附件（影子状态里的 render target 与它自己的 view）
//   att_w/att_h：附件有效尺寸（限了 mip 的视图按 `d.width >> base_level` 算，与 pass_begin 同口径）
//   samples    ：这一刻管线用的采样数（单个附件 > 1 时才是多采样）
//   vp/nvp、sc/nsc：游戏那一刻的动态视口 / 裁剪（`:game` 档用；缺省档也要靠它们恢复原值）
int vk_tri_game_try_draw(VkCommandBuffer cb, uint64_t draw_index,
                         const tex_t *const *colors, const view_t *cviews, const uint8_t *has_cview,
                         int ncolor, uint32_t att_w, uint32_t att_h, uint32_t samples,
                         const VkViewport *vp, int nvp, const VkRect2D *sc, int nsc);

// 心跳那一格用的计数：插入次数、本模块自己建的管线条数（开关关着时调用方不该读）。
void vk_tri_game_stats(uint64_t *inserts, uint64_t *pipes);

// 开关的文字自证（启动头 / 心跳用）。返回写进 buf 的字节数；返回 0 = 开关关着。
int vk_tri_game_desc(char *buf, size_t cap);

// ---------------------------------------------------------------------------
// N4-i：**二分阶梯**（`DM_TRI_LADDER=<档>`；缺省关）——拿 N4-h 这条"已经证明能落屏"的管线当标尺，
// 把游戏那一刻的输入**一次只接一样**地接上来，看**哪一项让它消失**。设计说明见 vk_tri_game.c。
// ---------------------------------------------------------------------------
enum {
    VK_TRI_LADDER_OFF = 0,
    VK_TRI_LADDER_VB = 1,      // ① 我们自己的管线 + **游戏的顶点缓冲**（位置取 Location 0）
    VK_TRI_LADDER_STATE = 2,   // ④ 我们自己的管线 + **游戏那一刻的状态**（拓扑/掩码/混合/剔除/深度）
    VK_TRI_LADDER_VS = 3,      // ③ **游戏的顶点着色器** + 我们自己的片段着色器（其余全是游戏的）
    VK_TRI_LADDER_BOTH = 4,    // 对照：**游戏的顶点着色器 + 游戏的片段着色器**（= 原地重放游戏那次绘制）
    // ⑤ `vsn`：与 ③ 只差一样 —— **不绑描述符集**（把"游戏的描述符/管线布局那一层"单独摘出来）。
    // 为什么要有它：`vb`（不绑描述符）**落屏**、`vs`（绑描述符）**不落屏**，两档之间差着两样东西
    // （游戏的顶点着色器模块 vs 描述符/布局），这一档把后一样隔离掉。
    VK_TRI_LADDER_VSN = 5,
    // ⑥ `vso`：**游戏的顶点着色器 + 我们自己的状态**（掩码只开附件 0、剔除关、混合关、深度关、不声明深度格式）
    // —— 与 ③/⑤ 只差"状态与那条管线的声明细节"。三档连起来读：`vb`（我们的 VS + 我们的状态）落屏、
    // `vso`（游戏的 VS + 我们的状态）、`vsn`（游戏的 VS + 游戏的状态，brute 放开）：
    //   · `vso` 落屏、`vsn` 不落屏 ⇒ 责任在**状态/声明细节**；
    //   · 两档都不落屏 ⇒ 责任在**游戏的顶点着色器模块**（配合 `vb` 落屏：顶点数据与流是通的）。
    VK_TRI_LADDER_VSO = 6,
    // N4-k ⑦ `vsf`：**游戏的顶点着色器 + 游戏的片段着色器 + 我们自己的状态**（描述符照 `both` 绑上）。
    // 为什么要这一档：N4-j 量到 `both`（含游戏的 FS）整片 152、`vs`（只把 FS 换成我们那份纯红）整片 85
    // ⇒ 两者之间**只差片段着色器模块这一样**。这一档把 `both` 的"游戏那一刻的状态"换成我们自己的状态、
    // **FS 仍然是游戏那份**：
    //   · 落屏 ⇒ FS 模块本身没问题，责任在"FS 与游戏状态的组合"；
    //   · 不落屏 ⇒ 责任落在**游戏的片段着色器模块**上（配合 `vso`（我们的 FS + 我们的状态）落屏 = 单变量）。
    // ⚠ 与提示词里写的"不绑描述符"有一个刻意的偏差：游戏那份 FS 要 `OpImageSample` 三张贴图
    // （binding 1/2/3），**不绑描述符集时采样结果是未定义的**，那会引入一个新变量；而描述符这条路
    // 已经由 `vs`（我们的 FS + 游戏的状态 + 描述符，叠 N4-j 的修法）**实测落屏**⇒ 把描述符绑上才是单变量。
    // 想量"不绑描述符"那一档时用 ⑧。
    VK_TRI_LADDER_VSF = 7,
    // N4-k ⑧ `vsfn`：与 ⑦ 只差一样 —— **不绑描述符集**（把"游戏 FS 的绑定表"单独摘出来）。
    VK_TRI_LADDER_VSFN = 8,
    // N4-k ⑨ `vsfull`：**我们自己的全屏三角形 VS** + **游戏的片段着色器** + 我们自己的状态 + 描述符。
    // 为什么要这一档（N4-k 第二轮）：`vsf` 三轮整片 152（游戏的 FS → 一个像素都不落），而我们自己的三个变体
    // （只多 interlock capability / 只多两个输出 / 两者）**三轮都落屏** ⇒ capability 与"输出数量"都排掉了。
    // 剩下两种可能：① **这份片段着色器模块在这台驱动上产不出片元**（管线是坏的）；② 它需要游戏那一刻的
    // 几何/管线才有片元。这一档把**几何**换成我们自己那个**已经证明能落屏**的全屏三角形（同一份状态、
    // 同一批附件、同一批描述符）：
    //   · 仍然 152 ⇒ ①：**模块本身产不出片元**（与几何无关）；
    //   · 落屏 ⇒ ②：模块是好的，要回到"几何/顶点输入那一条链"上找（但 `vso` 用同一份 VS/顶点缓冲能落屏，
    //     所以那时要查的是"游戏 FS 与这条顶点管线组合起来的声明细节"）。
    VK_TRI_LADDER_VSFULL = 9,
};

// `DM_TRI_FS_VARIANT=<cap|3out|3out_cap>`（**缺省关**，N4-k）：把阶梯用的那份**我们自己的**片段着色器
// 换成三个变体之一（见 vk_tri_fs_cap_spv.h 与 .work/n4k_fs_variants.py）：
//   · `cap`      = 纯红 + `OpCapability FragmentShaderPixelInterlockEXT` + `OpExtension "SPV_EXT_fragment_shader_interlock"`
//                  （**指令一条没多**）；
//   · `3out`     = 纯红但**写 3 个输出**（Location 0/1/2），形状照游戏那份；
//   · `3out_cap` = 上面两样合起来。
// 为什么要有它：全库 4556 份游戏片段着色器**全部**声明那个 capability，而设备上没有
// `VK_EXT_fragment_shader_interlock`（N4-g 的能力探针）⇒ 这一档量的是"只多那一条声明会不会让绘制消失"。
// 用法与判据：叠在**已知落屏**的 `vso` 档上（`DM_TRI_LADDER=vso`）⇒ 出现非 152 = 这一档仍然落屏。
// 返回：0 = 纯红（缺省）/ 1 = cap / 2 = 3out / 3 = 3out_cap（惰性解析一次；开了会打一行【查】自证）。
int vk_tri_fs_variant(void);

// `DM_TRI_LADDER` 是哪一档（0 = 关；**仍要 `DM_TRI_IN_GAME` 定"哪一次绘制 / 哪一段通道"**，否则返回 0）。
int vk_tri_ladder_mode(void);

// 档名的文字（日志用）。
const char *vk_tri_ladder_name(int mode);

// 这一次（绘制序号 draw_index、这一刻 color0 = color0_ser）该不该插：1 = 可以插。
// 门与 N4-h 那条路**共用** `DM_TRI_IN_GAME` 的字段与"每段通道只插一次"的标志；但**置标志**要等
// `vk_tri_ladder_done()`（回放层在"这一档真的画了"之后调）—— 因为还有一道形状判据（三角形族 + 数量 ≥ 3）。
int vk_tri_ladder_arm(uint64_t draw_index, uint64_t color0_ser);

// 这一档真的画了（回放层调；把"这一段通道已插过"置上）。
void vk_tri_ladder_done(void);

// 我们自己的三份着色器（包成 `vk_shader` 交给 `vk_pipe_graphics`）：
//   0 = 全屏三角形 VS（只读 `gl_VertexIndex`，与 N4-h 同一份）、1 = 读顶点缓冲 VS（Location 0）、2 = 纯红 FS。
// 三份都按需建模块（缺省关时一次都不建）；建不出来返回 NULL（调用方按"这一档没法跑"记一笔）。
const vk_shader *vk_tri_ladder_shader(int which);

// 阶梯自己的账（插了几次 / 骨架没搭起来几次），心跳那一格用（只在开关打开时出现）。
void vk_tri_ladder_stats(uint64_t *inserts, uint64_t *nopipe);

// 阶梯这一档**真的画了一次**（由回放层在录完之后调；只计数）。
void vk_tri_ladder_note(void);

// `DM_TRI_DECL_DS=1`（缺省关）：阶梯自己的状态也**声明游戏那份深度/模板附件格式**
// （叠在 `vb` 档上就是单变量：声明 vs 不声明）。设计说明见 vk_tri_game.c。
int vk_tri_decl_ds(void);

// `DM_TRI_MASK_ALL=1`（缺省关）：阶梯自己的状态把**每个**颜色附件的写掩码都开成 0xf
// （叠在已知落屏的 `vso` 档上就是单变量：只开附件 0 vs 全开）。设计说明见 vk_tri_game.c。
int vk_tri_mask_all(void);

// ---------------------------------------------------------------------------
// N4-j：在**已知落屏**的 `vso` 档上，把"游戏那一刻的状态"**逐项**接过来 —— 一次只接一样。
// ---------------------------------------------------------------------------
// 要回答的那句话（写死，与 N4-i 的结尾同一句）：**"游戏那一刻的状态 / 那条管线的声明细节里，哪一项让
// 片元消失？"** N4-i 已经把着色器、顶点数据、顶点流、索引、计数、描述符、写掩码全开、光栅化侧全放开
// 逐条排除或修掉，只剩这一格。做法与判据：
//   · 起点 = `DM_TRI_LADDER=vso`（**游戏的顶点着色器 + 我们自己的状态**，两轮实测**整片 85 = 落屏**）；
//   · 每个开关只把我们这条管线的**一个字段**换成游戏那一刻的值（形状照 `DM_TRI_MASK_ALL`）；
//   · 判据：`DM_PASS_PROBE=28` 的已知色底子（152）上**出现非 152 的值 = 这一档仍然落屏**；
//     **哪一项一接上就从"整片 85"变成"整片 152"，责任就落在那一项上**。
//   一次接全部（`DM_TRI_GAME_ALL=1`）是**对照**：它应当复现 `vsn` 的"不落屏"；
//   如果它仍然落屏 ⇒ 我们列的这些字段**不覆盖机制**，得去别处找（这条读数本身就很有用）。
enum {
    VK_TRI_F_TOPO = 1u << 0,    // DM_TRI_TOPO_GAME=1    拓扑
    VK_TRI_F_CULL = 1u << 1,    // DM_TRI_CULL_GAME=1    剔除面 + 前向面（两者只有一起才起作用）
    VK_TRI_F_FRONT = 1u << 2,   // DM_TRI_FRONT=<0|1>    只换前向面（值 = VkFrontFace）
    VK_TRI_F_BLEND = 1u << 3,   // DM_TRI_BLEND_GAME=1   混合那一组（en + src/dst rgb/a + eq rgb/a，逐目标）
    VK_TRI_F_DEPTH = 1u << 4,   // DM_TRI_DEPTH_GAME=1   深度/模板测试那一组（**不含**格式声明，那一条是 DM_TRI_DECL_DS）
    VK_TRI_F_A2C = 1u << 5,     // DM_TRI_A2C_GAME=1     alpha-to-coverage
    VK_TRI_F_BIAS = 1u << 6,    // DM_TRI_BIAS_GAME=1    深度偏置（en + 三个系数）
    VK_TRI_F_POLY = 1u << 7,    // DM_TRI_POLY_GAME=1    多边形模式
    VK_TRI_F_MASK = 1u << 8,    // DM_TRI_MASK_GAME=1    每个目标的写掩码（游戏那一刻的）
};
// 这一轮接了哪几样（位掩码；惰性解析一次，缺省 0 = 一样都不接）
uint32_t vk_tri_field_mask(void);
// `DM_TRI_FRONT=<vk 值>` 显式给的前向面（返回 1 = 给了，*out 是 VkFrontFace 的值）
int vk_tri_field_front(int *out);
// 自证：这一轮逐项接上了什么（写给插入明细/心跳用；开关关着返回 0）
int vk_tri_fields_desc(char *buf, size_t cap);
// 把我们这条管线的状态 `ours` 里**被开关选中**的字段换成游戏那一刻的值 `game`
void vk_tri_apply_fields(vk_gfx_state *ours, const vk_gfx_state *game);
