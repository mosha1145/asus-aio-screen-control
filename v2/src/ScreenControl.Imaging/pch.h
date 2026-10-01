#pragma once

// ---------------------------------------------------------------------------
// ScreenControl.Imaging 预编译头
// 帧几何变换（CPU）+ WIC JPEG 编码。不依赖 WinUI。
// ---------------------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <span>
#include <vector>
