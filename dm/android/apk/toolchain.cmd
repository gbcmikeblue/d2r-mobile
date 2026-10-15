@echo off
rem ============================================================================
rem 工具链发现：**一处**决定所有绝对路径（被 build_apk.cmd / install_apk.cmd call）
rem Toolchain discovery: **all** absolute paths are decided here (called by build_apk.cmd / install_apk.cmd)
rem
rem 为什么要这个文件：原先 NDK / JDK / build-tools / CMake / ninja / android.jar / adb 的路径**写死在两个脚本里**，
rem 换一台机器就要改一次。真踩过一次：Adoptium 把目录名从 jdk-21.0.12.1+1 改成 jdk-21.0.12.101-hotspot ——
rem **主版本没变、目录名会变**，于是"按文档装同一版 JDK"也会得到不一样的路径。JDK 又是唯一一处不在 Android SDK
rem 目录树里的（其余那些由 SDK Manager 装，按文档装就一致）。
rem Why this file: the paths of NDK / JDK / build-tools / CMake / ninja / android.jar / adb used to be **hard-coded in two
rem scripts**, so every new machine needed an edit. It really bit us once: Adoptium renamed its directory from
rem jdk-21.0.12.1+1 to jdk-21.0.12.101-hotspot -- **same major version, different directory name** -- so "install the JDK
rem the doc names" still yields an inconsistent path. The JDK is also the only one outside the Android SDK tree (the rest
rem come from the SDK Manager, so following the doc keeps them identical).
rem
rem 决定的顺序（谁先说话算数）／Order of precedence:
rem   ① DM_* 显式覆盖（CI，或装在别处的机器）／an explicit DM_* override (CI, or a machine that installs elsewhere)
rem   ② 机器自己声明的 JAVA_HOME / ANDROID_SDK_ROOT / ANDROID_HOME
rem      what the machine itself declares: JAVA_HOME / ANDROID_SDK_ROOT / ANDROID_HOME
rem   ③ 已知落点里**取最新的一版**（JDK 优先 jdk-21*，再退到任意 jdk-*）
rem      the **newest** version under the known install roots (JDK prefers jdk-21*, then any jdk-*)
rem   ④ 兜底：Android Studio 自带的 jbr，最后是 PATH 上的 java
rem      fallbacks: the jbr that ships with Android Studio, then java on PATH
rem
rem 本文件**不做 setlocal**：变量要留给调用方（调用方自己有 setlocal 兜着，不会漏到外层）。
rem This file deliberately has **no setlocal**: the variables are handed to the caller (which has its own setlocal, so
rem nothing leaks to the outer shell).
rem
rem 用法：call "toolchain.cmd"（相对本目录）—— 成功返回 0；SDK 根找不到返回 2 并说清怎么覆盖。
rem Usage: call toolchain.cmd (relative to this directory) -- returns 0 on success, 2 with an explanation when the SDK
rem root is missing.
rem
rem 覆盖开关／Overrides: DM_SDK_ROOT、DM_NDK、DM_BUILD_TOOLS、DM_CMAKE、DM_PLATFORM、DM_JDK
rem
rem 写这个文件的几条 cmd 纪律（都是踩过的）／A few cmd rules this file follows (each learned the hard way):
rem   · if 后面接 "set A & set B" 时，那个 & **脱离 if**，B 会无条件执行 → 一律写成 if 条件 (set A & set B)
rem     after if, a "set A & set B" tail lets the & **escape the if** and run B unconditionally -> always write
rem     if cond (set A & set B)
rem   · 括号块里的 for /f ... 2^>nul 解析会变（报"命令语法不正确"）→ for /f 一律放顶层或子例程
rem     for /f ... 2^>nul inside a parenthesised block parses differently (it errors out) -> keep for /f at the top level
rem     or in a subroutine
rem   · **rem 行不是纯注释**：里面的 & 、竖线、大于号、小于号会被当成命令分隔与重定向 → 注释里一律不写它们
rem     **a rem line is not pure comment**: & , pipe and the redirection signs in it are treated as separators and
rem     redirection -> none of them appear in the comments here
rem ============================================================================

rem ---------- 0. 结果先清空（每次都重新发现，不继承上一次）----------
rem ---------- 0. Clear the results first (every call re-discovers; nothing is inherited from a previous call) ----------
set "SDK="
set "NDKVER="
set "BTVER="
set "CMVER="
set "PLVER="
set "JDK="
set "NDK="
set "BT="
set "CMAKEX="
set "NINJAX="
set "ANDROIDJAR="
set "ADB="
set "_dm_show="
set "_dm_jhow="

