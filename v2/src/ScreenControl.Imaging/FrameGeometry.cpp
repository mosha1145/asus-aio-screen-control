#include "pch.h"
#include "FrameGeometry.h"

#include <algorithm>
#include <cstring>

namespace ScreenControl::Imaging
{
    namespace
    {
        /// 骨架版缩放：最近邻。
        /// TODO(下一步): 换成面积平均下采样（缩小时画质明显更好，对应 v1 的
        /// INTER_AREA 意图），并接入 SIMD（SSE2/AVX2）。
        void BlitNearest(FrameView src, FrameCanvas& canvas, int dstX, int dstY, int dstW, int dstH) noexcept
        {
            const int size = canvas.Size();
            std::uint8_t* dst = canvas.Data();
            const int dstStride = canvas.Stride();

            for (int y = 0; y < dstH; ++y)
            {
                const int sy = src.height > 0 ? std::min(src.height - 1, y * src.height / dstH) : 0;
                const int ty = dstY + y;
                if (ty < 0 || ty >= size)
                {
                    continue;
                }

                const std::uint8_t* srcRow = src.pixels + static_cast<std::size_t>(sy) * src.stride;
                std::uint8_t* dstRow = dst + static_cast<std::size_t>(ty) * dstStride;

                for (int x = 0; x < dstW; ++x)
                {
                    const int tx = dstX + x;
                    if (tx < 0 || tx >= size)
                    {
                        continue;
                    }
                    const int sx = src.width > 0 ? std::min(src.width - 1, x * src.width / dstW) : 0;
                    std::memcpy(dstRow + static_cast<std::size_t>(tx) * 4,
                                srcRow + static_cast<std::size_t>(sx) * 4, 4);
                }
            }
        }
    }

    FrameCanvas::FrameCanvas(int size)
        : m_size(size > 0 ? size : 320)
        , m_pixels(static_cast<std::size_t>(m_size) * m_size * 4, 0)
    {
    }

    void FrameCanvas::Clear() noexcept
    {
        std::memset(m_pixels.data(), 0, m_pixels.size());
    }

    bool FitInto(FrameView src, FitMode mode, FrameCanvas& canvas) noexcept
    {
        if (!src.Valid())
        {
            return false;
        }

        const int size = canvas.Size();
        canvas.Clear();

        if (mode == FitMode::Stretch)
        {
            BlitNearest(src, canvas, 0, 0, size, size);
            return true;
        }

        const double srcW = static_cast<double>(src.width);
        const double srcH = static_cast<double>(src.height);

        if (mode == FitMode::Contain)
        {
            // 长边填满，等比缩放后居中，四周留黑边。
            const double scale = size / (std::max)(srcW, srcH);
            const int dstW = (std::max)(1, static_cast<int>(srcW * scale + 0.5));
            const int dstH = (std::max)(1, static_cast<int>(srcH * scale + 0.5));
            BlitNearest(src, canvas, (size - dstW) / 2, (size - dstH) / 2, dstW, dstH);
            return true;
        }

        // Cover：短边填满，超出部分居中裁剪。
        const double scale = size / (std::min)(srcW, srcH);
        const int dstW = (std::max)(1, static_cast<int>(srcW * scale + 0.5));
        const int dstH = (std::max)(1, static_cast<int>(srcH * scale + 0.5));
        BlitNearest(src, canvas, (size - dstW) / 2, (size - dstH) / 2, dstW, dstH);
        return true;
    }

    FrameCanvas const& RotateInto(FrameCanvas const& canvas, Rotation rotation, FrameCanvas& scratch) noexcept
    {
        if (rotation == Rotation::Deg0)
        {
            return canvas;
        }

        const int size = canvas.Size();
        const std::uint8_t* src = canvas.Data();
        const int srcStride = canvas.Stride();
        std::uint8_t* dst = scratch.Data();
        const int dstStride = scratch.Stride();

        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                const std::uint8_t* p = src + static_cast<std::size_t>(y) * srcStride + static_cast<std::size_t>(x) * 4;

                int tx = x;
                int ty = y;
                switch (rotation)
                {
                case Rotation::Deg90:   // 顺时针 90
                    tx = size - 1 - y;
                    ty = x;
                    break;
                case Rotation::Deg180:
                    tx = size - 1 - x;
                    ty = size - 1 - y;
                    break;
                case Rotation::Deg270:  // 逆时针 90（= 顺时针 270）
                    tx = y;
                    ty = size - 1 - x;
                    break;
                default:
                    break;
                }

                std::memcpy(dst + static_cast<std::size_t>(ty) * dstStride + static_cast<std::size_t>(tx) * 4, p, 4);
            }
        }
        return scratch;
    }
}
