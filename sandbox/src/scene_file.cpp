#include "scene_file.h"

#include <DirectXMath.h>

#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

#include "scene.h"
#include "tubane/default_state.h"

namespace tubane::sandbox {

using json = nlohmann::json;
using namespace DirectX;

// ---------------------------------------------------------------------------
// Save / load
// ---------------------------------------------------------------------------
bool SaveScene(const std::filesystem::path& path, const SceneData& scene, std::string& error) {
    json devices = json::array();
    for (uint32_t slot = 0; slot < ipc::kMaxDevices; ++slot) {
        const ipc::DevicePose& p = scene.poses[slot];
        devices.push_back({
            {"slot", slot},
            {"name", DescribeSlot(slot).name},
            {"position", {p.position[0], p.position[1], p.position[2]}},
            {"rotation", {p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]}},
            {"connected", (p.flags & ipc::kPoseConnected) != 0},
            {"trackingValid", (p.flags & ipc::kPoseValid) != 0},
        });
    }
    const json doc = {
        {"format", "tubane-scene"},
        {"version", 1},
        {"devices", devices},
        {"settings",
         {{"trackerCount", scene.trackerCount},
          {"resolution", scene.resolution == ipc::ResolutionPreset::WQHD ? "WQHD" : "FHD"},
          {"viewerEye", scene.viewerEye == ipc::ViewerEye::Right ? "Right" : "Left"}}},
        {"room", {{"width", scene.roomWidth}, {"depth", scene.roomDepth}, {"height", scene.roomHeight}}},
    };

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot write file";
        return false;
    }
    out << doc.dump(2) << '\n';
    return static_cast<bool>(out);
}

bool LoadScene(const std::filesystem::path& path, SceneData& scene, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open file";
        return false;
    }
    json doc;
    try {
        doc = json::parse(in);
    } catch (const std::exception& e) {
        error = std::string("invalid JSON: ") + e.what();
        return false;
    }
    if (doc.value("format", "") != "tubane-scene") {
        error = "not a tubane scene file";
        return false;
    }

    try {
        SceneData result = scene;
        for (const json& d : doc.value("devices", json::array())) {
            const uint32_t slot = d.at("slot").get<uint32_t>();
            if (slot >= ipc::kMaxDevices) continue;
            ipc::DevicePose& p = result.poses[slot];
            const auto pos = d.at("position").get<std::vector<double>>();
            const auto rot = d.at("rotation").get<std::vector<double>>();
            if (pos.size() != 3 || rot.size() != 4) continue;
            for (int i = 0; i < 3; ++i) p.position[i] = pos[i];
            for (int i = 0; i < 4; ++i) p.rotation[i] = rot[i];
            p.velocity[0] = p.velocity[1] = p.velocity[2] = 0.0;
            p.angularVelocity[0] = p.angularVelocity[1] = p.angularVelocity[2] = 0.0;
            p.flags = (d.value("connected", true) ? ipc::kPoseConnected : 0u) |
                      (d.value("trackingValid", true) ? ipc::kPoseValid : 0u);
        }
        if (doc.contains("settings")) {
            const json& s = doc["settings"];
            result.trackerCount = std::min<uint32_t>(s.value("trackerCount", result.trackerCount), ipc::kMaxTrackers);
            result.resolution   = s.value("resolution", "FHD") == "WQHD" ? ipc::ResolutionPreset::WQHD : ipc::ResolutionPreset::FHD;
            result.viewerEye    = s.value("viewerEye", "Left") == "Right" ? ipc::ViewerEye::Right : ipc::ViewerEye::Left;
        }
        if (doc.contains("room")) {
            const json& r = doc["room"];
            result.roomWidth  = r.value("width", result.roomWidth);
            result.roomDepth  = r.value("depth", result.roomDepth);
            result.roomHeight = r.value("height", result.roomHeight);
        }
        scene = result;
    } catch (const std::exception& e) {
        error = std::string("unexpected content: ") + e.what();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// T-pose
// ---------------------------------------------------------------------------
const char* FacingLabel(Facing f) {
    switch (f) {
        case Facing::North: return "N (-Z)";
        case Facing::East:  return "E (+X)";
        case Facing::South: return "S (+Z)";
        case Facing::West:  return "W (-X)";
    }
    return "?";
}

float FacingYawRadians(Facing f) {
    // Positive yaw turns left (counter-clockwise seen from above).
    switch (f) {
        case Facing::North: return 0.0f;
        case Facing::East:  return -XM_PIDIV2;
        case Facing::South: return XM_PI;
        case Facing::West:  return XM_PIDIV2;
    }
    return 0.0f;
}

void BuildTPose(Facing facing, float heightMeters, ipc::DevicePose (&poses)[ipc::kMaxDevices]) {
    // Body proportions for a 1.70 m person, scaled by height. Positions are
    // in the person's own frame: +X = their right, -Z = their front.
    const float s = heightMeters / 1.70f;
    struct Part { float x, y, z; };
    const Part parts[ipc::kMaxDevices] = {
        {0.00f, 1.60f, 0.00f},    // HMD (eyes)
        {-0.80f, 1.40f, 0.00f},   // left hand, arm stretched sideways
        {0.80f, 1.40f, 0.00f},    // right hand
        {0.00f, 0.95f, 0.08f},    // tracker 0: waist (on the back)
        {-0.12f, 0.08f, 0.02f},   // tracker 1: left foot
        {0.12f, 0.08f, 0.02f},    // tracker 2: right foot
        {0.00f, 1.35f, 0.10f},    // tracker 3: chest (on the back)
        {-0.45f, 1.40f, 0.00f},   // tracker 4: left elbow
        {0.45f, 1.40f, 0.00f},    // tracker 5: right elbow
        {-0.12f, 0.50f, -0.05f},  // tracker 6: left knee
        {0.12f, 0.50f, -0.05f},   // tracker 7: right knee
    };

    const float    yaw    = FacingYawRadians(facing);
    const XMMATRIX rotate = XMMatrixRotationY(yaw);
    XMFLOAT4 q;
    XMStoreFloat4(&q, XMQuaternionRotationRollPitchYaw(0.0f, yaw, 0.0f));

    for (uint32_t slot = 0; slot < ipc::kMaxDevices; ++slot) {
        const Part& part = parts[slot];
        XMFLOAT3 p;
        // Feet stay on the floor regardless of height.
        const float y = (slot == ipc::TrackerSlot(1) || slot == ipc::TrackerSlot(2)) ? part.y : part.y * s;
        XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorSet(part.x * s, y, part.z * s, 1.0f), rotate));
        poses[slot] = ipc::MakePose(p.x, p.y, p.z, q.w, q.x, q.y, q.z);
    }
}

}  // namespace tubane::sandbox
