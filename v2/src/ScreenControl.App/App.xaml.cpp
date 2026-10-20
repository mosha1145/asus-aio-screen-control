#include "pch.h"
#include "App.h"
#include "MainWindow.h"

#include "AppConfig.h"

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

    namespace
    {
        /// 把强调色注入到应用级资源。
        ///
        /// 这是 WinUI 的「系统强调色」机制：控件（开关、复选框、选中态、
        /// 按钮等）的强调色画刷最终都引用 SystemAccentColor 系列资源，
        /// 因此只需要在创建任何界面元素之前覆盖这一组资源，
        /// 所有原生控件就会自动跟随，不需要逐个控件刷色。
        ///
        /// 注意：资源需要在界面创建前注入，所以修改主题色后需重启生效。
        void ApplyAccentResources(std::wstring const& themeColor)
        {
            auto resources = Application::Current().Resources();

            const bool tufGaming = themeColor == L"tuf";

            if (tufGaming)
            {
                // TUF GAMING 品牌橙。这里只定义一次，其余明暗变化由下面派生，
                // 避免到处散落硬编码颜色值。
                const auto accent = Windows::UI::ColorHelper::FromArgb(0xFF, 0xFF, 0x9E, 0x1B);

                auto lighten = [](Windows::UI::Color const& color, double factor)
                {
                    return Windows::UI::ColorHelper::FromArgb(
                        color.A,
                        static_cast<std::uint8_t>(color.R + (255 - color.R) * factor),
                        static_cast<std::uint8_t>(color.G + (255 - color.G) * factor),
                        static_cast<std::uint8_t>(color.B + (255 - color.B) * factor));
                };
                auto darken = [](Windows::UI::Color const& color, double factor)
                {
                    return Windows::UI::ColorHelper::FromArgb(
                        color.A,
                        static_cast<std::uint8_t>(color.R * (1.0 - factor)),
                        static_cast<std::uint8_t>(color.G * (1.0 - factor)),
                        static_cast<std::uint8_t>(color.B * (1.0 - factor)));
                };

                resources.Insert(box_value(L"SystemAccentColor"), box_value(accent));
                resources.Insert(box_value(L"SystemAccentColorLight1"), box_value(lighten(accent, 0.15)));
                resources.Insert(box_value(L"SystemAccentColorLight2"), box_value(lighten(accent, 0.30)));
                resources.Insert(box_value(L"SystemAccentColorLight3"), box_value(lighten(accent, 0.45)));
                resources.Insert(box_value(L"SystemAccentColorDark1"), box_value(darken(accent, 0.10)));
                resources.Insert(box_value(L"SystemAccentColorDark2"), box_value(darken(accent, 0.20)));
                resources.Insert(box_value(L"SystemAccentColorDark3"), box_value(darken(accent, 0.30)));
            }
            // 跟随系统：不覆盖任何资源，WinUI 默认就使用系统强调色。
        }
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        // 单实例：已有实例在运行时，通知它把窗口叫出来，然后本进程直接退出。
        if (!m_singleInstance.Acquire(L"AsusAioScreenControl"))
        {
            ::ExitProcess(0);
        }

        // 强调色必须在创建任何控件之前注入，否则已经实例化的控件不会跟随。
        // 因此主题色的改动是「重启后生效」。
        try
        {
            ::ScreenControl::Core::ConfigStore config;
            config.Load();
            ApplyAccentResources(config.Data().themeColor);
        }
        catch (...)
        {
        }

        window = make<MainWindow>();
        window.Activate();
    }
}
