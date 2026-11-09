// Hand skeleton synthesis for the virtual Index controllers.
//
// Each finger is blended between SteamVR's own open-hand and fist reference
// poses (hand_reference_poses.h, dumped from SteamVR) by its curl value:
// positions are lerped, rotations slerped, per bone, in parent space.
#pragma once

#include <openvr_driver.h>

namespace tubane::driver {

struct FingerCurls {
    float thumb  = 0.0f;  // 0 = open, 1 = fully curled
    float index  = 0.0f;
    float middle = 0.0f;
    float ring   = 0.0f;
    float pinky  = 0.0f;
};

inline constexpr uint32_t kHandBoneCount = 31;

void ComputeHandSkeleton(bool leftHand, const FingerCurls& curls, vr::VRBoneTransform_t (&out)[kHandBoneCount]);

}  // namespace tubane::driver
