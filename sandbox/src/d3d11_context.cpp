#include "d3d11_context.h"

#include <cwchar>
#include <iterator>

namespace tubane::sandbox {

bool D3D11Context::Create(HWND hwnd) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr,
                                   &context_);
#if defined(_DEBUG)
    if (FAILED(hr)) {  // debug layer not installed
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr, &context_);
    }
#endif
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IDXGIDevice>   dxgiDevice;
    ComPtr<IDXGIAdapter>  adapter;
    ComPtr<IDXGIFactory2> factory;
    device_.As(&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_ADAPTER_DESC desc{};
    adapter->GetDesc(&desc);
    adapterLuid_ = (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                   desc.AdapterLuid.LowPart;
    wcsncpy_s(adapterName_, desc.Description, _TRUNCATE);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    width_  = static_cast<uint32_t>(rc.right - rc.left);
    height_ = static_cast<uint32_t>(rc.bottom - rc.top);

    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width       = width_;
    sc.Height      = height_;
    sc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc  = {1, 0};
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.Scaling     = DXGI_SCALING_STRETCH;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sc, nullptr, nullptr, &swapChain_))) {
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    CreateBackBufferView();
    return true;
}

void D3D11Context::Destroy() {
    backBufferRtv_.Reset();
    swapChain_.Reset();
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    context_.Reset();
    device_.Reset();
}

void D3D11Context::CreateBackBufferView() {
    ComPtr<ID3D11Texture2D> backBuffer;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferRtv_);
}

void D3D11Context::Resize(uint32_t width, uint32_t height) {
    if (!swapChain_ || width == 0 || height == 0 || (width == width_ && height == height_)) {
        return;
    }
    width_  = width;
    height_ = height;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    backBufferRtv_.Reset();
    swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateBackBufferView();
}

void D3D11Context::BeginFrame(const float clearColor[4]) {
    ID3D11RenderTargetView* rtv = backBufferRtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->ClearRenderTargetView(rtv, clearColor);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    context_->RSSetViewports(1, &vp);
}

void D3D11Context::Present(bool vsync) {
    swapChain_->Present(vsync ? 1 : 0, 0);
}

}  // namespace tubane::sandbox
