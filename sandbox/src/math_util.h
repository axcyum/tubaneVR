// Small math helpers on top of DirectXMath.
//
// Convention (same as OpenVR / tubane IPC): right-handed, +Y up, -Z forward,
// meters. DirectXMath matrices are row-major with row vectors (v' = v * M),
// which is also the memory layout ImGuizmo expects (translation in [12..14]).
#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

#include "tubane/ipc_layout.h"

namespace tubane::sandbox {

using namespace DirectX;

inline constexpr float kPi      = 3.14159265358979323846f;
inline constexpr float kDegToRad = kPi / 180.0f;
inline constexpr float kRadToDeg = 180.0f / kPi;

// --- IPC <-> DirectXMath ------------------------------------------------------
// IPC quaternions are (w, x, y, z); XMFLOAT4 quaternions are (x, y, z, w).
inline XMFLOAT3 PositionOf(const ipc::DevicePose& p) {
    return {static_cast<float>(p.position[0]), static_cast<float>(p.position[1]), static_cast<float>(p.position[2])};
}
inline XMFLOAT4 RotationOf(const ipc::DevicePose& p) {
    return {static_cast<float>(p.rotation[1]), static_cast<float>(p.rotation[2]), static_cast<float>(p.rotation[3]),
            static_cast<float>(p.rotation[0])};
}
inline void SetPosition(ipc::DevicePose& p, const XMFLOAT3& v) {
    p.position[0] = v.x;
    p.position[1] = v.y;
    p.position[2] = v.z;
}
inline void SetRotation(ipc::DevicePose& p, FXMVECTOR q) {
    XMFLOAT4 f;
    XMStoreFloat4(&f, XMQuaternionNormalize(q));
    p.rotation[0] = f.w;
    p.rotation[1] = f.x;
    p.rotation[2] = f.y;
    p.rotation[3] = f.z;
}

// World matrix (rotation then translation) of a pose.
inline XMMATRIX PoseMatrix(const ipc::DevicePose& p) {
    const XMFLOAT3 pos = PositionOf(p);
    const XMFLOAT4 rot = RotationOf(p);
    XMVECTOR q = XMLoadFloat4(&rot);
    if (XMVectorGetX(XMVector4LengthSq(q)) < 1e-8f) {
        q = XMQuaternionIdentity();
    }
    return XMMatrixRotationQuaternion(XMQuaternionNormalize(q)) * XMMatrixTranslation(pos.x, pos.y, pos.z);
}

// --- Euler angles ---------------------------------------------------------------
// Degrees, x = pitch (about X), y = yaw (about Y), z = roll (about Z), applied
// roll -> pitch -> yaw (XMQuaternionRotationRollPitchYaw order).
inline XMVECTOR QuaternionFromEulerDeg(const XMFLOAT3& deg) {
    return XMQuaternionRotationRollPitchYaw(deg.x * kDegToRad, deg.y * kDegToRad, deg.z * kDegToRad);
}

inline XMFLOAT3 EulerDegFromQuaternion(FXMVECTOR q) {
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(q));
    // M = Rz(roll) * Rx(pitch) * Ry(yaw) for row vectors.
    const float sp = std::clamp(-m._32, -1.0f, 1.0f);
    const float pitch = std::asin(sp);
    float yaw, roll;
    if (std::fabs(sp) < 0.9999f) {
        yaw  = std::atan2(m._31, m._33);
        roll = std::atan2(m._12, m._22);
    } else {  // gimbal lock: fold roll into yaw
        yaw  = std::atan2(-m._13, m._11);
        roll = 0.0f;
    }
    return {pitch * kRadToDeg, yaw * kRadToDeg, roll * kRadToDeg};
}

// --- Picking ----------------------------------------------------------------------
struct Ray {
    XMFLOAT3 origin;
    XMFLOAT3 direction;  // normalized
};

// Ray vs. oriented box given as world matrix (rotation + translation only),
// local center offset and half extents. Returns hit distance or < 0.
inline float RayBoxDistance(const Ray& ray, FXMMATRIX world, const XMFLOAT3& center, const XMFLOAT3& halfExtents) {
    const XMMATRIX inv = XMMatrixInverse(nullptr, world);
    XMFLOAT3 o, d;
    XMStoreFloat3(&o, XMVector3TransformCoord(XMLoadFloat3(&ray.origin), inv));
    XMStoreFloat3(&d, XMVector3TransformNormal(XMLoadFloat3(&ray.direction), inv));

    const float origin[3] = {o.x - center.x, o.y - center.y, o.z - center.z};
    const float dir[3]    = {d.x, d.y, d.z};
    const float half[3]   = {halfExtents.x, halfExtents.y, halfExtents.z};

    float tMin = 0.0f;
    float tMax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dir[i]) < 1e-8f) {
            if (origin[i] < -half[i] || origin[i] > half[i]) return -1.0f;
            continue;
        }
        float t1 = (-half[i] - origin[i]) / dir[i];
        float t2 = (half[i] - origin[i]) / dir[i];
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return -1.0f;
    }
    return tMin;
}

}  // namespace tubane::sandbox
