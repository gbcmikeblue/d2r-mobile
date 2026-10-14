// Apple 的 _mh_execute_header（本可执行文件的 Mach-O 头）在安卓上没有对应物。
// 宿主用它定位自身映像里的段；安卓是 ELF，后续应按实际用途改用 ELF 方式（记为待办）。
// Apple's _mh_execute_header (the Mach-O header of this executable) has no Android counterpart.
// The host uses it to locate sections in its own image; Android is ELF, so this should be reworked
// later (tracked as a follow-up). For now it is a null pointer.
#pragma once

#include "getsect.h"

static const struct mach_header_64 *dm_mh_execute_header_placeholder = NULL;
#define _mh_execute_header dm_mh_execute_header_placeholder
