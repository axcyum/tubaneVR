// IServerTrackedDeviceProvider: owns all virtual devices and the IPC link.
//
// Threads:
//   * vrserver main thread -> Init / RunFrame / Cleanup
//                             (input updates, config changes, event polling)
//   * pose thread          -> reads poses from shared memory at ~500 Hz and
//                             calls TrackedDevicePoseUpdated
#pragma once

#include <openvr_driver.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "controller_device.h"
#include "hmd_device.h"
#include "settings.h"
#include "tracker_device.h"
#include "tubane/shm.h"

namespace tubane::driver {

class ServerProvider final : public vr::IServerTrackedDeviceProvider {
public:
    // --- vr::IServerTrackedDeviceProvider --------------------------------
    vr::EVRInitError   Init(vr::IVRDriverContext* driverContext) override;
    void               Cleanup() override;
    const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void               RunFrame() override;
    bool               ShouldBlockStandbyMode() override { return false; }
    void               EnterStandby() override {}
    void               LeaveStandby() override {}

private:
    void AddDevice(DeviceBase* device);
    void PoseThreadMain();
    void ApplyConfigChanges(const ipc::ControlConfig& config);
    void PublishDriverStatus();

    DriverSettings settings_;

    std::unique_ptr<HmdDevice>                  hmd_;
    std::unique_ptr<ControllerDevice>           leftHand_;
    std::unique_ptr<ControllerDevice>           rightHand_;
    std::vector<std::unique_ptr<TrackerDevice>> trackers_;
    std::vector<DeviceBase*>                    devices_;  // non-owning, all of the above

    ipc::SharedMemory shm_;
    uint32_t          lastConfigRevision_ = 0;
    bool              restartRequired_    = false;

    std::atomic<bool> running_{false};
    std::thread       poseThread_;
};

}  // namespace tubane::driver
