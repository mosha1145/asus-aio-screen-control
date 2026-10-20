#pragma once
#include "DisplayPage.g.h"
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
namespace winrt::ScreenControl::implementation
{
    struct DisplayPage : DisplayPageT<DisplayPage>
    {
        DisplayPage();
        void OnNavigatedFrom(Microsoft::UI::Xaml::Navigation::NavigationEventArgs const& e);
    private:
        winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource m_previewSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_previewPlaceholder{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_statusText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_deviceText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Image m_previewImage{ nullptr };
    };
}
namespace winrt::ScreenControl::factory_implementation
{
    struct DisplayPage : DisplayPageT<DisplayPage, implementation::DisplayPage>
    {
    };
}