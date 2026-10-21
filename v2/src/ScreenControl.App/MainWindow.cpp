#include "pch.h"
#include "MainWindow.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <Microsoft.UI.Windowing.h>

#include "AppConfig.h"
#include "PlaybackController.h"
#include "UiHelpers.h"
#include "DisplayPage.xaml.h"
#include "SettingsPage.xaml.h"

#include <filesystem>
#include <vector>

#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <functional>
#include <fstream>
#include <memory>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;

namespace winrt::ScreenControl::implementation
{

static ::winrt::ScreenControl::implementation::MainWindow* s_current = nullptr;


    int32_t MainWindow::MyProperty()
    {
        throw hresult_not_implemented();
    }

    void MainWindow::MyProperty(int32_t /* value */)
    {
        throw hresult_not_implemented();
    }

    MainWindow::MainWindow()
    {
        // 从 MainWindow.xaml 加载根容器（RootGrid）并注册窗口类型。
        InitializeComponent();

        // 重要：m_titleBar / m_contentFrame 在头文件里初始化为 nullptr，
        // 必须在这里先实例化，否则后续对它们调用任何成员都会导致
        // 空引用访问冲突（0xC0000005）——这正是之前的启动崩溃原因。
        s_current = this;

        m_titleBar = Grid{};
        m_contentFrame = Frame{};

        // 根容器背景留空：一旦设了实色，Mica 就被盖住了。
        Grid root = RootGrid();

        // ---- 自定义标题栏（占第一行，作为拖动区） ----
        m_titleBar.Height(44);
        StackPanel titleContent;
        titleContent.Orientation(Orientation::Horizontal);
        titleContent.Margin(Thickness{ 16, 0, 0, 0 });
        titleContent.VerticalAlignment(VerticalAlignment::Center);
        titleContent.Spacing(10);

        FontIcon icon;
        icon.Glyph(L"\xE7F4");
        icon.FontSize(15);
        titleContent.Children().Append(icon);

        TextBlock title;
        title.Text(L"ASUS AIO Screen Control");
        title.FontSize(13);
        title.VerticalAlignment(VerticalAlignment::Center);
        titleContent.Children().Append(title);

        m_titleBar.Children().Append(titleContent);

        // ---- NavigationView + Frame ----
        NavigationView nav;
        nav.IsBackButtonVisible(NavigationViewBackButtonVisible::Collapsed);
        nav.IsSettingsVisible(false);
        // 官方 NavigationView 的自适应行为：
        //   宽度 >= ExpandedModeThresholdWidth  -> Left（内联推拉）
        //   Compact <= 宽度 < Expanded          -> Compact
        //   宽度 <  CompactModeThresholdWidth   -> Minimal（面板悬浮在内容之上）
        nav.PaneDisplayMode(NavigationViewPaneDisplayMode::Auto);
        nav.CompactModeThresholdWidth(641.0);
        nav.ExpandedModeThresholdWidth(1008.0);
        nav.OpenPaneLength(216);

        NavigationViewItem itemDisplay;
        itemDisplay.Content(box_value(L"主控"));
        itemDisplay.Tag(box_value(L"display"));
        FontIcon displayIcon;
        displayIcon.Glyph(L"\xE7F4");
        itemDisplay.Icon(displayIcon);
        nav.MenuItems().Append(itemDisplay);

        NavigationViewItem itemSettings;
        itemSettings.Content(box_value(L"设置"));
        itemSettings.Tag(box_value(L"settings"));
        FontIcon settingsIcon;
        settingsIcon.Glyph(L"\xE713");
        itemSettings.Icon(settingsIcon);
        nav.MenuItems().Append(itemSettings);

        nav.Content(m_contentFrame);

        nav.SelectionChanged([this](NavigationView const&, NavigationViewSelectionChangedEventArgs const& args)
        {
            auto item = args.SelectedItem().try_as<NavigationViewItem>();
            if (!item)
            {
                return;
            }

            const std::wstring tag{ unbox_value<hstring>(item.Tag()).c_str() };
            if (!m_currentTag.empty() && tag == m_currentTag)
            {
                return;   // 判重：同一个页面不重复导航
            }
            m_currentTag = tag;

            // 显式指定"从下往上"的滑入动画（官方 SlideNavigationTransitionInfo）
            Microsoft::UI::Xaml::Media::Animation::SlideNavigationTransitionInfo slide;
            slide.Effect(Microsoft::UI::Xaml::Media::Animation::SlideNavigationTransitionEffect::FromBottom);
            if (tag == L"settings")
            {
                m_contentFrame.Navigate(xaml_typename<ScreenControl::SettingsPage>(), nullptr, slide);
            }
            else
            {
                m_contentFrame.Navigate(xaml_typename<ScreenControl::DisplayPage>(), nullptr, slide);
            }

            // 关键：必须在 Navigate 之后清空。
            // Navigate 会把"上一个页面"压入 BackStack，先清再导航等于没清，
            // 旧页面（连同它的预览位图）会被一直引用，表现为每次切换内存增长。
            m_contentFrame.BackStack().Clear();
        });

        // 布局：标题栏独占第 0 行，NavigationView 只占第 1 行。
        // 早前让 NavigationView 跨第 0~1 行，导致折叠面板的汉堡按钮/面板与
        // 自定义标题栏区域重叠，折叠后显示错乱。分开后互不干扰。
        Grid::SetRow(m_titleBar, 0);
        Grid::SetRow(nav, 1);
        root.Children().Append(m_titleBar);
        root.Children().Append(nav);

        Title(L"ASUS AIO Screen Control");

        SetupBackdropAndTitleBar();
        nav.SelectedItem(itemDisplay);

        // 启动时按配置恢复主题（含「跟随系统」）。
        m_theme = ::ScreenControl::NativeTheme::Parse(::ScreenControl::Ui::Config().Data().appTheme);
        ApplyTheme(m_theme, false);

        // 恢复上次的显示参数与屏幕开关状态
        ApplyPersistedDeviceState();

        // 电源 / 会话事件（睡眠、关机、注销）与窗口关闭处理。
        SetupPowerEvents();

        // 托盘图标 + 关闭行为（隐藏到托盘 / 退出 / 询问）
        SetupTrayAndCloseBehavior();
    }

