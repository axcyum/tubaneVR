#include "renderer.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace tubane::sandbox {

using namespace DirectX;

namespace {

constexpr UINT        kMsaaSamples = 4;
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// row_major: matrices are uploaded exactly as DirectXMath stores them.
constexpr char kShaderSource[] = R"(
cbuffer Frame  : register(b0) { row_major float4x4 viewProj; };
cbuffer Object : register(b1) { row_major float4x4 world; float4 color; };

struct LineIn  { float3 pos : POSITION; float4 col : COLOR; };
struct LineOut { float4 pos : SV_Position; float4 col : COLOR; };

LineOut LineVS(LineIn i) {
    LineOut o;
    o.pos = mul(float4(i.pos, 1.0), viewProj);
    o.col = i.col;
    return o;
}
float4 LinePS(LineOut i) : SV_Target { return i.col; }

struct MeshIn  { float3 pos : POSITION; float3 nrm : NORMAL; };
struct MeshOut { float4 pos : SV_Position; float3 nrm : NORMAL; };

MeshOut MeshVS(MeshIn i) {
    MeshOut o;
    float4 w = mul(float4(i.pos, 1.0), world);
    o.pos = mul(w, viewProj);
    o.nrm = mul(i.nrm, (float3x3)world);
    return o;
}
float4 MeshPS(MeshOut i) : SV_Target {
    float3 L = normalize(float3(0.35, 1.0, 0.55));
    float  d = saturate(dot(normalize(i.nrm), L)) * 0.65 + 0.35;
    return float4(color.rgb * d, color.a);
}
)";

struct FrameConstants {
    XMFLOAT4X4 viewProj;
};
struct ObjectConstants {
    XMFLOAT4X4 world;
    Color      color;
};

struct MeshVertex {
    XMFLOAT3 pos;
    XMFLOAT3 nrm;
};

ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "tubane_sandbox.hlsl", nullptr, nullptr,
                                  entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        }
        return nullptr;
    }
    return code;
}

}  // namespace

