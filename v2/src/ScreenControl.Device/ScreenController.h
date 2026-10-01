// ============================================================================
//  ScreenController.h — 水冷屏设备门面
// ----------------------------------------------------------------------------
//  对应 v1: tuf_hid.TufCooler
//
//  职责边界（内聚设计，不按 v1 文件平铺）：
//    * 设备枚举与双接口分类      -> HidEnumerator
//    * 原始 WriteFile 与重试策略  -> 本类内部（唯一写出口）
//    * 报文构造                  -> HidProtocol / FrameChunker
//    * 对外只暴露：打开/关闭/亮度/开关/发一帧/状态
//
//  线程模型：所有方法都必须在同一工作线程调用（HID 句柄不做跨线程同步）。
//  本版为骨架实现，发帧与掉线重连策略后续补全。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>

#include "HidProtocol.h"

namespace ScreenControl::Device
{
    class ScreenController
    {
    public:
        ScreenController() = default;
        ~ScreenController();

        ScreenController(ScreenController const&) = delete;
        ScreenController& operator=(ScreenController const&) = delete;

        /// 枚举并打开设备。返回是否至少打开了一个接口。
        /// 枚举在调用线程同步执行，调用方负责放到后台线程。
        bool Open() noexcept;

        /// 只打开图像接口（用于"零控制命令"验证）。
        bool OpenImageOnly() noexcept;

        /// 关闭并释放句柄。
        void Close() noexcept;

        DeviceState State() const noexcept;
        bool IsOpen() const noexcept;

        /// 状态变化回调（掉线/重连）。回调在工作线程触发。
        void SetStateChangedHandler(std::function<void(DeviceState)> handler);

        // ---------------- 控制命令 ----------------
        /// 亮度 0-100（0 是最低亮度，不是黑屏）。
        bool SetBrightness(std::uint8_t percent) noexcept;

        /// 屏幕开关：true=开(0x00)，false=关(0x01)。
        bool SetScreen(bool on) noexcept;

        // ---------------- 图像帧 ----------------
        /// 发送一帧 JPEG（分块 + 逐块重试）。
        /// 调用方保证 jpeg 是 320x320、BGR 直接编码、无通道交换。
        SendStatus SendFrame(std::span<const std::uint8_t> jpeg, unsigned blockDelayUs = 0) noexcept;

    private:
        /// 唯一写出口：补零到 wire 长度、前置报告 ID、WriteFile 写满 caps 长度。
        bool WriteReport(void* handle, std::size_t reportBytes,
                         std::span<const std::uint8_t> payload) noexcept;

        bool SendControl(std::uint32_t commandId, std::uint8_t value) noexcept;

        void* m_controlHandle = nullptr;
        void* m_imageHandle = nullptr;
        DeviceState m_state = DeviceState::Disconnected;
        std::function<void(DeviceState)> m_stateChanged;
        mutable std::mutex m_stateMutex;
    };
}
