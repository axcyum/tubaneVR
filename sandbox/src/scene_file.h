// Scene save / load (JSON) and the T-pose preset.
#pragma once

#include <filesystem>
#include <string>

#include "tubane/ipc_layout.h"

namespace tubane::sandbox {

struct SceneData {
    ipc::DevicePose       poses[ipc::kMaxDevices]{};
    uint32_t              trackerCount = ipc::kDefaultTrackers;
    ipc::ResolutionPreset resolution   = ipc::ResolutionPreset::FHD;
    ipc::ViewerEye        viewerEye    = ipc::ViewerEye::Left;
    float                 roomWidth = 4.0f, roomDepth = 4.0f, roomHeight = 2.5f;
};

// File format (UTF-8 JSON, *.tubane.json):
// {
//   "format": "tubane-scene", "version": 1,
//   "devices": [ { "slot": 0, "name": "HMD", "position": [x,y,z],
//                  "rotation": [w,x,y,z], "connected": true, "trackingValid": true }, ... ],
//   "settings": { "trackerCount": 3, "resolution": "FHD", "viewerEye": "Left" },
//   "room": { "width": 4, "depth": 4, "height": 2.5 }
// }
// Coordinates are OpenVR standing space (meters, +Y up, -Z forward).
bool SaveScene(const std::filesystem::path& path, const SceneData& scene, std::string& error);
// Devices missing from the file keep the values already in `scene`.
bool LoadScene(const std::filesystem::path& path, SceneData& scene, std::string& error);

// Compass facing for presets: North = -Z (the room's forward), East = +X,
// South = +Z, West = -X.
enum class Facing { North, East, South, West };
const char* FacingLabel(Facing f);
float       FacingYawRadians(Facing f);

// Fills all slots with a T-pose for a person `heightMeters` tall standing at
// the origin and facing `facing`. Hands are stretched sideways at shoulder
// height, trackers sit on their body parts.
void BuildTPose(Facing facing, float heightMeters, ipc::DevicePose (&poses)[ipc::kMaxDevices]);

}  // namespace tubane::sandbox
