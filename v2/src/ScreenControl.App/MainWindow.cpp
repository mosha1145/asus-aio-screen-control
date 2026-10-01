#include "pch.h"
#include "MainWindow.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;

namespace ScreenControl
{
    namespace
    {
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

    MainWindow::MainWindow()
    {
        // 根容器背景留空：一旦设了实色，Mica 就被盖住了。
        Grid root;

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

        Grid::SetRow(m_titleBar, 0);
        Grid::SetRow(nav, 0);
        Grid::SetRowSpan(nav, 2);
        root.Children().Append(m_titleBar);
        root.Children().Append(nav);

        Content(root);
        Title(L"ASUS AIO Screen Control");

        SetupBackdropAndTitleBar();
        NavigateTo(L"display");
        nav.SelectedItem(itemDisplay);
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
        info.Title(L"骨架阶段");
        info.Message(L"HID 通信、素材库、视频/图片播放将在后续迭代接入。当前版本用于确认工程结构、Mica 背衬、主题与配置迁移。");
        controls.Children().Append(info);

        auto controlsCard = MakeCard(controls);
        Grid::SetColumn(controlsCard, 1);
        body.Children().Append(controlsCard);

        page.Children().Append(body);
        return page;
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
        themeChoice.SelectedIndex(0);
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
        return page;
    }
}
