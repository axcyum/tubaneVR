#include "spout_receiver.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tubane::sandbox {

namespace {

constexpr size_t kNameSize = 256;

// Layout written by Spout2 senders (SpoutSenderNames.h). Only the leading
// fields are used.
struct SharedTextureInfo {
    uint32_t shareHandle;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t usage;
    uint8_t  description[256];
    uint32_t partnerId;
};

// Read-only view of a named shared memory block; size from VirtualQuery.
class SharedView {
public:
    explicit SharedView(const char* name) {
        mapping_ = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
        if (!mapping_) return;
        data_ = static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (data_) {
            MEMORY_BASIC_INFORMATION mbi{};
            VirtualQuery(data_, &mbi, sizeof(mbi));
            size_ = mbi.RegionSize;
        }
    }
    ~SharedView() {
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
    }
    SharedView(const SharedView&)            = delete;
    SharedView& operator=(const SharedView&) = delete;

    const uint8_t* Data() const { return data_; }
    size_t         Size() const { return size_; }

private:
    HANDLE         mapping_ = nullptr;
    const uint8_t* data_    = nullptr;
    size_t         size_    = 0;
};

std::string ReadName(const uint8_t* p) {
    return std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), kNameSize));
}

bool ReadSenderInfo(const std::string& name, SharedTextureInfo& info) {
    SharedView view(name.c_str());
    if (!view.Data() || view.Size() < sizeof(uint32_t) * 5) return false;
    std::memset(&info, 0, sizeof(info));
    std::memcpy(&info, view.Data(), std::min(view.Size(), sizeof(info)));
    return info.shareHandle != 0 && info.width != 0 && info.height != 0;
}

DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default:                                return f;
    }
}

}  // namespace

std::vector<std::string> SpoutReceiver::ListSenders() {
    std::vector<std::string> names;
    SharedView view("SpoutSenderNames");
    if (!view.Data()) return names;
    for (size_t off = 0; off + kNameSize <= view.Size(); off += kNameSize) {
        if (view.Data()[off] == 0) break;
        names.push_back(ReadName(view.Data() + off));
    }
    return names;
}

std::string SpoutReceiver::ActiveSender() {
    SharedView view("ActiveSenderName");
    if (!view.Data() || view.Size() < 1) return {};
    return ReadName(view.Data());
}

void SpoutReceiver::Reset() {
    connectedName_.clear();
    connectedHandle_ = 0;
    shared_.Reset();
    local_.Reset();
    srv_.Reset();
    width_ = height_ = 0;
}

bool SpoutReceiver::Connect(ID3D11Device* device, const std::string& name, uint32_t handle) {
    Reset();
    HRESULT hr = device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)),
                                            IID_PPV_ARGS(&shared_));
    if (FAILED(hr)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Cannot open '%s' (0x%08lX) - sender on another GPU?", name.c_str(),
                      static_cast<unsigned long>(hr));
        status_ = buf;
        shared_.Reset();
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    shared_->GetDesc(&td);
    td.MiscFlags = 0;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    td.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &local_))) {
        status_ = "Cannot create local texture";
        Reset();
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format              = ViewFormat(td.Format);
    sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(local_.Get(), &sv, &srv_);

    connectedName_   = name;
    connectedHandle_ = handle;
    width_           = td.Width;
    height_          = td.Height;
    status_          = "Receiving";
    return true;
}

void SpoutReceiver::Update(ID3D11Device* device, ID3D11DeviceContext* ctx, const std::string& senderName, double now) {
    const std::string name = senderName.empty() ? ActiveSender() : senderName;
    if (name.empty()) {
        if (!connectedName_.empty()) Reset();
        status_ = "No sender";
        return;
    }

    SharedTextureInfo info{};
    if (!ReadSenderInfo(name, info)) {
        if (!connectedName_.empty()) Reset();
        status_ = "Sender '" + name + "' not found";
        return;
    }

    // (Re)connect when the sender changed or re-created its texture (resize).
    if (name != connectedName_ || info.shareHandle != connectedHandle_) {
        if (now - lastAttempt_ < 0.5 && name == connectedName_) return;
        lastAttempt_ = now;
        if (!Connect(device, name, info.shareHandle)) return;
    }

    // Spout guards the texture with an optional named mutex.
    const std::string mutexName = name + "_SpoutAccessMutex";
    HANDLE mutex = OpenMutexA(SYNCHRONIZE, FALSE, mutexName.c_str());
    bool   locked = true;
    if (mutex) {
        const DWORD r = WaitForSingleObject(mutex, 4);
        locked = (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED);
    }
    if (locked) {
        ctx->CopySubresourceRegion(local_.Get(), 0, 0, 0, 0, shared_.Get(), 0, nullptr);
        if (mutex) ReleaseMutex(mutex);
    }
    if (mutex) CloseHandle(mutex);
}

}  // namespace tubane::sandbox
