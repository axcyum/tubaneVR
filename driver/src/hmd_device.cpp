#include "hmd_device.h"

#include <windows.h>

#include <chrono>
#include <cstring>

#include "driver_log.h"

namespace tubane::driver {

HmdDevice::HmdDevice(const DriverSettings& settings, DriverIpc* ipc)
    : DeviceBase(ipc::kSlotHmd, "TUBANE-HMD-0001", vr::TrackedDeviceClass_HMD), settings_(settings), ipc_(ipc) {}

HmdDevice::~HmdDevice() {
    StopVsyncThread();
    if (directMode_) directMode_->Shutdown();
}

void* HmdDevice::GetComponent(const char* componentNameAndVersion) {
    if (std::strcmp(componentNameAndVersion, vr::IVRDisplayComponent_Version) == 0) {
        return static_cast<vr::IVRDisplayComponent*>(this);
    }
    if (directMode_ && std::strcmp(componentNameAndVersion, vr::IVRDriverDirectModeComponent_Version) == 0) {
        return static_cast<vr::IVRDriverDirectModeComponent*>(directMode_.get());
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
    props->SetStringProperty(container, vr::Prop_RenderModelName_String, "generic_hmd");
    props->SetBoolProperty(container, vr::Prop_ContainsProximitySensor_Bool, false);

    // --- Display path --------------------------------------------------------
    if (settings_.directMode) {
        const ipc::Resolution res = RenderResolution();
        auto renderer = std::make_unique<DirectModeRenderer>(ipc_);
        if (renderer->Init(settings_.adapterIndex, res.width, res.height)) {
            directMode_ = std::move(renderer);
        } else {
            Log("direct mode unavailable - falling back to the desktop Headset Window");
        }
    }

    if (directMode_) {
        props->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, false);
        props->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, false);
        // The compositor must render on the adapter our swap textures live on.
        props->SetUint64Property(container, vr::Prop_GraphicsAdapterLuid_Uint64, directMode_->AdapterLuid());
        props->SetBoolProperty(container, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, true);
        StartVsyncThread();
    } else {
        props->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, true);
        props->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, true);
        ipc_->WriteDriver([](ipc::DriverBlock& d) {
            d.displayMode         = ipc::kDisplayExtended;
            d.frameTextureName[0] = L'\0';
        });
    }
}

void HmdDevice::Deactivate() {
    StopVsyncThread();
    DeviceBase::Deactivate();
}

// ---------------------------------------------------------------------------
// Vsync — in direct mode there is no real display, so the driver paces the
// compositor by reporting a vsync every 1 / displayFrequency seconds.
// ---------------------------------------------------------------------------
void HmdDevice::StartVsyncThread() {
    if (vsyncRunning_.exchange(true)) return;
    vsyncThread_ = std::thread([this] {
        SetThreadDescription(GetCurrentThread(), L"tubane vsync");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

        using Clock = std::chrono::steady_clock;
        const auto period = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / settings_.displayFrequency));
        auto next = Clock::now() + period;

        while (vsyncRunning_.load(std::memory_order_relaxed)) {
            const auto now = Clock::now();
            if (next > now) {
                const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(next - now).count();
                if (timer) {
                    LARGE_INTEGER due;
                    due.QuadPart = -static_cast<LONGLONG>(wait / 100);  // relative, 100 ns units
                    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer, INFINITE);
                } else {
                    std::this_thread::sleep_until(next);
                }
            }
            vr::VRServerDriverHost()->VsyncEvent(0.0);
            next += period;
            if (Clock::now() - next > period * 4) next = Clock::now() + period;  // resync after a stall
        }
        if (timer) CloseHandle(timer);
    });
}

void HmdDevice::StopVsyncThread() {
    if (!vsyncRunning_.exchange(false)) return;
    if (vsyncThread_.joinable()) vsyncThread_.join();
}

// ---------------------------------------------------------------------------
// IVRDisplayComponent
// ---------------------------------------------------------------------------
void HmdDevice::GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height) {
    if (DirectMode()) {
        const ipc::Resolution res = RenderResolution();
        *x      = 0;
        *y      = 0;
        *width  = res.width * 2;
        *height = res.height;
        return;
    }
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
    // Side by side inside the (virtual or desktop) window.
    uint32_t w = 0, h = 0;
    int32_t  wx = 0, wy = 0;
    GetWindowBounds(&wx, &wy, &w, &h);
    *y      = 0;
    *width  = w / 2;
    *height = h;
    *x      = (eye == vr::Eye_Left) ? 0 : w / 2;
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
