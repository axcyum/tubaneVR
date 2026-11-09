#include "hmd_viewer.h"

#include <cstdio>
#include <cwchar>

namespace tubane::sandbox {

void HmdViewer::Reset() {
    openedName_.clear();
    shared_.Reset();
    mutex_.Reset();
    local_.Reset();
    srv_.Reset();
    width_ = height_ = 0;
    lastFrameIndex_  = 0;
    framesCopied_    = 0;
    fps_             = 0.0;
}

bool HmdViewer::Open(ID3D11Device* device, const wchar_t* name) {
    Reset();
    ComPtr<ID3D11Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
        status_ = "ID3D11Device1 not available";
        return false;
    }
    HRESULT hr = device1->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                   IID_PPV_ARGS(&shared_));
    if (FAILED(hr)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Cannot open shared frame (0x%08lX) - different GPU?", static_cast<unsigned long>(hr));
        status_ = buf;
        shared_.Reset();
        return false;
    }
    shared_.As(&mutex_);

    D3D11_TEXTURE2D_DESC td{};
    shared_->GetDesc(&td);
    td.MiscFlags      = 0;
    td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.Usage          = D3D11_USAGE_DEFAULT;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &local_))) {
        status_ = "Cannot create local frame texture";
        Reset();
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    // The texture holds sRGB-encoded bytes; view them raw (UNORM) because the
    // swap chain is UNORM as well, so the bytes reach the screen unchanged.
    sv.Format              = (td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) ? DXGI_FORMAT_R8G8B8A8_UNORM : td.Format;
    sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(local_.Get(), &sv, &srv_);

    width_      = td.Width;
    height_     = td.Height;
    openedName_ = name;
    status_     = "Waiting for frames";
    return true;
}

void HmdViewer::Update(ID3D11Device* device, ID3D11DeviceContext* ctx, const ipc::DriverBlock& driver, bool driverAlive,
                       double now) {
    if (!driverAlive) {
        if (!openedName_.empty()) Reset();
        status_ = "Driver not running";
        return;
    }
    if (driver.displayMode != ipc::kDisplayDirect || driver.frameTextureName[0] == L'\0') {
        if (!openedName_.empty()) Reset();
        status_ = "Driver is in extended mode (directMode = false): no shared frame";
        return;
    }

    // (Re)open when the driver published a new texture.
    if (openedName_ != driver.frameTextureName) {
        if (now - lastOpenAttempt_ < 1.0) return;  // retry at most once per second
        lastOpenAttempt_ = now;
        if (!Open(device, driver.frameTextureName)) return;
    }

    if (driver.frameIndex != lastFrameIndex_ && mutex_) {
        // Non-blocking: if the driver is writing right now, try next frame.
        if (mutex_->AcquireSync(0, 0) == S_OK) {
            ctx->CopyResource(local_.Get(), shared_.Get());
            mutex_->ReleaseSync(0);
            lastFrameIndex_ = driver.frameIndex;
            ++framesCopied_;
            ++fpsWindowFrames_;
        }
    }

    if (now - fpsWindowStart_ >= 1.0) {
        fps_             = static_cast<double>(fpsWindowFrames_) / (now - fpsWindowStart_);
        fpsWindowStart_  = now;
        fpsWindowFrames_ = 0;
    }
    if (framesCopied_ > 0) status_ = "Receiving";
}

}  // namespace tubane::sandbox
