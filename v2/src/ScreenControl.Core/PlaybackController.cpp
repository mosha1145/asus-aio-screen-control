#include "pch.h"
#include "PlaybackController.h"

#include "FrameChunker.h"

#include <chrono>
#include <vector>

using namespace std::chrono_literals;

namespace ScreenControl::Core
{
    PlaybackController::PlaybackController()
        : m_pipeline(::ScreenControl::Device::kScreenSize)
    {
    }

    PlaybackController::~PlaybackController()
    {
        Stop();
        CloseDevice();
    }

    // ==================== 设备 ====================

    bool PlaybackController::OpenDevice()
    {
        const bool opened = m_device.Open();
        if (!opened)
        {
            Notify(L"未找到水冷屏设备（VID_0B05 / PID_1C7B）。请确认设备已连接，并关闭官方 InfoHub。");
        }
        return opened;
    }

    void PlaybackController::CloseDevice()
    {
        m_device.Close();
    }

    bool PlaybackController::DeviceReady() const noexcept
    {
        return m_device.IsOpen();
    }

    ::ScreenControl::Device::DeviceState PlaybackController::DeviceStateNow() const
    {
        return m_device.State();
    }

    namespace
    {
        /// 按需打开设备：控件操作时若设备尚未打开（例如启动时 InfoHub 占用，
        /// 之后用户退出了它），这里再试一次，避免"控件没反应"。
        bool EnsureDevice(::ScreenControl::Device::ScreenController& device)
        {
            if (device.IsOpen())
            {
                return true;
            }
            return device.Open();
        }
    }

    bool PlaybackController::SetBrightness(int percent)
    {
        if (percent < 0) { percent = 0; }
        if (percent > 100) { percent = 100; }
        if (!EnsureDevice(m_device))
        {
            return false;
        }
        return m_device.SetBrightness(static_cast<std::uint8_t>(percent));
    }

    bool PlaybackController::SetScreenPower(bool on)
    {
        if (!EnsureDevice(m_device))
        {
            return false;
        }

        // 只下发开/关屏命令。没有新帧时固件会自己回到自带动画，
        // 因此这里不做任何"补帧"动作。
        return m_device.SetScreen(on);
    }

    // ==================== 参数 ====================

    void PlaybackController::SetParams(Imaging::RenderParams const& params)
    {
        {
            std::lock_guard lock(m_mutex);
            m_params = params;
        }
        m_photoDirty.store(true);
    }

    Imaging::RenderParams PlaybackController::Params() const
    {
        std::lock_guard lock(m_mutex);
        return m_params;
    }

    void PlaybackController::SetPhotoFps(double fps)
    {
        std::lock_guard lock(m_mutex);
        // 下限 5 Hz：帧间隔过长时固件会在两帧之间回落自带动画，表现为「一亮一灭」。
        const double clamped = fps < 5.0 ? 5.0 : fps;
        m_photoFps = clamped;
    }

    void PlaybackController::SetPreviewCallback(PreviewCallback callback)
    {
        std::lock_guard lock(m_mutex);
        m_preview = std::move(callback);
    }

    void PlaybackController::SetStatusCallback(StatusCallback callback)
    {
        std::lock_guard lock(m_mutex);
        m_status = std::move(callback);
    }

    void PlaybackController::Notify(std::wstring const& message) const
    {
        StatusCallback callback;
        {
            std::lock_guard lock(m_mutex);
            callback = m_status;
        }
        if (callback)
        {
            callback(message);
        }
    }

    // ==================== 播放 ====================

    bool PlaybackController::Play(std::filesystem::path const& path)
    {
        Stop();

        if (path.empty() || !std::filesystem::exists(path))
        {
            Notify(L"素材不存在：" + path.wstring());
            return false;
        }

        // 播放前再试一次打开设备：用户可能刚关掉官方 InfoHub（它会独占 HID 接口）。
        if (!m_device.IsOpen())
        {
            if (!m_device.Open())
            {
                Notify(L"无法打开水冷屏设备：请关闭官方 InfoHub 后重试（该软件会独占 HID 接口）。");
                return false;
            }
        }

        const MediaKind kind = DetectMediaKind(path);

        m_running.store(true);
        m_playing.store(true);
        {
            std::lock_guard lock(m_stopMutex);
            m_stopRequested = false;
        }

        m_worker = std::thread(&PlaybackController::WorkerLoop, this, path, kind);
        return true;
    }

