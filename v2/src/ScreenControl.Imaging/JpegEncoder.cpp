#include "pch.h"
#include "JpegEncoder.h"

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ScreenControl::Imaging
{
    namespace
    {
        /// 按线程缓存 WIC 工厂（WIC 工厂本身较轻，但避免每帧 CoCreateInstance）。
        IWICImagingFactory* GetFactory() noexcept
        {
            thread_local ComPtr<IWICImagingFactory> factory;
            if (!factory)
            {
                ComPtr<IWICImagingFactory> created;
                if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(created.GetAddressOf()))))
                {
                    return nullptr;
                }
                factory = created;
            }
            return factory.Get();
        }
    }

    bool JpegEncoder::Encode(std::uint8_t const* bgra,
                             int width,
                             int height,
                             int stride,
                             int quality,
                             std::vector<std::uint8_t>& outJpeg) noexcept
    {
        outJpeg.clear();
        if (bgra == nullptr || width <= 0 || height <= 0 || stride < width * 4)
        {
            return false;
        }

        IWICImagingFactory* factory = GetFactory();
        if (factory == nullptr)
        {
            return false;
        }

        // 1) 用内存创建源位图
        ComPtr<IWICBitmap> bitmap;
        const UINT bufferSize = static_cast<UINT>(stride) * static_cast<UINT>(height);
        if (FAILED(factory->CreateBitmapFromMemory(
                static_cast<UINT>(width), static_cast<UINT>(height),
                GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(stride), bufferSize,
                const_cast<BYTE*>(bgra), bitmap.GetAddressOf())))
        {
            return false;
        }

        // 2) 内存流
        ComPtr<IStream> stream;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf())))
        {
            return false;
        }

        // 3) 编码器
        ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr,
                                          encoder.GetAddressOf())))
        {
            return false;
        }
        if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        {
            return false;
        }

        // 4) 帧 + 画质
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), options.GetAddressOf())))
        {
            return false;
        }

        if (options)
        {
            PROPBAG2 option = {};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            const int clamped = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
            value.fltVal = static_cast<float>(clamped) / 100.0f;
            options->Write(1, &option, &value);
        }

        if (FAILED(frame->Initialize(options.Get())))
        {
            return false;
        }
        frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));

        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (FAILED(frame->SetPixelFormat(&format)))
        {
            return false;
        }

        // 5) 写入并提交
        if (FAILED(frame->WriteSource(bitmap.Get(), nullptr)))
        {
            return false;
        }
        if (FAILED(frame->Commit()))
        {
            return false;
        }
        if (FAILED(encoder->Commit()))
        {
            return false;
        }

        // 6) 取出字节
        HGLOBAL global = nullptr;
        if (FAILED(GetHGlobalFromStream(stream.Get(), &global)) || global == nullptr)
        {
            return false;
        }
        const SIZE_T size = GlobalSize(global);
        const void* data = GlobalLock(global);
        if (data == nullptr || size == 0)
        {
            if (data != nullptr)
            {
                GlobalUnlock(global);
            }
            return false;
        }

        outJpeg.resize(size);
        memcpy(outJpeg.data(), data, size);
        GlobalUnlock(global);
        return true;
    }
}
