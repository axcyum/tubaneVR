// D3D11 device + flip-model swap chain for the sandbox main window.
#pragma once

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>

namespace tubane::sandbox {

using Microsoft::WRL::ComPtr;

class D3D11Context {
public:
    bool Create(HWND hwnd);
    void Destroy();

    // Call when the window client area changed size (0 = minimized, ignored).
    void Resize(uint32_t width, uint32_t height);

    void BeginFrame(const float clearColor[4]);
    void Present(bool vsync);

    ID3D11Device*        Device() const  { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }

    // LUID of the adapter the device was created on (compared with the
    // compositor's adapter for the Step 3 shared texture).
    uint64_t AdapterLuid() const { return adapterLuid_; }
    const wchar_t* AdapterName() const { return adapterName_; }

private:
    void CreateBackBufferView();

    ComPtr<ID3D11Device>           device_;
    ComPtr<ID3D11DeviceContext>    context_;
    ComPtr<IDXGISwapChain1>        swapChain_;
    ComPtr<ID3D11RenderTargetView> backBufferRtv_;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    uint64_t adapterLuid_ = 0;
    wchar_t  adapterName_[128] = {};
};

}  // namespace tubane::sandbox
