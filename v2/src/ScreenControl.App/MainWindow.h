#pragma once

// 主窗口（x:Class="ScreenControl.MainWindow" 的代码后台声明）。
// 与官方 WinUI3 C++ 模板一致：include C++/WinRT 生成的 MainWindow.g.h
// （内含 MainWindow_base 基类），它会按 __has_include 自动带上 XAML 编译器
// 生成的 MainWindow.xaml.g.h（MainWindowT 模板 + x:Name 元素访问器）。
// 实现类定义在 winrt::ScreenControl::implementation 命名空间。

#include "MainWindow.g.h"
#include "NativeTheme.h"
#include "PowerMonitor.h"
#include "TrayIcon.h"
#include "SingleInstance.h"

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


        // ---------------- 托盘 / 关闭行为 ----------------
        void SetupTrayAndCloseBehavior();
        void HideToTray();
        void ShowFromTray();
        void ShowTrayMenu();
        void ShowClosePrompt();
        void ExitApplication();

        /// 构建一张可折叠的设置卡片（自绘，替代 Expander 以便精确控制对齐与高度）。
        /// glyph: 左侧图标；title: 标题；trailing: 标题行右侧元素（可为 nullptr）；
        /// body: 展开后显示的内容。
        winrt::Microsoft::UI::Xaml::UIElement MakeSettingsCard(
            wchar_t const* glyph,
            winrt::hstring const& title,
            winrt::Microsoft::UI::Xaml::UIElement const& trailing,
            winrt::Microsoft::UI::Xaml::UIElement const& body);

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
        std::wstring m_currentTag{ L"display" };
        ::ScreenControl::PowerMonitor m_powerMonitor;
        ::ScreenControl::TrayIcon m_trayIcon;

        // 承载 WinUI 菜单的隐藏宿主窗口（托盘右键菜单在光标处弹出）
        winrt::Microsoft::UI::Xaml::Window m_menuHost{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_menuAnchor{ nullptr };

        bool m_exiting = false;
        bool m_closePromptOpen = false;
    };
}

namespace winrt::ScreenControl::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
