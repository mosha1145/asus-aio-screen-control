#pragma once

// ============================================================================
//  JpegEncoder.h — BGR/BGRA -> JPEG（WIC）
// ----------------------------------------------------------------------------
//  对应 v1 的 cv2.imencode('.jpg', bgr_frame, [IMWRITE_JPEG_QUALITY, q])。
//
//  颜色约定（关键，来自协议文档 §6）：
//    设备解码的是标准 JPEG。v1 之所以强调「BGR 直接编码、不要先转 RGB」，
//    是因为 OpenCV 把输入数组当作 BGR 解释；如果先 cvtColor 成 RGB，通道就被
//    错位解释，屏幕上红蓝互换。
//    这里用 WIC 的 GUID_WICPixelFormat32bppBGRA 作为输入格式，
//    同样把字节序声明为 B,G,R,A，语义与 OpenCV 完全一致 —— 不需要任何通道交换。
//
//  性能：工厂对象按线程缓存（thread_local），编码复用同一个 IStream 路径，
//        避免每帧重建 COM 对象的开销。
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace ScreenControl::Imaging
{
    class JpegEncoder
    {
    public:
        /// 把 BGRA 像素编码成 JPEG。quality 取 1..100。
        /// 成功返回 true，outJpeg 为完整的 JPEG 字节流。
        /// 注意：必须在已初始化 COM 的线程上调用（工作线程用 MTA）。
        static bool Encode(std::uint8_t const* bgra,
                           int width,
                           int height,
                           int stride,
                           int quality,
                           std::vector<std::uint8_t>& outJpeg) noexcept;
    };
}