bool Renderer::Create(ID3D11Device* device) {
    device_ = device;

    auto lineVsCode = Compile("LineVS", "vs_5_0");
    auto linePsCode = Compile("LinePS", "ps_5_0");
    auto meshVsCode = Compile("MeshVS", "vs_5_0");
    auto meshPsCode = Compile("MeshPS", "ps_5_0");
    if (!lineVsCode || !linePsCode || !meshVsCode || !meshPsCode) {
        return false;
    }
    device->CreateVertexShader(lineVsCode->GetBufferPointer(), lineVsCode->GetBufferSize(), nullptr, &lineVs_);
    device->CreatePixelShader(linePsCode->GetBufferPointer(), linePsCode->GetBufferSize(), nullptr, &linePs_);
    device->CreateVertexShader(meshVsCode->GetBufferPointer(), meshVsCode->GetBufferSize(), nullptr, &meshVs_);
    device->CreatePixelShader(meshPsCode->GetBufferPointer(), meshPsCode->GetBufferSize(), nullptr, &meshPs_);

    const D3D11_INPUT_ELEMENT_DESC lineElems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(lineElems, 2, lineVsCode->GetBufferPointer(), lineVsCode->GetBufferSize(), &lineLayout_);

    const D3D11_INPUT_ELEMENT_DESC meshElems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(meshElems, 2, meshVsCode->GetBufferPointer(), meshVsCode->GetBufferSize(), &meshLayout_);

    D3D11_BUFFER_DESC cb{};
    cb.Usage          = D3D11_USAGE_DYNAMIC;
    cb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth      = sizeof(FrameConstants);
    device->CreateBuffer(&cb, nullptr, &frameCb_);
    cb.ByteWidth = sizeof(ObjectConstants);
    device->CreateBuffer(&cb, nullptr, &objectCb_);

    // Unit cube [-0.5, 0.5]^3 with per-face normals.
    MeshVertex verts[24];
    uint16_t   indices[36];
    const XMFLOAT3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int face = 0; face < 6; ++face) {
        const XMVECTOR n = XMLoadFloat3(&normals[face]);
        const XMVECTOR helper = std::fabs(normals[face].y) > 0.5f ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 1, 0, 0);
        const XMVECTOR t = XMVector3Cross(helper, n);
        const XMVECTOR b = XMVector3Cross(n, t);
        const float su[4] = {-1, 1, 1, -1};
        const float sv[4] = {-1, -1, 1, 1};
        for (int k = 0; k < 4; ++k) {
            XMVECTOR p = XMVectorScale(XMVectorAdd(XMVectorAdd(n, XMVectorScale(t, su[k])), XMVectorScale(b, sv[k])), 0.5f);
            XMStoreFloat3(&verts[face * 4 + k].pos, p);
            verts[face * 4 + k].nrm = normals[face];
        }
        const uint16_t base = static_cast<uint16_t>(face * 4);
        const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; ++k) indices[face * 6 + k] = static_cast<uint16_t>(base + quad[k]);
    }
    D3D11_BUFFER_DESC vb{};
    vb.Usage     = D3D11_USAGE_IMMUTABLE;
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb.ByteWidth = sizeof(verts);
    D3D11_SUBRESOURCE_DATA init{verts, 0, 0};
    device->CreateBuffer(&vb, &init, &cubeVb_);
    vb.BindFlags = D3D11_BIND_INDEX_BUFFER;
    vb.ByteWidth = sizeof(indices);
    init.pSysMem = indices;
    device->CreateBuffer(&vb, &init, &cubeIb_);

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode              = D3D11_FILL_SOLID;
    rs.CullMode              = D3D11_CULL_NONE;  // winding-agnostic; boxes are tiny
    rs.DepthClipEnable       = TRUE;
    rs.MultisampleEnable     = TRUE;
    rs.AntialiasedLineEnable = TRUE;
    device->CreateRasterizerState(&rs, &raster_);

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable    = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
    device->CreateDepthStencilState(&ds, &depthState_);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &blend_);

    return true;
}

void Renderer::Destroy() {
    *this = Renderer{};
}

void Renderer::EnsureTarget(uint32_t width, uint32_t height) {
    width  = width ? width : 1;
    height = height ? height : 1;
    if (width == width_ && height == height_ && resolvedSrv_) {
        return;
    }
    width_  = width;
    height_ = height;

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = width;
    td.Height           = height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = kColorFormat;
    td.SampleDesc       = {kMsaaSamples, 0};
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET;
    device_->CreateTexture2D(&td, nullptr, msaaColor_.ReleaseAndGetAddressOf());
    device_->CreateRenderTargetView(msaaColor_.Get(), nullptr, msaaRtv_.ReleaseAndGetAddressOf());

    td.Format    = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    device_->CreateTexture2D(&td, nullptr, msaaDepth_.ReleaseAndGetAddressOf());
    device_->CreateDepthStencilView(msaaDepth_.Get(), nullptr, msaaDsv_.ReleaseAndGetAddressOf());

    td.Format     = kColorFormat;
    td.SampleDesc = {1, 0};
    td.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
    device_->CreateTexture2D(&td, nullptr, resolved_.ReleaseAndGetAddressOf());
    device_->CreateShaderResourceView(resolved_.Get(), nullptr, resolvedSrv_.ReleaseAndGetAddressOf());
}

