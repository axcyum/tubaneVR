#include "tracker_device.h"

#include <cstdio>
#include <iterator>
#include <string>

#include "driver_log.h"
#include "settings.h"

namespace tubane::driver {

namespace {

std::string TrackerSerial(uint32_t index) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "TUBANE-TRK-%02u", index);
    return buffer;
}

}  // namespace

const char* DefaultTrackerRole(uint32_t trackerIndex) {
    static constexpr const char* kRoles[] = {
        "TrackerRole_Waist",
        "TrackerRole_LeftFoot",
        "TrackerRole_RightFoot",
        "TrackerRole_Chest",
        "TrackerRole_LeftElbow",
        "TrackerRole_RightElbow",
        "TrackerRole_LeftKnee",
        "TrackerRole_RightKnee",
    };
    static_assert(std::size(kRoles) >= ipc::kMaxTrackers);
    return trackerIndex < std::size(kRoles) ? kRoles[trackerIndex] : "TrackerRole_None";
}

TrackerDevice::TrackerDevice(uint32_t trackerIndex)
    : DeviceBase(ipc::TrackerSlot(trackerIndex), TrackerSerial(trackerIndex), vr::TrackedDeviceClass_GenericTracker),
      trackerIndex_(trackerIndex) {}

void TrackerDevice::EnsureDefaultRole() const {
    // Key format used by SteamVR: "/devices/<driver name>/<serial>"
    const std::string key = std::string("/devices/") + kDriverName + "/" + Serial();

    char                 current[128] = {};
    vr::EVRSettingsError err          = vr::VRSettingsError_None;
    vr::VRSettings()->GetString("trackers", key.c_str(), current, sizeof(current), &err);
    if (err == vr::VRSettingsError_None && current[0] != '\0') {
        Log("tracker %s keeps role %s", Serial().c_str(), current);
        return;
    }
    const char* role = DefaultTrackerRole(trackerIndex_);
    vr::VRSettings()->SetString("trackers", key.c_str(), role);
    Log("tracker %s assigned default role %s", Serial().c_str(), role);
}

void TrackerDevice::SetupProperties(vr::PropertyContainerHandle_t container) {
    auto* props = vr::VRProperties();

    const std::string registered = "htc/vive_tracker" + Serial();
    SetCommonProperties(container, "HTC", "VIVE Tracker Pro MV", registered.c_str());

    props->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_OptOut);
    props->SetStringProperty(container, vr::Prop_ControllerType_String, "vive_tracker");
    props->SetStringProperty(container, vr::Prop_InputProfilePath_String, "{htc}/input/vive_tracker_profile.json");
    props->SetStringProperty(container, vr::Prop_RenderModelName_String, "{htc}vr_tracker_vive_1_0");
}

}  // namespace tubane::driver
