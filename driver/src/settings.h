// Driver settings stored in SteamVR's settings system.
//
// Defaults come from package/tubane/resources/settings/default.vrsettings;
// user overrides are written to Steam\config\steamvr.vrsettings under the
// "driver_tubane" section.
#pragma once

#include <openvr_driver.h>

#include <cstdint>
#include <string>

#include "tubane/ipc_layout.h"

namespace tubane::driver {

inline constexpr const char* kSettingsSection = "driver_tubane";
inline constexpr const char* kDriverName      = "tubane";  // must match driver.vrdrivermanifest "name"

struct DriverSettings {
    ipc::ResolutionPreset resolution       = ipc::ResolutionPreset::FHD;
    uint32_t              trackerCount     = ipc::kDefaultTrackers;
    float                 displayFrequency = 90.0f;
    float                 ipdMeters        = 0.063f;
    int32_t               windowX          = 0;
    int32_t               windowY          = 0;
    uint32_t              windowWidth      = 1920;
    uint32_t              windowHeight     = 540;
};

const char*           ToSettingString(ipc::ResolutionPreset preset);
ipc::ResolutionPreset ParseResolution(const std::string& text);

DriverSettings LoadSettings();
void           SaveResolution(ipc::ResolutionPreset preset);
void           SaveTrackerCount(uint32_t count);

}  // namespace tubane::driver
