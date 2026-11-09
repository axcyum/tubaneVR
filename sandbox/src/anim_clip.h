// Unity AnimationClip (.anim, YAML text serialization) loader and sampler.
//
// Two kinds of clips are mapped onto the virtual devices:
//
//  * Humanoid clips: the IK goal curves Unity bakes into every humanoid clip
//      RootT/RootQ           -> tracker 0 (waist)
//      LeftHandT/Q, Right..  -> left / right controller
//      LeftFootT/Q, Right..  -> tracker 1 / 2 (feet)
//      HMD                   -> estimated: RootT + RootQ * (0, headOffset, 0)
//    Values are in Unity's normalized humanoid units (scaled by `scale`).
//
//  * Generic clips: position / rotation curves whose object name looks like
//    a device ("Head", "HMD", "LeftHand", "Hips", "RightFoot", ...). The
//    values are used as world transforms, which is right for objects at the
//    root of the animated hierarchy (e.g. recorded VR rigs).
//
// Unity is left-handed (+Z forward); everything is converted to OpenVR
// space (right-handed, -Z forward): p = (x, y, -z), q = (-x, -y, z, w).
// Curves are sampled with linear interpolation (tangents are ignored), so
// densely keyed / baked clips reproduce best.
#pragma once

#include <DirectXMath.h>

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "tubane/ipc_layout.h"

namespace tubane::sandbox {

struct FloatCurve {
    std::vector<float> times;
    std::vector<float> values;
    float Evaluate(float t) const;
};

struct DeviceBinding {
    bool        hasPosition = false;
    bool        hasRotation = false;
    std::string posKey[3];  // keys into AnimClip::curves
    std::string rotKey[4];  // x, y, z, w
    std::string source;     // human-readable origin ("LeftHandT/Q", "Armature/Head", ...)
};

struct AnimClip {
    enum class Kind { Humanoid, Generic };

    std::string name;
    Kind        kind = Kind::Generic;
    float       duration = 0.0f;
    std::map<std::string, FloatCurve> curves;
    std::array<DeviceBinding, ipc::kMaxDevices> bindings{};
    bool        hmdEstimated = false;  // humanoid: HMD derived from RootT/RootQ
    std::vector<std::string> warnings;

    bool Empty() const;
};

bool LoadUnityAnim(const std::filesystem::path& path, AnimClip& clip, std::string& error);

struct AnimSampleOptions {
    float scale      = 1.0f;   // multiplies all positions
    float headOffset = 0.6f;   // humanoid HMD estimate: meters above RootT (before scale)
    float yaw        = 0.0f;   // radians, rotates the whole animation about the origin
    DirectX::XMFLOAT3 offset{0.0f, 0.0f, 0.0f};  // added after rotation (meters)
};

// Writes the pose of every bound device at time `t` (OpenVR space).
// `driven[slot]` tells which slots were written.
void SampleClip(const AnimClip& clip, float t, const AnimSampleOptions& options,
                ipc::DevicePose (&poses)[ipc::kMaxDevices], bool (&driven)[ipc::kMaxDevices]);

}  // namespace tubane::sandbox
