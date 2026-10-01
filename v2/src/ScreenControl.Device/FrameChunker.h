// ============================================================================
//  FrameChunker.h — HID 图像帧分块（协议层，纯逻辑，可单测）
// ----------------------------------------------------------------------------
//  对应 v1: tuf_hid.TufCooler.send_jpeg_frame
//  协议真值: PROTOCOL_REVERSE_REPORT.md §5
//
//  设备只接受 320x320 的 JPEG。一帧 JPEG 被切成 N 个 1020 字节的块，逐块发送。
//  每个报文 1024 字节数据 + 1 字节报告 ID(0x00)，共 1025 字节：
//
//      偏移 0    1        2    3       4..1023
//          08   [块编号]  00   [标志]  [1020 字节 JPEG 载荷]
//
//  首块: 块编号位置放的是【总块数 N】，标志位 0x80 表示帧起始。
//  后续: 块编号是序号 1..N-1，标志位 0x00。
//  最后一块不足 1020 字节时用 0x00 补满。
//
//  这是整个项目最容易写错、错了又最难察觉的地方，所以单独成类并配单测。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ScreenControl::Device
{
    /// 单块 JPEG 载荷长度（字节）。
    inline constexpr std::size_t kJpegChunkSize = 1020;

    /// 图像接口 OutputReportByteLength（含 1 字节报告 ID）。
    inline constexpr std::size_t kImageReportLength = 1025;

    /// 控制接口 OutputReportByteLength（含 1 字节报告 ID）。
    inline constexpr std::size_t kControlReportLength = 441;

    /// 控制接口线缆长度（不含报告 ID）。
    inline constexpr std::size_t kControlPayloadLength = kControlReportLength - 1;

    /// 屏幕原生分辨率。
    inline constexpr int kScreenSize = 320;

    /// 一个完整的图像报文（报告 ID + 4 字节块头 + 1020 字节载荷）。
    using ImageReport = std::uint8_t[kImageReportLength];

    class FrameChunker
    {
    public:
        /// 一帧所需的总块数；jpeg 为空时返回 0。
        static std::uint8_t BlockCount(std::size_t jpegBytes) noexcept;

        /// 把第 index 块写入 report。
        /// - index == 0 时为首块（块编号 = 总块数，标志位 0x80）
        /// - 其余为后续块（块编号 = index，标志位 0x00）
        /// 返回 false 表示 index 越界。
        static bool BuildBlock(std::uint8_t* report,
                               std::size_t jpegBytes,
                               std::uint8_t index,
                               std::span<const std::uint8_t> jpeg) noexcept;

        /// 首块的 4 字节头：{0x08, totalBlocks, 0x00, 0x80}
        static void BuildFirstHeader(std::uint8_t* header, std::uint8_t totalBlocks) noexcept;

        /// 后续块的 4 字节头：{0x08, index, 0x00, 0x00}
        static void BuildNextHeader(std::uint8_t* header, std::uint8_t index) noexcept;
    };
}
