#include "pch.h"
#include "AppClass.h"

#include "AppConfig.h"
#include "MainWindow.h"

#include <sstream>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace ScreenControl
{
    App* App::s_current = nullptr;

    namespace
    {
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
        s_current = this;

        // 配置：加载（含 v1 -> v2 一次性迁移）。
        // 骨架阶段先同步加载以便验证迁移结果；后续接入时移到后台线程。
        Core::ConfigStore store;
        const bool migrated = store.Load();
        m_configSummary = BuildConfigSummary(store, migrated);

        m_mainWindow = std::make_unique<ScreenControl::MainWindow>();
        m_mainWindow->Window().Activate();

        ApplyTheme(store.Data().appTheme);
    }

    void App::ApplyTheme(std::wstring const& theme)
    {
        if (m_mainWindow == nullptr)
        {
            return;
        }
        auto element = m_mainWindow->Window().Content().try_as<FrameworkElement>();
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
// Windows App SDK 在非打包模式下会自动注入 MddBootstrapAutoInitializer.cpp
// （WindowsPackageType=None 时由 NuGet 目标生成），因此这里不需要手工调用
// MddBootstrapInitialize —— 手工调用会与自动初始化冲突。
// ---------------------------------------------------------------------------
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    winrt::Microsoft::UI::Xaml::Application::Start([](auto&&)
    {
        winrt::make<ScreenControl::App>();
    });
    return 0;
}