    void MainWindow::ApplyPersistedDeviceState()
    {
        auto& config = ::ScreenControl::Ui::Config().Data();

        auto params = Player().Params();
        if (config.fitMode == L"cover")
        {
            params.fitMode = ::ScreenControl::Imaging::FitMode::Cover;
        }
        else if (config.fitMode == L"stretch")
        {
            params.fitMode = ::ScreenControl::Imaging::FitMode::Stretch;
        }
        else
        {
            params.fitMode = ::ScreenControl::Imaging::FitMode::Contain;
        }

        switch (config.rotation)
        {
        case 90:  params.rotation = ::ScreenControl::Imaging::Rotation::Deg90; break;
        case 180: params.rotation = ::ScreenControl::Imaging::Rotation::Deg180; break;
        case 270: params.rotation = ::ScreenControl::Imaging::Rotation::Deg270; break;
        default:  params.rotation = ::ScreenControl::Imaging::Rotation::Deg0; break;
        }

        Player().SetParams(params);
        Player().SetPhotoFps(config.photoFps);

        // 恢复屏幕状态：用户最后一次设定的是开就开、是关就关。
        // 这样即便上次退出时被「电源联动」关了屏，本次启动也会恢复成用户设定，
        // 不需要再手动点两次开关。
        Player().SetBrightness(config.brightness);
        Player().SetScreenPower(config.screenOn);
    }


