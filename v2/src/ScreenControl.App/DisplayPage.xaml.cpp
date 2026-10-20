#include "pch.h"
#include "DisplayPage.xaml.h"
#include "UiHelpers.h"
#include "PlaybackController.h"
#if __has_include("DisplayPage.g.cpp")
#include "DisplayPage.g.cpp"
#endif
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace ::ScreenControl::Ui;
namespace winrt::ScreenControl::implementation
{
    DisplayPage::DisplayPage()
    {
        InitializeComponent();

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
        preview.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 8, 8, 8, 8 });
        preview.BorderThickness(Thickness{ 1, 1, 1, 1 });
        preview.BorderBrush(Application::Current().Resources()
                                .Lookup(box_value(L"CardStrokeColorDefaultBrush")).as<Brush>());
        // 画面四周（letterbox 区域）一律黑色，与设备实际显示一致
        preview.Background(SolidColorBrush{ Windows::UI::Colors::Black() });

        // 与 Mica 背景区分开：加主题阴影并抬升 Z 轴
        auto previewShadow = ThemeShadow{};
        preview.Shadow(previewShadow);
        preview.Translation(Windows::Foundation::Numerics::float3{ 0.0f, 0.0f, 16.0f });
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
        // Border 设置 CornerRadius 后会裁剪子内容，画面圆角自然与外框一致。
        preview.Child(previewInner);
        Grid::SetColumn(preview, 0);
        body.Children().Append(preview);

        // 右侧：控制卡
        StackPanel controls;
        controls.Spacing(14);

        ToggleSwitch screenSwitch;
        screenSwitch.Header(box_value(L"屏幕开关"));
        // 初值来自配置；先设状态再挂事件，避免初始化时触发一次多余的命令。
        screenSwitch.IsOn(::ScreenControl::Ui::Config().Data().screenOn);
        screenSwitch.Toggled([this](IInspectable const& sender, RoutedEventArgs const&)
        {
            if (auto toggle = sender.try_as<ToggleSwitch>())
            {
                // 记住用户设定，下次启动按此恢复（与电源联动关屏互不覆盖）
                ::ScreenControl::Ui::Config().Data().screenOn = toggle.IsOn();
                ::ScreenControl::Ui::Config().Save();
                MainWindow::Current()->Player().SetScreenPower(toggle.IsOn());
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
        brightness.Value(::ScreenControl::Ui::Config().Data().brightness);
        brightnessRow.Children().Append(brightness);
        // 拖动时实时下发命令；松手（失焦）时才写配置，避免拖一次写上百次文件。
        brightness.ValueChanged([this](IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
        {
            MainWindow::Current()->Player().SetBrightness(static_cast<int>(args.NewValue()));
        });
        brightness.LostFocus([this, brightness](IInspectable const&, RoutedEventArgs const&)
        {
            ::ScreenControl::Ui::Config().Data().brightness = static_cast<int>(brightness.Value());
            ::ScreenControl::Ui::Config().Save();
        });
        controls.Children().Append(brightnessRow);

        ComboBox fitMode;
        fitMode.Header(box_value(L"图像适应"));
        fitMode.Items().Append(box_value(L"适应 · contain"));
        fitMode.Items().Append(box_value(L"填充 · cover"));
        fitMode.Items().Append(box_value(L"拉伸 · stretch"));
        {
            const std::wstring stored = ::ScreenControl::Ui::Config().Data().fitMode;
            fitMode.SelectedIndex(stored == L"cover" ? 1 : (stored == L"stretch" ? 2 : 0));
        }
        fitMode.SelectionChanged([this, fitMode](IInspectable const&, SelectionChangedEventArgs const&)
        {
            auto params = MainWindow::Current()->Player().Params();
            switch (fitMode.SelectedIndex())
            {
            case 1: params.fitMode = ::ScreenControl::Imaging::FitMode::Cover; break;
            case 2: params.fitMode = ::ScreenControl::Imaging::FitMode::Stretch; break;
            default: params.fitMode = ::ScreenControl::Imaging::FitMode::Contain; break;
            }
            MainWindow::Current()->Player().SetParams(params);
            ::ScreenControl::Ui::Config().Data().fitMode =
                fitMode.SelectedIndex() == 1 ? L"cover"
                : (fitMode.SelectedIndex() == 2 ? L"stretch" : L"contain");
            ::ScreenControl::Ui::Config().Save();
        });
        controls.Children().Append(fitMode);

        ComboBox rotation;
        rotation.Header(box_value(L"画面旋转"));
        rotation.Items().Append(box_value(L"0°"));
        rotation.Items().Append(box_value(L"90°"));
        rotation.Items().Append(box_value(L"180°"));
        rotation.Items().Append(box_value(L"270°"));
        {
            const int storedRotation = ::ScreenControl::Ui::Config().Data().rotation;
            rotation.SelectedIndex(storedRotation == 90 ? 1 : (storedRotation == 180 ? 2 : (storedRotation == 270 ? 3 : 0)));
        }
        rotation.SelectionChanged([this, rotation](IInspectable const&, SelectionChangedEventArgs const&)
        {
            auto params = MainWindow::Current()->Player().Params();
            switch (rotation.SelectedIndex())
            {
            case 1: params.rotation = ::ScreenControl::Imaging::Rotation::Deg90; break;
            case 2: params.rotation = ::ScreenControl::Imaging::Rotation::Deg180; break;
            case 3: params.rotation = ::ScreenControl::Imaging::Rotation::Deg270; break;
            default: params.rotation = ::ScreenControl::Imaging::Rotation::Deg0; break;
            }
            MainWindow::Current()->Player().SetParams(params);
            ::ScreenControl::Ui::Config().Data().rotation =
                rotation.SelectedIndex() == 1 ? 90
                : (rotation.SelectedIndex() == 2 ? 180 : (rotation.SelectedIndex() == 3 ? 270 : 0));
            ::ScreenControl::Ui::Config().Save();
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
        m_deviceText.Text(MainWindow::Current()->Player().DeviceReady() ? L"设备：已连接" : L"设备：未连接（若官方 InfoHub 正在运行，请先退出它——它会独占 HID 接口）");
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
        MainWindow::Current()->PopulateMediaList(photoList,
                          (::ScreenControl::Ui::Config().AssetRoot() / L"photos").wstring(),
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
                MainWindow::Current()->Player().Play(std::filesystem::path(path));
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
        MainWindow::Current()->PopulateMediaList(videoList,
                          (::ScreenControl::Ui::Config().AssetRoot() / L"videos").wstring(),
                          L".mp4,.avi,.mov,.mkv,.webm,.m4v");
        videoPanel.Children().Append(videoList);
        Button playVideo;
        playVideo.Content(box_value(L"播放选中的视频"));
        playVideo.Click([this, videoList](auto const&, RoutedEventArgs const&)
        {
            if (auto item = videoList.SelectedItem().try_as<ListViewItem>())
            {
                const std::wstring path = unbox_value<hstring>(item.Tag()).c_str();
                MainWindow::Current()->Player().Play(std::filesystem::path(path));
            }
        });
        videoPanel.Children().Append(playVideo);
        Grid::SetColumn(videoPanel, 1);
        mediaRow.Children().Append(videoPanel);

        media.Children().Append(mediaRow);

        Button stopButton;
        stopButton.Content(box_value(L"停止显示"));
        stopButton.Click([this](auto const&, RoutedEventArgs const&) { MainWindow::Current()->Player().Stop(); });
        media.Children().Append(stopButton);

        page.Children().Append(MakeCard(media));

        MainWindow::Current()->AttachPreview(m_previewSource, m_previewPlaceholder);
        MainWindow::Current()->AttachStatusText(m_statusText);
        MainWindow::Current()->AttachDeviceText(m_deviceText);
        Content(MakeScrollable(page));
    }

    void DisplayPage::OnNavigatedFrom(Microsoft::UI::Xaml::Navigation::NavigationEventArgs const&)
    {
        MainWindow::Current()->DetachPageUi();
    }
}