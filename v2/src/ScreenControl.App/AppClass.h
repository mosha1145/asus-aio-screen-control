#pragma once

#include "pch.h"

#include "MainWindow.h"

namespace ScreenControl
{
    // 应用对象。
    // 刻意使用纯 C++/WinRT 的 ApplicationT<App>，不用 XAML Application（App.xaml）：
    // 命令行 MSBuild 下 XAML Application 的 C++ 代码生成不产出 *.xaml.g.h，
    // 会导致编译失败；纯 C++ App 类不依赖生成头，实测可正常启动。
    struct App : winrt::Microsoft::UI::Xaml::ApplicationT<App>
    {
        App() = default;

        void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

        /// 应用级主题：仅「跟随系统 / 浅色 / 深色」。
        void ApplyTheme(std::wstring const& theme);

        static App* Current() noexcept { return s_current; }

        std::wstring const& ConfigSummary() const noexcept { return m_configSummary; }

    private:
        std::unique_ptr<MainWindow> m_mainWindow;
        std::wstring m_configSummary;

        static App* s_current;
    };
}
