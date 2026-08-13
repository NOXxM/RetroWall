#include "media/VideoPlayer.hpp"

#include <mfapi.h>
#include <mferror.h>

#include <cstring>
#include <stdexcept>

#include "win32/Error.hpp"
#include "win32/Log.hpp"

namespace lwe::media {

using win32::ThrowIfFailed;

VideoPlayer::VideoPlayer(ID3D11Device* device, const std::wstring& path) {
    device_ = device;
    if (device_) {
        device_->GetImmediateContext(context_.GetAddressOf());
    }

    ThrowIfFailed(::MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
    mfStarted_ = true;

    // If any step below throws, the object is only partially constructed and the
    // destructor won't run — so pair MFStartup with MFShutdown here explicitly.
    try {
        // DXGI device manager: the bridge that lets MF's decoder allocate and
        // write frames on OUR D3D11 device (VRAM), not in system memory.
        ThrowIfFailed(::MFCreateDXGIDeviceManager(&resetToken_,
                                                  deviceManager_.GetAddressOf()),
                      "MFCreateDXGIDeviceManager");
        ThrowIfFailed(deviceManager_->ResetDevice(device, resetToken_),
                      "IMFDXGIDeviceManager::ResetDevice");

        ConfigureReader(path);
        RefreshFormat();
        log::Writef(L"Video opened: %ux%u @ %.2f fps", width_, height_, fps_);
    } catch (...) {
        reader_.Reset();
        deviceManager_.Reset();
        ::MFShutdown();
        mfStarted_ = false;
        throw;
    }
}

VideoPlayer::~VideoPlayer() {
    reader_.Reset();
    deviceManager_.Reset();
    if (mfStarted_) {
        ::MFShutdown();
    }
}

void VideoPlayer::ConfigureReader(const std::wstring& path) {
    ComPtr<IMFAttributes> attributes;
    ThrowIfFailed(::MFCreateAttributes(attributes.GetAddressOf(), 3),
                  "MFCreateAttributes");

    // Hand MF the device manager and allow hardware transforms (the decoder).
    ThrowIfFailed(attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,
                                         deviceManager_.Get()),
                  "SetUnknown(D3D_MANAGER)");
    ThrowIfFailed(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                                        TRUE),
                  "SetUINT32(ENABLE_HARDWARE_TRANSFORMS)");
    // Keep DXVA enabled (do NOT disable it): FALSE == allow DXVA video decode.
    ThrowIfFailed(attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE),
                  "SetUINT32(DISABLE_DXVA=FALSE)");

    // Opening covers container + codec resolution. A failure here usually means
    // Windows has no decoder/source for this format — point the user at the free
    // extensions rather than a bare HRESULT.
    if (FAILED(::MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(),
                                             reader_.GetAddressOf()))) {
        throw std::runtime_error(
            "Couldn't open this video. Windows may lack a decoder for its "
            "container or codec.\n\nInstall the free 'Web Media Extensions', "
            "'HEVC Video Extensions', and 'AV1 Video Extension' from the "
            "Microsoft Store (they add VP9 / WebM / HEVC / AV1), or install LAV "
            "Filters for MKV and other containers, then try again.");
    }

    // Only the first video stream.
    ThrowIfFailed(reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE),
                  "SetStreamSelection(ALL=FALSE)");
    ThrowIfFailed(reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                              TRUE),
                  "SetStreamSelection(FIRST_VIDEO=TRUE)");

    // Force NV12 output. Hardware decoders emit NV12 natively, so this keeps the
    // decode fully on the GPU with no format-conversion MFT inserted.
    ComPtr<IMFMediaType> nv12Type;
    ThrowIfFailed(::MFCreateMediaType(nv12Type.GetAddressOf()), "MFCreateMediaType");
    ThrowIfFailed(nv12Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video),
                  "SetGUID(MAJOR_TYPE=Video)");
    ThrowIfFailed(nv12Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12),
                  "SetGUID(SUBTYPE=NV12)");
    if (FAILED(reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                            nullptr, nv12Type.Get()))) {
        throw std::runtime_error(
            "This video decoded, but its decoder can't output NV12 on this "
            "system. Try a standard H.264/HEVC .mp4, or install the matching "
            "Microsoft Store video extension.");
    }
}

void VideoPlayer::RefreshFormat() {
    ComPtr<IMFMediaType> current;
    ThrowIfFailed(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               current.GetAddressOf()),
                  "GetCurrentMediaType");

    UINT32 w = 0, h = 0;
    ThrowIfFailed(::MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h),
                  "MFGetAttributeSize(FRAME_SIZE)");
    width_ = w;
    height_ = h;

    UINT32 num = 0, den = 0;
    if (SUCCEEDED(::MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &num,
                                        &den)) &&
        den != 0) {
        fps_ = static_cast<double>(num) / static_cast<double>(den);
    }
}

void VideoPlayer::SeekToStart() {
    PROPVARIANT var;
    ::PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = 0;  // 100-ns units, GUID_NULL == presentation time
    const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, var);
    ::PropVariantClear(&var);
    ThrowIfFailed(hr, "SetCurrentPosition(0)");
}

