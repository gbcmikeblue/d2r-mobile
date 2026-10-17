#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""公共触发器：**等到"读数出现 / 进程退 / 出图到 N"**，而不是 `time.sleep(固定秒数)`。

为什么要有它（2026-10-17 在第一台 Adreno 830 上量的）：N4-o 的 24 轮里，首条结果行（`第 1 次插入`）
在 **≈2.1 s** 就到了、`DM_NVN_SHOT_SCAN` 的亮度读数 **≈3.1 s** 到齐、`出图` 到最终值的中位时刻 **16.4 s**，
而每轮固定等 **49.4 s** ⇒ **平均 27.8 s/轮是纯等**；更糟的是 App 已经退出（日志只有 1.1 s）也照样睡满 50 s。
改成触发式之后：判据类一轮 **≈7 s**、废样本 **≈3 s** 收口，判据行与原 50 s 轮**逐字相同**。

用法（两种）：

  1) **当库用**（跑轮脚本就是这么用的）：
     ```python
     import sys, pathlib
     sys.path.insert(0, str(ROOT / "dm" / "tools"))
     import wait_until as W

     base = W.snapshot()                       # **am start 之前**拍基线（见下面的坑②）
     ... am start -W ...
     res = W.wait_run(until=[r"次插入", r"亮度 均值"], min_secs=6, max_secs=50, baseline=base)
     print(res["reason"], res["elapsed"], res["frames"])   # until / dead / timeout / nostart
     ```

  2) **单独跑**（手动看一眼触发器在等什么）：
     `& "<仓库>/.venv/Scripts/python.exe" -X utf8 dm/tools/wait_until.py --until "次插入" --min-secs 6 --max-secs 50`

环境（都可选，换机器时用它们，不必改代码）：
  · `DM_ADB`    —— adb 的路径（缺省：Windows 走 `C:\\Android\\Sdk\\platform-tools\\adb.exe`，否则 PATH 里的 `adb`）
  · `DM_SERIAL` —— 设备串号（缺省 `8b1f1e50`）
  · `DM_PKG`    —— 包名（缺省 `ai.d2rm.dm`；设备日志路径由包名拼出来，也可用 `DM_LOG` 直接给）

原语（都只读、不碰设备状态）：
  · `dev_size()` / `dev_head()` / `dev_tail()` / `dev_since()` —— 读设备上 `latest.log` 的长度 / 头部 / 尾部 / 增量；
  · `pid_alive()` / `wait_gone()` —— `pidof <包名>`（判"废样本：App 已经退了"、等 `force-stop` 真的生效）；
  · `snapshot()` —— 启动**之前**拍"日志基线"（长度 + 第一行），用来判"这份日志是不是这一轮新开的"；
  · `out_frames()` —— 从心跳里取最后一个 `出图 N`；
  · `wait_run()` —— 按 `until`（任一命中）/ `also`（AND）+ `min_secs` / `min_frames`（下限）+ `max_secs`（上限）收口，
    返回 `reason / elapsed / frames / tail`。

**三条实现坑（都是真机踩出来的，改这个文件之前先看）**：
  ① **只读 `tail -c 8192` 会漏事件**：心跳约 1.1 KB/s，`第 1 次插入`（2.1 s）与扫描亮度（3.1 s）等到
     `min_secs=6` 再判时已被挤出窗口 ⇒ 现场是"明明有判据却 `timeout@50.3s`"。做法：从第 0 字节起**增量累加**
     （`tail -c +<off+1>`），文件被重开就把 `off` 归零。
  ② **基线必须在 `am start` 之前拍**：`am start -W` 会等到窗口可见才返回，那时 App 早已把日志重开并写了几十 KB；
     在 `wait_run()` 里现拍基线 ⇒ 每轮都判 `nostart`（白等 12 s）。所以 API 是"调用方 `snapshot()` 后传 `baseline=`"。
  ③ **别用 `=== 本次启动 pid` 判"日志是新的"**：它在 11 行配置回显之后（实测 ≈2.2 KB），`head -c 2048` 会漏；
     改用「**文件变短** 或 **第一行换了**」（第一行带启动时刻，每次不同）。

**什么时候不许用它**（口径见 `docs/rewrite/NEWSESSION.zh-CN.md` 的坑表）：
  · **累计型/统计型读数**（帧率、`最大空隙`、G1~G4 的字节账、停顿窗口）要**跑满窗口** ⇒ 不给 `until`
    （跑轮脚本里的 `--no-until`），否则"最大空隙"会变小、把"停顿型"读成"健康"；
  · 早停后 **累计字段跨轮不可比** ⇒ 每轮必须把 `停止原因`（`until@7.0s` / `timeout@50s` / `dead@3.0s`）留在读数里。
