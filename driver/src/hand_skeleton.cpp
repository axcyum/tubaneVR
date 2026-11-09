#include "hand_skeleton.h"

#include <algorithm>
#include <cmath>

#include "hand_reference_poses.h"

namespace tubane::driver {

namespace {

vr::HmdQuaternionf_t Slerp(const vr::HmdQuaternionf_t& a, vr::HmdQuaternionf_t b, float t) {
    float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot < 0.0f) {  // shortest path
        b   = {-b.w, -b.x, -b.y, -b.z};
        dot = -dot;
    }
    float wa, wb;
    if (dot > 0.9995f) {  // nearly identical: lerp
        wa = 1.0f - t;
        wb = t;
    } else {
        const float theta = std::acos(dot);
        const float s     = std::sin(theta);
        wa = std::sin((1.0f - t) * theta) / s;
        wb = std::sin(t * theta) / s;
    }
    vr::HmdQuaternionf_t r{wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z};
    const float len = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
    if (len > 1e-6f) {
        r.w /= len; r.x /= len; r.y /= len; r.z /= len;
    }
    return r;
}

vr::VRBoneTransform_t Blend(const vr::VRBoneTransform_t& open, const vr::VRBoneTransform_t& fist, float t) {
    vr::VRBoneTransform_t r{};
    for (int i = 0; i < 3; ++i) r.position.v[i] = open.position.v[i] + (fist.position.v[i] - open.position.v[i]) * t;
    r.position.v[3] = 1.0f;
    r.orientation   = Slerp(open.orientation, fist.orientation, t);
    return r;
}

// Which curl drives each bone (0 thumb .. 4 pinky, -1 = hand/wrist).
constexpr int kBoneFinger[kHandBoneCount] = {
    -1, -1,                 // root, wrist
    0, 0, 0, 0,             // thumb 0-3
    1, 1, 1, 1, 1,          // index 0-4
    2, 2, 2, 2, 2,          // middle 0-4
    3, 3, 3, 3, 3,          // ring 0-4
    4, 4, 4, 4, 4,          // pinky 0-4
    0, 1, 2, 3, 4,          // aux thumb .. aux pinky
};

static_assert(hand_reference::kBoneCount == kHandBoneCount);

}  // namespace

void ComputeHandSkeleton(bool leftHand, const FingerCurls& curls, vr::VRBoneTransform_t (&out)[kHandBoneCount]) {
    const vr::VRBoneTransform_t* open = leftHand ? hand_reference::kLeftOpenHand : hand_reference::kRightOpenHand;
    const vr::VRBoneTransform_t* fist = leftHand ? hand_reference::kLeftFist : hand_reference::kRightFist;
    const float c[5] = {curls.thumb, curls.index, curls.middle, curls.ring, curls.pinky};

    for (uint32_t bone = 0; bone < kHandBoneCount; ++bone) {
        const int   finger = kBoneFinger[bone];
        const float t      = finger < 0 ? 0.0f : std::clamp(c[finger], 0.0f, 1.0f);
        out[bone]          = Blend(open[bone], fist[bone], t);
    }
}

}  // namespace tubane::driver
