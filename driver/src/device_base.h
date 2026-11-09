// Common base for every tubane virtual device.
//
// Holds the device's slot in the shared-memory arrays, its serial number, the
// OpenVR object id assigned on Activate(), and the most recent pose.
#pragma once

#include <openvr_driver.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "tubane/ipc_layout.h"

namespace tubane::driver {

class DeviceBase : public vr::ITrackedDeviceServerDriver {
public:
    DeviceBase(uint32_t slot, std::string serial, vr::ETrackedDeviceClass deviceClass);
    // OpenVR interfaces have no virtual destructor; declare one here so
    // derived devices are destroyed correctly through DeviceBase*.
    virtual ~DeviceBase() = default;

    // --- vr::ITrackedDeviceServerDriver ---------------------------------
    vr::EVRInitError Activate(uint32_t objectId) final;
    void             Deactivate() override;
    void             EnterStandby() override {}
    void*            GetComponent(const char* componentNameAndVersion) override;
    void             DebugRequest(const char* request, char* response, uint32_t responseSize) override;
    vr::DriverPose_t GetPose() override;

    // --- tubane ----------------------------------------------------------
    // Converts the shared-memory pose and pushes it to vrserver.
    void PushPose(const ipc::DevicePose& pose);

    // Called on the RunFrame thread with the latest controller input.
    virtual void UpdateInput(const ipc::ControllerInput& /*input*/) {}

    uint32_t                Slot() const        { return slot_; }
    const std::string&      Serial() const      { return serial_; }
    vr::ETrackedDeviceClass DeviceClass() const { return class_; }
    uint32_t                ObjectId() const    { return objectId_.load(std::memory_order_acquire); }
    bool                    IsActive() const    { return ObjectId() != vr::k_unTrackedDeviceIndexInvalid; }

protected:
    // Set device properties. Called from Activate() before the device is
    // reported active.
    virtual void SetupProperties(vr::PropertyContainerHandle_t container) = 0;

    // Common identity properties (manufacturer, model, tracking system ...).
    void SetCommonProperties(vr::PropertyContainerHandle_t container,
                             const char* manufacturer, const char* model,
                             const char* registeredDeviceType);

    vr::PropertyContainerHandle_t Container() const { return container_; }

private:
    const uint32_t                slot_;
    const std::string             serial_;
    const vr::ETrackedDeviceClass class_;

    std::atomic<uint32_t>         objectId_{vr::k_unTrackedDeviceIndexInvalid};
    vr::PropertyContainerHandle_t container_ = vr::k_ulInvalidPropertyContainer;

    std::mutex       poseMutex_;
    vr::DriverPose_t lastPose_{};
};

// Shared-memory pose -> OpenVR driver pose.
vr::DriverPose_t ToDriverPose(const ipc::DevicePose& pose);

}  // namespace tubane::driver
