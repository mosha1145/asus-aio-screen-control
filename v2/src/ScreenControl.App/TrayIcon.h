#pragma once

// ============================================================================
//  TrayIcon.h — 系统托盘图标（Win32 Shell_NotifyIcon）
// ----------------------------------------------------------------------------
//  WinUI 3 没有托盘 API，托盘图标只能走 Win32：
//    * Shell_NotifyIconW（NOTIFYICONDATAW，必须用 W 版本）添加/删除图标
//    * NOTIFYICON_VERSION_4 协议：回调消息的 LOWORD(lParam) 是事件
//        NIN_SELECT / NIN_KEYSELECT -> 左键
//        WM_CONTEXTMENU             -> 右键
//
//  回调窗口直接使用主窗口 HWND（AppWindow().Id().Value）。
//  这里对主窗口做一次 WNDPROC 子类化来接收回调消息，链式转发给原有窗口过程，
//  因此不会与 PowerMonitor 的子类化冲突。
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

        /// 托盘回调消息号
        static constexpr UINT CallbackMessage = WM_APP + 101;

        TrayIcon() = default;
        ~TrayIcon();

        TrayIcon(TrayIcon const&) = delete;
        TrayIcon& operator=(TrayIcon const&) = delete;

        /// 在 owner 窗口上注册托盘图标（owner 必须是本进程的有效顶层窗口）。
        bool Add(HWND owner, std::wstring const& tooltip, Handler onOpen, Handler onMenu);

        void Remove() noexcept;

        /// 当前鼠标位置（屏幕坐标），用于在光标处弹出菜单。
        static POINT CursorPosition() noexcept;

    private:
        static LRESULT CALLBACK ForwardedWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

        void HandleCallback(LPARAM lParam) const;

        HWND m_owner = nullptr;
        HICON m_icon = nullptr;
        WNDPROC m_originalProc = nullptr;
        Handler m_onOpen;
        Handler m_onMenu;
    };
}
