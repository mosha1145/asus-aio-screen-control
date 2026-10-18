#include "pch.h"
#include "App.h"
#include "MainWindow.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::ScreenControl::implementation
{
    App::App()
    {
        // 注意：XAML 对象不应在构造函数里调用 InitializeComponent，
        // 由框架在合适时机调用（见 cppwinrt 文档 #initializecomponent）。
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto message = e.Message();
                (void)message;
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        window = make<MainWindow>();
        window.Activate();
    }
}
