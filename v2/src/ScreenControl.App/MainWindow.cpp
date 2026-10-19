#include "pch.h"
#include "MainWindow.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <Microsoft.UI.Windowing.h>

#include "AppConfig.h"
#include "PlaybackController.h"

#include <filesystem>
#include <vector>

#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <functional>

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

        // 恢复上次的显示参数与屏幕开关状态
        ApplyPersistedDeviceState();

        // 电源 / 会话事件（睡眠、关机、注销）与窗口关闭处理。
        SetupPowerEvents();
    }

    void MainWindow::ApplyPersistedDeviceState()
    {
        auto& config = AppConfigStore().Data();

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

        // 实时预览：播放时把收到的帧画在这里（与设备上显示的内容一致）。
        m_previewSource = Media::Imaging::SoftwareBitmapSource{};
        m_previewImage = Image{};
        m_previewImage.Source(m_previewSource);
        m_previewImage.Stretch(Stretch::Uniform);
        m_previewImage.Width(320);
        m_previewImage.Height(320);
        m_previewPlaceholder = previewText;

        Grid previewInner;
        previewInner.Children().Append(m_previewImage);
        previewInner.Children().Append(previewText);
        preview.Child(previewInner);
        Grid::SetColumn(preview, 0);
        body.Children().Append(preview);

        // 右侧：控制卡
        StackPanel controls;
        controls.Spacing(14);

        ToggleSwitch screenSwitch;
        screenSwitch.Header(box_value(L"屏幕开关"));
        // 初值来自配置；先设状态再挂事件，避免初始化时触发一次多余的命令。
        screenSwitch.IsOn(AppConfigStore().Data().screenOn);
        screenSwitch.Toggled([this](IInspectable const& sender, RoutedEventArgs const&)
        {
            if (auto toggle = sender.try_as<ToggleSwitch>())
            {
                // 记住用户设定，下次启动按此恢复（与电源联动关屏互不覆盖）
                AppConfigStore().Data().screenOn = toggle.IsOn();
                AppConfigStore().Save();
                Player().SetScreenPower(toggle.IsOn());
            }
        });
        controls.Children().Append(screenSwitch);

        StackPanel brightnessRow;
        brightnessRow.Spacing(6);
        TextBlock brightnessLabel;
        brightnessLabel.Text(L"亮度");
        brightnessRow.Children().Append(brightnessLabel);
        Slider brightness;
        brightness.Minimum(0);
        brightness.Maximum(100);
        brightness.Value(AppConfigStore().Data().brightness);
        brightnessRow.Children().Append(brightness);
        // 拖动时实时下发命令；松手（失焦）时才写配置，避免拖一次写上百次文件。
        brightness.ValueChanged([this](IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
        {
            Player().SetBrightness(static_cast<int>(args.NewValue()));
        });
        brightness.LostFocus([this, brightness](IInspectable const&, RoutedEventArgs const&)
        {
            AppConfigStore().Data().brightness = static_cast<int>(brightness.Value());
            AppConfigStore().Save();
        });
        controls.Children().Append(brightnessRow);

        ComboBox fitMode;
        fitMode.Header(box_value(L"图像适应"));
        fitMode.Items().Append(box_value(L"适应 · contain"));
        fitMode.Items().Append(box_value(L"填充 · cover"));
        fitMode.Items().Append(box_value(L"拉伸 · stretch"));
        {
            const std::wstring stored = AppConfigStore().Data().fitMode;
            fitMode.SelectedIndex(stored == L"cover" ? 1 : (stored == L"stretch" ? 2 : 0));
        }
        fitMode.SelectionChanged([this, fitMode](IInspectable const&, SelectionChangedEventArgs const&)
        {
            auto params = Player().Params();
            switch (fitMode.SelectedIndex())
            {
            case 1: params.fitMode = ::ScreenControl::Imaging::FitMode::Cover; break;
            case 2: params.fitMode = ::ScreenControl::Imaging::FitMode::Stretch; break;
            default: params.fitMode = ::ScreenControl::Imaging::FitMode::Contain; break;
            }
            Player().SetParams(params);
            AppConfigStore().Data().fitMode =
                fitMode.SelectedIndex() == 1 ? L"cover"
                : (fitMode.SelectedIndex() == 2 ? L"stretch" : L"contain");
            AppConfigStore().Save();
        });
        controls.Children().Append(fitMode);

        ComboBox rotation;
        rotation.Header(box_value(L"画面旋转"));
        rotation.Items().Append(box_value(L"0°"));
        rotation.Items().Append(box_value(L"90°"));
        rotation.Items().Append(box_value(L"180°"));
        rotation.Items().Append(box_value(L"270°"));
        {
            const int storedRotation = AppConfigStore().Data().rotation;
            rotation.SelectedIndex(storedRotation == 90 ? 1 : (storedRotation == 180 ? 2 : (storedRotation == 270 ? 3 : 0)));
        }
        rotation.SelectionChanged([this, rotation](IInspectable const&, SelectionChangedEventArgs const&)
        {
            auto params = Player().Params();
            switch (rotation.SelectedIndex())
            {
            case 1: params.rotation = ::ScreenControl::Imaging::Rotation::Deg90; break;
            case 2: params.rotation = ::ScreenControl::Imaging::Rotation::Deg180; break;
            case 3: params.rotation = ::ScreenControl::Imaging::Rotation::Deg270; break;
            default: params.rotation = ::ScreenControl::Imaging::Rotation::Deg0; break;
            }
            Player().SetParams(params);
            AppConfigStore().Data().rotation =
                rotation.SelectedIndex() == 1 ? 90
                : (rotation.SelectedIndex() == 2 ? 180 : (rotation.SelectedIndex() == 3 ? 270 : 0));
            AppConfigStore().Save();
        });
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
        m_deviceText.Text(Player().DeviceReady() ? L"设备：已连接" : L"设备：未连接（若官方 InfoHub 正在运行，请先退出它——它会独占 HID 接口）");
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

        // 主题：参考系统设置页的做法 —— 可展开条目，标题行左侧图标 + 名称、
        // 右侧显示当前选项；展开后三选一。顺序按参考图：浅色 / 深色 / 使用系统设置。
        auto themeLabelOf = [](::ScreenControl::AppTheme value) -> wchar_t const*
        {
            switch (value)
            {
            case ::ScreenControl::AppTheme::Light: return L"浅色";
            case ::ScreenControl::AppTheme::Dark: return L"深色";
            default: return L"使用系统设置";
            }
        };

        TextBlock themeValue;
        themeValue.FontSize(12);
        themeValue.VerticalAlignment(VerticalAlignment::Center);
        themeValue.Text(themeLabelOf(m_theme));

        Grid themeHeader;
        themeHeader.ColumnSpacing(12);
        themeHeader.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());
        themeHeader.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); return c; }());
        themeHeader.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());

        FontIcon themeIcon;
        themeIcon.Glyph(L"\xE790");
        themeIcon.FontSize(16);
        themeIcon.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(themeIcon, 0);
        themeHeader.Children().Append(themeIcon);

        TextBlock themeTitle;
        themeTitle.Text(L"应用主题");
        themeTitle.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(themeTitle, 1);
        themeHeader.Children().Append(themeTitle);

        Grid::SetColumn(themeValue, 2);
        themeHeader.Children().Append(themeValue);

        RadioButtons themeChoice;
        themeChoice.Items().Append(box_value(L"浅色"));
        themeChoice.Items().Append(box_value(L"深色"));
        themeChoice.Items().Append(box_value(L"使用系统设置"));

        // 先回显再挂事件，避免初始化时触发一次多余的应用。
        themeChoice.SelectedIndex(
            m_theme == ::ScreenControl::AppTheme::Light ? 0
            : (m_theme == ::ScreenControl::AppTheme::Dark ? 1 : 2));

        themeChoice.SelectionChanged([this, themeValue, themeLabelOf](IInspectable const& sender, SelectionChangedEventArgs const&)
        {
            auto radio = sender.try_as<RadioButtons>();
            if (!radio)
            {
                return;
            }
            ::ScreenControl::AppTheme theme = ::ScreenControl::AppTheme::System;
            switch (radio.SelectedIndex())
            {
            case 0: theme = ::ScreenControl::AppTheme::Light; break;
            case 1: theme = ::ScreenControl::AppTheme::Dark; break;
            default: theme = ::ScreenControl::AppTheme::System; break;
            }
            ApplyTheme(theme, true);
            themeValue.Text(themeLabelOf(theme));
        });

        Expander themeExpander;
        themeExpander.HorizontalAlignment(HorizontalAlignment::Stretch);
        themeExpander.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        themeExpander.Header(themeHeader);
        themeExpander.Content(themeChoice);

        // 「常规」与「个性化」拆成两张卡：开机自启属于启动行为，主题属于外观，不应混在一起。
        page.Children().Append(MakeCard(general));

        // Expander 自身就是一张卡片，不再额外套一层 MakeCard，
        // 否则会出现双层边框与多余内边距（也就是"多包了一层"）。
        page.Children().Append(themeExpander);

        // ---------------- 电源联动（参考系统设置页：顶栏总开关 + 子项） ----------------
        StackPanel powerSection;
        powerSection.Spacing(4);

        auto& config = AppConfigStore().Data();

        // 子项容器：总开关关闭时整体置灰
        StackPanel powerItemsInner;
        powerItemsInner.Spacing(4);
        powerItemsInner.Margin(Thickness{ 56, 0, 0, 0 });   // 与标题文字左对齐（让开图标）

        // 只有 Control 才有 IsEnabled，StackPanel 没有，所以包一层 ContentControl。
        ContentControl powerItems;
        powerItems.Content(powerItemsInner);
        powerItems.HorizontalContentAlignment(HorizontalAlignment::Stretch);

        auto makePowerItem = [this, &powerItemsInner](wchar_t const* title, wchar_t const* description,
                                                 bool initial, std::function<void(bool)> persist)
        {
            CheckBox box;
            box.IsChecked(initial);
            box.Checked([persist](IInspectable const& sender, RoutedEventArgs const&)
            {
                if (auto control = sender.try_as<CheckBox>())
                {
                    if (auto state = control.IsChecked()) { persist(state.GetBoolean()); }
                }
            });
            box.Unchecked([persist](IInspectable const& sender, RoutedEventArgs const&)
            {
                if (auto control = sender.try_as<CheckBox>())
                {
                    if (auto state = control.IsChecked()) { persist(state.GetBoolean()); }
                }
            });

            StackPanel text;
            text.Spacing(2);
            TextBlock titleBlock;
            titleBlock.Text(title);
            titleBlock.TextWrapping(TextWrapping::Wrap);
            text.Children().Append(titleBlock);

            TextBlock descriptionBlock;
            descriptionBlock.Text(description);
            descriptionBlock.FontSize(12);
            descriptionBlock.TextWrapping(TextWrapping::Wrap);
            descriptionBlock.Foreground(Application::Current().Resources()
                                            .Lookup(box_value(L"TextFillColorSecondaryBrush")).as<Brush>());
            text.Children().Append(descriptionBlock);

            box.Content(text);
            powerItemsInner.Children().Append(box);
        };

        makePowerItem(L"关闭程序时关闭水冷屏", L"退出本程序时，同时关闭水冷屏的显示。",
                      config.powerCloseApp,
                      [this](bool value) { AppConfigStore().Data().powerCloseApp = value; AppConfigStore().Save(); });
        makePowerItem(L"关闭电脑时关闭水冷屏", L"系统关机前关闭显示，避免残留画面。",
                      config.powerShutdown,
                      [this](bool value) { AppConfigStore().Data().powerShutdown = value; AppConfigStore().Save(); });
        makePowerItem(L"注销时关闭水冷屏", L"注销当前用户时关闭显示。",
                      config.powerLogout,
                      [this](bool value) { AppConfigStore().Data().powerLogout = value; AppConfigStore().Save(); });
        makePowerItem(L"睡眠时关闭水冷屏", L"进入睡眠前关闭显示，唤醒后需重新播放。",
                      config.powerSleep,
                      [this](bool value) { AppConfigStore().Data().powerSleep = value; AppConfigStore().Save(); });

        // 顶栏：图标 + 标题/说明 + 总开关
        Grid powerHeader;
        powerHeader.ColumnSpacing(16);
        powerHeader.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());
        powerHeader.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); return c; }());
        powerHeader.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());

        FontIcon powerIcon;
        powerIcon.Glyph(L"\xE7E8");
        powerIcon.FontSize(20);
        powerIcon.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(powerIcon, 0);
        powerHeader.Children().Append(powerIcon);

        StackPanel powerText;
        powerText.Spacing(2);
        TextBlock powerTitle;
        powerTitle.Text(L"电源联动");
        powerText.Children().Append(powerTitle);
        TextBlock powerDescription;
        powerDescription.Text(L"系统电源或会话事件发生时，自动关闭水冷屏显示");
        powerDescription.FontSize(12);
        powerDescription.TextWrapping(TextWrapping::Wrap);
        powerDescription.Foreground(Application::Current().Resources()
                                        .Lookup(box_value(L"TextFillColorSecondaryBrush")).as<Brush>());
        powerText.Children().Append(powerDescription);
        Grid::SetColumn(powerText, 1);
        powerHeader.Children().Append(powerText);

        ToggleSwitch powerMaster;
        powerMaster.IsOn(config.powerActions);
        powerMaster.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(powerMaster, 2);
        powerHeader.Children().Append(powerMaster);

        // 总开关关闭时子项灰显（与系统设置页一致）
        powerItems.IsEnabled(config.powerActions);
        powerMaster.Toggled([this, &powerItems](IInspectable const& sender, RoutedEventArgs const&)
        {
            if (auto control = sender.try_as<ToggleSwitch>())
            {
                AppConfigStore().Data().powerActions = control.IsOn();
                AppConfigStore().Save();
                powerItems.IsEnabled(control.IsOn());
            }
        });

        powerSection.Children().Append(powerHeader);
        powerSection.Children().Append(powerItems);

        Expander powerExpander;
        powerExpander.HorizontalAlignment(HorizontalAlignment::Stretch);
        powerExpander.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        powerExpander.Header(powerHeader);
        powerExpander.Content(powerItems);
        page.Children().Append(powerExpander);

        StackPanel deferred;
        deferred.Spacing(12);
        InfoBar deferredInfo;
        deferredInfo.IsOpen(true);
        deferredInfo.IsClosable(false);
        deferredInfo.Severity(InfoBarSeverity::Informational);
        deferredInfo.Title(L"以下功能本期不实现");
        deferredInfo.Message(L"温度/硬件信息监测、系统信息采集、自定义主题、悬浮窗、桌面实时投射、SMTC 媒体封面捕获。相关配置键保留以兼容 v1 配置，界面项在此处灰显占位。");
        deferred.Children().Append(deferredInfo);

        ToggleSwitch smtcSwitch;
        smtcSwitch.Header(box_value(L"SMTC 媒体封面捕获（暂缓）"));
        smtcSwitch.IsEnabled(false);
        deferred.Children().Append(smtcSwitch);

        page.Children().Append(MakeCard(deferred));
        return MakeScrollable(page);
    }
}
