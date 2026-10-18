#pragma once

// ============================================================================
//  PowerMonitor.h — 电源 / 会话事件监听
// ----------------------------------------------------------------------------
//  对应 v1 tuf_gui.py 里通过 Qt nativeEvent 捕获的那几个 Windows 消息：
//      WM_QUERYENDSESSION / WM_ENDSESSION   -> 关机、注销
//      WM_POWERBROADCAST + PBT_APMSUSPEND   -> 睡眠
//
//  v1 的注释里已经写明这条路「够用但不可靠」（消息可能来不及处理就被挂起），
//  并建议 C++ 侧改用 RegisterSuspendResumeNotification /
//  WTSRegisterSessionNotification / 任务计划程序兜底。
//  这里先实现最直接的一层：子类化窗口过程，拿到消息后立刻关屏。
//  后续可以叠加更可靠的 API。
//
//  注意：这里刻意不用 SetWindowSubclass（依赖 comctl32 的 v6 激活上下文），
//  改用 SetWindowLongPtr(GWLP_WNDPROC) 的经典子类化，兼容性最好。
// ============================================================================

#include <windows.h>

#include <functional>

namespace ScreenControl
{
    class PowerMonitor
    {
    public:
        using Handler = std::function<void()>;

        struct Handlers
        {
            Handler suspend;   ///< 进入睡眠
            Handler shutdown;  ///< 关机
            Handler logoff;    ///< 注销
        };

        PowerMonitor() = default;
        ~PowerMonitor();

        PowerMonitor(PowerMonitor const&) = delete;
        PowerMonitor& operator=(PowerMonitor const&) = delete;

        /// 开始监听指定窗口。重复调用会先停止上一次。
        bool Start(HWND hwnd, Handlers handlers);

        /// 停止监听并恢复原始窗口过程。
        void Stop() noexcept;

        bool Active() const noexcept { return m_hwnd != nullptr; }

    private:
        static LRESULT CALLBACK ForwardedWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

        HANDLE m_owner = nullptr;      // 保留原窗口的 USERDATA
        HWND m_hwnd = nullptr;
        WNDPROC m_originalProc = nullptr;
        Handlers m_handlers;
    };
}
