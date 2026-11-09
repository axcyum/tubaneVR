// Static description of the virtual devices shown in the sandbox: names,
// simple box models and pick volumes. Poses live in IpcLink::Control().
#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

#include "renderer.h"
#include "tubane/ipc_layout.h"

namespace tubane::sandbox {

enum class DeviceKind { Hmd, Controller, Tracker };

struct BoxPart {
    DirectX::XMFLOAT3 center;  // local space, meters
    DirectX::XMFLOAT3 size;
    float             shade;   // multiplies the device color
};

struct DeviceModel {
    std::vector<BoxPart> parts;
    DirectX::XMFLOAT3    pickCenter;
    DirectX::XMFLOAT3    pickHalfExtents;
};

struct DeviceInfo {
    uint32_t    slot;
    DeviceKind  kind;
    std::string name;
    Color       color;
};

// Name/color/kind for a slot (trackers get body-role names).
DeviceInfo DescribeSlot(uint32_t slot);

// Box model for a device kind. Local axes follow OpenVR: -Z forward, +Y up.
const DeviceModel& ModelFor(DeviceKind kind, uint32_t slot);

}  // namespace tubane::sandbox
