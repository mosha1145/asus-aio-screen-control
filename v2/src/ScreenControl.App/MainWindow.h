#pragma once

// 主窗口（x:Class="ScreenControl.MainWindow" 的代码后台声明）。
// 与官方 WinUI3 C++ 模板一致：include C++/WinRT 生成的 MainWindow.g.h
// （内含 MainWindow_base 基类），它会按 __has_include 自动带上 XAML 编译器
// 生成的 MainWindow.xaml.g.h（MainWindowT 模板 + x:Name 元素访问器）。
// 实现类定义在 winrt::ScreenControl::implementation 命名空间。

#include "MainWindow.g.h"

namespace winrt::ScreenControl::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        int32_t MyProperty();
        void MyProperty(int32_t value);

        void NavigateTo(std::wstring const& tag);

    private:
        void SetupBackdropAndTitleBar();
        winrt::Microsoft::UI::Xaml::UIElement BuildDisplayPage();
        winrt::Microsoft::UI::Xaml::UIElement BuildSettingsPage();

        winrt::Microsoft::UI::Xaml::Controls::Frame m_contentFrame{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_titleBar{ nullptr };
    };
}

namespace winrt::ScreenControl::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
