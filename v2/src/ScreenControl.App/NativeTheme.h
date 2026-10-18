#pragma once

#include <windows.h>
#include <string>

namespace ScreenControl
{
    /// 界面主题。仅三档：跟随系统 / 浅色 / 深色。
    enum class AppTheme
    {
        System = 0,
        Light = 1,
        Dark = 2,
    };

    /// 原生主题辅助。
    ///
    /// 除了 XAML 的 ElementTheme（决定 WinUI 控件外观），Windows 还有两处
    /// 系统级外观需要单独告知，否则会出现「控件变深色、但标题栏/右键菜单还是浅色」
    /// 的割裂：
    ///
    ///   1. 窗口标题栏的深色：DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)
    ///   2. Win32 原生菜单（含标题栏右键的系统菜单）的深色：
    ///      uxtheme.dll 的未公开导出 —— 序号 135 SetPreferredAppMode、
    ///      序号 136 FlushMenuThemes。
    ///      这两个接口没有文档、也不保证跨版本兼容，因此这里全部用
    ///      GetProcAddress + 空指针兜底：取不到就静默跳过，绝不影响主流程。
    class NativeTheme
    {
    public:
        /// "system" / "light" / "dark" -> 枚举（未知值按跟随系统处理）。
        static AppTheme Parse(std::wstring const& value) noexcept;

        /// 枚举 -> 存进配置的字符串。
        static wchar_t const* ToString(AppTheme theme) noexcept;

        /// 读取系统当前是否为深色（跟着「个性化 → 颜色 → 应用模式」）。
        static bool IsSystemDark() noexcept;

        /// 把（可能为「跟随系统」的）主题解析成实际是否为深色。
        static bool ResolveIsDark(AppTheme theme) noexcept;

        /// 进程级：设置 Win32 原生菜单的深色偏好，并刷新已创建的菜单。
        static void ApplyPreferredAppMode(AppTheme theme) noexcept;

        /// 窗口级：设置标题栏深色。
        static void ApplyWindowDarkMode(HWND hwnd, bool dark) noexcept;

        /// 一次性应用进程级 + 窗口级的原生主题。
        static void ApplyAll(AppTheme theme, HWND hwnd) noexcept;
    };
}
