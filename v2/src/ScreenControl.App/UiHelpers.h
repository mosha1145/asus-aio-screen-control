#pragma once
#include <windows.h>
#include <string>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include "AppConfig.h"
namespace ScreenControl::Ui
{
    ::ScreenControl::Core::ConfigStore& Config();
    winrt::Microsoft::UI::Xaml::Controls::TextBlock MakeSectionTitle(winrt::hstring const& text);
    winrt::Microsoft::UI::Xaml::Controls::TextBlock MakeTitle(winrt::hstring const& text);
    winrt::Microsoft::UI::Xaml::Controls::Border MakeCard(winrt::Microsoft::UI::Xaml::UIElement const& child);
    winrt::Microsoft::UI::Xaml::Controls::Border MakeBehaviorCard(winrt::Microsoft::UI::Xaml::UIElement const& child);
    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer MakeScrollable(winrt::Microsoft::UI::Xaml::UIElement const& content);
    winrt::Microsoft::UI::Xaml::UIElement MakeSettingsCard(wchar_t const* glyph, winrt::hstring const& title,
        winrt::Microsoft::UI::Xaml::UIElement const& trailing, winrt::Microsoft::UI::Xaml::UIElement const& body);
}