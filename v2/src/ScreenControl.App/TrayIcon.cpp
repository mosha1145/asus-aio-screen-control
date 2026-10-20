#include "pch.h"
#include "TrayIcon.h"

#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

namespace ScreenControl
{
    namespace
    {
        constexpr wchar_t kTrayWindowClass[] = L"ScreenControl.TrayWindow";
        constexpr UINT kTrayCallbackMessage = WM_APP + 101;
        constexpr UINT kTrayIconId = 1;

        /// 取当前 exe 的图标（用户要求"用现有的程序图标"）。
        HICON LoadApplicationIcon() noexcept
        {
            wchar_t modulePath[MAX_PATH]{};
            if (::GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
            {
                return ::LoadIconW(nullptr, IDI_APPLICATION);
            }

            SHFILEINFOW info{};
            if (::SHGetFileInfoW(modulePath, 0, &info, sizeof(info),
                                 SHGFI_ICON | SHGFI_LARGEICON) != 0 &&
                info.hIcon != nullptr)
            {
                return info.hIcon;
            }

            HICON large = nullptr;
            if (::ExtractIconExW(modulePath, 0, &large, nullptr, 1) > 0 && large != nullptr)
            {
                return large;
            }

            return ::LoadIconW(nullptr, IDI_APPLICATION);
        }
    }

    TrayIcon::~TrayIcon()
    {
        Remove();
    }

    bool TrayIcon::Add(std::wstring const& tooltip, Handler onOpen, Handler onMenu)
    {
        Remove();

        m_onOpen = std::move(onOpen);
        m_onMenu = std::move(onMenu);

        // message-only 窗口：只用来收托盘回调，不显示
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &TrayIcon::WindowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kTrayWindowClass;
        ::RegisterClassExW(&windowClass);   // 重复注册会失败，忽略即可

        m_window = ::CreateWindowExW(0, kTrayWindowClass, L"", 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), this);
        if (m_window == nullptr)
        {
            return false;
        }

        m_icon = LoadApplicationIcon();

        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = m_window;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayCallbackMessage;
        data.hIcon = m_icon;
        wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);

        if (!::Shell_NotifyIconW(NIM_ADD, &data))
        {
            return false;
        }

        // 启用 V4 协议：可以拿到 NIN_SELECT / WM_CONTEXTMENU 等语义化事件
        data.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &data);
        return true;
    }

    void TrayIcon::Remove() noexcept
    {
        if (m_window != nullptr)
        {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = m_window;
            data.uID = kTrayIconId;
            ::Shell_NotifyIconW(NIM_DELETE, &data);
        }

        if (m_icon != nullptr)
        {
            ::DestroyIcon(m_icon);
            m_icon = nullptr;
        }

        if (m_window != nullptr)
        {
            ::DestroyWindow(m_window);
            m_window = nullptr;
        }
    }

    POINT TrayIcon::CursorPosition() noexcept
    {
        POINT point{};
        ::GetCursorPos(&point);
        return point;
    }

    LRESULT CALLBACK TrayIcon::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == kTrayCallbackMessage)
        {
            auto* self = reinterpret_cast<TrayIcon*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self != nullptr)
            {
                switch (LOWORD(lParam))
                {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (self->m_onOpen) { self->m_onOpen(); }
                    return 0;

                case WM_CONTEXTMENU:
                    if (self->m_onMenu) { self->m_onMenu(); }
                    return 0;

                default:
                    break;
                }
            }
            return 0;
        }

        if (message == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }

        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
