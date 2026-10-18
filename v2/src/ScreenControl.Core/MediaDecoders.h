#pragma once

// ============================================================================
//  MediaDecoders.h — 照片 / 视频解码
// ----------------------------------------------------------------------------
//  v1 用 OpenCV（cv2.imread / cv2.VideoCapture）同时搞定两种素材。
//  v2 换成系统原生组件，避免引入 OpenCV 运行时：
//    * 照片：WIC（Windows Imaging Component）解码，统一转成 BGRA
//    * 视频：Media Foundation SourceReader 解码，输出 RGB32(BGRA)
//
//  两者都输出「任意尺寸 BGRA 帧」，后续由 Imaging::FramePipeline 统一
//  适配到 320x320 并编码成 JPEG。
//
//  Media Foundation 的 RGB32 输出是自底向上的（stride 为负），这里在
//  ReadFrame 内部翻正成自顶向下，方便下游统一按 stride>0 处理。
// ============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ScreenControl::Core
{
    /// 一帧解码结果（BGRA，自顶向下）。
    struct DecodedFrame
    {
        std::vector<std::uint8_t> pixels;
        int width = 0;
        int height = 0;
        int stride = 0;

        bool Empty() const noexcept { return pixels.empty() || width <= 0 || height <= 0; }
    };

    /// 素材来源。
    enum class MediaKind
    {
        Photo,
        Video,
    };

    /// 统一的解码器接口。
    class IMediaDecoder
    {
    public:
        virtual ~IMediaDecoder() = default;

        /// 打开素材。视频会初始化解码器并定位到第一帧。
        virtual bool Open(std::filesystem::path const& path) = 0;

        virtual void Close() = 0;

        /// 取下一帧。照片重复返回同一帧；视频顺序解码，到结尾时回到开头（循环播放）。
        virtual bool ReadFrame(DecodedFrame& frame) = 0;

        /// 建议的播放帧率（照片固定由外部控制）。
        virtual double Fps() const = 0;
    };

    /// 照片解码器（WIC）。解码一次，之后每帧返回同一份像素。
    class PhotoDecoder final : public IMediaDecoder
    {
    public:
        bool Open(std::filesystem::path const& path) override;
        void Close() override;
        bool ReadFrame(DecodedFrame& frame) override;
        double Fps() const override { return 0.0; }

    private:
        DecodedFrame m_frame;
        bool m_loaded = false;
    };

    /// 视频解码器（Media Foundation SourceReader）。
    class VideoDecoder final : public IMediaDecoder
    {
    public:
        VideoDecoder() = default;
        ~VideoDecoder() override;

        VideoDecoder(VideoDecoder const&) = delete;
        VideoDecoder& operator=(VideoDecoder const&) = delete;

        bool Open(std::filesystem::path const& path) override;
        void Close() override;
        bool ReadFrame(DecodedFrame& frame) override;
        double Fps() const override { return m_fps; }

    private:
        bool SeekToStart();

        void* m_reader = nullptr;          // IMFSourceReader*
        void* m_outputType = nullptr;      // IMFMediaType*
        int m_width = 0;
        int m_height = 0;
        int m_stride = 0;                  // 正的步长（已按翻转后的朝向）
        bool m_bottomUp = false;
        double m_fps = 30.0;
        bool m_mfStarted = false;
        std::vector<std::uint8_t> m_flipBuffer;
    };

    /// 根据扩展名判断素材类型。
    MediaKind DetectMediaKind(std::filesystem::path const& path);
}