"""
import argparse
import datetime
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def _find_adb():
    env = os.environ.get("DM_ADB", "").strip()
    if env:
        return env
    win = r"C:\Android\Sdk\platform-tools\adb.exe"
    if pathlib.Path(win).exists():
        return win
    return shutil.which("adb") or "adb"


ADB = _find_adb()
SERIAL = os.environ.get("DM_SERIAL", "8b1f1e50")
PKG = os.environ.get("DM_PKG", "ai.d2rm.dm")
DEV_LOG = os.environ.get("DM_LOG", "/sdcard/Android/data/{}/files/logs/latest.log".format(PKG))

# 本次启动的标记（App 每次启动都会把它写进最新的日志头部；也说明日志文件已被重开）
START_RX = re.compile(r"=== 本次启动 pid")
HB_RX = re.compile(r"心跳：出图 (\d+)")


def sh(args, timeout=60, quiet=True):
    try:
        return subprocess.run([str(a) for a in args], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


# ---------------------------------------------------------------- 原语

def dev_size(serial=SERIAL, path=DEV_LOG):
    """设备上日志当前字节数（`stat -c %s`；读不到就当 0）。"""
    r = sh([ADB, "-s", serial, "shell", "stat", "-c", "%s", path])
    if not r or r.returncode != 0:
        return 0
    m = re.search(r"(\d+)", r.stdout or "")
    return int(m.group(1)) if m else 0


def dev_head(serial=SERIAL, path=DEV_LOG, n=2048):
    """只读开头 n 字节（判"这份日志是不是本轮新开的"）。"""
    r = sh([ADB, "-s", serial, "shell", "head", "-c", str(n), path])
    return (r.stdout or "") if r else ""


def dev_tail(serial=SERIAL, path=DEV_LOG, n=8192):
    """只读末尾 n 字节（比 `cat` / `pull` 整份快一个量级）。"""
    r = sh([ADB, "-s", serial, "shell", "tail", "-c", str(n), path])
    return (r.stdout or "") if r else ""


def dev_since(serial=SERIAL, path=DEV_LOG, off=0):
    """从第 `off+1` 个字节读到文件尾（**增量**；`off` 是"已读掉的字节数"）。

    ⚠ 这一条是 2026-10-17 用真机自证逼出来的：只用 `tail -c 8192` 会**漏掉早于窗口的事件** ——
    心跳一行约 1.1 KB/s，而 `第 1 次插入` 在 2.1 s、扫描亮度在 3.1 s，等到 `min_secs=6` 再判时
    它们已经被挤出 8 KB 窗口，于是"明明有判据却一直等到上限"。
    """
    r = sh([ADB, "-s", serial, "shell", "tail", "-c", "+" + str(off + 1), path])
    return (r.stdout or "") if r else ""


def pid_alive(serial=SERIAL, pkg=PKG):
    """进程还在不在（`pidof` 空 = 退了/崩了）。"""
    r = sh([ADB, "-s", serial, "shell", "pidof", pkg])
    return bool(r and r.returncode == 0 and (r.stdout or "").strip())


def out_frames(text):
    """从心跳里取**最后一个** `出图 N`。"""
    got = HB_RX.findall(text or "")
    return int(got[-1]) if got else 0


def snapshot(serial=SERIAL, path=DEV_LOG):
    """**启动 App 之前**拍一张"日志基线"（长度 + 第一行）—— 用来判"这份日志是不是这一轮新开的"。

    ⚠ 必须**在 `am start` 之前**拍：`am start -W` 会等到窗口可见才返回，那时 App 早就把日志重开并写了
    几十 KB（2026-10-17 的现场：在 `wait_run` 里拍基线 ⇒ 每次都判 `nostart`，白等 12 s）。
    """
    return dict(size=dev_size(serial, path), head=dev_head(serial, path, 400))


def wait_gone(serial=SERIAL, pkg=PKG, timeout=8.0, poll=0.15):
    """等进程真的没了（替代 `am force-stop` 之后的 `time.sleep(2)`）。返回等了多久。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not pid_alive(serial, pkg):
            return time.time() - t0
        time.sleep(poll)
    return time.time() - t0


# ---------------------------------------------------------------- 主函数

