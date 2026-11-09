#include "settings.h"

#include <algorithm>

namespace tubane::driver {

const char* ToSettingString(ipc::ResolutionPreset preset) {
    return preset == ipc::ResolutionPreset::WQHD ? "WQHD" : "FHD";
}

ipc::ResolutionPreset ParseResolution(const std::string& text) {
    return (text == "WQHD" || text == "wqhd" || text == "2560x1440") ? ipc::ResolutionPreset::WQHD
                                                                      : ipc::ResolutionPreset::FHD;
}

namespace {

int32_t GetInt(const char* key, int32_t fallback) {
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    int32_t value = vr::VRSettings()->GetInt32(kSettingsSection, key, &err);
    return err == vr::VRSettingsError_None ? value : fallback;
}

float GetFloat(const char* key, float fallback) {
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    float value = vr::VRSettings()->GetFloat(kSettingsSection, key, &err);
    return err == vr::VRSettingsError_None ? value : fallback;
}

bool GetBool(const char* key, bool fallback) {
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    bool value = vr::VRSettings()->GetBool(kSettingsSection, key, &err);
    return err == vr::VRSettingsError_None ? value : fallback;
}

std::string GetString(const char* key, const char* fallback) {
    char buffer[256] = {};
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(kSettingsSection, key, buffer, sizeof(buffer), &err);
    return (err == vr::VRSettingsError_None && buffer[0] != '\0') ? std::string(buffer) : std::string(fallback);
}

}  // namespace

DriverSettings LoadSettings() {
    DriverSettings s;
    s.resolution       = ParseResolution(GetString("resolutionPreset", "FHD"));
    s.trackerCount     = static_cast<uint32_t>(std::clamp<int32_t>(
        GetInt("trackerCount", static_cast<int32_t>(ipc::kDefaultTrackers)), 0, static_cast<int32_t>(ipc::kMaxTrackers)));
    s.displayFrequency = std::clamp(GetFloat("displayFrequency", 90.0f), 30.0f, 240.0f);
    s.ipdMeters        = std::clamp(GetFloat("ipd", 0.063f), 0.04f, 0.09f);
    s.windowX          = GetInt("windowX", 0);
    s.windowY          = GetInt("windowY", 0);
    s.windowWidth      = static_cast<uint32_t>(std::max(GetInt("windowWidth", 1920), 64));
    s.windowHeight     = static_cast<uint32_t>(std::max(GetInt("windowHeight", 540), 64));
    s.directMode       = GetBool("directMode", true);
    s.adapterIndex     = static_cast<uint32_t>(std::max(GetInt("adapterIndex", 0), 0));
    return s;
}

void SaveResolution(ipc::ResolutionPreset preset) {
    vr::VRSettings()->SetString(kSettingsSection, "resolutionPreset", ToSettingString(preset));
}

void SaveTrackerCount(uint32_t count) {
    vr::VRSettings()->SetInt32(kSettingsSection, "trackerCount",
                               static_cast<int32_t>(std::min(count, ipc::kMaxTrackers)));
}

}  // namespace tubane::driver
