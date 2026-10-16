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
