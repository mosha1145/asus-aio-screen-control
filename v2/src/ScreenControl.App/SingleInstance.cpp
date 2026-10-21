#include "pch.h"
#include "SingleInstance.h"

#include <filesystem>
#include <fstream>

namespace ScreenControl
{
    namespace
    {
        constexpr wchar_t kActivateMessageName[] = L"AsusAioScreenControl.Activate.v1";
    }

    UINT SingleInstance::ActivateMessageId() noexcept
    {
        static const UINT id = ::RegisterWindowMessageW(kActivateMessageName);
        return id;
    }

    bool SingleInstance::Acquire(std::wstring const& name)
    {
        Release();

        const std::wstring mutexName = L"Local\\" + name;
        m_mutex = ::CreateMutexW(nullptr, TRUE, mutexName.c_str());
        if (m_mutex == nullptr)
        {
            return false;
        }

        if (::GetLastError() == ERROR_ALREADY_EXISTS)
        {
            // 已有实例：通知它把窗口叫出来，然后让调用方退出。
            ::CloseHandle(m_mutex);
            m_mutex = nullptr;
            BroadcastActivate();
            return false;
        }

        return true;
    }

    void SingleInstance::BroadcastActivate()
    {
        const UINT message = ActivateMessageId();
        if (message == 0)
        {
            return;
        }

        // 广播只会送达顶层窗口，且某些情况下会被过滤；
        // 这里再直接按窗口标题定位主窗口投递一次，可靠得多
        // （窗口隐藏时依然是顶层窗口，照样能收到）。
        HWND hwnd = ::FindWindowW(nullptr, L"ASUS AIO Screen Control");
        if (hwnd != nullptr)
        {
            ::PostMessageW(hwnd, message, 0, 0);
        }

        ::PostMessageW(HWND_BROADCAST, message, 0, 0);
    }

    void SingleInstance::Release() noexcept
    {
        if (m_mutex != nullptr)
        {
            ::ReleaseMutex(m_mutex);
            ::CloseHandle(m_mutex);
            m_mutex = nullptr;
        }
    }

    SingleInstance::~SingleInstance()
    {
        Release();
    }
}
