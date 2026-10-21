#include "pch.h"
#include "TrayIcon.h"

#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

namespace ScreenControl
{
    namespace
    {
        constexpr wchar_t kTrayIconProp[] = L"ScreenControl.TrayIcon.Instance";
        constexpr UINT kTrayIconId = 1;

        /// 取当前 exe 的图标；失败时退回系统默认图标（保证 hIcon 非空）。
        HICON LoadApplicationIcon() noexcept
        {
            wchar_t modulePath[MAX_PATH]{};
            if (::GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
            {
                SHFILEINFOW info{};
                if (::SHGetFileInfoW(modulePath, 0, &info, sizeof(info),
                                     SHGFI_ICON | SHGFI_LARGEICON) != 0 &&
                    info.hIcon != nullptr)
                {
                    return info.hIcon;
                }
            }

            return ::LoadIconW(nullptr, MAKEINTRESOURCEW(32512));   // IDI_APPLICATION
        }
    }

    TrayIcon::~TrayIcon()
    {
        Remove();
    }

    bool TrayIcon::Add(HWND owner, std::wstring const& tooltip, Handler onOpen, Handler onMenu)
    {
        Remove();

        if (owner == nullptr || !::IsWindow(owner))
        {
            return false;
        }

        m_owner = owner;
        m_onOpen = std::move(onOpen);
        m_onMenu = std::move(onMenu);

        // 子类化主窗口，接收托盘回调消息（链式转发给原有窗口过程）
        ::SetPropW(m_owner, kTrayIconProp, reinterpret_cast<HANDLE>(this));
        m_originalProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
            m_owner, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&TrayIcon::ForwardedWndProc)));

        if (m_icon == nullptr)
        {
            m_icon = LoadApplicationIcon();
        }

        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = m_owner;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = CallbackMessage;
        data.hIcon = m_icon;
        wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);

        if (!::Shell_NotifyIconW(NIM_ADD, &data))
        {
            return false;
        }

        data.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &data);
        return true;
    }

    void TrayIcon::Remove() noexcept
    {
        if (m_owner != nullptr)
        {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = m_owner;
            data.uID = kTrayIconId;
            ::Shell_NotifyIconW(NIM_DELETE, &data);

            ::RemovePropW(m_owner, kTrayIconProp);

            if (m_originalProc != nullptr && ::IsWindow(m_owner))
            {
                ::SetWindowLongPtrW(m_owner, GWLP_WNDPROC,
                                    reinterpret_cast<LONG_PTR>(m_originalProc));
            }
            m_originalProc = nullptr;
        }

        if (m_icon != nullptr)
        {
            ::DestroyIcon(m_icon);
            m_icon = nullptr;
        }

        m_owner = nullptr;
        m_onOpen = nullptr;
        m_onMenu = nullptr;
    }

    POINT TrayIcon::CursorPosition() noexcept
    {
        POINT point{};
        ::GetCursorPos(&point);
        return point;
    }

    void TrayIcon::HandleCallback(LPARAM lParam) const
    {
        switch (LOWORD(lParam))
        {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (m_onOpen) { m_onOpen(); }
            break;

        case WM_CONTEXTMENU:
            if (m_onMenu) { m_onMenu(); }
            break;

        default:
            break;
        }
    }

    LRESULT CALLBACK TrayIcon::ForwardedWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<TrayIcon*>(::GetPropW(hwnd, kTrayIconProp));

        if (self != nullptr && message == CallbackMessage)
        {
            // 窗口过程里绝不能让异常逃逸（会变成 0xC000041D 崩溃）
            try
            {
                self->HandleCallback(lParam);
            }
            catch (...)
            {
            }
            return 0;
        }

        if (self != nullptr && self->m_originalProc != nullptr)
        {
            return ::CallWindowProcW(self->m_originalProc, hwnd, message, wParam, lParam);
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
