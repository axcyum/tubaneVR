#include "direct_mode.h"

#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>

#include "driver_log.h"

namespace tubane::driver {

namespace {

constexpr char kBlitShader[] = R"(
cbuffer Bounds : register(b0) { float4 bounds; };   // uMin, vMin, uMax, vMax
Texture2D    src     : register(t0);
SamplerState smp     : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VS(uint id : SV_VertexID) {
    // Fullscreen triangle; uv interpolates linearly so t = 1 maps to bounds.zw.
    float2 t = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv  = lerp(bounds.xy, bounds.zw, t);
    return o;
}

float4 PS(VSOut i) : SV_Target {
    return float4(src.Sample(smp, i.uv).rgb, 1.0);
}
)";

// Typed view format for a (possibly typeless) swap texture format.
DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default:                                return f;
    }
}

// Formats whose sampled values are linear light -> must be sRGB-encoded on write.
bool SamplesLinear(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return true;
        default:
            return false;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
bool DirectModeRenderer::Init(uint32_t adapterIndex, uint32_t width, uint32_t height) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        Log("direct mode: CreateDXGIFactory1 failed");
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(adapterIndex, &adapter))) {
        Log("direct mode: adapter %u not found, using adapter 0", adapterIndex);
        if (FAILED(factory->EnumAdapters1(0, &adapter))) return false;
    }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    adapterLuid_ = (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                   desc.AdapterLuid.LowPart;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr,
                                   &context_);
    if (FAILED(hr)) {
        Log("direct mode: D3D11CreateDevice failed (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    char name[128] = {};
    std::snprintf(name, sizeof(name), "%ls", desc.Description);
    Log("direct mode: adapter '%s' LUID 0x%016llX", name, static_cast<unsigned long long>(adapterLuid_));

    outWidth_  = width;
    outHeight_ = height;
    if (!CreateBlitPipeline() || !CreateOutputTexture()) {
        Shutdown();
        return false;
    }
    return true;
}

void DirectModeRenderer::Shutdown() {
    {
        std::lock_guard lock(mutex_);
        sets_.clear();
        syncTextures_.clear();
        haveLayer_ = false;
    }
    if (outputHandle_) {
        CloseHandle(outputHandle_);
        outputHandle_ = nullptr;
    }
    ipc_->WriteDriver([](ipc::DriverBlock& d) { d.frameTextureName[0] = L'\0'; });
    outputRtvUnorm_.Reset();
    outputRtvSrgb_.Reset();
    outputMutex_.Reset();
    output_.Reset();
    vs_.Reset();
    ps_.Reset();
    sampler_.Reset();
    boundsCb_.Reset();
    raster_.Reset();
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    context_.Reset();
    device_.Reset();
}

bool DirectModeRenderer::CreateBlitPipeline() {
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    if (FAILED(D3DCompile(kBlitShader, sizeof(kBlitShader) - 1, "tubane_blit.hlsl", nullptr, nullptr, "VS", "vs_5_0", 0, 0,
                          &vsCode, &errors)) ||
        FAILED(D3DCompile(kBlitShader, sizeof(kBlitShader) - 1, "tubane_blit.hlsl", nullptr, nullptr, "PS", "ps_5_0", 0, 0,
                          &psCode, &errors))) {
        Log("direct mode: shader compile failed: %s",
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
        return false;
    }
    device_->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &sampler_);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = 16;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&bd, nullptr, &boundsCb_);

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode        = D3D11_FILL_SOLID;
    rs.CullMode        = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&rs, &raster_);
    return vs_ && ps_ && sampler_ && boundsCb_ && raster_;
}

