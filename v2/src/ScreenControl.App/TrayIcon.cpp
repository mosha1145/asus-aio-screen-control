#include "pch.h"
#include "TrayIcon.h"

#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>

#pragma comment(lib, "shell32.lib")

namespace ScreenControl
{
    namespace
    {
        /// 诊断日志：写到 exe 同目录 tray_debug.log（UTF-8，追加）
        void DiagLog(wchar_t const* message, DWORD code = 0) noexcept
        {
            wchar_t modulePath[MAX_PATH]{};
            ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            const std::filesystem::path logPath =
                std::filesystem::path(modulePath).parent_path() / L"tray_debug.log";

            std::wstring line = message;
            if (code != 0)
            {
                line += L" err=";
                line += std::to_wstring(code);
            }
            line += L"\r\n";

            const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                                    static_cast<int>(line.size()),
                                                    nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<size_t>(bytes), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                  utf8.data(), bytes, nullptr, nullptr);

            std::ofstream log(logPath, std::ios::binary | std::ios::app);
            log.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        }

        constexpr wchar_t kTrayWindowClass[] = L"ScreenControl.TrayWindow";
        constexpr UINT kTrayCallbackMessage = WM_APP + 101;
        constexpr UINT kTrayIconId = 1;
        constexpr UINT_PTR kRetryTimerId = 1;

        /// 资源管理器重启后需要重新添加图标
        UINT TaskbarCreatedMessage() noexcept
        {
            static const UINT id = ::RegisterWindowMessageW(L"TaskbarCreated");
            return id;
        }

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

    bool TrayIcon::Add(std::wstring const& tooltip, Handler onOpen, Handler onMenu,
                       UINT activateMessage, Handler onActivate)
    {
        Remove();

        m_tooltip = tooltip;
        m_onOpen = std::move(onOpen);
        m_activateMessage = activateMessage;
        m_onActivate = std::move(onActivate);
        m_onMenu = std::move(onMenu);

        // message-only 窗口：只用来收托盘回调，不显示
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &TrayIcon::WindowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kTrayWindowClass;
        ::RegisterClassExW(&windowClass);   // 重复注册会失败，忽略即可

        const DWORD classError = ::GetLastError();
        // 注意：不能用 HWND_MESSAGE（message-only 窗口）。
        // 实测 message-only 窗口下 Shell_NotifyIcon 返回 ERROR_ACCESS_DENIED，
        // 托盘图标根本加不进去。这里改用隐藏的顶层工具窗口。
        m_window = ::CreateWindowExW(WS_EX_TOOLWINDOW, kTrayWindowClass, L"", WS_POPUP,
                                     0, 0, 0, 0, nullptr, nullptr,
                                     ::GetModuleHandleW(nullptr), this);
        if (m_window == nullptr)
        {
            DiagLog(L"tray: CreateWindowEx failed", ::GetLastError());
            return false;
        }
        DiagLog(L"tray: window created");

        m_icon = LoadApplicationIcon();
        DiagLog(m_icon != nullptr ? L"tray: icon loaded" : L"tray: icon NULL");

        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = m_window;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = kTrayCallbackMessage;
        data.hIcon = m_icon;
        wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);

        if (!::Shell_NotifyIconW(NIM_ADD, &data))
        {
            // 失败原因可能是 Explorer 还没就绪（或权限差异），这里定时重试几次。
            DiagLog(L"tray: NIM_ADD FAILED (will retry)", ::GetLastError());
            m_retryCount = 0;
            ::SetTimer(m_window, kRetryTimerId, 1000, nullptr);
            return false;
        }
        DiagLog(L"tray: NIM_ADD ok");

        // 启用 V4 协议：可以拿到 NIN_SELECT / WM_CONTEXTMENU 等语义化事件
        data.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &data);
        return true;
    }

    void TrayIcon::RetryAdd() noexcept
    {
        if (m_window == nullptr)
        {
            return;
        }

        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = m_window;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = kTrayCallbackMessage;
        data.hIcon = m_icon;
        wcsncpy_s(data.szTip, m_tooltip.c_str(), _TRUNCATE);

        if (::Shell_NotifyIconW(NIM_ADD, &data))
        {
            data.uVersion = NOTIFYICON_VERSION_4;
            ::Shell_NotifyIconW(NIM_SETVERSION, &data);
            ::KillTimer(m_window, kRetryTimerId);
            DiagLog(L"tray: NIM_ADD ok (retry)");
            return;
        }

        if (++m_retryCount >= 10)
        {
            ::KillTimer(m_window, kRetryTimerId);
            DiagLog(L"tray: NIM_ADD gave up", ::GetLastError());
        }
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

        // 单实例激活消息：投递到本窗口（类名固定，跨进程可达）
        {
            auto* self = reinterpret_cast<TrayIcon*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self != nullptr && self->m_activateMessage != 0 &&
                message == self->m_activateMessage)
            {
                if (self->m_onActivate) { self->m_onActivate(); }
                return 0;
            }
        }

        if (message == WM_TIMER && wParam == kRetryTimerId)
        {
            auto* self = reinterpret_cast<TrayIcon*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self != nullptr)
            {
                self->RetryAdd();
            }
            return 0;
        }

        if (message == TaskbarCreatedMessage())
        {
            // 资源管理器重启后托盘区会清空，这里把图标加回去
            auto* self = reinterpret_cast<TrayIcon*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self != nullptr && self->m_icon != nullptr)
            {
                NOTIFYICONDATAW data{};
                data.cbSize = sizeof(data);
                data.hWnd = hwnd;
                data.uID = kTrayIconId;
                data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
                data.uCallbackMessage = kTrayCallbackMessage;
                data.hIcon = self->m_icon;
                wcsncpy_s(data.szTip, self->m_tooltip.c_str(), _TRUNCATE);
                ::Shell_NotifyIconW(NIM_ADD, &data);
                data.uVersion = NOTIFYICON_VERSION_4;
                ::Shell_NotifyIconW(NIM_SETVERSION, &data);
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
