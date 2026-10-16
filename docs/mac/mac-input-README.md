# 给 Mac 侧的回填件（`d2r-bridge/in/mac-input/`）—— 放了什么、什么时候放的

**什么时候放的**：2026-10-16 20:41（Windows 侧 / 第一台 `8b1f1e50` 旁）；
**谁放的**：Windows 侧（`.work/n4d_fill_mac_input.py`，一次性搬运、没改任何代码）。

**为什么放**：Mac 那份交接件（[`jiaojie.log`](jiaojie.log) 正文 §4.1 与 §6）说它开工时
`/sdcard/d2r-bridge/` 还不存在 ⇒ **四份 vk2 探针一件没拿到**，做不了"地址口径 / 页粒度 / 环切片"的逐条对账。
这六件在 Windows 侧 `.work/logs/` 里本来就有，搬过去 Mac 就能算（**不用改代码**）。

| 手机上的文件（`/sdcard/d2r-bridge/in/mac-input/`） | 干什么用 |
|---|---|
| `probe-8b1f1e50-vk2_pool_probe.txt` | N1 的四轮读数（29 个字段一致）：**G3**、块数、跨块率分布 |
| `probe-8b1f1e50-vk2_xfer_probe.txt` | N2 的四轮读数：**G1/G2**、往返闭环、超时行为、脏位图逐位一致 |
| `probe-8b1f1e50-vk2_sub_probe.txt` | N3 的四轮读数：**G4 前两条**、D=1/2/3/4/8 的每提交耗时、"3 还是 4" = 3 |
| `probe-8b1f1e50-vk2_takeover_probe.txt` | N4-b/N4-c 的十一问（反查形状 / 环按槽分 / 接管闭环 / 半页竞态两对策 / 回收钩子） |
| `vk2-readings.tsv` | 新基线六个字段的正式读数表（N0 补录 + N1 ~ N4-c） |
| `n4-n4d-a5.log` | 一轮**游戏路径**（A 臂 `DM_GFX2=1`）的完整日志，含心跳逐秒与判据行 |

**没有的**：Mac 另外要的那份 `<pck>.json`（PC 反射，T4.4 的 `bit0/bit2` 只有它有）—— Windows 侧的仓库里没有，
如果 Mac 还需要，说一声我们从出包侧再导一份。

**口径提醒**（免得两边对不上）：
- 探针读数的**时间/内存字段轮间会抖**，判据字段才是比对的基准（Mac 侧的四条"重算"结论见 `jiaojie.log` §3.6）。
- 游戏日志里"帧"= **出图次数**，不是画面帧号（真帧号是 `【地基】帧开头上传：第 N 帧`）。
