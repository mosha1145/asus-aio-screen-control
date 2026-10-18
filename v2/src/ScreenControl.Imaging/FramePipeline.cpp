#include "pch.h"
#include "FramePipeline.h"
#include "JpegEncoder.h"

namespace ScreenControl::Imaging
{
    FramePipeline::FramePipeline(int size)
        : m_canvas(size)
        , m_rotated(size)
    {
    }

    bool FramePipeline::RenderJpeg(FrameView src,
                                   FitMode fit,
                                   Rotation rotation,
                                   int jpegQuality,
                                   std::vector<std::uint8_t>& outJpeg) noexcept
    {
        outJpeg.clear();

        if (!FitInto(src, fit, m_canvas))
        {
            return false;
        }

        FrameCanvas const& rotated = RotateInto(m_canvas, rotation, m_rotated);
        return JpegEncoder::Encode(rotated.Data(),
                                   rotated.Size(),
                                   rotated.Size(),
                                   rotated.Stride(),
                                   jpegQuality,
                                   outJpeg);
    }
}
