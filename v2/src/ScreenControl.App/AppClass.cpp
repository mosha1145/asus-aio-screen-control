#include "pch.h"
#include "AppClass.h"

#include "AppConfig.h"

#include <sstream>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace ScreenControl
{
    App* App::s_current = nullptr;

    namespace
    {
        /// 骨架阶段的启动诊断日志。
        /// 写到 exe 同目录的 AsusAioScreenControl.log。
        /// 存在的意义：WinUI 启动失败时若无 unhandled exception 处理器，
        /// 进程会「静默退出」且事件日志里什么都没有；有了这个日志才能定位到哪一步断掉。
        void LogStep(char const* step)
        {
            wchar_t exePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring path{ exePath };
            const auto slash = path.find_last_of(L'\\');
            path = (slash == std::wstring::npos) ? L"AsusAioScreenControl.log"
                                                 : path.substr(0, slash + 1) + L"AsusAioScreenControl.log";

            std::wstring line;
            const char* s = step;
            while (s != nullptr && *s != '\0')
            {
                line.push_back(static_cast<wchar_t>(*s++));
            }
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

        /// 骨架阶段用 wostringstream 拼配置摘要：
        /// std::format 在 wchar_t + 多参数场景下 MSVC 的编译期检查容易误报，先避开。
        std::wstring BuildConfigSummary(Core::ConfigStore const& store, bool migrated)
        {
            Core::AppConfig const& c = store.Data();
            std::wostringstream out;
            out << L"配置位置: " << store.Path().wstring() << L"\n";
            if (migrated)
            {
                out << L"（已从 v1 配置迁移）\n";
            }
            out << L"schema_version: " << c.schemaVersion << L"\n";
            out << L"屏幕开关: " << (c.screenOn ? L"开" : L"关")
                << L"   亮度: " << c.brightness << L"\n";
            out << L"适应模式: " << c.fitMode << L"   旋转: " << c.rotation << L"°\n";
            out << L"上次内容: " << (c.displayType.empty() ? L"无" : c.displayType)
                << L"（视频: " << (c.videoPath.empty() ? L"无" : c.videoPath) << L"）\n";
            out << L"视频质量: " << c.videoQuality
                << L"   照片帧率: " << c.photoFps << L" Hz\n";
            out << L"界面主题: " << c.appTheme;
            return out.str();
        }
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        LogStep("[App] OnLaunched entered");
        s_current = this;

        try
        {
            // 配置：加载（含 v1 -> v2 一次性迁移）。
            // 骨架阶段先同步加载以便验证迁移结果；后续接入时移到后台线程。
            Core::ConfigStore store;
            const bool migrated = store.Load();
            m_configSummary = BuildConfigSummary(store, migrated);
            LogStep("[App] config loaded");

            // 窗口必须在这里创建：此时 WinUI 的 UI 线程与 DispatcherQueue 已就绪。
            m_mainWindow = winrt::make<ScreenControl::MainWindow>();
            LogStep("[App] MainWindow constructed");

            m_mainWindow.Activate();
            LogStep("[App] MainWindow activated");

            ApplyTheme(store.Data().appTheme);
            LogStep("[App] theme applied");
        }
        catch (hresult_error const& e)
        {
            std::wstring msg = L"[App] OnLaunched hresult_error: ";
            msg += e.message().c_str();
            LogStep(winrt::to_string(msg).c_str());
            throw;
        }
        catch (std::exception const& e)
        {
            LogStep(e.what());
            throw;
        }
    }

    void App::ApplyTheme(std::wstring const& theme)
    {
        if (m_mainWindow == nullptr)
        {
            return;
        }
        auto element = m_mainWindow.Content().try_as<FrameworkElement>();
        if (element == nullptr)
        {
            return;
        }

        // Default = 跟随系统；Light/Dark 为显式覆盖。
        if (theme == L"light")
        {
            element.RequestedTheme(ElementTheme::Light);
        }
        else if (theme == L"dark")
        {
            element.RequestedTheme(ElementTheme::Dark);
        }
        else
        {
            element.RequestedTheme(ElementTheme::Default);
        }
    }
}

// ---------------------------------------------------------------------------
// 程序入口。
//
// 关于消息循环：WinUI 3 不需要（也不应该）手工建 DispatcherQueueController 或
// 自己跑 RunEventLoop —— Microsoft::UI::Xaml::Application::Start 内部会创建
// UI 线程的 DispatcherQueue 并驱动消息泵，直到最后一个窗口关闭。
// 我们只要保证：
//   1) Application::Start 的 λ 里创建 App 实例（ApplicationT<App> 会把它注册为
//      应用回调目标，OnLaunched 才会被调用）；
//   2) OnLaunched 里创建并 Activate 窗口。
//
// 注意：这里【不】调用 init_apartment。Application::Start 会自行初始化 UI 线程
// 的 apartment；提前手工初始化会与之冲突。
//
// Windows App SDK 在非打包模式下会自动注入 MddBootstrapAutoInitializer.cpp，
// 因此也不需要手工调用 MddBootstrapInitialize。
// ---------------------------------------------------------------------------
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        winrt::Microsoft::UI::Xaml::Application::Start([](auto&&)
        {
            winrt::make<ScreenControl::App>();
        });
    }
    catch (winrt::hresult_error const& e)
    {
        std::wstring msg = L"Application::Start failed: ";
        msg += e.message().c_str();
        MessageBoxW(nullptr, msg.c_str(), L"ASUS AIO Screen Control", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
