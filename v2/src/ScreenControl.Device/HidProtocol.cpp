#include "pch.h"
#include "HidProtocol.h"

namespace ScreenControl::Device
{
    SendStatus ClassifyWin32Error(std::uint32_t lastError) noexcept
    {
        switch (lastError)
        {
        case ERROR_DEVICE_NOT_CONNECTED:        // 1167
        case ERROR_NO_SUCH_DEVICE:              // 433
        case ERROR_DEVICE_REMOVED:              // 1617
        case ERROR_BAD_COMMAND:                 // 22
            return SendStatus::DeviceGone;

        case ERROR_ACCESS_DENIED:               // 5  —— 通常是官方 InfoHub 占用了接口
        case ERROR_INVALID_PARAMETER:           // 87 —— 报文长度/格式不对（协议问题）
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OPERATION_ABORTED:
        default:
            return SendStatus::WriteFailed;
        }
    }
}
