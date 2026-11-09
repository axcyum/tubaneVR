// Virtual HMD.
//
// Two display modes (setting "directMode"):
//   * direct mode (default): IVRDriverDirectModeComponent. The compositor
//     renders into driver-owned swap textures; the selected eye is copied into
//     a shared texture shown by tubane-sandbox's HMD Viewer. No desktop window.
//   * extended mode: IsDisplayOnDesktop() == true. SteamVR opens its own
//     "Headset Window" on the desktop (null-driver style). Fallback / debug.
#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "device_base.h"
#include "direct_mode.h"
#include "driver_ipc.h"
#include "settings.h"

namespace tubane::driver {

class HmdDevice final : public DeviceBase, public vr::IVRDisplayComponent {
public:
    HmdDevice(const DriverSettings& settings, DriverIpc* ipc);
    ~HmdDevice() override;

    // --- DeviceBase ------------------------------------------------------
    void* GetComponent(const char* componentNameAndVersion) override;
    void  Deactivate() override;

    // --- vr::IVRDisplayComponent ------------------------------------------
    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height) override;
    bool IsDisplayOnDesktop() override { return !DirectMode(); }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) override;
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* width, uint32_t* height) override;
    void GetProjectionRaw(vr::EVREye eye, float* left, float* right, float* top, float* bottom) override;
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye eye, float u, float v) override;
    bool ComputeInverseDistortion(vr::HmdVector2_t* result, vr::EVREye eye, uint32_t channel, float u, float v) override;

    ipc::Resolution RenderResolution() const { return ipc::ResolutionOf(settings_.resolution); }
    bool            DirectMode() const { return directMode_ != nullptr; }

protected:
    void SetupProperties(vr::PropertyContainerHandle_t container) override;

private:
    void StartVsyncThread();
    void StopVsyncThread();

    DriverSettings settings_;
    DriverIpc*     ipc_;

    std::unique_ptr<DirectModeRenderer> directMode_;  // null in extended mode

    std::atomic<bool> vsyncRunning_{false};
    std::thread       vsyncThread_;
};

}  // namespace tubane::driver
