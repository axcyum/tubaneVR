// Initial ("pon-oki") layout of every virtual device.
//
// Shared by the driver (fallback when no sandbox is running) and the sandbox
// (initial scene), so both sides always agree on where devices start.
// All values are in OpenVR standing space: meters, +Y up, -Z forward.
#pragma once

#include <cstring>

#include "tubane/ipc_layout.h"

namespace tubane::ipc {

inline DevicePose MakePose(double x, double y, double z,
                           double qw = 1.0, double qx = 0.0, double qy = 0.0, double qz = 0.0) {
    DevicePose p{};
    p.position[0] = x;
    p.position[1] = y;
    p.position[2] = z;
    p.rotation[0] = qw;
    p.rotation[1] = qx;
    p.rotation[2] = qy;
    p.rotation[3] = qz;
    p.flags       = kPoseValid | kPoseConnected;
    return p;
}

// Default pose for any slot. Roughly a person standing at the origin facing -Z.
inline DevicePose DefaultPose(uint32_t slot) {
    switch (slot) {
        case kSlotHmd:       return MakePose( 0.00, 1.60,  0.00);
        case kSlotLeftHand:  return MakePose(-0.20, 1.10, -0.30);
        case kSlotRightHand: return MakePose( 0.20, 1.10, -0.30);
        default:             break;
    }
    const uint32_t tracker = slot - kSlotFirstTracker;
    switch (tracker) {
        case 0:  return MakePose( 0.00, 0.95, 0.05);  // waist
        case 1:  return MakePose(-0.12, 0.08, 0.00);  // left foot
        case 2:  return MakePose( 0.12, 0.08, 0.00);  // right foot
        default: break;
    }
    // Extra trackers: lined up on the floor behind the user.
    return MakePose(-0.6 + 0.3 * static_cast<double>(tracker - 3), 0.05, 0.6);
}

inline void FillDefaultControl(ControlBlock& c) {
    std::memset(&c, 0, sizeof(c));
    c.config.viewerEye    = ViewerEye::Left;
    c.config.resolution   = ResolutionPreset::FHD;
    c.config.trackerCount = kDefaultTrackers;
    for (uint32_t i = 0; i < kMaxDevices; ++i) {
        c.poses[i] = DefaultPose(i);
    }
}

inline void FillDefaultDriver(DriverBlock& d) {
    std::memset(&d, 0, sizeof(d));
    for (uint32_t i = 0; i < kMaxDevices; ++i) {
        d.deviceIds[i] = 0xFFFFFFFFu;
    }
}

}  // namespace tubane::ipc