bool DirectModeRenderer::CreateOutputTexture() {
    D3D11_TEXTURE2D_DESC td{};
    td.Width      = outWidth_;
    td.Height     = outHeight_;
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc = {1, 0};
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags  = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &output_);
    if (FAILED(hr)) {
        Log("direct mode: output texture creation failed (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }
    output_.As(&outputMutex_);

    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rv.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
    device_->CreateRenderTargetView(output_.Get(), &rv, &outputRtvUnorm_);
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    device_->CreateRenderTargetView(output_.Get(), &rv, &outputRtvSrgb_);

    // Unique name per vrserver process / texture so a stale sandbox handle
    // can never alias a new texture.
    static uint32_t generation = 0;
    wchar_t name[64];
    std::swprintf(name, std::size(name), L"tubane_frame_%lu_%u", GetCurrentProcessId(), ++generation);
    outputName_ = name;

    ComPtr<IDXGIResource1> resource;
    output_.As(&resource);
    hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                      outputName_.c_str(), &outputHandle_);
    if (FAILED(hr)) {
        Log("direct mode: CreateSharedHandle failed (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    // Start black.
    if (outputMutex_->AcquireSync(0, 100) == S_OK) {
        const float black[4] = {0, 0, 0, 1};
        context_->ClearRenderTargetView(outputRtvUnorm_.Get(), black);
        outputMutex_->ReleaseSync(0);
        context_->Flush();
    }

    const uint64_t luid = adapterLuid_;
    ipc_->WriteDriver([&](ipc::DriverBlock& d) {
        d.displayMode = ipc::kDisplayDirect;
        d.adapterLuid = luid;
        d.frameIndex  = 0;
        wcsncpy_s(d.frameTextureName, outputName_.c_str(), _TRUNCATE);
    });
    Log("direct mode: shared frame texture '%ls' %ux%u", outputName_.c_str(), outWidth_, outHeight_);
    return true;
}

// ---------------------------------------------------------------------------
// Swap texture sets
// ---------------------------------------------------------------------------
void DirectModeRenderer::CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t* desc, SwapTextureSet_t* outSet) {
    *outSet = {};
    if (!device_) return;

    D3D11_TEXTURE2D_DESC td{};
    td.Width      = desc->nWidth;
    td.Height     = desc->nHeight;
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = static_cast<DXGI_FORMAT>(desc->nFormat);
    td.SampleDesc = {std::max<uint32_t>(desc->nSampleCount, 1u), 0};
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags  = D3D11_RESOURCE_MISC_SHARED;

    SwapSet set;
    set.pid = pid;
    for (int i = 0; i < 3; ++i) {
        SwapTexture& t = set.textures[i];
        HRESULT hr = device_->CreateTexture2D(&td, nullptr, &t.texture);
        if (FAILED(hr)) {
            Log("direct mode: swap texture %ux%u fmt %u failed (0x%08lX)", td.Width, td.Height, desc->nFormat,
                static_cast<unsigned long>(hr));
            *outSet = {};
            return;
        }
        ComPtr<IDXGIResource> resource;
        t.texture.As(&resource);
        resource->GetSharedHandle(&t.handle);
        outSet->rSharedTextureHandles[i] = reinterpret_cast<vr::SharedTextureHandle_t>(t.handle);
    }
    outSet->unTextureFlags = 0;  // legacy (non-NT) shared handles

    std::lock_guard lock(mutex_);
    sets_.push_back(std::move(set));
    Log("direct mode: swap set for pid %u: %ux%u fmt %u samples %u", pid, td.Width, td.Height, desc->nFormat,
        td.SampleDesc.Count);
}

void DirectModeRenderer::DestroySwapTextureSet(vr::SharedTextureHandle_t handle) {
    std::lock_guard lock(mutex_);
    for (auto it = sets_.begin(); it != sets_.end(); ++it) {
        for (const SwapTexture& t : it->textures) {
            if (reinterpret_cast<vr::SharedTextureHandle_t>(t.handle) == handle) {
                sets_.erase(it);
                return;
            }
        }
    }
}

void DirectModeRenderer::DestroyAllSwapTextureSets(uint32_t pid) {
    std::lock_guard lock(mutex_);
    std::erase_if(sets_, [pid](const SwapSet& s) { return s.pid == pid; });
}

DirectModeRenderer::SwapSet* DirectModeRenderer::FindSet(vr::SharedTextureHandle_t handle, int* textureIndex) {
    for (SwapSet& set : sets_) {
        for (int i = 0; i < 3; ++i) {
            if (reinterpret_cast<vr::SharedTextureHandle_t>(set.textures[i].handle) == handle) {
                if (textureIndex) *textureIndex = i;
                return &set;
            }
        }
    }
    return nullptr;
}

void DirectModeRenderer::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) {
    std::lock_guard lock(mutex_);
    for (int eye = 0; eye < 2; ++eye) {
        int current = 0;
        FindSet(handles[eye], &current);
        (*indices)[eye] = static_cast<uint32_t>((current + 1) % 3);
    }
}