    void MainWindow::SetupTrayAndCloseBehavior()
    {
        // 拦截窗口关闭：按配置「隐藏到托盘 / 退出 / 询问」处理。
        if (auto appWindow = AppWindow())
        {
            appWindow.Closing([this](Microsoft::UI::Windowing::AppWindow const&,
                                     Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args)
            {
                if (m_exiting)
                {
                    return;   // 真退出：放行，交给 Closed 处理
                }

                auto& config = ::ScreenControl::Ui::Config().Data();
                std::wstring action = config.closeAction;
                if (!config.closePromptShown)
                {
                    action = L"ask";   // 首次关闭一律询问
                }

                if (action == L"tray")
                {
                    if (!m_trayAvailable)
                    {
                        // 托盘图标创建失败（例如进程令牌与 Explorer 不一致被拒绝）时，
                        // 隐藏窗口会让用户无处找回，退化为直接退出。
                        m_exiting = true;
                        return;
                    }
                    args.Cancel(true);
                    HideToTray();
                }
                else if (action == L"exit")
                {
                    m_exiting = true;
                }
                else
                {
                    args.Cancel(true);
                    ShowClosePrompt();
                }
            });
        }

        m_trayAvailable = m_trayIcon.Add(GetHwnd(), L"ASUS AIO Screen Control",
                                         [this]() { ShowFromTray(); },
                                         [this]() { ShowTrayMenu(); });
    }

    void MainWindow::PrepareWindowForHide()
    {
        // 隐藏前先把界面换成"实色底 + 无 Mica"：
        // Mica 背衬在窗口隐藏的瞬间不再被绘制，根容器若还是 Transparent，
        // DWM 就会露出默认白色底，表现为"关闭时白闪"。
        if (auto root = Content().try_as<Panel>())
        {
            const auto background = ::ScreenControl::NativeTheme::ResolveIsDark(m_theme)
                ? Windows::UI::ColorHelper::FromArgb(0xFF, 0x20, 0x20, 0x20)
                : Windows::UI::ColorHelper::FromArgb(0xFF, 0xF3, 0xF3, 0xF3);
            root.Background(Media::SolidColorBrush{ background });
        }
        SystemBackdrop(nullptr);
    }

    void MainWindow::HideToTray()
    {
        PrepareWindowForHide();

        // 关键：等一帧真正合成后再隐藏。
        // DispatcherQueue.TryEnqueue 的回调仍可能早于合成，屏幕上是改底色之前的白帧；
        // 这里用 80ms 定时器确保实色底已经上屏。
        auto appWindow = AppWindow();
        if (!appWindow)
        {
            return;
        }

        auto dispatcher = DispatcherQueue();
        if (dispatcher)
        {
            auto timer = dispatcher.CreateTimer();
            timer.Interval(std::chrono::milliseconds(80));
            timer.IsRepeating(false);
            timer.Tick([appWindow, timer](auto const&, auto const&) mutable
            {
                appWindow.Hide();
                timer.Stop();
            });
            timer.Start();
            return;
        }

        appWindow.Hide();
    }
    void MainWindow::ShowFromTray()
    {
        if (auto appWindow = AppWindow())
        {
            appWindow.Show();
        }

        // 恢复 Mica 背衬：去掉隐藏前刷的实色底
        if (auto root = Content().try_as<Panel>())
        {
            root.Background(nullptr);
        }
        SystemBackdrop(MicaBackdrop{});

        Activate();
        if (HWND hwnd = GetHwnd())
        {
            ::SetForegroundWindow(hwnd);
            ::ShowWindow(hwnd, SW_SHOW);
            ::ShowWindow(hwnd, SW_RESTORE);
        }
    }

