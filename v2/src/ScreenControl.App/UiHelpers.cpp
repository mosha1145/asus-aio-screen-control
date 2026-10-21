#include "pch.h"
#include "UiHelpers.h"
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
namespace ScreenControl::Ui
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

        /// 行为卡片：与下面的 Expander 卡片保持一致的圆角(4)与高度(64)。
        Border MakeBehaviorCard(UIElement const& child)
        {
            Border card;
            card.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
            card.Padding(Thickness{ 16, 0, 16, 0 });
            card.MinHeight(64);
            card.BorderThickness(Thickness{ 1, 1, 1, 1 });
            card.BorderBrush(Application::Current().Resources()
                                 .Lookup(box_value(L"CardStrokeColorDefaultBrush")).as<Brush>());
            card.Background(Application::Current().Resources()
                                .Lookup(box_value(L"CardBackgroundFillColorDefaultBrush")).as<Brush>());
            if (auto element = child.try_as<FrameworkElement>())
            {
                element.VerticalAlignment(VerticalAlignment::Center);
            }
            card.Child(child);
            return card;
        }
        /// 设置页的小节标题（与系统设置页一致：BodyStrong，14px 加粗、主文本色）
        TextBlock MakeSectionTitle(hstring const& text)
        {
            TextBlock block;
            block.Text(text);
            block.FontSize(14);
            block.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            block.Margin(Thickness{ 2, 16, 0, 6 });
            return block;
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
            card.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
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

    UIElement MakeSettingsCard(wchar_t const* glyph, hstring const& title,
                                           UIElement const& trailing, UIElement const& body)
    {
        // 用原生 Expander 作外壳，保留 Fluent 卡片的默认样式与展开动画；
        // 只把标题行做成可拉伸的 Grid，保证右侧元素贴右、标题垂直居中。
        Grid header;
        header.MinHeight(64);
        header.ColumnSpacing(16);
        header.HorizontalAlignment(HorizontalAlignment::Stretch);
        header.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());
        header.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); return c; }());
        header.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());

        FontIcon icon;
        icon.Glyph(glyph);
        icon.FontSize(16);
        icon.VerticalAlignment(VerticalAlignment::Center);
        icon.HorizontalAlignment(HorizontalAlignment::Center);
        Grid::SetColumn(icon, 0);
        header.Children().Append(icon);

        TextBlock titleBlock;
        titleBlock.Text(title);
        titleBlock.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(titleBlock, 1);
        header.Children().Append(titleBlock);

        if (auto element = trailing.try_as<FrameworkElement>())
        {
            element.VerticalAlignment(VerticalAlignment::Center);
            element.HorizontalAlignment(HorizontalAlignment::Right);
            Grid::SetColumn(element, 2);
            header.Children().Append(element);
        }

        Expander expander;
        expander.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
        expander.HorizontalAlignment(HorizontalAlignment::Stretch);
        expander.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        expander.Header(header);
        // 子选项缩进：与标题文字对齐（图标列 16 + 列间距 16，约 32），
        // 而不是贴着卡片左侧与图标对齐。
        Grid bodyHost;
        bodyHost.Margin(Thickness{ 28, 0, 0, 0 });
        if (auto bodyElement = body.try_as<FrameworkElement>())
        {
            bodyElement.HorizontalAlignment(HorizontalAlignment::Stretch);
        }
        bodyHost.Children().Append(body);
        expander.Content(bodyHost);
        return expander;
    }
    ::ScreenControl::Core::ConfigStore& Config()
    {
        return AppConfigStore();
    }

}
