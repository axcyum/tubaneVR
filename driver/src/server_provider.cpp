#include "server_provider.h"

#include <windows.h>

#include <chrono>
#include <cstring>

#include "driver_log.h"
#include "tubane/default_state.h"

namespace tubane::driver {

namespace {
constexpr auto kPoseInterval = std::chrono::microseconds(2000);  // ~500 Hz
}

vr::EVRInitError ServerProvider::Init(vr::IVRDriverContext* driverContext) {
    VR_INIT_SERVER_DRIVER_CONTEXT(driverContext);

    settings_ = LoadSettings();
    Log("init: resolution=%s trackers=%u freq=%.1f directMode=%d", ToSettingString(settings_.resolution),
        settings_.trackerCount, settings_.displayFrequency, settings_.directMode ? 1 : 0);

    // --- IPC ---------------------------------------------------------------
    // Failure is not fatal: devices fall back to their default poses.
    const ipc::OpenResult shmResult = ipc_.Open();
    if (shmResult == ipc::OpenResult::Ok) {
        ipc::ControlBlock control{};
        if (ipc_.ReadControl(control)) {
            lastConfigRevision_ = control.config.revision;  // never re-apply stale requests
        }
        Log("shared memory %s (%s)", ipc_.Created() ? "created" : "opened", ipc::ToString(shmResult));
    } else {
        Log("shared memory unavailable: %s (win32 error %lu) - using default poses", ipc::ToString(shmResult),
            ipc_.LastError());
    }

    // --- Devices -----------------------------------------------------------
    hmd_       = std::make_unique<HmdDevice>(settings_, &ipc_);
    leftHand_  = std::make_unique<ControllerDevice>(Hand::Left);
    rightHand_ = std::make_unique<ControllerDevice>(Hand::Right);

    AddDevice(hmd_.get());
    AddDevice(leftHand_.get());
    AddDevice(rightHand_.get());

    for (uint32_t i = 0; i < settings_.trackerCount; ++i) {
        auto tracker = std::make_unique<TrackerDevice>(i);
        tracker->EnsureDefaultRole();
        AddDevice(tracker.get());
        trackers_.push_back(std::move(tracker));
    }

    PublishDriverStatus();

    running_.store(true);
    poseThread_ = std::thread(&ServerProvider::PoseThreadMain, this);
    return vr::VRInitError_None;
}

void ServerProvider::AddDevice(DeviceBase* device) {
    if (!vr::VRServerDriverHost()->TrackedDeviceAdded(device->Serial().c_str(), device->DeviceClass(), device)) {
        Log("TrackedDeviceAdded failed for %s", device->Serial().c_str());
        return;
    }
    devices_.push_back(device);
}

void ServerProvider::Cleanup() {
    running_.store(false);
    if (poseThread_.joinable()) {
        poseThread_.join();
    }
    // vrserver calls Deactivate() on every device before Cleanup().
    devices_.clear();
    trackers_.clear();
    rightHand_.reset();
    leftHand_.reset();
    hmd_.reset();
    ipc_.WriteDriver([](ipc::DriverBlock& d) { d.driverProcessId = 0; });
    ipc_.Close();
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

void ServerProvider::PoseThreadMain() {
    SetThreadDescription(GetCurrentThread(), L"tubane pose thread");

    auto              next = std::chrono::steady_clock::now();
    ipc::ControlBlock control{};
    bool              haveControl = false;

    while (running_.load(std::memory_order_relaxed)) {
        if (ipc_.IsOpen()) {
            // On a failed read keep the previous snapshot; better than snapping
            // devices back to defaults for one tick.
            haveControl = ipc_.ReadControl(control) || haveControl;
            ipc_.WriteDriver([](ipc::DriverBlock& d) { ++d.driverHeartbeat; });
        }

        for (DeviceBase* device : devices_) {
            device->PushPose(haveControl ? control.poses[device->Slot()] : ipc::DefaultPose(device->Slot()));
        }

        next += kPoseInterval;
        const auto now = std::chrono::steady_clock::now();
        if (next < now) {
            next = now;  // fell behind (debugger, suspend); don't burst
        }
        std::this_thread::sleep_until(next);
    }
}

void ServerProvider::RunFrame() {
    // Drain events. Haptics are accepted but ignored for now.
    vr::VREvent_t event{};
    while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
    }

    if (!ipc_.IsOpen()) {
        return;
    }

    // vrserver activates devices after Init() returns, so object ids become
    // known only later. Re-publish the status whenever one changes.
    bool idsChanged = false;
    for (const DeviceBase* device : devices_) {
        if (publishedIds_[device->Slot()] != device->ObjectId()) {
            idsChanged = true;
            break;
        }
    }
    if (idsChanged) {
        PublishDriverStatus();
    }

    ipc::ControlBlock control{};
    if (!ipc_.ReadControl(control)) {
        return;
    }

    leftHand_->UpdateInput(control.controllers[0]);
    rightHand_->UpdateInput(control.controllers[1]);

    if (control.config.revision != lastConfigRevision_) {
        lastConfigRevision_ = control.config.revision;
        ApplyConfigChanges(control.config);
        PublishDriverStatus();
    }
}

void ServerProvider::ApplyConfigChanges(const ipc::ControlConfig& config) {
    // Persistent values: saved to steamvr.vrsettings, effective after restart.
    const DriverSettings saved = LoadSettings();

    if (config.resolution != saved.resolution) {
        SaveResolution(config.resolution);
        Log("resolution preset saved: %s (restart SteamVR to apply)", ToSettingString(config.resolution));
    }
    const uint32_t trackerCount = (config.trackerCount > ipc::kMaxTrackers) ? ipc::kMaxTrackers : config.trackerCount;
    if (trackerCount != saved.trackerCount) {
        SaveTrackerCount(trackerCount);
        Log("tracker count saved: %u (restart SteamVR to apply)", trackerCount);
    }

    restartRequired_ = (config.resolution != settings_.resolution) || (trackerCount != settings_.trackerCount);

    // viewerEye is live: DirectModeRenderer::Present reads it every frame.
}

void ServerProvider::PublishDriverStatus() {
    const ipc::Resolution res = ipc::ResolutionOf(settings_.resolution);
    ipc_.WriteDriver([&](ipc::DriverBlock& d) {
        d.driverProcessId    = GetCurrentProcessId();
        d.activeTrackerCount = static_cast<uint32_t>(trackers_.size());
        d.renderWidth        = res.width;
        d.renderHeight       = res.height;
        d.activeResolution   = settings_.resolution;
        d.restartRequired    = restartRequired_ ? 1u : 0u;
        d.appliedRevision    = lastConfigRevision_;
        for (uint32_t i = 0; i < ipc::kMaxDevices; ++i) {
            d.deviceIds[i] = vr::k_unTrackedDeviceIndexInvalid;
        }
        for (const DeviceBase* device : devices_) {
            d.deviceIds[device->Slot()] = device->ObjectId();
        }
        std::memcpy(publishedIds_, d.deviceIds, sizeof(publishedIds_));
    });
}

}  // namespace tubane::driver
