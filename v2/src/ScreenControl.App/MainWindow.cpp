#include "pch.h"
#include "MainWindow.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace ScreenControl
{
    namespace
    {
        // 启动诊断日志（与 AppClass.cpp 写同一个文件）。
        void LogStep(wchar_t const* step)
        {
            wchar_t exePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring path{ exePath };
            const auto slash = path.find_last_of(L'\\');
            path = (slash == std::wstring::npos) ? L"AsusAioScreenControl.log"
                                                 : path.substr(0, slash + 1) + L"AsusAioScreenControl.log";
            std::wstring line{ step };
            line += L"\r\n";
            HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(h, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
                CloseHandle(h);
            }
        }
    }

    MainWindow::MainWindow()
    {
        LogStep(L"[MainWindow] ctor enter");

        // 窗口由 MainWindowT 的基类构造函数创建（IDL: runtimeclass : Window），
        // 这里只需构建内容树。
        Grid root;
        LogStep(L"[MainWindow] root Grid created");

        TextBlock text;
        text.Text(L"ASUS AIO Screen Control");
        text.HorizontalAlignment(HorizontalAlignment::Center);
        text.VerticalAlignment(VerticalAlignment::Center);
        root.Children().Append(text);
        LogStep(L"[MainWindow] text appended");

        m_window.Content(root);
        LogStep(L"[MainWindow] content set");

        m_window.Title(L"ASUS AIO Screen Control");
        LogStep(L"[MainWindow] ctor done");
    }

    void MainWindow::SetupBackdropAndTitleBar() {}
    void MainWindow::NavigateTo(std::wstring const&) {}
    UIElement MainWindow::BuildDisplayPage() { return nullptr; }
    UIElement MainWindow::BuildSettingsPage() { return nullptr; }
}