VideoFrame VideoPlayer::NextFrame() {
    VideoFrame frame;

    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG timeStamp = 0;
    ComPtr<IMFSample> sample;
    const HRESULT hr = reader_->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &timeStamp,
        sample.GetAddressOf());
    // A hard failure (device removed, decode error) is terminal: throw so the
    // engine tears down cleanly instead of spinning on a failing ReadSample.
    // Transient no-sample cases (STREAMTICK) succeed with a null sample and are
    // handled below; the synchronous reader blocks between real frames, so that
    // path does not busy-loop.
    ThrowIfFailed(hr, "IMFSourceReader::ReadSample");

    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        RefreshFormat();
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        SeekToStart();          // rewind the SAME reader — no re-creation
        frame.looped = true;
        return frame;           // valid==false; caller re-anchors and re-reads
    }
    if (!sample) {
        return frame;  // stream tick / gap with no sample
    }

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, buffer.GetAddressOf()))) {
        return frame;
    }

    // Preferred path: the sample is DXGI-backed (hardware decode) — hand the
    // texture straight over with NO CPU copy (zero host-RAM).
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (SUCCEEDED(buffer.As(&dxgiBuffer))) {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(
                dxgiBuffer->GetResource(IID_PPV_ARGS(texture.GetAddressOf())))) {
            UINT subresource = 0;
            dxgiBuffer->GetSubresourceIndex(&subresource);
            frame.texture = texture;
            frame.subresource = subresource;
            frame.timeStamp = timeStamp;
            frame.width = width_;
            frame.height = height_;
            frame.valid = true;
            if (!pathLogged_) {
                log::Write(L"Frame route: hardware (DXGI, zero-copy)");
                pathLogged_ = true;
            }
            return frame;
        }
    }

    // Fallback path: the frame decoded to SYSTEM memory (a software decoder /
    // no DXVA for this codec). Upload the NV12 bytes to a texture so it still
    // displays. This costs one CPU->GPU copy per frame — the zero-copy guarantee
    // is a hardware-path property — but it makes far more formats play.
    if (UploadSoftwareFrame(buffer.Get(), frame)) {
        frame.timeStamp = timeStamp;
        frame.valid = true;
        if (!pathLogged_) {
            log::Write(L"Frame route: software (system-memory upload)");
            pathLogged_ = true;
        }
    } else if (!pathLogged_) {
        log::Write(L"Frame route: FAILED (not DXGI and software upload failed)");
        pathLogged_ = true;
    }
    return frame;
}

void VideoPlayer::EnsureStagingTexture(UINT width, UINT height) {
    if (staging_ && width == stagingW_ && height == stagingH_) {
        return;
    }
    staging_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;               // CPU-writable, copy source
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.BindFlags = 0;
    if (SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, staging_.GetAddressOf()))) {
        stagingW_ = width;
        stagingH_ = height;
    }
}

bool VideoPlayer::UploadSoftwareFrame(IMFMediaBuffer* buffer, VideoFrame& out) {
    if (device_ == nullptr || context_ == nullptr || width_ == 0 || height_ == 0) {
        return false;
    }
    EnsureStagingTexture(width_, height_);
    if (!staging_) {
        return false;
    }

    // Locate the NV12 source planes + row stride. IMF2DBuffer gives the true
    // stride; otherwise fall back to a contiguous buffer (stride == width).
    BYTE* src = nullptr;
    LONG srcStride = 0;
    ComPtr<IMF2DBuffer> buffer2d;
    bool locked2d = false;
    bool locked = false;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(buffer2d.GetAddressOf()))) &&
        SUCCEEDED(buffer2d->Lock2D(&src, &srcStride))) {
        locked2d = true;
    } else {
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buffer->Lock(&src, &maxLen, &curLen))) {
            return false;
        }
        srcStride = static_cast<LONG>(width_);  // contiguous NV12
        locked = true;
    }

    bool ok = false;
    if (src != nullptr && srcStride > 0) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(staging_.Get(), 0, D3D11_MAP_WRITE, 0,
                                    &mapped))) {
            auto* dst = static_cast<BYTE*>(mapped.pData);
            const UINT dstPitch = mapped.RowPitch;
            const UINT w = width_;
            const UINT h = height_;
            const auto sp = static_cast<size_t>(srcStride);

            // Y plane: h rows of w bytes.
            for (UINT y = 0; y < h; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * dstPitch,
                            src + static_cast<size_t>(y) * sp, w);
            }
            // Interleaved CbCr plane: h/2 rows of w bytes. In both the NV12
            // source buffer and the mapped NV12 texture the chroma plane starts
            // one Y-plane-height below the base.
            BYTE* dstUV = dst + static_cast<size_t>(dstPitch) * h;
            const BYTE* srcUV = src + sp * h;
            for (UINT y = 0; y < h / 2; ++y) {
                std::memcpy(dstUV + static_cast<size_t>(y) * dstPitch,
                            srcUV + static_cast<size_t>(y) * sp, w);
            }
            context_->Unmap(staging_.Get(), 0);
            ok = true;
        }
    }

    if (locked2d) {
        buffer2d->Unlock2D();
    } else if (locked) {
        buffer->Unlock();
    }

    if (!ok) {
        return false;
    }
    out.texture = staging_;
    out.subresource = 0;
    out.width = width_;
    out.height = height_;
    return true;
}

}  // namespace lwe::media
