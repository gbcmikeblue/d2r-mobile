@echo off
rem 把窗口最小工程装到手机并启动（M3 第一块）。
rem Install the minimal window project on the phone and start it (the first piece of M3).
rem
rem 期望看到：底色在缓慢变色、一根白条左右来回滑 —— 动起来才算"上屏这条链通了"。
rem What to expect: the background slowly shifts colour and a white bar sweeps back and forth -- it has to move to count
rem as "the present chain works".
setlocal
set "APKDIR=%~dp0"
pushd "%APKDIR%..\..\.."
set "REPO=%CD%"
popd
set "ADB=C:\Android\Sdk\platform-tools\adb.exe"
set "APK=%REPO%\.work\apk-build\dm.apk"
if not exist "%APK%" ( echo [红] 没有 %APK%，先跑 build_apk.cmd & exit /b 2 )

"%ADB%" install -r "%APK%" || exit /b 3
"%ADB%" shell am start -n ai.d2rm.dm/android.app.NativeActivity || exit /b 4
echo.
echo 已启动。日志： %ADB% logcat -s dmwin
endlocal
