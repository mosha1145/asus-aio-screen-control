//------------------------------------------------------------------------------
// 补充实现：应用入口 wWinMain
// 本文件对应官方 WinUI3 模板中由 XAML 编译器 pass2 生成的 App.xaml.g.cpp。
// 当前构建链（VS 官方模板 + WindowsAppSDK 1.8）在 C++ 工程下没有产出该文件，
// 导致 LNK2019：WinMain 无法解析（MSVCRTD exe_winmain 引用）。
// 实现与官方生成完全一致的入口：初始化 apartment 并启动 XAML 应用。
//------------------------------------------------------------------------------
#include "pch.h"

int __stdcall wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int /*nCmdShow*/)
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    winrt::Microsoft::UI::Xaml::Application::Start(
        [](auto&&)
        {
            winrt::make<winrt::AsusAioScreenControl::App>();
        });

    return 0;
}
