#pragma once

// ============================================================================
//  TrayIcon.h — 系统托盘图标（Win32 Shell_NotifyIcon）
// ----------------------------------------------------------------------------
//  WinUI 3 没有托盘 API，托盘图标只能走 Win32：
//    * Shell_NotifyIconW 添加/删除图标
//    * 使用 NOTIFYICON_VERSION_4 协议，回调消息里：
//        LOWORD(lParam) 是具体事件（NIN_SELECT / WM_CONTEXTMENU ...）
//        wParam 低位是鼠标坐标
//    * 回调需要一个窗口，这里自建一个 message-only 窗口，不依赖主窗口
//
//  菜单本身不在这里创建（要 WinUI 的 MenuFlyout），只把点击事件回调出去。
// ============================================================================

#include <windows.h>

#include <functional>
#include <string>

namespace ScreenControl
{
    class TrayIcon
    {
    public:
        using Handler = std::function<void()>;

        TrayIcon() = default;
        ~TrayIcon();

        TrayIcon(TrayIcon const&) = delete;
        TrayIcon& operator=(TrayIcon const&) = delete;

        /// 创建并显示托盘图标。图标取当前 exe 的图标。
        bool Add(std::wstring const& tooltip, Handler onOpen, Handler onMenu);

        void Remove() noexcept;

        /// 当前鼠标位置（屏幕坐标），用于在光标处弹出菜单。
        static POINT CursorPosition() noexcept;

    private:
        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

        HWND m_window = nullptr;
        HICON m_icon = nullptr;
        Handler m_onOpen;
        Handler m_onMenu;
    };
}
