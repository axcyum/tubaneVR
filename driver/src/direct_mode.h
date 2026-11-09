// IVRDriverDirectModeComponent implementation.
//
// The SteamVR compositor renders each eye into swap textures allocated here
// (CreateSwapTextureSet) and hands them back per frame (SubmitLayer/Present).
// On Present the eye selected by the sandbox (ControlConfig::viewerEye) is
// blitted into a named, keyed-mutex shared texture that tubane-sandbox opens
// with ID3D11Device1::OpenSharedResourceByName.
//
//   compositor ──SubmitLayer──▶ swap texture (per eye)
//                                   │ Present: blit selected eye (shader, scales + converts)
//                                   ▼
//                    "Local\tubane_frame_<pid>_<n>"  R8G8B8A8, NT handle + keyed mutex
//                                   │
//                                   ▼
//                            tubane-sandbox HMD Viewer
#pragma once

#include <d3d11_1.h>
#include <openvr_driver.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "driver_ipc.h"

namespace tubane::driver {

using Microsoft::WRL::ComPtr;

class DirectModeRenderer final : public vr::IVRDriverDirectModeComponent {
public:
    explicit DirectModeRenderer(DriverIpc* ipc) : ipc_(ipc) {}

    // Creates the D3D11 device on adapter `adapterIndex` (DXGI enumeration
    // order) and the shared output texture (`width` x `height`).
    bool Init(uint32_t adapterIndex, uint32_t width, uint32_t height);
    void Shutdown();

    uint64_t AdapterLuid() const { return adapterLuid_; }

    // --- vr::IVRDriverDirectModeComponent ---------------------------------
    void CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t* desc, SwapTextureSet_t* outSet) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t handle) override;
    void DestroyAllSwapTextureSets(uint32_t pid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;

private:
    struct SwapTexture {
        ComPtr<ID3D11Texture2D>          texture;
        ComPtr<ID3D11ShaderResourceView> srv;  // created lazily
        DXGI_FORMAT                      srvFormat = DXGI_FORMAT_UNKNOWN;
        HANDLE                           handle = nullptr;
    };
    struct SwapSet {
        uint32_t    pid = 0;
        SwapTexture textures[3];
        uint32_t    nextIndex = 0;
    };

    bool CreateBlitPipeline();
    bool CreateOutputTexture();
    SwapSet*     FindSet(vr::SharedTextureHandle_t handle, int* textureIndex = nullptr);
    void         BlitToOutput(SwapTexture& source, const vr::VRTextureBounds_t& bounds);
    ID3D11Texture2D* OpenSyncTexture(vr::SharedTextureHandle_t handle);

    DriverIpc* ipc_;

    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> context_;
    uint64_t                    adapterLuid_ = 0;

    // Blit pipeline
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader>  ps_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer>       boundsCb_;
    ComPtr<ID3D11RasterizerState> raster_;

    // Shared output
    uint32_t                       outWidth_ = 0, outHeight_ = 0;
    ComPtr<ID3D11Texture2D>        output_;
    ComPtr<IDXGIKeyedMutex>        outputMutex_;
    ComPtr<ID3D11RenderTargetView> outputRtvUnorm_;
    ComPtr<ID3D11RenderTargetView> outputRtvSrgb_;
    HANDLE                         outputHandle_ = nullptr;
    std::wstring                   outputName_;

    std::mutex                                                  mutex_;  // guards sets_ / layers
    std::vector<SwapSet>                                        sets_;
    std::map<vr::SharedTextureHandle_t, ComPtr<ID3D11Texture2D>> syncTextures_;

    bool                 haveLayer_ = false;
    SubmitLayerPerEye_t  layer_[2]{};
    uint64_t             frameIndex_ = 0;
    uint32_t             loggedFailures_ = 0;
};

}  // namespace tubane::driver
