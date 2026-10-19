#include "pch.h"
#include "App.h"
#include "MainWindow.h"

#include <filesystem>
#include <fstream>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::ScreenControl::implementation
{
    App::App()
    {
        // 注意：XAML 对象不应在构造函数里调用 InitializeComponent，
        // 由框架在合适时机调用（见 cppwinrt 文档 #initializecomponent）。

        // 全局兜底：把未处理异常写进 exe 同目录的 crash.log，
        // 否则 0xC000027B（stowed exception）在事件日志里只有模块偏移，无法定位。
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            try
            {
                wchar_t modulePath[MAX_PATH]{};
                GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
                const std::filesystem::path logPath =
                    std::filesystem::path(modulePath).parent_path() / L"crash.log";

                std::wstring text = L"=== 未处理异常 ===\r\n消息: ";
                text += std::wstring(e.Message());
                text += L"\r\n\r\n";

                // 用 UTF-8 写文件：直接 wofstream 会受 locale 影响，中文会写不出来。
                const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                                        static_cast<int>(text.size()),
                                                        nullptr, 0, nullptr, nullptr);
                std::string utf8(static_cast<size_t>(bytes), '\0');
                ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                      utf8.data(), bytes, nullptr, nullptr);

                std::ofstream log(logPath, std::ios::binary | std::ios::app);
                log.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
                log.flush();
            }
            catch (...)
            {
            }

            if (IsDebuggerPresent())
            {
                __debugbreak();
            }
        });
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        window = make<MainWindow>();
        window.Activate();
    }
}
