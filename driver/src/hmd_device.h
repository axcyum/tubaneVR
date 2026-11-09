// Virtual HMD.
//
// Step 1: implements IVRDisplayComponent in "extended" (desktop window) mode,
// like the OpenVR sample null driver. The compositor opens a window at
// windowX/Y/Width/Height and draws both eyes side by side.
// Step 3 replaces this with IVRDriverDirectModeComponent + a D3D11 shared
// texture read by tubane-sandbox.
#pragma once

#include "device_base.h"
#include "settings.h"

namespace tubane::driver {

class HmdDevice final : public DeviceBase, public vr::IVRDisplayComponent {
public:
    explicit HmdDevice(const DriverSettings& settings);

    // --- DeviceBase ------------------------------------------------------
    void* GetComponent(const char* componentNameAndVersion) override;

    // --- vr::IVRDisplayComponent ------------------------------------------
    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height) override;
    bool IsDisplayOnDesktop() override { return true; }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) override;
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* width, uint32_t* height) override;
    void GetProjectionRaw(vr::EVREye eye, float* left, float* right, float* top, float* bottom) override;
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye eye, float u, float v) override;
    bool ComputeInverseDistortion(vr::HmdVector2_t* result, vr::EVREye eye, uint32_t channel, float u, float v) override;

    ipc::Resolution RenderResolution() const { return ipc::ResolutionOf(settings_.resolution); }

protected:
    void SetupProperties(vr::PropertyContainerHandle_t container) override;

private:
    DriverSettings settings_;
};

}  // namespace tubane::driver
