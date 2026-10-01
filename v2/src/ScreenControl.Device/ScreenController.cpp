#include "pch.h"
#include "ScreenController.h"
#include "FrameChunker.h"

#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace ScreenControl::Device
{
    namespace
    {
        constexpr std::size_t kImageWireBytes = kImageReportLength - 1; // 1024数据

        /// 补零 + 前置 0x00 报告 ID 之后的完整报文，用栈上复用缓冲避免每块分配。
        thread_local std::vector<std::uint8_t> t_reportBuffer;

        /// 读取某接口的 OutputReportByteLength；失败返回 0。
        std::uint16_t QueryOutputReportLength(std::wstring const& path) noexcept
        {
            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return 0;
            }
            std::uint16_t result = 0;
            HIDP_CAPS caps{};
            PHIDP_PREPARSED_DATA preparsed = nullptr;
            if (HidD_GetPreparsedData(h, &preparsed) && preparsed != nullptr)
            {
                if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS)
                {
                    result = caps.OutputReportByteLength;
                }
                HidD_FreePreparsedData(preparsed);
            }
            CloseHandle(h);
            return result;
        }

        /// 枚举 VID/PID 匹配的 HID 接口，并按 OutputReportByteLength 分类。
        HidInterfacePaths EnumerateInterfaces() noexcept
        {
            HidInterfacePaths paths;

            GUID hidGuid{};
            HidD_GetHidGuid(&hidGuid);

            HDEVINFO devices = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (devices == INVALID_HANDLE_VALUE)
            {
                return paths;
            }

            SP_DEVICE_INTERFACE_DATA interfaceData{};
            interfaceData.cbSize = sizeof(interfaceData);

            for (DWORD index = 0;
                 SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGuid, index, &interfaceData);
                 ++index)
            {
                DWORD required = 0;
                SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &required, nullptr);
                if (required == 0)
                {
                    continue;
                }

                std::vector<std::uint8_t> buffer(required);
                auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, required, nullptr, nullptr))
                {
                    continue;
                }

                std::wstring path = detail->DevicePath;
                std::wstring lower = path;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

                wchar_t vidToken[16]{};
                wchar_t pidToken[16]{};
                swprintf_s(vidToken, L"vid_%04x", kAsusVendorId);
                swprintf_s(pidToken, L"pid_%04x", kScreenProductId);
                if (lower.find(vidToken) == std::wstring::npos ||
                    lower.find(pidToken) == std::wstring::npos)
                {
                    continue;
                }

                const std::uint16_t outputLength = QueryOutputReportLength(path);
                if (outputLength == 440 || outputLength == 441)
                {
                    paths.controlPath = path;
                }
                else if (outputLength == 1024 || outputLength == 1025)
                {
                    paths.imagePath = path;
                }
            }

            SetupDiDestroyDeviceInfoList(devices);
            return paths;
        }

        HANDLE OpenInterface(std::wstring const& path) noexcept
        {
            if (path.empty())
            {
                return INVALID_HANDLE_VALUE;
            }
            return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        }
    }

    ScreenController::~ScreenController()
    {
        Close();
    }

    bool ScreenController::Open() noexcept
    {
        Close();

        const HidInterfacePaths paths = EnumerateInterfaces();
        if (paths.HasControl())
        {
            m_controlHandle = OpenInterface(paths.controlPath);
            if (m_controlHandle == INVALID_HANDLE_VALUE)
            {
                m_controlHandle = nullptr;
            }
        }
        if (paths.HasImage())
        {
            m_imageHandle = OpenInterface(paths.imagePath);
            if (m_imageHandle == INVALID_HANDLE_VALUE)
            {
                m_imageHandle = nullptr;
            }
        }

        {
            std::lock_guard lock(m_stateMutex);
            if (m_controlHandle != nullptr && m_imageHandle != nullptr)
            {
                m_state = DeviceState::Ready;
            }
            else if (m_imageHandle != nullptr)
            {
                m_state = DeviceState::ImageOnly;
            }
            else if (m_controlHandle != nullptr)
            {
                m_state = DeviceState::ControlOnly;
            }
            else
            {
                m_state = DeviceState::Disconnected;
            }
        }

        if (m_stateChanged)
        {
            m_stateChanged(m_state);
        }
        return m_controlHandle != nullptr || m_imageHandle != nullptr;
    }

    bool ScreenController::OpenImageOnly() noexcept
    {
        Close();
        const HidInterfacePaths paths = EnumerateInterfaces();
        m_imageHandle = OpenInterface(paths.imagePath);
        if (m_imageHandle == INVALID_HANDLE_VALUE)
        {
            m_imageHandle = nullptr;
        }

        {
            std::lock_guard lock(m_stateMutex);
            m_state = (m_imageHandle != nullptr) ? DeviceState::ImageOnly : DeviceState::Disconnected;
        }
        if (m_stateChanged)
        {
            m_stateChanged(m_state);
        }
        return m_imageHandle != nullptr;
    }

    void ScreenController::Close() noexcept
    {
        if (m_controlHandle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(m_controlHandle));
            m_controlHandle = nullptr;
        }
        if (m_imageHandle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(m_imageHandle));
            m_imageHandle = nullptr;
        }
        std::lock_guard lock(m_stateMutex);
        m_state = DeviceState::Disconnected;
    }

    DeviceState ScreenController::State() const noexcept
    {
        std::lock_guard lock(m_stateMutex);
        return m_state;
    }

    bool ScreenController::IsOpen() const noexcept
    {
        return m_controlHandle != nullptr || m_imageHandle != nullptr;
    }

    void ScreenController::SetStateChangedHandler(std::function<void(DeviceState)> handler)
    {
        m_stateChanged = std::move(handler);
    }

    bool ScreenController::WriteReport(void* handle, std::size_t reportBytes,
                                       std::span<const std::uint8_t> payload) noexcept
    {
        if (handle == nullptr)
        {
            return false;
        }

        // 报文布局：[0x00 报告ID][payload... 补零到 reportBytes-1]
        const std::size_t wireBytes = reportBytes - 1;
        t_reportBuffer.assign(reportBytes, 0);
        t_reportBuffer[0] = 0x00;
        const std::size_t copyBytes = payload.size() < wireBytes ? payload.size() : wireBytes;
        if (copyBytes > 0)
        {
            std::memcpy(t_reportBuffer.data() + 1, payload.data(), copyBytes);
        }

        DWORD written = 0;
        const BOOL ok = WriteFile(static_cast<HANDLE>(handle), t_reportBuffer.data(),
                                  static_cast<DWORD>(reportBytes), &written, nullptr);
        return ok != FALSE && written == reportBytes;
    }

    bool ScreenController::SendControl(std::uint32_t commandId, std::uint8_t value) noexcept
    {
        if (m_controlHandle == nullptr)
        {
            return false;
        }

        // 小端 4 字节命令 ID + 1 字节参数，其余由 WriteReport 补零。
        std::uint8_t command[5]{
            static_cast<std::uint8_t>(commandId & 0xFF),
            static_cast<std::uint8_t>((commandId >> 8) & 0xFF),
            static_cast<std::uint8_t>((commandId >> 16) & 0xFF),
            static_cast<std::uint8_t>((commandId >> 24) & 0xFF),
            value,
        };
        return WriteReport(m_controlHandle, kControlReportLength, command);
    }

    bool ScreenController::SetBrightness(std::uint8_t percent) noexcept
    {
        const std::uint8_t clamped = percent > 100 ? 100 : percent;
        return SendControl(kCmdBrightness, clamped);
    }

    bool ScreenController::SetScreen(bool on) noexcept
    {
        return SendControl(kCmdScreenOnOff, on ? 0x00 : 0x01);
    }

    SendStatus ScreenController::SendFrame(std::span<const std::uint8_t> jpeg,
                                           unsigned blockDelayUs) noexcept
    {
        if (m_imageHandle == nullptr)
        {
            return SendStatus::NoImageInterface;
        }
        const std::uint8_t total = FrameChunker::BlockCount(jpeg.size());
        if (total == 0)
        {
            return SendStatus::WriteFailed;
        }

        alignas(8) std::uint8_t report[kImageReportLength]{};

        for (std::uint8_t index = 0; index < total; ++index)
        {
            if (!FrameChunker::BuildBlock(report, jpeg.size(), index, jpeg))
            {
                return SendStatus::WriteFailed;
            }

            // 每块最多重试 3 次，间隔 5ms（与 v1 实测策略一致）。
            bool written = false;
            for (int attempt = 0; attempt < 3 && !written; ++attempt)
            {
                DWORD bytesWritten = 0;
                const BOOL ok = WriteFile(static_cast<HANDLE>(m_imageHandle), report,
                                          static_cast<DWORD>(kImageReportLength),
                                          &bytesWritten, nullptr);
                if (ok != FALSE && bytesWritten == kImageReportLength)
                {
                    written = true;
                    break;
                }
                const DWORD lastError = GetLastError();
                const SendStatus status = ClassifyWin32Error(lastError);
                if (status == SendStatus::DeviceGone)
                {
                    std::lock_guard lock(m_stateMutex);
                    m_state = DeviceState::Disconnected;
                    if (m_stateChanged)
                    {
                        m_stateChanged(m_state);
                    }
                    return SendStatus::DeviceGone;
                }
                Sleep(5);
            }

            if (!written)
            {
                return SendStatus::WriteFailed;
            }

            if (blockDelayUs > 0)
            {
                Sleep(blockDelayUs / 1000);
            }
        }
        return SendStatus::Ok;
    }
}
