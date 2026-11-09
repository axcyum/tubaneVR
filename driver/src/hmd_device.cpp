#include "hmd_device.h"

#include <cstring>

namespace tubane::driver {

HmdDevice::HmdDevice(const DriverSettings& settings)
    : DeviceBase(ipc::kSlotHmd, "TUBANE-HMD-0001", vr::TrackedDeviceClass_HMD), settings_(settings) {}

void* HmdDevice::GetComponent(const char* componentNameAndVersion) {
    if (std::strcmp(componentNameAndVersion, vr::IVRDisplayComponent_Version) == 0) {
        return static_cast<vr::IVRDisplayComponent*>(this);
    }
    return nullptr;
}

void HmdDevice::SetupProperties(vr::PropertyContainerHandle_t container) {
    auto* props = vr::VRProperties();
    SetCommonProperties(container, "tubane", "tubane Virtual HMD", "tubane/virtual_hmd");

    props->SetBoolProperty(container, vr::Prop_DeviceIsWireless_Bool, false);
    props->SetBoolProperty(container, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);

    props->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, settings_.ipdMeters);
    props->SetFloatProperty(container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
    props->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, settings_.displayFrequency);
    props->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.011f);
    props->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, true);
    props->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, true);
    props->SetStringProperty(container, vr::Prop_RenderModelName_String, "generic_hmd");

    // No proximity sensor: always report "on head".
    props->SetBoolProperty(container, vr::Prop_ContainsProximitySensor_Bool, false);
}

void HmdDevice::GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height) {
    *x      = settings_.windowX;
    *y      = settings_.windowY;
    *width  = settings_.windowWidth;
    *height = settings_.windowHeight;
}

void HmdDevice::GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) {
    const ipc::Resolution res = RenderResolution();
    *width  = res.width;
    *height = res.height;
}

void HmdDevice::GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* width, uint32_t* height) {
    // Side-by-side inside the desktop window.
    *y      = 0;
    *width  = settings_.windowWidth / 2;
    *height = settings_.windowHeight;
    *x      = (eye == vr::Eye_Left) ? 0 : settings_.windowWidth / 2;
}

void HmdDevice::GetProjectionRaw(vr::EVREye /*eye*/, float* left, float* right, float* top, float* bottom) {
    // Symmetric frustum: 90 deg horizontal FOV, vertical derived from the
    // per-eye aspect ratio (16:9 for both FHD and WQHD).
    const ipc::Resolution res = RenderResolution();
    const float halfH = 1.0f;  // tan(45 deg)
    const float halfV = halfH * static_cast<float>(res.height) / static_cast<float>(res.width);
    *left   = -halfH;
    *right  = halfH;
    *top    = -halfV;
    *bottom = halfV;
}

vr::DistortionCoordinates_t HmdDevice::ComputeDistortion(vr::EVREye /*eye*/, float u, float v) {
    // No lens: identity mapping for all three color channels.
    vr::DistortionCoordinates_t c{};
    c.rfRed[0]   = u; c.rfRed[1]   = v;
    c.rfGreen[0] = u; c.rfGreen[1] = v;
    c.rfBlue[0]  = u; c.rfBlue[1]  = v;
    return c;
}

bool HmdDevice::ComputeInverseDistortion(vr::HmdVector2_t* /*result*/, vr::EVREye /*eye*/, uint32_t /*channel*/,
                                         float /*u*/, float /*v*/) {
    // Not provided -> SteamVR falls back to a numeric inverse of ComputeDistortion.
    return false;
}

}  // namespace tubane::driver
