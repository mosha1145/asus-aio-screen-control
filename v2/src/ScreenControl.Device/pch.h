#pragma once

// ---------------------------------------------------------------------------
// ScreenControl.Device 预编译头
// 协议层只依赖 Win32 / HID / SetupAPI，不依赖 WinUI，因此可脱离 UI 单测。
// ---------------------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <cstdint>
#include <string>
#include <vector>