    void PlaybackController::Stop()
    {
        {
            std::lock_guard lock(m_stopMutex);
            m_stopRequested = true;
        }
        m_stopCv.notify_all();

        m_running.store(false);
        m_playing.store(false);

        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

    void PlaybackController::RequestStop() noexcept
    {
        {
            std::lock_guard lock(m_stopMutex);
            m_stopRequested = true;
        }
        m_stopCv.notify_all();
        m_running.store(false);
        m_playing.store(false);
    }
    void PlaybackController::SleepUntil(std::chrono::steady_clock::time_point deadline) const
    {
        // 用条件变量等待，停止请求可以立即打断，避免 Stop() 卡在 sleep 上。
        std::unique_lock lock(m_stopMutex);
        m_stopCv.wait_until(lock, deadline, [this] { return m_stopRequested; });
    }

    void PlaybackController::WorkerLoop(std::filesystem::path path, MediaKind kind)
    {
        std::unique_ptr<IMediaDecoder> decoder;
        if (kind == MediaKind::Photo)
        {
            decoder = std::make_unique<PhotoDecoder>();
        }
        else
        {
            decoder = std::make_unique<VideoDecoder>();
        }

        if (!decoder->Open(path))
        {
            Notify(L"无法打开素材：" + path.filename().wstring());
            m_running.store(false);
            m_playing.store(false);
            return;
        }

        // 换素材时必须丢弃上一张照片的编码缓存，否则会继续发送上一张的内容
        // （表现为"切换照片无效"）。
        m_photoJpeg.clear();
        m_photoDirty.store(true);

        Notify(L"正在播放：" + path.filename().wstring());

        DecodedFrame frame;
        std::vector<std::uint8_t> jpeg;
        int consecutiveFailures = 0;

        while (m_running.load() && !m_stopRequested)
        {
            const auto tickStart = std::chrono::steady_clock::now();

            if (!decoder->ReadFrame(frame) || frame.Empty())
            {
                Notify(L"解码失败：" + path.filename().wstring());
                break;
            }

            Imaging::RenderParams params = Params();

            // 照片是静止画面：编码一次就复用，避免每帧重复「缩放 + 旋转 + JPEG 编码」。
            // 这既是性能优化，也避免编码耗时把发送间隔拉长、导致屏幕一亮一灭。
            if (kind == MediaKind::Photo && !m_photoDirty.load() && !m_photoJpeg.empty())
            {
                jpeg = m_photoJpeg;
            }
            else
            {
                Imaging::FrameView view{ frame.pixels.data(), frame.width, frame.height, frame.stride };
                if (!m_pipeline.RenderJpeg(view, params.fitMode, params.rotation,
                                           params.jpegQuality, jpeg))
                {
                    Notify(L"帧编码失败");
                    break;
                }
                if (kind == MediaKind::Photo)
                {
                    m_photoJpeg = jpeg;
                    m_photoDirty.store(false);
                }
            }

            // 预览：已适配、未旋转
            PreviewCallback preview;
            {
                std::lock_guard lock(m_mutex);
                preview = m_preview;
            }
            if (preview)
            {
                const Imaging::FrameCanvas& canvas = m_pipeline.FittedCanvas();
                preview(canvas.Data(), canvas.Size(), canvas.Size(), canvas.Stride());
            }

            // 发送到设备
            if (m_device.IsOpen())
            {
                const auto status = m_device.SendFrame(jpeg, 0);
                if (status == ::ScreenControl::Device::SendStatus::Ok)
                {
                    consecutiveFailures = 0;
                }
                else if (status == ::ScreenControl::Device::SendStatus::DeviceGone)
                {
                    Notify(L"设备已断开，播放已停止。");
                    break;
                }
                else
                {
                    ++consecutiveFailures;
                    if (consecutiveFailures >= 30)
                    {
                        Notify(L"连续多帧发送失败，播放已停止。");
                        break;
                    }
                }
            }

            // 节流
            double fps = decoder->Fps();
            if (kind == MediaKind::Video && fps > 30.0)
            {
                // 上限 30 fps：320x320 JPEG 在 HID 上的带宽有限，
                // 60fps 素材直接按原速发会持续排队、画面越播越滞后。
                fps = 30.0;
            }
            if (kind == MediaKind::Photo)
            {
                std::lock_guard lock(m_mutex);
                fps = m_photoFps;
            }
            if (fps < 0.1)
            {
                fps = 0.1;
            }
            const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / fps));
            SleepUntil(tickStart + period);
        }

        decoder->Close();
        m_running.store(false);
        m_playing.store(false);
        Notify(L"已停止显示");
    }
}