// ---------------------------------------------------------------------------
// Frame submission
// ---------------------------------------------------------------------------
void DirectModeRenderer::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    std::lock_guard lock(mutex_);
    // Layer 0 is the scene (application or SteamVR environment); later layers
    // are overlays such as the dashboard, which the viewer does not composite.
    if (!haveLayer_) {
        layer_[0]  = perEye[0];
        layer_[1]  = perEye[1];
        haveLayer_ = true;
    }
}

ID3D11Texture2D* DirectModeRenderer::OpenSyncTexture(vr::SharedTextureHandle_t handle) {
    auto it = syncTextures_.find(handle);
    if (it != syncTextures_.end()) return it->second.Get();
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device_->OpenSharedResource(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&tex)))) {
        return nullptr;
    }
    return (syncTextures_[handle] = tex).Get();
}

void DirectModeRenderer::Present(vr::SharedTextureHandle_t syncTexture) {
    std::lock_guard lock(mutex_);
    if (!haveLayer_ || !device_) {
        haveLayer_ = false;
        return;
    }
    haveLayer_ = false;

    ipc::ControlBlock control{};
    const int eye = (ipc_->ReadControl(control) && control.config.viewerEye == ipc::ViewerEye::Right) ? 1 : 0;
    const SubmitLayerPerEye_t& layer = layer_[eye];

    int      index = 0;
    SwapSet* set   = FindSet(layer.hTexture, &index);
    if (!set) return;

    // Wait (briefly) until the compositor finished rendering this frame.
    ComPtr<IDXGIKeyedMutex> syncMutex;
    if (ID3D11Texture2D* sync = syncTexture ? OpenSyncTexture(syncTexture) : nullptr) {
        sync->QueryInterface(IID_PPV_ARGS(&syncMutex));
        if (syncMutex && syncMutex->AcquireSync(0, 10) != S_OK) {
            return;
        }
    }

    bool wrote = false;
    // Never block on the sandbox: if it is copying right now, drop this frame.
    if (outputMutex_->AcquireSync(0, 0) == S_OK) {
        BlitToOutput(set->textures[index], layer.bounds);
        outputMutex_->ReleaseSync(0);
        wrote = true;
    }
    if (syncMutex) syncMutex->ReleaseSync(0);
    context_->Flush();

    if (wrote) {
        const uint64_t frame = ++frameIndex_;
        ipc_->WriteDriver([frame](ipc::DriverBlock& d) { d.frameIndex = frame; });
    }
}

void DirectModeRenderer::BlitToOutput(SwapTexture& source, const vr::VRTextureBounds_t& bounds) {
    if (!source.srv) {
        D3D11_TEXTURE2D_DESC td{};
        source.texture->GetDesc(&td);
        if (td.SampleDesc.Count > 1) {
            if (loggedFailures_++ < 3) Log("direct mode: multisampled swap textures are not supported");
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format              = TypedFormat(td.Format);
        sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(source.texture.Get(), &sv, &source.srv))) {
            if (loggedFailures_++ < 3) Log("direct mode: SRV creation failed for format %u", td.Format);
            return;
        }
        source.srvFormat = sv.Format;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context_->Map(boundsCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float* b = static_cast<float*>(mapped.pData);
        b[0] = bounds.uMin;
        b[1] = bounds.vMin;
        b[2] = bounds.uMax;
        b[3] = bounds.vMax;
        context_->Unmap(boundsCb_.Get(), 0);
    }

    ID3D11RenderTargetView* rtv = SamplesLinear(source.srvFormat) ? outputRtvSrgb_.Get() : outputRtvUnorm_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(outWidth_), static_cast<float>(outHeight_), 0.0f, 1.0f};
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(raster_.Get());
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->OMSetDepthStencilState(nullptr, 0);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, boundsCb_.GetAddressOf());
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = source.srv.Get();
    context_->PSSetShaderResources(0, 1, &srv);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->Draw(3, 0);

    // Unbind so the compositor's next write to this texture has no hazard.
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context_->PSSetShaderResources(0, 1, &nullSrv);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
}

}  // namespace tubane::driver
