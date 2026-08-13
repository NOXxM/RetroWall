#pragma once
// ---------------------------------------------------------------------------
// media/VideoPlayer.hpp  (Phase 3)
// Hardware-accelerated MP4 decode via Media Foundation's IMFSourceReader, wired
// to the render device through an IMFDXGIDeviceManager so decoded NV12 frames
// stay in VRAM (IMFDXGIBuffer -> ID3D11Texture2D). Loops by seeking the SAME
// reader to position 0 — the decoder is never recreated.
//
// Threading: construct and drive from one thread (the render thread), which
// must have called CoInitializeEx before creating this object.
// ---------------------------------------------------------------------------
#include <windows.h>

#include <d3d11.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <string>

namespace lwe::media {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

struct VideoFrame {
    ComPtr<ID3D11Texture2D> texture;  // decoder NV12 output (kept alive here)
    UINT subresource = 0;             // array slice within `texture`
    LONGLONG timeStamp = 0;           // presentation time, 100-ns units
    UINT width = 0;
    UINT height = 0;
    bool valid = false;               // a decoded frame is present
    bool looped = false;              // end-of-stream hit; reader was rewound
};

class VideoPlayer {
public:
    VideoPlayer(ID3D11Device* device, const std::wstring& path);
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // Decode the next frame. At end-of-stream it rewinds and returns a frame
    // with looped=true/valid=false; the caller re-anchors pacing and calls again.
    VideoFrame NextFrame();

    [[nodiscard]] UINT Width() const noexcept { return width_; }
    [[nodiscard]] UINT Height() const noexcept { return height_; }
    [[nodiscard]] double Fps() const noexcept { return fps_; }

private:
    void ConfigureReader(const std::wstring& path);
    void RefreshFormat();
    void SeekToStart();

    // Software-decode fallback: for formats/codecs that decode to SYSTEM memory
    // (no DXVA), upload the NV12 bytes into a texture so the renderer can still
    // sample them. Returns false (skip the frame) on any failure.
    bool UploadSoftwareFrame(IMFMediaBuffer* buffer, VideoFrame& out);
    void EnsureStagingTexture(UINT width, UINT height);

    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    ComPtr<IMFSourceReader> reader_;

    // Kept for the software-upload fallback.
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> staging_;  // CPU-writable NV12 upload texture
    UINT stagingW_ = 0;
    UINT stagingH_ = 0;

    UINT resetToken_ = 0;
    UINT width_ = 0;
    UINT height_ = 0;
    double fps_ = 0.0;
    bool mfStarted_ = false;
    bool pathLogged_ = false;  // log the first frame's route (hw/sw) once
};

}  // namespace lwe::media
