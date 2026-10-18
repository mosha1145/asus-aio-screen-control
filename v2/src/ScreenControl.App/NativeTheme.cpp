#include "pch.h"
#include "NativeTheme.h"

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace ScreenControl
{
    namespace
    {
        // ---- uxtheme.dll 未公开导出（按序号调用）---------------------------------
        // 135: int SetPreferredAppMode(int preferredAppMode)
        //       0 = Default（跟随系统）1 = AllowDark 2 = ForceDark 3 = ForceLight
        // 136: void FlushMenuThemes()
        // 这两个接口没有官方文档、也不保证跨版本兼容；取不到就跳过。
        using SetPreferredAppModeFn = int(__stdcall*)(int);
        using FlushMenuThemesFn = void(__stdcall*)();

        struct UxThemeExports
        {
            SetPreferredAppModeFn SetPreferredAppMode = nullptr;
            FlushMenuThemesFn FlushMenuThemes = nullptr;
        };

        UxThemeExports const& GetUxThemeExports() noexcept
        {
            static UxThemeExports const exports = []()
            {
                UxThemeExports result;
                if (HMODULE ux = LoadLibraryW(L"uxtheme.dll"))
                {
                    result.SetPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
                        GetProcAddress(ux, MAKEINTRESOURCEA(135)));
                    result.FlushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
                        GetProcAddress(ux, MAKEINTRESOURCEA(136)));
                }
                return result;
            }();
            return exports;
        }

        // PreferredAppMode 取值（来自 Explorer 的用法）
        constexpr int kPreferredAppModeDefault = 0;
        constexpr int kPreferredAppModeForceDark = 2;
        constexpr int kPreferredAppModeForceLight = 3;

        // DWMWA_USE_IMMERSIVE_DARK_MODE = 20（Win10 2004+）
        constexpr DWORD kDwmUseImmersiveDarkMode = 20;

        /// 读取 HKCU\...\Themes\Personalize\AppsUseLightTheme。取不到按浅色处理。
        bool SystemPrefersLight() noexcept
        {
            DWORD value = 1;
            DWORD size = sizeof(value);
            LONG status = RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme",
                RRF_RT_REG_DWORD,
                nullptr,
                &value,
                &size);
            if (status != ERROR_SUCCESS)
            {
                return true;
            }
            return value != 0;
        }
    }

    AppTheme NativeTheme::Parse(std::wstring const& value) noexcept
    {
        if (value == L"light")
        {
            return AppTheme::Light;
        }
        if (value == L"dark")
        {
            return AppTheme::Dark;
        }
        return AppTheme::System;
    }

    wchar_t const* NativeTheme::ToString(AppTheme theme) noexcept
    {
        switch (theme)
        {
        case AppTheme::Light: return L"light";
        case AppTheme::Dark: return L"dark";
        default: return L"system";
        }
    }

    bool NativeTheme::IsSystemDark() noexcept
    {
        return !SystemPrefersLight();
    }

    bool NativeTheme::ResolveIsDark(AppTheme theme) noexcept
    {
        switch (theme)
        {
        case AppTheme::Light: return false;
        case AppTheme::Dark: return true;
        default: return IsSystemDark();
        }
    }

    void NativeTheme::ApplyPreferredAppMode(AppTheme theme) noexcept
    {
        UxThemeExports const& exports = GetUxThemeExports();
        if (exports.SetPreferredAppMode == nullptr)
        {
            return;   // 系统不支持：静默跳过
        }

        int mode = kPreferredAppModeDefault;   // 跟随系统
        if (theme == AppTheme::Dark)
        {
            mode = kPreferredAppModeForceDark;
        }
        else if (theme == AppTheme::Light)
        {
            mode = kPreferredAppModeForceLight;
        }
        exports.SetPreferredAppMode(mode);

        if (exports.FlushMenuThemes != nullptr)
        {
            exports.FlushMenuThemes();
        }
    }

    void NativeTheme::ApplyWindowDarkMode(HWND hwnd, bool dark) noexcept
    {
        if (hwnd == nullptr)
        {
            return;
        }
        BOOL enable = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &enable, sizeof(enable));
    }

    void NativeTheme::ApplyAll(AppTheme theme, HWND hwnd) noexcept
    {
        ApplyPreferredAppMode(theme);
        ApplyWindowDarkMode(hwnd, ResolveIsDark(theme));
    }
}
