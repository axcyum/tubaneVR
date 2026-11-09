// Virtual HTC Vive Tracker.
//
// Uses SteamVR's bundled {htc} resources. The body role (waist / feet / ...)
// is written to steamvr.vrsettings "trackers" section on first start, the
// same place SteamVR's "Manage Vive Trackers" UI stores it — so the user can
// still change it from the SteamVR UI afterwards.
#pragma once

#include "device_base.h"

namespace tubane::driver {

class TrackerDevice final : public DeviceBase {
public:
    explicit TrackerDevice(uint32_t trackerIndex);

    uint32_t TrackerIndex() const { return trackerIndex_; }

    // Assigns a default role if none is stored yet. Must be called before
    // TrackedDeviceAdded() so SteamVR picks it up on activation.
    void EnsureDefaultRole() const;

    // Prop_RegisteredDeviceType value; also the key of the role setting.
    std::string RegisteredDeviceType() const;

protected:
    void SetupProperties(vr::PropertyContainerHandle_t container) override;

private:
    const uint32_t trackerIndex_;
};

// "TrackerRole_Waist", "TrackerRole_LeftFoot", ... for tracker index 0..N.
const char* DefaultTrackerRole(uint32_t trackerIndex);

}  // namespace tubane::driver