    void MainWindow::ShowClosePrompt()
    {
        if (m_closePromptOpen)
        {
            return;
        }

        auto root = Content().try_as<FrameworkElement>();
        if (!root || !root.XamlRoot())
        {
            HideToTray();
            return;
        }

        m_closePromptOpen = true;

        ContentDialog dialog;
        dialog.XamlRoot(root.XamlRoot());
        dialog.Title(box_value(L"关闭窗口时"));
        dialog.Content(box_value(L"关闭主窗口后，程序可以继续在系统托盘运行，水冷屏显示不会中断。"));
        dialog.PrimaryButtonText(L"隐藏到系统托盘");
        dialog.SecondaryButtonText(L"退出程序");
        dialog.CloseButtonText(L"取消");
        dialog.DefaultButton(ContentDialogButton::Primary);

        auto operation = dialog.ShowAsync();
        operation.Completed([this, dialog](auto const& op, Windows::Foundation::AsyncStatus status)
        {
            m_closePromptOpen = false;

            ContentDialogResult result = ContentDialogResult::None;
            if (status == Windows::Foundation::AsyncStatus::Completed)
            {
                result = op.GetResults();
            }

            auto& config = ::ScreenControl::Ui::Config().Data();
            config.closePromptShown = true;


            if (result == ContentDialogResult::Primary)
            {
                config.closeAction = L"tray";
                ::ScreenControl::Ui::Config().Save();
                if (m_trayAvailable)
                {
                    HideToTray();
                }
                else
                {
                    ExitApplication();   // 托盘不可用：直接退出，避免窗口丢失
                }
            }
            else if (result == ContentDialogResult::Secondary)
            {
                config.closeAction = L"exit";
                ::ScreenControl::Ui::Config().Save();
                ExitApplication();
            }
            else
            {
                config.closeAction = L"ask";
                ::ScreenControl::Ui::Config().Save();
            }
        });
    }

    void MainWindow::ExitApplication()
    {
        m_exiting = true;

        // 先隐藏窗口：直接关闭会有一帧白底闪现
        // 先准备实色底并隐藏窗口，避免关闭瞬间合成出白底
        PrepareWindowForHide();
        if (auto appWindow = AppWindow())
        {
            appWindow.Hide();
        }
        m_trayIcon.Remove();

        if (m_player)
        {
            auto const& config = ::ScreenControl::Ui::Config().Data();
            if (config.powerActions && config.powerCloseApp)
            {
                m_player->SetScreenPower(false);
            }
            m_player->RequestStop();    // 不 join，避免卡住
        }

        m_powerMonitor.Stop();

        // 等实色底上屏后再结束进程，避免 DWM 合成出一帧白底
        auto exitDispatcher = DispatcherQueue();
        if (exitDispatcher)
        {
            auto exitTimer = exitDispatcher.CreateTimer();
            exitTimer.Interval(std::chrono::milliseconds(80));
            exitTimer.IsRepeating(false);
            exitTimer.Tick([exitTimer](auto const&, auto const&) mutable
            {
                exitTimer.Stop();
                ::ExitProcess(0);
            });
            exitTimer.Start();
            return;
        }

        ::ExitProcess(0);
    }

