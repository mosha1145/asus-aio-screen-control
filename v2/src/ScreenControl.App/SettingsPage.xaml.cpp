#include "pch.h"
#include "SettingsPage.xaml.h"
#include "UiHelpers.h"
#if __has_include("SettingsPage.g.cpp")
#include "SettingsPage.g.cpp"
#endif
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace ::ScreenControl::Ui;
namespace winrt::ScreenControl::implementation
{
    SettingsPage::SettingsPage()
    {
        InitializeComponent();

        StackPanel page;
        page.Spacing(4);
        page.Margin(Thickness{ 24, 8, 24, 24 });

        page.Children().Append(MakeTitle(L"设置"));

        // 开机自启动：左侧文本、右侧开关（与系统设置页的条目一致，开关不显示文字）
        ToggleSwitch autoStart;
        autoStart.MinWidth(0);
        autoStart.OnContent(nullptr);
        autoStart.OffContent(nullptr);
        autoStart.VerticalAlignment(VerticalAlignment::Center);
        autoStart.HorizontalAlignment(HorizontalAlignment::Right);

        Grid autoStartRow;
        autoStartRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());
        autoStartRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); return c; }());
        autoStartRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());

        FontIcon autoStartIcon;
        autoStartIcon.Glyph(L"\xE7E8");
        autoStartIcon.FontSize(16);
        autoStartIcon.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(autoStartIcon, 0);
        autoStartRow.Children().Append(autoStartIcon);

        TextBlock autoStartLabel;
        autoStartLabel.Text(L"开机自启动");
        autoStartLabel.VerticalAlignment(VerticalAlignment::Center);
        autoStartLabel.Margin(Thickness{ 16, 0, 0, 0 });
        Grid::SetColumn(autoStartLabel, 1);
        autoStartRow.Children().Append(autoStartLabel);

        Grid::SetColumn(autoStart, 2);
        autoStartRow.Children().Append(autoStart);
        // 关闭窗口时的行为（下拉选择，与开机自启动同一张卡）
        Grid closeRow;
        closeRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());
        closeRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); return c; }());
        closeRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());

        FontIcon closeIcon;
        closeIcon.Glyph(L"\xE8BB");
        closeIcon.FontSize(16);
        closeIcon.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(closeIcon, 0);
        closeRow.Children().Append(closeIcon);

        TextBlock closeLabel;
        closeLabel.Text(L"关闭窗口时");
        closeLabel.VerticalAlignment(VerticalAlignment::Center);
        closeLabel.Margin(Thickness{ 16, 0, 0, 0 });
        Grid::SetColumn(closeLabel, 1);
        closeRow.Children().Append(closeLabel);

        ComboBox closeChoice;
        closeChoice.MinWidth(180);
        closeChoice.VerticalAlignment(VerticalAlignment::Center);
        closeChoice.Items().Append(box_value(L"隐藏到系统托盘"));
        closeChoice.Items().Append(box_value(L"退出程序"));


        auto const& closeConfig = ::ScreenControl::Ui::Config().Data();
        closeChoice.SelectedIndex(closeConfig.closeAction == L"exit" ? 1 : 0);

        closeChoice.SelectionChanged([this, closeChoice](IInspectable const&, SelectionChangedEventArgs const&)
        {
            const std::wstring value = closeChoice.SelectedIndex() == 1 ? L"exit" : L"tray";
            auto& config = ::ScreenControl::Ui::Config().Data();
            config.closeAction = value;
            ::ScreenControl::Ui::Config().Save();
        });

        Grid::SetColumn(closeChoice, 2);
        closeRow.Children().Append(closeChoice);

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
        themeValue.Text(themeLabelOf(MainWindow::Current()->CurrentTheme()));

        RadioButtons themeChoice;
        themeChoice.Items().Append(box_value(L"浅色"));
        themeChoice.Items().Append(box_value(L"深色"));
        themeChoice.Items().Append(box_value(L"使用系统设置"));

        // 先回显再挂事件，避免初始化时触发一次多余的应用。
        themeChoice.SelectedIndex(
            MainWindow::Current()->CurrentTheme() == ::ScreenControl::AppTheme::Light ? 0
            : (MainWindow::Current()->CurrentTheme() == ::ScreenControl::AppTheme::Dark ? 1 : 2));

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
            MainWindow::Current()->ApplyTheme(theme, true);
            themeValue.Text(themeLabelOf(theme));
        });


        // 「常规」与「个性化」拆成两张卡：开机自启属于启动行为，主题属于外观，不应混在一起。
        page.Children().Append(MakeSectionTitle(L"应用行为"));
        page.Children().Append(MakeBehaviorCard(autoStartRow));
        page.Children().Append(MakeBehaviorCard(closeRow));
        page.Children().Append(MakeSectionTitle(L"个性化"));

        // Expander 自身就是一张卡片，不再额外套一层 MakeCard，
        // 否则会出现双层边框与多余内边距（也就是"多包了一层"）。
        page.Children().Append(MakeSettingsCard(L"\xE706", L"应用主题", themeValue, themeChoice));

        // ---------------- 主题色 ----------------
        auto& config = ::ScreenControl::Ui::Config().Data();

        auto colorLabelOf = [](std::wstring const& value) -> wchar_t const*
        {
            return value == L"tuf" ? L"TUF GAMING" : L"使用系统设置";
        };

        TextBlock colorValue;
        colorValue.FontSize(12);
        colorValue.VerticalAlignment(VerticalAlignment::Center);
        colorValue.Text(colorLabelOf(config.themeColor));


        RadioButtons colorChoice;
        colorChoice.Items().Append(box_value(L"TUF GAMING"));
        colorChoice.Items().Append(box_value(L"使用系统设置"));
        colorChoice.SelectedIndex(config.themeColor == L"tuf" ? 0 : 1);
        colorChoice.SelectionChanged([this, colorValue, colorLabelOf](IInspectable const& sender, SelectionChangedEventArgs const&)
        {
            auto radio = sender.try_as<RadioButtons>();
            if (!radio)
            {
                return;
            }
            const std::wstring value = radio.SelectedIndex() == 0 ? L"tuf" : L"system";
            ::ScreenControl::Ui::Config().Data().themeColor = value;
            ::ScreenControl::Ui::Config().Save();
            colorValue.Text(colorLabelOf(value));
        });

        page.Children().Append(MakeSettingsCard(L"\xE790", L"主题色", colorValue, colorChoice));

        // ---------------- 智能关屏（顶栏总开关 + 子项） ----------------

        // 子项容器：总开关关闭时整体置灰
        StackPanel powerItemsInner;
        powerItemsInner.Spacing(4);


        // 只有 Control 才有 IsEnabled，StackPanel 没有，所以包一层 ContentControl。
        ContentControl powerItems;
        powerItems.Content(powerItemsInner);
        powerItems.HorizontalContentAlignment(HorizontalAlignment::Stretch);

        auto makePowerItem = [&powerItemsInner](wchar_t const* title, bool initial,
                                                std::function<void(bool)> persist)
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

            // 子项只要一个名称，不再配说明小字。
            TextBlock titleBlock;
            titleBlock.Text(title);
            titleBlock.TextWrapping(TextWrapping::Wrap);
            box.Content(titleBlock);
            powerItemsInner.Children().Append(box);
        };

        makePowerItem(L"关闭程序时", config.powerCloseApp,
                      [this](bool value) { ::ScreenControl::Ui::Config().Data().powerCloseApp = value; ::ScreenControl::Ui::Config().Save(); });
        makePowerItem(L"关闭电脑时", config.powerShutdown,
                      [this](bool value) { ::ScreenControl::Ui::Config().Data().powerShutdown = value; ::ScreenControl::Ui::Config().Save(); });
        makePowerItem(L"睡眠时", config.powerSleep,
                      [this](bool value) { ::ScreenControl::Ui::Config().Data().powerSleep = value; ::ScreenControl::Ui::Config().Save(); });
        makePowerItem(L"注销时", config.powerLogout,
                      [this](bool value) { ::ScreenControl::Ui::Config().Data().powerLogout = value; ::ScreenControl::Ui::Config().Save(); });

        ToggleSwitch powerMaster;
        powerMaster.IsOn(config.powerActions);
        powerMaster.MinWidth(0);
        powerMaster.OnContent(nullptr);
        powerMaster.OffContent(nullptr);
        powerMaster.VerticalAlignment(VerticalAlignment::Center);
        powerMaster.HorizontalAlignment(HorizontalAlignment::Right);
        Grid::SetColumn(powerMaster, 2);

        // 总开关关闭时子项灰显（与系统设置页一致）
        powerItems.IsEnabled(config.powerActions);
        powerMaster.Toggled([this, powerItems](IInspectable const& sender, RoutedEventArgs const&)
        {
            if (auto control = sender.try_as<ToggleSwitch>())
            {
                ::ScreenControl::Ui::Config().Data().powerActions = control.IsOn();
                ::ScreenControl::Ui::Config().Save();
                powerItems.IsEnabled(control.IsOn());
            }
        });

        // 注意：powerHeader 与 powerItems 只能各有一个父节点。
        // 之前先把它们塞进 powerSection，随后又设为 Expander 的 Header/Content，
        // 同一个 UIElement 被挂到两处 -> XAML 抛 E_INVALIDARG（"参数错误"）导致闪退。
        page.Children().Append(MakeSettingsCard(L"\xE7E8", L"智能关屏", powerMaster, powerItems));

        Content(MakeScrollable(page));
    }
}