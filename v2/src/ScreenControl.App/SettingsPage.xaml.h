#pragma once
#include "SettingsPage.g.h"
namespace winrt::ScreenControl::implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage>
    {
        SettingsPage();
    };
}
namespace winrt::ScreenControl::factory_implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage, implementation::SettingsPage>
    {
    };
}