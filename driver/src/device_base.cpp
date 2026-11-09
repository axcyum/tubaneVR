#include "device_base.h"

#include <cstdio>
#include <cstring>

#include "driver_log.h"
#include "settings.h"
#include "tubane/default_state.h"

namespace tubane::driver {

vr::DriverPose_t ToDriverPose(const ipc::DevicePose& pose) {
    vr::DriverPose_t out{};
    out.poseTimeOffset             = 0.0;
    out.qWorldFromDriverRotation.w = 1.0;
    out.qDriverFromHeadRotation.w  = 1.0;

    for (int i = 0; i < 3; ++i) {
        out.vecPosition[i]        = pose.position[i];
        out.vecVelocity[i]        = pose.velocity[i];
        out.vecAngularVelocity[i] = pose.angularVelocity[i];
    }
    out.qRotation.w = pose.rotation[0];
    out.qRotation.x = pose.rotation[1];
    out.qRotation.y = pose.rotation[2];
    out.qRotation.z = pose.rotation[3];

    // Guard against an all-zero quaternion (e.g. an uninitialized slot):
    // SteamVR would produce NaNs from it.
    const double lenSq = out.qRotation.w * out.qRotation.w + out.qRotation.x * out.qRotation.x +
                         out.qRotation.y * out.qRotation.y + out.qRotation.z * out.qRotation.z;
    if (lenSq < 1e-12) {
        out.qRotation = {1.0, 0.0, 0.0, 0.0};
    }

    const bool valid      = (pose.flags & ipc::kPoseValid) != 0;
    out.poseIsValid       = valid;
    out.result            = valid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    out.deviceIsConnected = (pose.flags & ipc::kPoseConnected) != 0;
    out.willDriftInYaw    = false;
    out.shouldApplyHeadModel = false;
    return out;
}

DeviceBase::DeviceBase(uint32_t slot, std::string serial, vr::ETrackedDeviceClass deviceClass)
    : slot_(slot), serial_(std::move(serial)), class_(deviceClass) {
    lastPose_ = ToDriverPose(ipc::DefaultPose(slot));
}

vr::EVRInitError DeviceBase::Activate(uint32_t objectId) {
    container_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId);
    SetupProperties(container_);
    objectId_.store(objectId, std::memory_order_release);
    Log("activated %s (slot %u) as device %u", serial_.c_str(), slot_, objectId);
    return vr::VRInitError_None;
}

void DeviceBase::Deactivate() {
    Log("deactivated %s", serial_.c_str());
    objectId_.store(vr::k_unTrackedDeviceIndexInvalid, std::memory_order_release);
}

void* DeviceBase::GetComponent(const char* /*componentNameAndVersion*/) {
    return nullptr;
}

void DeviceBase::DebugRequest(const char* request, char* response, uint32_t responseSize) {
    // `vrcmd --debugrequest <id> <text>` lands here; useful to check a device is ours.
    if (responseSize > 0) {
        std::snprintf(response, responseSize, "tubane %s slot=%u request=%s", serial_.c_str(), slot_,
                      request ? request : "");
    }
}

vr::DriverPose_t DeviceBase::GetPose() {
    std::lock_guard lock(poseMutex_);
    return lastPose_;
}

void DeviceBase::PushPose(const ipc::DevicePose& pose) {
    const uint32_t id = ObjectId();
    if (id == vr::k_unTrackedDeviceIndexInvalid) {
        return;
    }
    const vr::DriverPose_t driverPose = ToDriverPose(pose);
    {
        std::lock_guard lock(poseMutex_);
        lastPose_ = driverPose;
    }
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(id, driverPose, sizeof(vr::DriverPose_t));
}

void DeviceBase::SetCommonProperties(vr::PropertyContainerHandle_t container, const char* manufacturer,
                                     const char* model, const char* registeredDeviceType) {
    auto* props = vr::VRProperties();
    props->SetStringProperty(container, vr::Prop_ManufacturerName_String, manufacturer);
    props->SetStringProperty(container, vr::Prop_ModelNumber_String, model);
    props->SetStringProperty(container, vr::Prop_TrackingSystemName_String, kDriverName);
    props->SetStringProperty(container, vr::Prop_RegisteredDeviceType_String, registeredDeviceType);
    props->SetStringProperty(container, vr::Prop_SerialNumber_String, serial_.c_str());
    props->SetBoolProperty(container, vr::Prop_DeviceIsWireless_Bool, true);
    props->SetBoolProperty(container, vr::Prop_DeviceIsCharging_Bool, false);
    props->SetBoolProperty(container, vr::Prop_DeviceProvidesBatteryStatus_Bool, true);
    props->SetFloatProperty(container, vr::Prop_DeviceBatteryPercentage_Float, 1.0f);
    props->SetBoolProperty(container, vr::Prop_Identifiable_Bool, false);
    props->SetBoolProperty(container, vr::Prop_Firmware_UpdateAvailable_Bool, false);
    props->SetBoolProperty(container, vr::Prop_Firmware_ManualUpdate_Bool, false);
    props->SetBoolProperty(container, vr::Prop_NeverTracked_Bool, false);
}

}  // namespace tubane::driver
