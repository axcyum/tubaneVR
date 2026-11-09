// Receives the HMD image from the driver's shared texture.
//
// The driver publishes the texture name in DriverBlock::frameTextureName and
// bumps frameIndex per frame. We open it by name, and whenever a new frame is
// available copy it (under the keyed mutex, non-blocking) into a local texture
// that ImGui can display at any time without holding the mutex.
#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

#include "tubane/ipc_layout.h"

namespace tubane::sandbox {

using Microsoft::WRL::ComPtr;

class HmdViewer {
public:
    void Update(ID3D11Device* device, ID3D11DeviceContext* ctx, const ipc::DriverBlock& driver, bool driverAlive,
                double nowSeconds);
    void Reset();

    ID3D11ShaderResourceView* Texture() const { return srv_.Get(); }
    uint32_t    Width() const { return width_; }
    uint32_t    Height() const { return height_; }
    bool        HasFrame() const { return srv_ && framesCopied_ > 0; }
    double      Fps() const { return fps_; }
    uint64_t    FramesCopied() const { return framesCopied_; }
    const char* Status() const { return status_.c_str(); }

private:
    bool Open(ID3D11Device* device, const wchar_t* name);

    std::wstring                     openedName_;
    ComPtr<ID3D11Texture2D>          shared_;
    ComPtr<IDXGIKeyedMutex>          mutex_;
    ComPtr<ID3D11Texture2D>          local_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    uint32_t width_ = 0, height_ = 0;

    uint64_t lastFrameIndex_ = 0;
    uint64_t framesCopied_   = 0;
    double   fpsWindowStart_ = 0.0;
    uint64_t fpsWindowFrames_ = 0;
    double   fps_ = 0.0;
    double   lastOpenAttempt_ = -10.0;
    std::string status_ = "Driver not running";
};

}  // namespace tubane::sandbox
