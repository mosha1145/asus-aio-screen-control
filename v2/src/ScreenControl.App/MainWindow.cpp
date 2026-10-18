#include "pch.h"
#include "MainWindow.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <Microsoft.UI.Windowing.h>

#include "AppConfig.h"
#include "PlaybackController.h"

#include <filesystem>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;

namespace winrt::ScreenControl::implementation
{
    namespace
    {
        /// 全局配置（首次访问时加载，含 v1 -> v2 迁移）。主题等设置经它持久化。
        ::ScreenControl::Core::ConfigStore& AppConfigStore()
        {
            static ::ScreenControl::Core::ConfigStore store = []()
            {
                ::ScreenControl::Core::ConfigStore s;
                s.Load();
                return s;
            }();
            return store;
        }

        /// 把页面内容包进可滚动容器。
        /// 窗口变小时页面不会被裁剪，而是出现纵向滚动条。
        ScrollViewer MakeScrollable(UIElement const& content)
        {
            ScrollViewer scroller;
            scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            scroller.Content(content);
            return scroller;
        }

        TextBlock MakeTitle(hstring const& text)
        {
            TextBlock block;
            block.Text(text);
            block.FontSize(24);
            block.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            block.Margin(Thickness{ 0, 0, 0, 8 });
            return block;
        }

        Border MakeCard(UIElement const& child)
        {
            Border card;
            card.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
            card.Padding(Thickness{ 16, 16, 16, 16 });
            card.BorderThickness(Thickness{ 1, 1, 1, 1 });
            card.BorderBrush(Application::Current().Resources()
                                 .Lookup(box_value(L"CardStrokeColorDefaultBrush"))
                                 .as<Brush>());
            card.Background(Application::Current().Resources()
                                .Lookup(box_value(L"CardBackgroundFillColorDefaultBrush"))
                                .as<Brush>());
            card.Child(child);
            return card;
        }
    }

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
        nav.PaneDisplayMode(NavigationViewPaneDisplayMode::Left);
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
            if (auto item = args.SelectedItem().try_as<NavigationViewItem>())
            {
                NavigateTo(std::wstring{ unbox_value<hstring>(item.Tag()).c_str() });
            }
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
        NavigateTo(L"display");
        nav.SelectedItem(itemDisplay);

        // 启动时按配置恢复主题（含「跟随系统」）。
        m_theme = ::ScreenControl::NativeTheme::Parse(AppConfigStore().Data().appTheme);
        ApplyTheme(m_theme, false);

