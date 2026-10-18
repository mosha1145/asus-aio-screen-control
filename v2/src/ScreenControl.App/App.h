#pragma once

#include "App.xaml.g.h"

namespace winrt::ScreenControl::implementation
{
    // 应用对象（x:Class="ScreenControl.App" 的代码后台声明）。
    // AppT<App> 由 XAML 编译器生成的 App.xaml.g.h 提供。
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        winrt::Microsoft::UI::Xaml::Window window{ nullptr };
    };
}
