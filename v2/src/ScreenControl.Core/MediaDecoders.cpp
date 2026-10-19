#include "pch.h"
#include "MediaDecoders.h"

#include "FrameChunker.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace ScreenControl::Core
{
    namespace
    {
        bool EndsWithIgnoreCase(std::wstring const& value, wchar_t const* suffix)
        {
            const size_t suffixLen = wcslen(suffix);
            if (value.size() < suffixLen)
            {
                return false;
            }
            return _wcsicmp(value.c_str() + (value.size() - suffixLen), suffix) == 0;
        }

        /// 保证当前线程已初始化 COM（工作线程是 MTA）。
        void EnsureCom() noexcept
        {
            static thread_local bool initialized = false;
            if (!initialized)
            {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                initialized = true;
            }
        }

        /// WIC 解码为 BGRA。
        bool DecodeWithWic(std::filesystem::path const& path, DecodedFrame& out) noexcept
        {
            EnsureCom();

            ComPtr<IWICImagingFactory> factory;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(factory.GetAddressOf()))))
            {
                return false;
            }

            ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand,
                                                          decoder.GetAddressOf())))
            {
                return false;
            }

            ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
            {
                return false;
            }

            ComPtr<IWICFormatConverter> converter;
            if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())))
            {
                return false;
            }
            if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                             WICBitmapDitherTypeNone, nullptr, 0.0,
                                             WICBitmapPaletteTypeCustom)))
            {
                return false;
            }

            UINT width = 0;
            UINT height = 0;
            if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
            {
                return false;
            }

            out.width = static_cast<int>(width);
            out.height = static_cast<int>(height);
            out.stride = out.width * 4;
            out.pixels.resize(static_cast<size_t>(out.stride) * out.height);

            return SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(out.stride),
                                                   static_cast<UINT>(out.pixels.size()),
                                                   out.pixels.data()));
        }
    }

    MediaKind DetectMediaKind(std::filesystem::path const& path)
    {
        const std::wstring ext = path.extension().wstring();
        if (EndsWithIgnoreCase(ext, L".jpg") || EndsWithIgnoreCase(ext, L".jpeg") ||
            EndsWithIgnoreCase(ext, L".png") || EndsWithIgnoreCase(ext, L".bmp") ||
            EndsWithIgnoreCase(ext, L".webp") || EndsWithIgnoreCase(ext, L".tif") ||
            EndsWithIgnoreCase(ext, L".tiff"))
        {
            return MediaKind::Photo;
        }
        return MediaKind::Video;
    }

    // ======================= PhotoDecoder =======================

    bool PhotoDecoder::Open(std::filesystem::path const& path)
    {
        Close();
        if (!DecodeWithWic(path, m_frame))
        {
            return false;
        }
        m_loaded = true;
        return true;
    }

    void PhotoDecoder::Close()
    {
        m_frame = {};
        m_loaded = false;
    }

    bool PhotoDecoder::ReadFrame(DecodedFrame& frame)
    {
        if (!m_loaded)
        {
            return false;
        }
        frame = m_frame;
        return true;
    }

    // ======================= VideoDecoder =======================

    VideoDecoder::~VideoDecoder()
    {
        Close();
    }

    bool VideoDecoder::Open(std::filesystem::path const& path)
    {
        Close();
        EnsureCom();

        if (!m_mfStarted)
        {
            if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)))
            {
                return false;
            }
            m_mfStarted = true;
        }

        // 关键：SourceReader 默认不加载「视频处理器 MFT」，因此不能直接输出 RGB32，
        // SetCurrentMediaType 会返回 MF_E_INVALIDMEDIATYPE (0xC00D36B4)。
        // 打开高级视频处理后，SourceReader 会自动插入视频处理器，
        // 从而支持色彩空间转换，并且可以顺带把画面缩放到我们需要的尺寸。
        ComPtr<IMFAttributes> readerAttributes;
        MFCreateAttributes(readerAttributes.GetAddressOf(), 2);
        if (readerAttributes)
        {
            readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            readerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }

        IMFSourceReader* reader = nullptr;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), readerAttributes.Get(), &reader)) ||
            reader == nullptr)
        {
            return false;
        }
        m_reader = reader;

        // 只启用视频流
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

        // 要求解码成 RGB32（等价 BGRA）。
        // 优先请求直接缩放到 320x320：由视频处理器做缩放比我们自己逐像素采样快得多。
        ComPtr<IMFMediaType> requested;
        if (FAILED(MFCreateMediaType(requested.GetAddressOf())))
        {
            Close();
            return false;
        }
        requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        MFSetAttributeSize(requested.Get(), MF_MT_FRAME_SIZE,
                           static_cast<UINT32>(::ScreenControl::Device::kScreenSize), static_cast<UINT32>(::ScreenControl::Device::kScreenSize));

        HRESULT hrSet = reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, requested.Get());

        if (FAILED(hrSet))
        {
            // 某些解码器不接受指定尺寸，退回「只要求 RGB32、保持原始尺寸」。
            ComPtr<IMFMediaType> plainRgb;
            if (FAILED(MFCreateMediaType(plainRgb.GetAddressOf())))
            {
                Close();
                return false;
            }
            plainRgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            plainRgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            hrSet = reader->SetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, plainRgb.Get());
        }

        if (FAILED(hrSet))
        {
            Close();
            return false;
        }

        ComPtr<IMFMediaType> actual;
        if (FAILED(reader->GetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                actual.GetAddressOf())))
        {
            Close();
            return false;
        }
        m_outputType = actual.Detach();

        UINT32 width = 0;
        UINT32 height = 0;
        if (FAILED(MFGetAttributeSize(static_cast<IMFMediaType*>(m_outputType),
                                      MF_MT_FRAME_SIZE, &width, &height)) ||
            width == 0 || height == 0)
        {
            Close();
            return false;
        }
        m_width = static_cast<int>(width);
        m_height = static_cast<int>(height);

        // 帧率
        UINT32 fpsNum = 0;
        UINT32 fpsDen = 1;
        if (SUCCEEDED(MFGetAttributeRatio(static_cast<IMFMediaType*>(m_outputType),
                                          MF_MT_FRAME_RATE, &fpsNum, &fpsDen)) &&
            fpsDen != 0 && fpsNum != 0)
        {
            m_fps = static_cast<double>(fpsNum) / static_cast<double>(fpsDen);
        }
        if (m_fps <= 1.0 || m_fps > 240.0)
        {
            m_fps = 30.0;
        }

        // RGB32 输出通常是自底向上（stride 为负），这里记录下来
        UINT32 defaultStride = 0;
        if (SUCCEEDED(static_cast<IMFMediaType*>(m_outputType)
                          ->GetUINT32(MF_MT_DEFAULT_STRIDE, &defaultStride)))
        {
            const LONG signedStride = static_cast<LONG>(defaultStride);
            m_bottomUp = signedStride < 0;
            m_stride = m_bottomUp ? -signedStride : signedStride;
        }
        else
        {
            m_bottomUp = true;
            m_stride = m_width * 4;
        }
        if (m_stride < m_width * 4)
        {
            m_stride = m_width * 4;
        }

        return true;
    }

    void VideoDecoder::Close()
    {
        if (m_outputType != nullptr)
        {
            static_cast<IMFMediaType*>(m_outputType)->Release();
            m_outputType = nullptr;
        }
        if (m_reader != nullptr)
        {
            static_cast<IMFSourceReader*>(m_reader)->Release();
            m_reader = nullptr;
        }
        if (m_mfStarted)
        {
            MFShutdown();
            m_mfStarted = false;
        }
        m_flipBuffer.clear();
        m_width = 0;
        m_height = 0;
        m_stride = 0;
    }

    bool VideoDecoder::SeekToStart()
    {
        if (m_reader == nullptr)
        {
            return false;
        }
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = 0;
        const HRESULT hr = static_cast<IMFSourceReader*>(m_reader)->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
        return SUCCEEDED(hr);
    }

    bool VideoDecoder::ReadFrame(DecodedFrame& frame)
    {
        if (m_reader == nullptr)
        {
            return false;
        }

        IMFSourceReader* reader = static_cast<IMFSourceReader*>(m_reader);

        for (int attempt = 0; attempt < 2; ++attempt)
        {
            DWORD actualStreamIndex = 0;
            DWORD streamFlags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;

            const HRESULT hr = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                &actualStreamIndex, &streamFlags, &timestamp, &sample);

            if (FAILED(hr))
            {
                if (sample) { sample->Release(); }
                return false;
            }

            if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
            {
                if (sample) { sample->Release(); }
                // 循环播放：回到开头再读一次
                if (!SeekToStart())
                {
                    return false;
                }
                continue;
            }

            if (sample == nullptr)
            {
                // 该次没有数据（例如流间隙），再试一次
                continue;
            }

            ComPtr<IMFMediaBuffer> buffer;
            HRESULT bufferResult = sample->ConvertToContiguousBuffer(buffer.GetAddressOf());
            sample->Release();
            if (FAILED(bufferResult))
            {
                return false;
            }

            BYTE* raw = nullptr;
            DWORD maxLength = 0;
            DWORD currentLength = 0;
            if (FAILED(buffer->Lock(&raw, &maxLength, &currentLength)))
            {
                return false;
            }

            const size_t rowBytes = static_cast<size_t>(m_width) * 4;
            frame.width = m_width;
            frame.height = m_height;
            frame.stride = static_cast<int>(rowBytes);
            frame.pixels.resize(rowBytes * static_cast<size_t>(m_height));

            if (m_bottomUp)
            {
                // 翻正：源第 0 行是画面最底行
                for (int y = 0; y < m_height; ++y)
                {
                    const size_t srcOffset = static_cast<size_t>(m_stride) * static_cast<size_t>(m_height - 1 - y);
                    if (srcOffset + rowBytes > currentLength)
                    {
                        break;
                    }
                    memcpy(frame.pixels.data() + rowBytes * static_cast<size_t>(y),
                           raw + srcOffset, rowBytes);
                }
            }
            else
            {
                for (int y = 0; y < m_height; ++y)
                {
                    const size_t srcOffset = static_cast<size_t>(m_stride) * static_cast<size_t>(y);
                    if (srcOffset + rowBytes > currentLength)
                    {
                        break;
                    }
                    memcpy(frame.pixels.data() + rowBytes * static_cast<size_t>(y),
                           raw + srcOffset, rowBytes);
                }
            }

            buffer->Unlock();
            return true;
        }

        return false;
    }
}