        // 电源 / 会话事件（睡眠、关机、注销）与窗口关闭处理。
        SetupPowerEvents();
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
            auto const& cfg = AppConfigStore().Data();
            if (cfg.powerActions && cfg.powerSleep)
            {
                turnOffScreen();
            }
        };

        handlers.shutdown = [this, turnOffScreen]()
        {
            auto const& cfg = AppConfigStore().Data();
            if (cfg.powerActions && cfg.powerShutdown)
            {
                turnOffScreen();
            }
        };

        handlers.logoff = [this, turnOffScreen]()
        {
            auto const& cfg = AppConfigStore().Data();
            if (cfg.powerActions && cfg.powerLogout)
            {
                turnOffScreen();
            }
        };

        m_powerMonitor.Start(hwnd, handlers);

        // 关闭程序时关屏（对应 v1 的 power_close_app）
        Closed([this, turnOffScreen](auto&&, auto&&)
        {
            auto const& cfg = AppConfigStore().Data();
            if (cfg.powerActions && cfg.powerCloseApp)
            {
                turnOffScreen();
            }
            m_powerMonitor.Stop();
            ShutdownPlayer();
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

        // 3) 持久化
        if (persist)
        {
            AppConfigStore().Data().appTheme = ::ScreenControl::NativeTheme::ToString(theme);
            AppConfigStore().Save();
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

    void MainWindow::NavigateTo(std::wstring const& tag)
    {
        if (tag == L"settings")
        {
            m_contentFrame.Content(BuildSettingsPage());
        }
        else
        {
            m_contentFrame.Content(BuildDisplayPage());
        }
    }

    UIElement MainWindow::BuildDisplayPage()
    {
        StackPanel page;
        page.Spacing(16);
        page.Margin(Thickness{ 24, 8, 24, 24 });

        page.Children().Append(MakeTitle(L"主控"));

        Grid body;
        body.ColumnSpacing(16);
        body.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());
        body.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); return c; }());

        // 左侧：预览框（水冷屏是 1:1 的 320x320）
        Border preview;
        preview.Width(332);
        preview.Height(332);
        preview.CornerRadius(CornerRadius{ 12, 12, 12, 12 });
        preview.BorderThickness(Thickness{ 1, 1, 1, 1 });
        preview.BorderBrush(Application::Current().Resources()
                                .Lookup(box_value(L"CardStrokeColorDefaultBrush")).as<Brush>());
        preview.Background(Application::Current().Resources()
                               .Lookup(box_value(L"CardBackgroundFillColorDefaultBrush")).as<Brush>());
        TextBlock previewText;
        previewText.Text(L"未在显示");
        previewText.HorizontalAlignment(HorizontalAlignment::Center);
        previewText.VerticalAlignment(VerticalAlignment::Center);
        previewText.Foreground(Application::Current().Resources()
                                   .Lookup(box_value(L"TextFillColorTertiaryBrush")).as<Brush>());
        preview.Child(previewText);
        Grid::SetColumn(preview, 0);
        body.Children().Append(preview);

        // 右侧：控制卡
        StackPanel controls;
        controls.Spacing(14);

        ToggleSwitch screenSwitch;
        screenSwitch.Header(box_value(L"屏幕开关"));
        screenSwitch.IsOn(true);
        controls.Children().Append(screenSwitch);

        StackPanel brightnessRow;
        brightnessRow.Spacing(6);
        TextBlock brightnessLabel;
        brightnessLabel.Text(L"亮度");
        brightnessRow.Children().Append(brightnessLabel);
        Slider brightness;
        brightness.Minimum(0);
        brightness.Maximum(100);
        brightness.Value(50);
        brightnessRow.Children().Append(brightness);
        controls.Children().Append(brightnessRow);

        ComboBox fitMode;
        fitMode.Header(box_value(L"图像适应"));
        fitMode.Items().Append(box_value(L"适应 · contain"));
        fitMode.Items().Append(box_value(L"填充 · cover"));
        fitMode.Items().Append(box_value(L"拉伸 · stretch"));
        fitMode.SelectedIndex(0);
        controls.Children().Append(fitMode);

        ComboBox rotation;
        rotation.Header(box_value(L"画面旋转"));
        rotation.Items().Append(box_value(L"0°"));
        rotation.Items().Append(box_value(L"90°"));
        rotation.Items().Append(box_value(L"180°"));
        rotation.Items().Append(box_value(L"270°"));
        rotation.SelectedIndex(0);
        controls.Children().Append(rotation);

        InfoBar info;
        info.IsOpen(true);
        info.IsClosable(false);
        info.Severity(InfoBarSeverity::Informational);
        info.Title(L"当前进度");
        info.Message(L"HID 发送与照片/视频播放已接入；素材导入（ffmpeg 转码）与实时预览将在后续迭代补齐。");
        controls.Children().Append(info);

        auto controlsCard = MakeCard(controls);
        Grid::SetColumn(controlsCard, 1);
        body.Children().Append(controlsCard);

        page.Children().Append(body);

        // ---------------- 素材与播放 ----------------
        StackPanel media;
        media.Spacing(10);

        m_deviceText = TextBlock{};
        m_deviceText.Text(Player().DeviceReady() ? L"设备：已连接" : L"设备：未连接");
        media.Children().Append(m_deviceText);

        m_statusText = TextBlock{};
        m_statusText.Text(L"未在显示");
        m_statusText.FontSize(12);
        media.Children().Append(m_statusText);

        Grid mediaRow;
        mediaRow.ColumnSpacing(16);
        mediaRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); return c; }());
        mediaRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); return c; }());

        StackPanel photoPanel;
        photoPanel.Spacing(6);
        TextBlock photoLabel;
        photoLabel.Text(L"照片");
        photoPanel.Children().Append(photoLabel);
        ListView photoList;
        photoList.Height(180);
        PopulateMediaList(photoList,
                          (AppConfigStore().AssetRoot() / L"photos").wstring(),
                          L".jpg,.jpeg,.png,.bmp");
        photoPanel.Children().Append(photoList);
        Button playPhoto;
        playPhoto.Content(box_value(L"播放选中的照片"));
        playPhoto.Click([this, photoList](auto const&, RoutedEventArgs const&)
        {
            // SelectedItem() 返回的是 ListViewItem 本身，不是 hstring。
            // 之前直接 unbox_value<hstring>(item) 类型不匹配抛异常 -> 崩溃。
            // 路径存在 Tag 里，所以先转成 ListViewItem 再取 Tag。
            if (auto item = photoList.SelectedItem().try_as<ListViewItem>())
            {
                const std::wstring path = unbox_value<hstring>(item.Tag()).c_str();
                Player().Play(std::filesystem::path(path));
            }
        });
        photoPanel.Children().Append(playPhoto);
        Grid::SetColumn(photoPanel, 0);
        mediaRow.Children().Append(photoPanel);

        StackPanel videoPanel;
        videoPanel.Spacing(6);
        TextBlock videoLabel;
        videoLabel.Text(L"视频");
        videoPanel.Children().Append(videoLabel);
        ListView videoList;
        videoList.Height(180);
        PopulateMediaList(videoList,
                          (AppConfigStore().AssetRoot() / L"videos").wstring(),
                          L".mp4,.avi,.mov,.mkv,.webm,.m4v");
        videoPanel.Children().Append(videoList);
        Button playVideo;
        playVideo.Content(box_value(L"播放选中的视频"));
        playVideo.Click([this, videoList](auto const&, RoutedEventArgs const&)
        {
            if (auto item = videoList.SelectedItem().try_as<ListViewItem>())
            {
                const std::wstring path = unbox_value<hstring>(item.Tag()).c_str();
                Player().Play(std::filesystem::path(path));
            }
        });
        videoPanel.Children().Append(playVideo);
        Grid::SetColumn(videoPanel, 1);
        mediaRow.Children().Append(videoPanel);

        media.Children().Append(mediaRow);

        Button stopButton;
        stopButton.Content(box_value(L"停止显示"));
        stopButton.Click([this](auto const&, RoutedEventArgs const&) { Player().Stop(); });
        media.Children().Append(stopButton);

        page.Children().Append(MakeCard(media));

        return MakeScrollable(page);
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

    UIElement MainWindow::BuildSettingsPage()
    {
        StackPanel page;
        page.Spacing(12);
        page.Margin(Thickness{ 24, 8, 24, 24 });

        page.Children().Append(MakeTitle(L"设置"));

        StackPanel general;
        general.Spacing(14);

        ToggleSwitch autoStart;
        autoStart.Header(box_value(L"开机自启"));
        autoStart.OffContent(box_value(L"关闭"));
        autoStart.OnContent(box_value(L"开启"));
        general.Children().Append(autoStart);

        TextBlock themeLabel;
        themeLabel.Text(L"界面主题");
        general.Children().Append(themeLabel);

        RadioButtons themeChoice;
        themeChoice.Items().Append(box_value(L"跟随系统"));
        themeChoice.Items().Append(box_value(L"浅色"));
        themeChoice.Items().Append(box_value(L"深色"));

        // 先按当前主题回显选中项，再挂事件（避免设置选中项时触发一次多余的应用）。
        int themeIndex = 0;
        if (m_theme == ::ScreenControl::AppTheme::Light)
        {
            themeIndex = 1;
        }
        else if (m_theme == ::ScreenControl::AppTheme::Dark)
        {
            themeIndex = 2;
        }
        themeChoice.SelectedIndex(themeIndex);

        themeChoice.SelectionChanged([this](IInspectable const& sender, SelectionChangedEventArgs const&)
        {
            auto radio = sender.try_as<RadioButtons>();
            if (!radio)
            {
                return;
            }
            ::ScreenControl::AppTheme theme = ::ScreenControl::AppTheme::System;
            switch (radio.SelectedIndex())
            {
            case 1: theme = ::ScreenControl::AppTheme::Light; break;
            case 2: theme = ::ScreenControl::AppTheme::Dark; break;
            default: theme = ::ScreenControl::AppTheme::System; break;
            }
            ApplyTheme(theme, true);
        });
        general.Children().Append(themeChoice);

        TextBlock themeHint;
        themeHint.Text(L"仅深色 / 浅色 / 跟随系统，Mica 背衬会随主题自动调整色调。");
        themeHint.FontSize(12);
        themeHint.Foreground(Application::Current().Resources()
                                 .Lookup(box_value(L"TextFillColorTertiaryBrush")).as<Brush>());
        general.Children().Append(themeHint);

        page.Children().Append(MakeCard(general));

        StackPanel deferred;
        deferred.Spacing(12);
        InfoBar deferredInfo;
        deferredInfo.IsOpen(true);
        deferredInfo.IsClosable(false);
        deferredInfo.Severity(InfoBarSeverity::Informational);
        deferredInfo.Title(L"以下功能本期不实现");
        deferredInfo.Message(L"温度/硬件信息监测、系统信息采集、电源联动、自定义主题、悬浮窗、桌面实时投射、SMTC 媒体封面捕获。相关配置键保留以兼容 v1 配置，界面项在此处灰显占位。");
        deferred.Children().Append(deferredInfo);

        ToggleSwitch powerSwitch;
        powerSwitch.Header(box_value(L"电源联动（暂缓）"));
        powerSwitch.IsEnabled(false);
        deferred.Children().Append(powerSwitch);

        ToggleSwitch smtcSwitch;
        smtcSwitch.Header(box_value(L"SMTC 媒体封面捕获（暂缓）"));
        smtcSwitch.IsEnabled(false);
        deferred.Children().Append(smtcSwitch);

        page.Children().Append(MakeCard(deferred));
        return MakeScrollable(page);
    }
}