    void MainWindow::ShowTrayMenu()
    {
        // 托盘菜单用 Win32 HMENU（TrackPopupMenu）：
        // 不依赖任何 WinUI 宿主窗口，行为与系统托盘菜单一致，也不会被遮挡。
        enum Command : UINT
        {
            CommandOpen = 1,
            CommandTogglePlay,
            CommandStop,
            CommandExit,
        };

        HMENU menu = ::CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        const bool paused = m_player && m_player->IsPaused();

        ::AppendMenuW(menu, MF_STRING, CommandOpen, L"打开");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, CommandTogglePlay, paused ? L"播放：继续" : L"播放：暂停");
        ::AppendMenuW(menu, MF_STRING, CommandStop, L"停止显示");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, CommandExit, L"退出");

        POINT cursor = ::ScreenControl::TrayIcon::CursorPosition();
        HWND hwnd = GetHwnd();

        // 托盘菜单必须先把窗口前置，否则点击菜单外部时菜单不会消失
        ::SetForegroundWindow(hwnd);

        const UINT command = ::TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
            cursor.x, cursor.y, 0, hwnd, nullptr);

        ::PostMessageW(hwnd, WM_NULL, 0, 0);
        ::DestroyMenu(menu);

        switch (command)
        {
        case CommandOpen:
            ShowFromTray();
            break;
        case CommandTogglePlay:
            if (m_player)
            {
                m_player->SetPaused(!m_player->IsPaused());
            }
            break;
        case CommandStop:
            if (m_player)
            {
                m_player->Stop();
            }
            break;
        case CommandExit:
            ExitApplication();
            break;
        default:
            break;
        }
    }
    void MainWindow::SetupPowerEvents()
    {
        HWND hwnd = GetHwnd();
        if (hwnd == nullptr)
        {
            return;
        }

        // 统一的关屏动作：受设置页「电源联动」总开关与各事件独立开关控制。
        auto turnOffScreen = [this]()
        {
            if (m_player)
            {
                m_player->SetScreenPower(false);
            }
        };

        ::ScreenControl::PowerMonitor::Handlers handlers;

        handlers.suspend = [this, turnOffScreen]()
        {
            auto const& cfg = ::ScreenControl::Ui::Config().Data();
            if (cfg.powerActions && cfg.powerSleep)
            {
                turnOffScreen();
            }
        };

        handlers.shutdown = [this, turnOffScreen]()
        {
            auto const& cfg = ::ScreenControl::Ui::Config().Data();
            if (cfg.powerActions && cfg.powerShutdown)
            {
                turnOffScreen();
            }
        };

        handlers.resume = [this]()
        {
            // 唤醒后按用户设定恢复：上次是开就点亮，上次是关就保持关闭。
            // 睡眠时可能已被关屏，这里重新下发开屏命令。
            if (m_player)
            {
                m_player->SetScreenPower(::ScreenControl::Ui::Config().Data().screenOn);
            }
        };

        handlers.logoff = [this, turnOffScreen]()
        {
            auto const& cfg = ::ScreenControl::Ui::Config().Data();
            if (cfg.powerActions && cfg.powerLogout)
            {
                turnOffScreen();
            }
        };

        // 第二个实例启动时会广播这条消息，这里把窗口叫回来
        handlers.activateMessage = ::ScreenControl::SingleInstance::ActivateMessageId();
        handlers.activate = [this]() { ShowFromTray(); };

        m_powerMonitor.Start(hwnd, handlers);

        // 关闭程序时关屏（对应 v1 的 power_close_app）
        Closed([this, turnOffScreen](auto&&, auto&&)
        {
            auto const& cfg = ::ScreenControl::Ui::Config().Data();
            if (cfg.powerActions && cfg.powerCloseApp)
            {
                turnOffScreen();
            }

            m_powerMonitor.Stop();

            // 关键：不要在这里做「优雅关闭」。
            // ShutdownPlayer() 会 Stop() -> join 工作线程，而线程可能正卡在
            // WriteFile 上（设备忙 / 已拔掉），join 不返回 -> 关闭流程卡住 ->
            // 进程带着 HID 句柄残留 -> 设备一直停在最后一帧，也不会按固件超时
            // 回到自带动画；之后再开官方 InfoHub，刚点亮就被我们残留的命令干扰。
            // 直接把播放器停掉并结束进程，由操作系统立即回收全部句柄。
            if (m_player)
            {
                m_player->RequestStop();   // 只置停止标志，不等待线程
            }
            ExitProcess(0);
        });
    }
    HWND MainWindow::GetHwnd() const noexcept
    {
        if (auto appWindow = AppWindow())
        {
            return reinterpret_cast<HWND>(appWindow.Id().Value);
        }
        return nullptr;
    }

    void MainWindow::ApplyTheme(::ScreenControl::AppTheme theme, bool persist)
    {
        m_theme = theme;

        // 1) XAML 层：设根元素的 RequestedTheme，WinUI 控件与 Mica 背衬一起切换。
        //    Default = 跟随系统；Light / Dark 为显式覆盖。
        if (auto root = Content().try_as<FrameworkElement>())
        {
            switch (theme)
            {
            case ::ScreenControl::AppTheme::Light:
                root.RequestedTheme(ElementTheme::Light);
                break;
            case ::ScreenControl::AppTheme::Dark:
                root.RequestedTheme(ElementTheme::Dark);
                break;
            default:
                root.RequestedTheme(ElementTheme::Default);
                break;
            }
        }

        // 2) 原生层：标题栏深色 + Win32 系统菜单（标题栏右键菜单）深色。
        //    这两处不受 XAML ElementTheme 影响，必须单独设置，否则会出现
        //    「控件已变深色、标题栏右键菜单仍是浅色」的割裂。
        ::ScreenControl::NativeTheme::ApplyAll(theme, GetHwnd());

        // 3) 标题栏系统按钮的前景色必须自己跟随主题。
        //    ExtendsContentIntoTitleBar 之后三个按钮（最小化/最大化/关闭）由我们自己着色，
        //    在浅色主题下如果仍是白色前景，就会出现「按钮看不见、只有悬停才显形」的问题。
        if (auto appWindow = AppWindow())
        {
            if (auto titleBar = appWindow.TitleBar())
            {
                const bool dark = ::ScreenControl::NativeTheme::ResolveIsDark(theme);
                auto const foreground = dark ? Windows::UI::Colors::White() : Windows::UI::Colors::Black();

                titleBar.ButtonForegroundColor(foreground);
                titleBar.ButtonInactiveForegroundColor(foreground);
                titleBar.ButtonHoverForegroundColor(foreground);
                titleBar.ButtonPressedForegroundColor(foreground);

                // 背景色也要跟随主题：否则浅色主题下悬停/按下会露出深色底块。
                auto const transparent = Windows::UI::Colors::Transparent();
                auto const hoverBackground = dark
                    ? Windows::UI::ColorHelper::FromArgb(0x1A, 0xFF, 0xFF, 0xFF)
                    : Windows::UI::ColorHelper::FromArgb(0x1A, 0x00, 0x00, 0x00);
                auto const pressedBackground = dark
                    ? Windows::UI::ColorHelper::FromArgb(0x33, 0xFF, 0xFF, 0xFF)
                    : Windows::UI::ColorHelper::FromArgb(0x33, 0x00, 0x00, 0x00);

                titleBar.ButtonBackgroundColor(transparent);
                titleBar.ButtonInactiveBackgroundColor(transparent);
                titleBar.ButtonHoverBackgroundColor(hoverBackground);
                titleBar.ButtonPressedBackgroundColor(pressedBackground);
            }
        }

        // 4) 持久化
        if (persist)
        {
            ::ScreenControl::Ui::Config().Data().appTheme = ::ScreenControl::NativeTheme::ToString(theme);
            ::ScreenControl::Ui::Config().Save();
        }
    }

    void MainWindow::SetupBackdropAndTitleBar()
    {
        // 内容扩展进标题栏：标题栏与窗口内容融为一体。
        ExtendsContentIntoTitleBar(true);
        SetTitleBar(m_titleBar);

        // 系统按钮透明，让 Mica 从按钮下方透出来。
        if (auto appWindow = AppWindow())
        {
            if (auto titleBar = appWindow.TitleBar())
            {
                titleBar.ButtonBackgroundColor(Windows::UI::Colors::Transparent());
                titleBar.ButtonInactiveBackgroundColor(Windows::UI::Colors::Transparent());
            }
        }

        // Mica 系统背衬。深浅色由根元素 RequestedTheme 驱动，Mica 自动跟随；
        // 系统不支持 Mica 时由 WinUI 自行降级，无需写分支。
        SystemBackdrop(MicaBackdrop{});
    }


    void MainWindow::PopulateMediaList(ListView const& list, std::wstring const& directory,
                                       std::wstring const& extensionsCsv)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(directory, ec))
        {
            return;
        }

        auto matches = [&extensionsCsv](std::filesystem::path const& file)
        {
            const std::wstring ext = file.extension().wstring();
            size_t start = 0;
            while (start <= extensionsCsv.size())
            {
                const size_t comma = extensionsCsv.find(L',', start);
                const std::wstring token = extensionsCsv.substr(
                    start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                if (!token.empty() && _wcsicmp(ext.c_str(), token.c_str()) == 0)
                {
                    return true;
                }
                if (comma == std::wstring::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            return false;
        };

        for (auto const& entry : fs::directory_iterator(directory, ec))
        {
            if (!entry.is_regular_file() || !matches(entry.path()))
            {
                continue;
            }
            ListViewItem item;
            item.Content(box_value(entry.path().filename().wstring()));
            item.Tag(box_value(entry.path().wstring()));
            list.Items().Append(item);
        }
    }

    ::ScreenControl::Core::PlaybackController& MainWindow::Player()
    {
        if (!m_player)
        {
            m_player = std::make_unique<::ScreenControl::Core::PlaybackController>();
            m_player->OpenDevice();

            // 状态回调在工作线程触发，需封送到 UI 线程。
            m_player->SetStatusCallback([this](std::wstring const& message)
            {
                auto dispatcher = DispatcherQueue();
                if (!dispatcher)
                {
                    return;
                }
                const auto text = winrt::hstring(message);
                dispatcher.TryEnqueue([this, text]()
                {
                    if (m_statusText)
                    {
                        m_statusText.Text(text);
                    }
                });
            });

            // 预览回调同样在工作线程触发：复制像素后封送到 UI 线程渲染。
            m_player->SetPreviewCallback(
                [this](std::uint8_t const* pixels, int width, int height, int stride)
                {
                    if (pixels == nullptr || width <= 0 || height <= 0 || stride <= 0)
                    {
                        return;
                    }
                    auto frame = std::make_shared<std::vector<std::uint8_t>>(
                        pixels, pixels + static_cast<size_t>(stride) * static_cast<size_t>(height));

                    auto dispatcher = DispatcherQueue();
                    if (!dispatcher)
                    {
                        return;
                    }
                    dispatcher.TryEnqueue([this, frame, width, height]()
                    {
                        UpdatePreview(*frame, width, height);
                    });
                });
        }
        return *m_player;
    }

    void MainWindow::ShutdownPlayer() noexcept
    {
        if (m_player)
        {
            m_player->Stop();
            m_player->CloseDevice();
            m_player.reset();
        }
    }

    MainWindow::~MainWindow()
    {
        ShutdownPlayer();
    }

    void MainWindow::UpdatePreview(std::vector<std::uint8_t> const& pixels, int width, int height)
    {
        if (!m_previewSource || pixels.empty() || width <= 0 || height <= 0)
        {
            return;
        }
        if (pixels.size() < static_cast<size_t>(width) * 4 * static_cast<size_t>(height))
        {
            return;
        }

        try
        {
            Windows::Storage::Streams::InMemoryRandomAccessStream stream;
            Windows::Storage::Streams::DataWriter writer(stream);
            writer.WriteBytes(pixels);
            const auto buffer = writer.DetachBuffer();

            const auto bitmap = Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
                buffer,
                Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
                width,
                height,
                Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied);

            m_previewSource.SetBitmapAsync(bitmap);

            if (m_previewPlaceholder)
            {
                m_previewPlaceholder.Visibility(Visibility::Collapsed);
            }
        }
        catch (...)
        {
            // 预览只是辅助功能，失败不应影响播放主流程
        }
    }

    MainWindow* MainWindow::Current() noexcept
    {
        return s_current;
    }

    ::ScreenControl::AppTheme MainWindow::CurrentTheme() const noexcept
    {
        return m_theme;
    }

    void MainWindow::AttachStatusText(TextBlock const& text)
    {
        m_statusText = text;
    }

    void MainWindow::AttachDeviceText(TextBlock const& text)
    {
        m_deviceText = text;
    }

    void MainWindow::AttachPreview(Media::Imaging::SoftwareBitmapSource const& source,
                                   TextBlock const& placeholder)
    {
        m_previewSource = source;
        m_previewPlaceholder = placeholder;
    }

    void MainWindow::DetachPageUi()
    {
        m_statusText = nullptr;
        m_deviceText = nullptr;
        m_previewSource = nullptr;
        m_previewPlaceholder = nullptr;
    }
}
