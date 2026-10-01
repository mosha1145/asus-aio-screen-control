#pragma once

#include "pch.h"

namespace ScreenControl
{
    // 主窗口：NavigationView + Frame 导航外壳。
    // Mica 背衬与自定义标题栏在构造函数里配置。
    // 注意：投影类型在 winrt:: 命名空间下，头文件里必须写全 winrt:: 限定。
    class MainWindow
    {
    public:
        MainWindow();

        winrt::Microsoft::UI::Xaml::Window const& Window() const noexcept { return m_window; }

        void NavigateTo(std::wstring const& tag);

    private:
        void SetupBackdropAndTitleBar();
        winrt::Microsoft::UI::Xaml::UIElement BuildDisplayPage();
        winrt::Microsoft::UI::Xaml::UIElement BuildSettingsPage();

        winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Frame m_contentFrame{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_titleBar{ nullptr };
    };
}
