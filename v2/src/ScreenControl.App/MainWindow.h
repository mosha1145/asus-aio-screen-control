#pragma once

#include "MainWindow.g.h"

namespace ScreenControl
{
    // 主窗口：NavigationView + Frame 导航外壳。
    //
    // 继承 MIDL 生成的 MainWindowT（对应 IDL 里的
    //   runtimeclass MainWindow : Microsoft.UI.Xaml.Window
    // ）。窗口实例由基类构造函数创建，因此不再持有 Window 成员。
    //
    // 说明：命令行 MSBuild 下 C++/WinRT 生成的 ScreenControl 投影是空的
    // （见 README 已知问题，与 WMC1007 同源），所以本文件在纯命令行下无法编译；
    // 用 Visual Studio 2022 打开 AsusAioScreenControl.sln 构建即可正常生成投影与
    // XAML 代码，界面层由此正常工作。
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        void NavigateTo(std::wstring const& tag);

    private:
        void SetupBackdropAndTitleBar();
        winrt::Microsoft::UI::Xaml::UIElement BuildDisplayPage();
        winrt::Microsoft::UI::Xaml::UIElement BuildSettingsPage();

        winrt::Microsoft::UI::Xaml::Controls::Frame m_contentFrame{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_titleBar{ nullptr };
    };
}

namespace ScreenControl::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
