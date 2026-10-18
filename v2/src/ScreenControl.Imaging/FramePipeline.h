#pragma once

// ============================================================================
//  FramePipeline.h — 一帧的完整加工：适配 -> 旋转 -> JPEG
// ----------------------------------------------------------------------------
//  对应 v1 每帧的这条链（asset_lib.py）：
//      fit_frame -> rotate_frame -> bgr_to_jpeg
//
//  v2 把它收敛成一个对象，内部复用 320x320 画布与编码缓冲，避免每帧分配。
//  输入统一为 BGRA（WIC / Media Foundation 解码出来都是这个格式）。
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "FrameGeometry.h"
#include "RenderParams.h"

namespace ScreenControl::Imaging
{
    class FramePipeline
    {
    public:
        explicit FramePipeline(int size = 320);

        /// 把任意尺寸的 BGRA 源帧加工成可直接发给设备的 JPEG。
        /// 处理顺序：适配(fit) -> 旋转(rotation) -> JPEG 编码。
        /// outJpeg 复用调用方缓冲（清空后写入）。
        bool RenderJpeg(FrameView src,
                        FitMode fit,
                        Rotation rotation,
                        int jpegQuality,
                        std::vector<std::uint8_t>& outJpeg) noexcept;

        /// 已适配但未旋转的画布（供界面预览，与 v1「预览不旋转」一致）。
        FrameCanvas const& FittedCanvas() const noexcept { return m_canvas; }

        int Size() const noexcept { return m_canvas.Size(); }

    private:
        FrameCanvas m_canvas;
        FrameCanvas m_rotated;
    };
}
