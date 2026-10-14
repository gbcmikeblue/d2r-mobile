@echo off
rem ============================================================================
rem 安卓窗口最小工程：建 .so → 打 APK → 对齐 → 签名（M3 第一块，见 docs/ANDROID_PLAN）
rem Minimal Android window project: build the .so, package the APK, align, sign (the first piece of M3)
rem
rem 为什么不用 gradle：本机没有 Android Studio 的构建链（没有 gradle/java 在 PATH 上，但装了 JDK 21）。
rem 这个包只有 NativeActivity、没有一行 Java（清单里 hasCode="false"），所以 aapt2 + zip + zipalign + apksigner
rem 就够了 —— 也就绕开了"必须有 gradle"这件事。
rem Why not gradle: this machine has no Android Studio build chain (no gradle/java on PATH, but JDK 21 is installed).
rem The package is a single NativeActivity with no Java at all (hasCode="false" in the manifest), so aapt2 + zip +
rem zipalign + apksigner suffice -- which also removes the "gradle is mandatory" problem.
rem
rem 用法：double-click，或在仓库根目录执行 dm\android\apk\build_apk.cmd
rem Usage: double-click it, or run dm\android\apk\build_apk.cmd from the repository root.
rem ============================================================================
setlocal
set "APKDIR=%~dp0"
pushd "%APKDIR%..\..\.."
set "REPO=%CD%"
popd

set "NDK=C:\Android\Sdk\ndk\30.0.16248370"
set "JDK=C:\Program Files\Eclipse Adoptium\jdk-21.0.12.101-hotspot"
set "BT=C:\Android\Sdk\build-tools\36.0.0"
set "CMAKEX=C:\Android\Sdk\cmake\3.31.6\bin\cmake.exe"
set "NINJAX=C:\Android\Sdk\cmake\3.31.6\bin\ninja.exe"
set "ANDROIDJAR=C:\Android\Sdk\platforms\android-36\android.jar"
set "WORK=%REPO%\.work\apk-build"
set "KEYS=%REPO%\.work\apk-keys"

if not exist "%ANDROIDJAR%" (
  echo [红] 找不到 %ANDROIDJAR%（aapt2 需要它做框架资源表）
  exit /b 2
)
if not exist "%WORK%" mkdir "%WORK%"
if not exist "%KEYS%" mkdir "%KEYS%"

echo == 1/5 用 NDK 工具链配置并编 .so ==
"%CMAKEX%" -S "%APKDIR%." -B "%WORK%" -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE="%NDK%\build\cmake\android.toolchain.cmake" ^
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 ^
  -DCMAKE_MAKE_PROGRAM="%NINJAX%" -DCMAKE_BUILD_TYPE=Release || exit /b 3
"%CMAKEX%" --build "%WORK%" || exit /b 4
if not exist "%WORK%\libdm.so" ( echo [红] 没编出 libdm.so & exit /b 5 )

echo == 2/5 aapt2 link 出基础包 ==
"%BT%\aapt2.exe" link -o "%WORK%\base.apk" --manifest "%APKDIR%AndroidManifest.xml" -I "%ANDROIDJAR%" || exit /b 6

echo == 3/5 把 lib/arm64-v8a/libdm.so 以"不压缩"方式加进包里 ==
rem 不压缩是打包 .so 的老规矩（进包即页对齐、装完不用解压）；zipalign -p 随后把页对齐做实
rem Uncompressed is the old rule for packaging .so files (page-aligned inside the package, no extraction at install);
rem zipalign -p then makes the page alignment real
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.IO.Compression.FileSystem; $apk='%WORK%\base.apk'; $so='%WORK%\libdm.so'; $z=[System.IO.Compression.ZipFile]::Open($apk,'Update'); $old=$z.GetEntry('lib/arm64-v8a/libdm.so'); if($old){$old.Delete()}; $e=$z.CreateEntry('lib/arm64-v8a/libdm.so',[System.IO.Compression.CompressionLevel]::NoCompression); $o=$e.Open(); $b=[System.IO.File]::ReadAllBytes($so); $o.Write($b,0,$b.Length); $o.Close(); $z.Dispose(); Write-Output ('  libdm.so ' + $b.Length + ' 字节已入包')" || exit /b 7

echo == 4/5 zipalign（-p：不压缩的 .so 按页对齐）==
"%BT%\zipalign.exe" -p -f 4 "%WORK%\base.apk" "%WORK%\aligned.apk" || exit /b 8

echo == 5/5 签名（第一次会用 keytool 生成调试密钥，只放在 .work 里，不进仓库）==
if not exist "%KEYS%\debug.keystore" (
  "%JDK%\bin\keytool.exe" -genkeypair -keystore "%KEYS%\debug.keystore" -alias dm ^
    -keyalg RSA -keysize 2048 -validity 10000 -storepass dmdmdm -keypass dmdmdm ^
    -dname "CN=dm-android-port, O=dm, C=CN" || exit /b 9
)
set "JAVA_HOME=%JDK%"
call "%BT%\apksigner.bat" sign --ks "%KEYS%\debug.keystore" --ks-pass pass:dmdmdm --key-pass pass:dmdmdm ^
  --out "%WORK%\dm.apk" "%WORK%\aligned.apk" || exit /b 10
call "%BT%\apksigner.bat" verify --print-certs "%WORK%\dm.apk" | findstr /i "Signer #1 certificate DN" 

echo.
echo 完成：%WORK%\dm.apk
echo 装到手机并启动：dm\android\apk\install_apk.cmd
endlocal
