#pragma once

#include "pch.h"

namespace ScreenControl
{
    // 主窗口：NavigationView + Frame 导航外壳。
    //
    // 刻意做成「普通 C++ 类 + 持有 Window 成员」，而不是 MIDL runtimeclass：
    // 继承 Microsoft.UI.Xaml.Window 的 runtimeclass 需要 C++/WinRT 的
    // composable 投影，而该投影依赖 WinUI XAML 编译器的代码生成（命令行 MSBuild
    // 下不产出，见 README 已知问题）。直接构造 Window 不依赖任何生成头，
    // 与已验证可运行的探针程序一致。
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
