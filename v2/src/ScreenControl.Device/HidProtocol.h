// ============================================================================
//  HidProtocol.h — HID 控制命令与设备常量
// ----------------------------------------------------------------------------
//  对应 v1: tuf_hid.py 的控制命令部分
//  协议真值: PROTOCOL_REVERSE_REPORT.md §3 §4
//
//  控制报文 441 字节 = 1 字节报告 ID(0x00) + 5 字节命令 + 435 字节零填充
//  命令 = 小端 4 字节命令 ID + 1 字节参数
//
//  亮度  0x80000112 -> 12 01 00 80 <V>   V ∈ 0x00..0x64(0-100)
//  开关  0x80000110 -> 10 01 00 80 <V>   V: 0x00=开  0x01=关
//
//  注意：设备【不需要】任何唤醒/接管命令。报告早期"必须先发 0x80000112 才能
//  接管屏幕"的结论已被实机推翻——纯 JPEG 流即可驱动显示，0x80000112 只是亮度。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ScreenControl::Device
{
    /// 设备 USB 标识。
    inline constexpr std::uint16_t kAsusVendorId = 0x0B05;
    inline constexpr std::uint16_t kScreenProductId = 0x1C7B;

    /// 控制命令 ID。
    inline constexpr std::uint32_t kCmdBrightness = 0x80000112;
    inline constexpr std::uint32_t kCmdScreenOnOff = 0x80000110;

    /// 控制报文长度（含 1 字节报告 ID）。
    inline constexpr std::size_t kControlReportBytes = 441;

    /// 设备连接状态。
    enum class DeviceState
    {
        Disconnected,   ///< 未连接
        Ready,          ///< 控制接口与图像接口均可用
        ImageOnly,      ///< 只有图像接口可用（仍可发帧，无法调亮度/开关）
        ControlOnly,    ///< 只有控制接口可用（无法显示画面）
        Error,          ///< 打开失败
    };

    /// 枚举结果：一对接口路径。
    struct HidInterfacePaths
    {
        std::wstring controlPath;   ///< MI_00，OutputReportByteLength 为 440/441
        std::wstring imagePath;     ///< MI_01，OutputReportByteLength 为 1024/1025

        bool HasControl() const noexcept { return !controlPath.empty(); }
        bool HasImage() const noexcept { return !imagePath.empty(); }
    };

    /// 发送一帧的结果。
    enum class SendStatus
    {
        Ok,
        DeviceGone,       ///< 设备已断开（ERROR_DEVICE_NOT_CONNECTED 等）
        WriteFailed,      ///< 写入失败（重试后仍失败）
        NoImageInterface, ///< 图像接口未打开
    };

    /// 把 Win32 错误码归类，便于上层区分"临时抖动"和"设备没了"。
    SendStatus ClassifyWin32Error(std::uint32_t lastError) noexcept;
}