def wait_run(serial=SERIAL, path=DEV_LOG, pkg=PKG, until=(), also=(), min_secs=6.0,
             max_secs=50.0, min_frames=0, settle=0.5, poll=0.7, start_timeout=12.0,
             baseline=None, verbose=True):
    """等一轮的读数，返回 dict（reason ∈ until / dead / timeout / nostart）。

    · `until`：**任一**正则命中就算"读数到齐"；`also`：必须**同时**命中（AND）。
    · `min_secs` / `min_frames`：下限 —— 时间不到、出图不够时，即使命中也不收（防"文件里残留的上一轮行"）。
    · `pkg`：给出就顺带判"废样本"（进程连两次轮询都不在 ⇒ reason=dead，**立刻返回**）。
    · `max_secs`：兜底 = 老口径的固定时长。
    · `baseline`：**启动之前** `snapshot()` 的结果（强烈建议给；不给就现拍，可能把"已经重开过"的日志当旧的）。
    """
    t_start = time.time()
    rxs = [re.compile(x) for x in until]
    arcs = [re.compile(x) for x in also]

    # ① 先确认"日志文件是本轮新开的"（否则会把上一轮的 `次插入` / `亮度` 当成这一轮的）
    if baseline:
        size_before, head_before = baseline["size"], baseline["head"]
    else:
        size_before = dev_size(serial, path)
        head_before = dev_head(serial, path, 400)  # 只比**第一行**（带着这一轮的启动时刻，每次都不一样）
    seen_start = False
    while time.time() - t_start < start_timeout:
        head_now = dev_head(serial, path, 400)
        # 「文件变短」或「第一行换了」= 日志被这一轮重开
        if dev_size(serial, path) < size_before or (head_now and head_now != head_before):
            seen_start = True
            break
        time.sleep(poll)
    if verbose:
        print(f"[wait] 本轮日志的启动标记 {'✓' if seen_start else '✗'}"
              f"（{time.time() - t_start:.1f}s；之前 {size_before} 字节）", flush=True)
    if not seen_start:
        # 不拿旧内容凑结论：直接收口，由调用方判"这一轮无效"
        return dict(reason="nostart", elapsed=time.time() - t_start, frames=0,
                    seen_start=False, matches="", tail=dev_tail(serial, path, 2000))

    t0 = time.time()
    last_report = 0.0
    dead_count = 0
    off = 0          # 已读掉的字节数（增量累加；文件被重开就归零）
    acc = ""         # 本轮从第 0 字节起读到的全部文本（尾部保留 300 KB）
    frames = 0
    while True:
        el = time.time() - t0
        size = dev_size(serial, path)
        if size < off:                      # 日志被重开（新的一轮）⇒ 从头攒
            off, acc = 0, ""
        chunk = dev_since(serial, path, off)
        if chunk:
            acc = (acc + chunk)[-300000:]
            off = max(off, size) if size > off else off + len(chunk.encode("utf-8"))
        text = acc if acc else dev_tail(serial, path, 8192)
        frames = out_frames(text)

        hit = (el >= min_secs and frames >= min_frames
               and START_RX.search(text)          # 只认本轮日志（双保险；①已经确认过一次）
               and any(rx.search(text) for rx in rxs)
               and all(rx.search(text) for rx in arcs))
        if hit:
            if verbose:
                why = " | ".join(x for x in until)
                print(f"[wait] **触发** @{el:.1f}s（出图 {frames}；命中 {why}）", flush=True)
            time.sleep(settle)          # 让这一批结果行落盘
            chunk = dev_since(serial, path, off)
            if chunk:
                acc = (acc + chunk)[-300000:]
            frames = out_frames(acc)
            return dict(reason="until", elapsed=el, frames=frames, seen_start=seen_start,
                        matches=" | ".join(x for x in until), tail=acc)

        if pkg:
            if pid_alive(serial, pkg):
                dead_count = 0
            else:
                dead_count += 1
                if dead_count >= 2:
                    if verbose:
                        print(f"[wait] **进程已退** @{el:.1f}s（出图 {frames}）⇒ 按无效样本收口", flush=True)
                    return dict(reason="dead", elapsed=el, frames=frames, seen_start=seen_start,
                                matches="", tail=acc)

        if el >= max_secs:
            if verbose:
                print(f"[wait] 到上限 {max_secs:.0f}s（出图 {frames}）⇒ 按老口径收口", flush=True)
            return dict(reason="timeout", elapsed=el, frames=frames, seen_start=seen_start,
                        matches="", tail=acc)

        if verbose and el - last_report >= 5.0:
            last_report = el
            print(f"[wait] …{el:4.1f}s 出图 {frames}（已攒 {len(acc)} 字符）", flush=True)
        time.sleep(poll)


# ---------------------------------------------------------------- CLI（手动看一眼）

def main():
    ap = argparse.ArgumentParser(description="等设备上 latest.log 里的读数出现，而不是睡固定秒数")
    ap.add_argument("--serial", default=SERIAL)
    ap.add_argument("--path", default=DEV_LOG)
    ap.add_argument("--until", action="append", default=[], help="任一命中即收（可重复）")
    ap.add_argument("--also", action="append", default=[], help="必须也命中（可重复）")
    ap.add_argument("--min-secs", type=float, default=0.0)
    ap.add_argument("--max-secs", type=float, default=60.0)
    ap.add_argument("--min-frames", type=int, default=0)
    ap.add_argument("--no-pkg", action="store_true", help="不判进程存活")
    a = ap.parse_args()
    r = wait_run(serial=a.serial, path=a.path, pkg=None if a.no_pkg else PKG,
                 until=a.until, also=a.also, min_secs=a.min_secs, max_secs=a.max_secs,
                 min_frames=a.min_frames)
    print(f"\n[wait] 结果：{r['reason']} @{r['elapsed']:.1f}s 出图 {r['frames']}")
    print(f"[wait] 尾部 2000 字节（{datetime.datetime.now():%H:%M:%S}）：")
    print(r["tail"][-2000:])
    return 0 if r["reason"] == "until" else 1


if __name__ == "__main__":
    sys.exit(main())
