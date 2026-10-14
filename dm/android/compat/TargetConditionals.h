// 安卓上没有 Apple 的 TargetConditionals.h：给宿主里的平台判断提供全 0 的替身
// （宿主现有代码只有 Mac / iOS 两种分支，安卓下都走"非 iOS"这一支；后续再按实际需要细分）。
// Apple's TargetConditionals.h does not exist on Android: provide an all-zero stand-in for the
// platform checks in the host. The host only has Mac / iOS branches today, so Android takes the
// "not iOS" side for now; refine later as needed.
#pragma once

#define TARGET_OS_MAC 0
#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_TV 0
#define TARGET_OS_WATCH 0
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_EMBEDDED 0
#define TARGET_OS_DRIVERKIT 0

#define TARGET_CPU_ARM64 1
#define TARGET_CPU_X86_64 0

#define TARGET_OS_ANDROID 1
