#pragma once

// ============================================================================
//  SingleInstance.h — 单实例守卫
// ----------------------------------------------------------------------------
//  防多开，并让第二次启动能把已运行的窗口"叫回来"。
//
//  实现：
//    * 命名互斥量判断是否已有实例（Local\ 前缀，只在本会话生效）
//    * 已存在时广播一条自定义窗口消息，第一个实例收到后显示并激活窗口
//    * 广播用 HWND_BROADCAST，无需 IPC、无需共享内存，进程崩溃也不会留下垃圾
// ============================================================================

#include <windows.h>

#include <string>

namespace ScreenControl
{
    class SingleInstance
    {
    public:
        /// 第二个实例调用时返回的消息 ID；用于"激活已有窗口"。
        static UINT ActivateMessageId() noexcept;

        /// 尝试成为唯一实例。
        /// 返回 true 表示当前进程是第一个实例；false 表示已有实例在运行
        /// （此时内部已经向已有实例广播了激活消息，调用方应直接退出）。
        bool Acquire(std::wstring const& name);

        /// 广播激活消息（第二个实例使用）。
        static void BroadcastActivate();

        void Release() noexcept;

        ~SingleInstance();

    private:
        HANDLE m_mutex = nullptr;
    };
}
