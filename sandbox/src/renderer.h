// Minimal 3D renderer for the sandbox viewport.
//
// Renders into an off-screen 4x MSAA target that is resolved into a texture
// shown by an ImGui::Image in the "Viewport" window. Two primitives:
//   * colored line lists (grid, room box, axes, frustums) — batched per frame
//   * flat-shaded boxes (device models)
#pragma once

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace tubane::sandbox {

using Microsoft::WRL::ComPtr;

struct Color {
    float r, g, b, a;
};

class Renderer {
public:
    bool Create(ID3D11Device* device);
    void Destroy();

    // (Re)creates the off-screen target if the size changed.
    void EnsureTarget(uint32_t width, uint32_t height);

    void Begin(ID3D11DeviceContext* ctx, DirectX::FXMMATRIX viewProj, const Color& clear);
    void End();  // flushes lines and resolves MSAA

    // Lines are buffered and drawn in End().
    void Line(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, const Color& c);
    void Line(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, const Color& ca, const Color& cb);
    void Axes(DirectX::FXMMATRIX world, float length, float alpha = 1.0f);

    // Box of `size` centered at `center` in the space of `world`. Drawn immediately.
    void Box(DirectX::FXMMATRIX world, const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& size, const Color& c);

    ID3D11ShaderResourceView* Texture() const { return resolvedSrv_.Get(); }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

private:
    struct LineVertex {
        DirectX::XMFLOAT3 pos;
        Color             color;
    };

    void FlushLines();

    ID3D11Device*        device_ = nullptr;
    ID3D11DeviceContext* ctx_    = nullptr;

    // Off-screen target
    uint32_t width_ = 0, height_ = 0;
    ComPtr<ID3D11Texture2D>          msaaColor_;
    ComPtr<ID3D11RenderTargetView>   msaaRtv_;
    ComPtr<ID3D11Texture2D>          msaaDepth_;
    ComPtr<ID3D11DepthStencilView>   msaaDsv_;
    ComPtr<ID3D11Texture2D>          resolved_;
    ComPtr<ID3D11ShaderResourceView> resolvedSrv_;

    // Pipeline
    ComPtr<ID3D11VertexShader> lineVs_, meshVs_;
    ComPtr<ID3D11PixelShader>  linePs_, meshPs_;
    ComPtr<ID3D11InputLayout>  lineLayout_, meshLayout_;
    ComPtr<ID3D11Buffer>       frameCb_, objectCb_;
    ComPtr<ID3D11Buffer>       cubeVb_, cubeIb_;
    ComPtr<ID3D11Buffer>       lineVb_;
    size_t                     lineVbCapacity_ = 0;
    ComPtr<ID3D11RasterizerState>   raster_;
    ComPtr<ID3D11DepthStencilState> depthState_;
    ComPtr<ID3D11BlendState>        blend_;

    std::vector<LineVertex> lines_;
};

}  // namespace tubane::sandbox
