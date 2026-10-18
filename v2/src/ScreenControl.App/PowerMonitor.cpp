#include "pch.h"
#include "PowerMonitor.h"

namespace ScreenControl
{
    namespace
    {
        /// 每个窗口只允许一个 PowerMonitor 实例，用窗口 USERDATA 挂接。
        constexpr wchar_t kPowerMonitorProp[] = L"ScreenControl.PowerMonitor";

        void Invoke(::ScreenControl::PowerMonitor::Handler const& handler) noexcept
        {
            if (!handler)
            {
                return;
            }
            try
            {
                handler();
            }
            catch (...)
            {
                // 电源事件里绝不能因为回调异常打断系统关机/睡眠流程
            }
        }
    }

    PowerMonitor::~PowerMonitor()
    {
        Stop();
    }

    bool PowerMonitor::Start(HWND hwnd, Handlers handlers)
    {
        Stop();

        if (hwnd == nullptr || !IsWindow(hwnd))
        {
            return false;
        }

        m_handlers = std::move(handlers);
        m_hwnd = hwnd;

        // 保存原始窗口过程，替换成我们的转发过程。
        m_originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PowerMonitor::ForwardedWndProc)));
        if (m_originalProc == nullptr)
        {
            m_hwnd = nullptr;
            return false;
        }

        // 把 this 挂到窗口属性上（避免占用 USERDATA，WinUI 自己也要用）。
        SetPropW(m_hwnd, kPowerMonitorProp, reinterpret_cast<HANDLE>(this));
        return true;
    }

    void PowerMonitor::Stop() noexcept
    {
        if (m_hwnd == nullptr)
        {
            return;
        }

        RemovePropW(m_hwnd, kPowerMonitorProp);

        if (m_originalProc != nullptr && IsWindow(m_hwnd))
        {
            SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(m_originalProc));
        }

        m_originalProc = nullptr;
        m_hwnd = nullptr;
        m_handlers = {};
    }

    LRESULT CALLBACK PowerMonitor::ForwardedWndProc(HWND hwnd, UINT message,
                                                    WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<PowerMonitor*>(GetPropW(hwnd, kPowerMonitorProp));

        if (self != nullptr)
        {
            switch (message)
            {
            case WM_POWERBROADCAST:
                if (wParam == PBT_APMSUSPEND)
                {
                    Invoke(self->m_handlers.suspend);
                }
                break;

            case WM_QUERYENDSESSION:
                // lParam 低位为 ENDSESSION_LOGOFF 标志
                if ((lParam & ENDSESSION_LOGOFF) != 0)
                {
                    Invoke(self->m_handlers.logoff);
                }
                else
                {
                    Invoke(self->m_handlers.shutdown);
                }
                break;

            case WM_ENDSESSION:
                // 兜底：如果 QUERYENDSESSION 没走到（例如被跳过），这里再试一次。
                // wParam 为 TRUE 表示会话确实要结束。
                if (wParam != 0)
                {
                    if ((lParam & ENDSESSION_LOGOFF) != 0)
                    {
                        Invoke(self->m_handlers.logoff);
                    }
                    else
                    {
                        Invoke(self->m_handlers.shutdown);
                    }
                }
                break;

            default:
                break;
            }
        }

        // 转回原始窗口过程。注意：self 可能在回调里被 Stop() 掉，
        // 所以这里用成员保存的原始过程地址（self 可能已失效）。
        if (self != nullptr && self->m_originalProc != nullptr)
        {
            return CallWindowProcW(self->m_originalProc, hwnd, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