rem ---------- 1. SDK 根：机器自己声明的优先，其次固定落点 ----------
rem 装在别处的话，这一条是唯一要动的地方 —— 而且是设环境变量，不是改仓库里的文件
rem If it is installed elsewhere this is the only thing to change -- as an environment variable, not a file in the repo
if defined DM_SDK_ROOT (set "SDK=%DM_SDK_ROOT%" & set "_dm_show=DM_SDK_ROOT")
if not defined SDK if defined ANDROID_SDK_ROOT (set "SDK=%ANDROID_SDK_ROOT%" & set "_dm_show=ANDROID_SDK_ROOT")
if not defined SDK if defined ANDROID_HOME (set "SDK=%ANDROID_HOME%" & set "_dm_show=ANDROID_HOME")
if not defined SDK if exist "C:\Android\Sdk" (set "SDK=C:\Android\Sdk" & set "_dm_show=缺省落点 C:\Android\Sdk")
if not defined SDK if exist "%LOCALAPPDATA%\Android\Sdk" (set "SDK=%LOCALAPPDATA%\Android\Sdk" & set "_dm_show=缺省落点 LOCALAPPDATA\Android\Sdk")
if not defined SDK (set "SDK=C:\Android\Sdk" & set "_dm_show=缺省（不存在）")
if not exist "%SDK%" (
  echo [红] 找不到 Android SDK 根：%SDK%  （来自 %_dm_show%）
  echo      装到别处就设 DM_SDK_ROOT 或 ANDROID_HOME 指过去；清单见 docs/CROSS_MACHINE.zh-CN.md 第二节
  goto :_dm_fail
)
if "%SDK:~-1%"=="\" set "SDK=%SDK:~0,-1%"

rem ---------- 2. 组件版本：DM_* 覆盖，其次目录里最新的一版 ----------
rem 为什么"取最新"而不是钉死：钉死就回到"每台机改一次"；要复现某一份工具链时用 DM_* 钉住即可。
rem Why "newest" rather than pinned: pinning goes back to "edit once per machine"; to reproduce an exact toolchain, pin
rem it with DM_*.
set "NDKVER=%DM_NDK%"
if not defined NDKVER for /f "delims=" %%v in ('dir /b /ad /o-n "%SDK%\ndk" 2^>nul') do if not defined NDKVER set "NDKVER=%%v"
set "BTVER=%DM_BUILD_TOOLS%"
if not defined BTVER for /f "delims=" %%v in ('dir /b /ad /o-n "%SDK%\build-tools" 2^>nul') do if not defined BTVER set "BTVER=%%v"
set "CMVER=%DM_CMAKE%"
if not defined CMVER for /f "delims=" %%v in ('dir /b /ad /o-n "%SDK%\cmake" 2^>nul') do if not defined CMVER set "CMVER=%%v"
rem 平台目录名是 android-N：这里**不能用字典序**（android-9 会排在 android-36 前面），所以按数字比
rem Platform directories are named android-N: **lexicographic order will not do** (android-9 sorts above android-36), so
rem they are compared numerically
set "PLVER=%DM_PLATFORM%"
rem 任何 :STR=REPL 或 ~n,m 的子串语法都不该**直接出现在 if 条件里**：cmd 的 if 解析对 % 的配对是贪心且带边界猜的，
rem 与 `"=="` 串味后整段会被错读，第二个症状是报"命令语法不正确"、第一个症状是 echo 出来看到 `PLVER:~8` 这种拼接
rem No `:STR=REPL` or `~n,m` substring syntax may appear **directly inside an if condition**: cmd's if parser's `%`
rem matching is greedy and boundary-guessing, and once it tangles with `"=="` the whole thing misreads; the visible
rem symptoms are "syntax incorrect" or an echo that shows the parts concatenated (e.g. `PLVER:~8`).
rem 所以：先 set 进临时变量，再用纯变量名比较。| Hence: set into a temp first, then compare plain variable names.
if defined PLVER set "_dm_strip=%PLVER:android-=%"
if defined PLVER if not "%_dm_strip%"=="%PLVER%" set "PLVER=%_dm_strip%"
if not defined PLVER set "_dm_best=0"
if not defined PLVER for /f "delims=" %%v in ('dir /b /ad "%SDK%\platforms" 2^>nul') do call :_dm_pl "%%v"

if defined NDKVER set "NDK=%SDK%\ndk\%NDKVER%"
if defined BTVER set "BT=%SDK%\build-tools\%BTVER%"
if defined CMVER (set "CMAKEX=%SDK%\cmake\%CMVER%\bin\cmake.exe" & set "NINJAX=%SDK%\cmake\%CMVER%\bin\ninja.exe")
if defined PLVER set "ANDROIDJAR=%SDK%\platforms\%PLVER%\android.jar"
set "ADB=%SDK%\platform-tools\adb.exe"

