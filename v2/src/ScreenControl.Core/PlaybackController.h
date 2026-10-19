#pragma once

// ============================================================================
//  PlaybackController.h — 播放编排（照片 / 视频 -> 编码 -> HID 发送）
// ----------------------------------------------------------------------------
//  对应 v1 tuf_gui.py 里的 PhotoStreamThread / VideoStreamThread 那套「帧泵」，
//  以及 MainWindow 里的 start_photo / start_video / stop_display。
//
//  v1 的做法是每种素材一个 QThread，参数靠 getter 回调逐帧读 UI。
//  v2 收敛成一个控制器：
//    * 只有一个工作线程，切换素材不影响界面
//    * 参数用不可变快照（RenderParams）传递，不再依赖 UI
//    * 设备掉线时停止播放并通过状态回调报告
// ============================================================================


#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "FramePipeline.h"
#include "HidProtocol.h"
#include "MediaDecoders.h"
#include "RenderParams.h"
#include "ScreenController.h"

namespace ScreenControl::Core
{
    class PlaybackController
    {
    public:
        /// 预览回调：传入已适配(未旋转)的 BGRA 画布。在工作线程触发。
        using PreviewCallback =
            std::function<void(std::uint8_t const*, int width, int height, int stride)>;

        /// 状态/错误回调：在工作线程触发。
        using StatusCallback = std::function<void(std::wstring const& message)>;

        PlaybackController();
        ~PlaybackController();

        PlaybackController(PlaybackController const&) = delete;
        PlaybackController& operator=(PlaybackController const&) = delete;

        // ---------------- 设备 ----------------
        bool OpenDevice();
        void CloseDevice();
        bool DeviceReady() const noexcept;
        ::ScreenControl::Device::DeviceState DeviceStateNow() const;

        bool SetBrightness(int percent);
        bool SetScreenPower(bool on);

        /// 发送一张全黑帧（清掉屏幕上残留的上一帧画面）。
        void SendBlankFrame();

        // ---------------- 播放 ----------------
        /// 播放指定素材（照片或视频自动识别）。会先停止当前播放。
        bool Play(std::filesystem::path const& path);

        /// 停止播放（不关闭设备），会等待工作线程结束。
        void Stop();

        /// 只置停止标志、不等待工作线程：用于进程退出路径，避免卡在 join 上。
        void RequestStop() noexcept;

        bool IsPlaying() const noexcept { return m_playing.load(); }

        // ---------------- 参数 ----------------
        void SetParams(Imaging::RenderParams const& params);
        Imaging::RenderParams Params() const;

        /// 照片循环发送的频率（Hz）。视频用素材自身帧率。
        void SetPhotoFps(double fps);

        void SetPreviewCallback(PreviewCallback callback);
        void SetStatusCallback(StatusCallback callback);

    private:
        void WorkerLoop(std::filesystem::path path, MediaKind kind);
        void Notify(std::wstring const& message) const;
        void SleepUntil(std::chrono::steady_clock::time_point deadline) const;

        ::ScreenControl::Device::ScreenController m_device;
        Imaging::FramePipeline m_pipeline;

        mutable std::mutex m_mutex;
        Imaging::RenderParams m_params{};
        double m_photoFps = 10.0;

        PreviewCallback m_preview;
        StatusCallback m_status;

        // 照片静止帧的编码缓存（照片每帧复用，避免重复编码）
        std::vector<std::uint8_t> m_photoJpeg;
        std::vector<std::uint8_t> m_blankJpeg;
        std::atomic<bool> m_photoDirty{ true };

        std::thread m_worker;
        std::atomic<bool> m_running{ false };
        std::atomic<bool> m_playing{ false };
        mutable std::mutex m_stopMutex;
        mutable std::condition_variable m_stopCv;
        bool m_stopRequested = false;
    };
}
