#pragma once

// 主窗口（x:Class="ScreenControl.MainWindow" 的代码后台声明）。
// 与官方 WinUI3 C++ 模板一致：include C++/WinRT 生成的 MainWindow.g.h
// （内含 MainWindow_base 基类），它会按 __has_include 自动带上 XAML 编译器
// 生成的 MainWindow.xaml.g.h（MainWindowT 模板 + x:Name 元素访问器）。
// 实现类定义在 winrt::ScreenControl::implementation 命名空间。

#include "MainWindow.g.h"
#include "NativeTheme.h"

namespace winrt::ScreenControl::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

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

        winrt::Microsoft::UI::Xaml::Controls::Frame m_contentFrame{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_titleBar{ nullptr };
        ::ScreenControl::AppTheme m_theme{ ::ScreenControl::AppTheme::System };
    };
}

namespace winrt::ScreenControl::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
