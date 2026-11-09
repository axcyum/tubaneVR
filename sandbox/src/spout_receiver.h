// Minimal Spout2 receiver (no SDK dependency).
//
// Spout2 senders publish:
//   * "SpoutSenderNames"   shared memory: list of sender names, 256 bytes each
//   * "ActiveSenderName"   shared memory: the active sender's name
//   * "<sender name>"      shared memory: SharedTextureInfo (DX11 shared handle,
//                          width, height, format, ...)
//   * "<sender name>_SpoutAccessMutex"  optional mutex guarding the texture
// The texture handle is a legacy (non-NT) DXGI shared handle that we open with
// ID3D11Device::OpenSharedResource and copy into a local texture each frame.
// Sender and receiver must be on the same GPU.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tubane::sandbox {

class SpoutReceiver {
public:
    // Current sender names (empty when no Spout sender is running).
    static std::vector<std::string> ListSenders();
    static std::string              ActiveSender();

    // `senderName` empty = follow the active sender.
    void Update(ID3D11Device* device, ID3D11DeviceContext* ctx, const std::string& senderName, double nowSeconds);
    void Reset();

    ID3D11ShaderResourceView* Texture() const { return srv_.Get(); }
    uint32_t           Width() const { return width_; }
    uint32_t           Height() const { return height_; }
    bool               HasFrame() const { return srv_ != nullptr; }
    const std::string& ConnectedSender() const { return connectedName_; }
    const std::string& Status() const { return status_; }

private:
    bool Connect(ID3D11Device* device, const std::string& name, uint32_t handle);

    std::string connectedName_;
    uint32_t    connectedHandle_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          shared_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          local_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    uint32_t    width_ = 0, height_ = 0;
    double      lastAttempt_ = -10.0;
    std::string status_ = "No sender";
};

}  // namespace tubane::sandbox
