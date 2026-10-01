// ============================================================================
//  FrameGeometry.h — 帧适配与旋转（对应 v1 asset_lib.fit_frame / rotate_frame）
// ----------------------------------------------------------------------------
//  v1 每帧都用 OpenCV 无条件重算，是主要的 CPU 浪费点之一。
//  v2 这里把几何变换做成「纯函数 + 目标画布复用」，为后续加脏标记/缓存留接口。
//
//  尺寸约定：设备只接受 320x320。
//  颜色约定：全程 BGR/BGRA8，绝对不做 BGR<->RGB 交换（否则红蓝互换）。
// ============================================================================
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "RenderParams.h"

namespace ScreenControl::Imaging
{
    /// 一帧像素：BGRA8，行距 = width * 4。
    struct FrameView
    {
        const std::uint8_t* pixels = nullptr;
        int width = 0;
        int height = 0;
        int stride = 0;

        bool Valid() const noexcept
        {
            return pixels != nullptr && width > 0 && height > 0 && stride >= width * 4;
        }
    };

    /// 目标画布：预分配 320x320 BGRA，避免每帧分配。
    class FrameCanvas
    {
    public:
        explicit FrameCanvas(int size = 320);

        std::uint8_t* Data() noexcept { return m_pixels.data(); }
        const std::uint8_t* Data() const noexcept { return m_pixels.data(); }
        int Size() const noexcept { return m_size; }
        int Stride() const noexcept { return m_size * 4; }

        /// 以 0 填充整块画布（contain 模式的黑边来源）。
        void Clear() noexcept;

    private:
        int m_size;
        std::vector<std::uint8_t> m_pixels;
    };

    /// 把 src 按 fitMode 缩放进 canvas（尺寸固定 canvas.Size()）。
    /// 返回 false 表示输入无效。
    bool FitInto(FrameView src, FitMode mode, FrameCanvas& canvas) noexcept;

    /// 就地顺时针旋转 canvas。rotation 为 Deg90/Deg180/Deg270 时返回新的画布，
    /// Deg0 时直接返回引用本身（避免无谓拷贝）。
    FrameCanvas const& RotateInto(FrameCanvas const& canvas, Rotation rotation, FrameCanvas& scratch) noexcept;
}
