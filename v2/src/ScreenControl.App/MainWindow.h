#pragma once

// 主窗口（x:Class="ScreenControl.MainWindow" 的代码后台声明）。
// 与官方 WinUI3 C++ 模板一致：include C++/WinRT 生成的 MainWindow.g.h
// （内含 MainWindow_base 基类），它会按 __has_include 自动带上 XAML 编译器
// 生成的 MainWindow.xaml.g.h（MainWindowT 模板 + x:Name 元素访问器）。
// 实现类定义在 winrt::ScreenControl::implementation 命名空间。

#include "MainWindow.g.h"
#include "NativeTheme.h"
#include "PowerMonitor.h"

#include <memory>

namespace ScreenControl::Core { class PlaybackController; }

namespace winrt::ScreenControl::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        int32_t MyProperty();
        void MyProperty(int32_t value);

        void NavigateTo(std::wstring const& tag);

        /// 应用主题（跟随系统 / 浅色 / 深色）。
        /// persist = true 时同时写回配置。
        void ApplyTheme(::ScreenControl::AppTheme theme, bool persist);

    private:
        void SetupBackdropAndTitleBar();
        winrt::Microsoft::UI::Xaml::UIElement BuildDisplayPage();
        winrt::Microsoft::UI::Xaml::UIElement BuildSettingsPage();

        /// 当前窗口的原生 HWND（用于设置标题栏深色 / 系统菜单深色）。
        HWND GetHwnd() const noexcept;

        /// 懒创建播放控制器（并尝试打开设备）。
        ::ScreenControl::Core::PlaybackController& Player();
        void ShutdownPlayer() noexcept;

        /// 注册电源 / 会话事件（睡眠、关机、注销）与窗口关闭处理。
        void SetupPowerEvents();

        /// 启动时把配置里的显示参数与屏幕状态应用到设备。
        void ApplyPersistedDeviceState();

        /// 把素材目录里的文件填进 ListView。
        void PopulateMediaList(winrt::Microsoft::UI::Xaml::Controls::ListView const& list,
                               std::wstring const& directory,
                               std::wstring const& extensionsCsv);

        /// 用一帧 BGRA 像素刷新预览图（必须在 UI 线程调用）。
        void UpdatePreview(std::vector<std::uint8_t> const& pixels, int width, int height);

        winrt::Microsoft::UI::Xaml::Controls::Frame m_contentFrame{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_titleBar{ nullptr };
        ::ScreenControl::AppTheme m_theme{ ::ScreenControl::AppTheme::System };

        std::unique_ptr<::ScreenControl::Core::PlaybackController> m_player;
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_statusText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_deviceText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Image m_previewImage{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource m_previewSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_previewPlaceholder{ nullptr };
        ::ScreenControl::PowerMonitor m_powerMonitor;
    };
}

namespace winrt::ScreenControl::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
