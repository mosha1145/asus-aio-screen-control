#include "pch.h"
#include "FrameChunker.h"

#include <cstring>

namespace ScreenControl::Device
{
    std::uint8_t FrameChunker::BlockCount(std::size_t jpegBytes) noexcept
    {
        if (jpegBytes == 0)
        {
            return 0;
        }
        // N = ceil(jpegBytes / 1020)
        const std::size_t blocks = (jpegBytes + kJpegChunkSize - 1) / kJpegChunkSize;
        // 协议里块编号只有 1 字节；超过 255 块说明 JPEG 异常大，钳位以保持协议合法。
        return static_cast<std::uint8_t>(blocks > 255 ? 255 : blocks);
    }

    void FrameChunker::BuildFirstHeader(std::uint8_t* header, std::uint8_t totalBlocks) noexcept
    {
        header[0] = 0x08;
        header[1] = totalBlocks;   // 首块此处是"总块数"，不是序号
        header[2] = 0x00;
        header[3] = 0x80;          // 帧起始标志
    }

    void FrameChunker::BuildNextHeader(std::uint8_t* header, std::uint8_t index) noexcept
    {
        header[0] = 0x08;
        header[1] = index;         // 后续块是序号 1..N-1
        header[2] = 0x00;
        header[3] = 0x00;
    }

    bool FrameChunker::BuildBlock(std::uint8_t* report,
                                  std::size_t jpegBytes,
                                  std::uint8_t index,
                                  std::span<const std::uint8_t> jpeg) noexcept
    {
        const std::uint8_t total = BlockCount(jpegBytes);
        if (report == nullptr || total == 0 || index >= total)
        {
            return false;
        }

        // 报告 ID：本设备为未编号报告，恒为 0x00。
        report[0] = 0x00;

        if (index == 0)
        {
            BuildFirstHeader(report + 1, total);
        }
        else
        {
            BuildNextHeader(report + 1, index);
        }

        // 载荷：从 index * 1020 开始取最多 1020 字节，不足部分补 0。
        std::uint8_t* payload = report + 1 + 4;
        std::memset(payload, 0, kJpegChunkSize);

        const std::size_t offset = static_cast<std::size_t>(index) * kJpegChunkSize;
        if (offset < jpeg.size())
        {
            const std::size_t available = jpeg.size() - offset;
            const std::size_t take = available < kJpegChunkSize ? available : kJpegChunkSize;
            std::memcpy(payload, jpeg.data() + offset, take);
        }
        return true;
    }
}