void Renderer::Begin(ID3D11DeviceContext* ctx, FXMMATRIX viewProj, const Color& clear) {
    ctx_ = ctx;
    lines_.clear();

    ID3D11RenderTargetView* rtv = msaaRtv_.Get();
    ctx->OMSetRenderTargets(1, &rtv, msaaDsv_.Get());
    const float clearColor[4] = {clear.r, clear.g, clear.b, clear.a};
    ctx->ClearRenderTargetView(rtv, clearColor);
    ctx->ClearDepthStencilView(msaaDsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(raster_.Get());
    ctx->OMSetDepthStencilState(depthState_.Get(), 0);
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(blend_.Get(), blendFactor, 0xFFFFFFFF);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(frameCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        XMStoreFloat4x4(&static_cast<FrameConstants*>(mapped.pData)->viewProj, viewProj);
        ctx->Unmap(frameCb_.Get(), 0);
    }
    ID3D11Buffer* cbs[2] = {frameCb_.Get(), objectCb_.Get()};
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetConstantBuffers(0, 2, cbs);
}

void Renderer::Line(const XMFLOAT3& a, const XMFLOAT3& b, const Color& c) {
    Line(a, b, c, c);
}

void Renderer::Line(const XMFLOAT3& a, const XMFLOAT3& b, const Color& ca, const Color& cb) {
    lines_.push_back({a, ca});
    lines_.push_back({b, cb});
}

void Renderer::Axes(FXMMATRIX world, float length, float alpha) {
    XMFLOAT3 o, x, y, z;
    XMStoreFloat3(&o, XMVector3TransformCoord(XMVectorZero(), world));
    XMStoreFloat3(&x, XMVector3TransformCoord(XMVectorSet(length, 0, 0, 1), world));
    XMStoreFloat3(&y, XMVector3TransformCoord(XMVectorSet(0, length, 0, 1), world));
    XMStoreFloat3(&z, XMVector3TransformCoord(XMVectorSet(0, 0, length, 1), world));
    Line(o, x, {0.95f, 0.25f, 0.25f, alpha});
    Line(o, y, {0.35f, 0.9f, 0.3f, alpha});
    Line(o, z, {0.3f, 0.5f, 1.0f, alpha});
}

void Renderer::Box(FXMMATRIX world, const XMFLOAT3& center, const XMFLOAT3& size, const Color& c) {
    const XMMATRIX m = XMMatrixScaling(size.x, size.y, size.z) * XMMatrixTranslation(center.x, center.y, center.z) * world;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(objectCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    auto* oc = static_cast<ObjectConstants*>(mapped.pData);
    XMStoreFloat4x4(&oc->world, m);
    oc->color = c;
    ctx_->Unmap(objectCb_.Get(), 0);

    const UINT stride = sizeof(MeshVertex), offset = 0;
    ID3D11Buffer* vb = cubeVb_.Get();
    ctx_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx_->IASetIndexBuffer(cubeIb_.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx_->IASetInputLayout(meshLayout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(meshVs_.Get(), nullptr, 0);
    ctx_->PSSetShader(meshPs_.Get(), nullptr, 0);
    ctx_->DrawIndexed(36, 0, 0);
}

void Renderer::FlushLines() {
    if (lines_.empty()) {
        return;
    }
    if (lines_.size() > lineVbCapacity_) {
        lineVbCapacity_ = lines_.size() * 2;
        D3D11_BUFFER_DESC bd{};
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth      = static_cast<UINT>(lineVbCapacity_ * sizeof(LineVertex));
        device_->CreateBuffer(&bd, nullptr, lineVb_.ReleaseAndGetAddressOf());
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(lineVb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, lines_.data(), lines_.size() * sizeof(LineVertex));
    ctx_->Unmap(lineVb_.Get(), 0);

    const UINT stride = sizeof(LineVertex), offset = 0;
    ID3D11Buffer* vb = lineVb_.Get();
    ctx_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx_->IASetInputLayout(lineLayout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx_->VSSetShader(lineVs_.Get(), nullptr, 0);
    ctx_->PSSetShader(linePs_.Get(), nullptr, 0);
    ctx_->Draw(static_cast<UINT>(lines_.size()), 0);
}

void Renderer::End() {
    FlushLines();
    ctx_->ResolveSubresource(resolved_.Get(), 0, msaaColor_.Get(), 0, kColorFormat);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    ctx_ = nullptr;
}

}  // namespace tubane::sandbox