rem ---------- 3. JDK：DM_JDK 优先于 JAVA_HOME，再优先于落点探测，最后 jbr 与 PATH ----------
rem 每一档都要求 bin\java.exe 真的在：apksigner 要 JAVA_HOME 指向一个能跑的 JDK，不是"一个目录名"
rem Every step requires bin\java.exe to actually be there: apksigner needs JAVA_HOME to point at a working JDK, not at
rem "some directory name"
if defined DM_JDK if exist "%DM_JDK%\bin\java.exe" (set "JDK=%DM_JDK%" & set "_dm_jhow=DM_JDK")
if not defined JDK if defined JAVA_HOME if exist "%JAVA_HOME%\bin\java.exe" (set "JDK=%JAVA_HOME%" & set "_dm_jhow=JAVA_HOME")
set "_dm_jroots="C:\Program Files\Eclipse Adoptium" "C:\Program Files\Java" "C:\Program Files\Microsoft" "C:\Program Files\Zulu" "C:\Program Files\Amazon Corretto" "C:\Program Files\BellSoft" "C:\Program Files\Semeru" "C:\Program Files (x86)\Eclipse Adoptium" "C:\Program Files (x86)\Java""
if not defined JDK for %%r in (%_dm_jroots%) do call :_dm_jdk "%%~r" "jdk-21*"
if not defined JDK for %%r in (%_dm_jroots%) do call :_dm_jdk "%%~r" "jdk-*"
if not defined JDK if exist "C:\Program Files\Android\Android Studio\jbr\bin\java.exe" (set "JDK=C:\Program Files\Android\Android Studio\jbr" & set "_dm_jhow=Android Studio 自带的 jbr")
if not defined JDK if exist "%LOCALAPPDATA%\Programs\Android Studio\jbr\bin\java.exe" (set "JDK=%LOCALAPPDATA%\Programs\Android Studio\jbr" & set "_dm_jhow=Android Studio 自带的 jbr")
rem PATH 上那个 java 还要旁边有 keytool 才算数：微软商店的 java.exe 是个只有自己的占位壳
rem java on PATH only counts with a keytool next to it: the Windows Store java.exe is a stub on its own
if not defined JDK for /f "delims=" %%j in ('where java 2^>nul') do if not defined JDK if exist "%%~dpjkeytool.exe" for %%p in ("%%~dpj..") do (set "JDK=%%~fp" & set "_dm_jhow=PATH 上的 java")

rem ---------- 4. 把选中了什么打出来（"结论不该靠记忆"）----------
rem ---------- 4. Print what was chosen ("a conclusion should not rest on memory") ----------
echo [工具链] SDK=%SDK%  （来自 %_dm_show%）
echo [工具链] NDK=%NDKVER%  build-tools=%BTVER%  CMake=ninja=%CMVER%  platform=%PLVER%
if defined JDK (echo [工具链] JDK=%JDK%  （来自 %_dm_jhow%）) else (echo [红] 没找到 JDK：设 JAVA_HOME，或用 DM_JDK 指一个 JDK 目录)
if not defined NDKVER echo 【黄】%SDK%\ndk 下没有版本：用 DM_NDK 指，或按 docs/CROSS_MACHINE 装一个
if not defined BTVER echo 【黄】%SDK%\build-tools 下没有版本：用 DM_BUILD_TOOLS 指，或按 docs/CROSS_MACHINE 装一个
if not defined CMVER echo 【黄】%SDK%\cmake 下没有版本：用 DM_CMAKE 指，或按 docs/CROSS_MACHINE 装一个
if not defined PLVER echo 【黄】%SDK%\platforms 下没有平台：用 DM_PLATFORM 指（如 36 或 android-36）
if defined JDK if not exist "%JDK%\bin\keytool.exe" echo 【黄】%JDK%\bin 下没有 keytool：首次生成调试密钥那一步会失败（换一个完整 JDK）
if not exist "%ADB%" echo 【黄】%ADB% 不在：platform-tools 没装（install_apk.cmd 要用它）

rem 清掉本文件自己的临时变量（调用方要的那几个留着）／Drop this file's own temporaries (the caller's are kept)
set "_dm_show="
set "_dm_jhow="
set "_dm_jroots="
set "_dm_best="
goto :eof

rem ---------- 子例程：在某个根目录下找 jdk 目录（第一个参数 = 根，第二个 = 通配）----------
rem ---------- Subroutine: find a jdk directory under a root (argument 1 = root, 2 = pattern) ----------
:_dm_jdk
if defined JDK goto :eof
if not exist "%~1" goto :eof
for /f "delims=" %%d in ('dir /b /ad /o-n "%~1\%~2" 2^>nul') do (
  if not defined JDK if exist "%~1\%%d\bin\java.exe" (set "JDK=%~1\%%d" & set "_dm_jhow=按落点找到的 %~2")
)
goto :eof

rem ---------- 子例程：从 android-N 里挑 N 最大的那个 ----------
rem ---------- Subroutine: pick the largest N among android-N ----------
:_dm_pl
if not defined _dm_best set "_dm_best=0"
set "_dm_n=%~1"
if not "%_dm_n:~0,8%"=="android-" goto :eof
set "_dm_num=%_dm_n:~8%"
set /a "_dm_val=_dm_num" >nul 2>nul
if errorlevel 1 goto :eof
if not defined _dm_val goto :eof
if %_dm_val% LEQ %_dm_best% goto :eof
set "_dm_best=%_dm_val%"
set "PLVER=%_dm_n%"
goto :eof

:_dm_fail
set "_dm_show="
set "_dm_jhow="
set "_dm_jroots="
set "_dm_best="
set "_dm_strip="
exit /b 2
