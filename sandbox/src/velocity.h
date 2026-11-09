// Velocity estimation for poses published to the driver.
//
// SteamVR uses DriverPose_t velocities to predict poses (render-time
// prediction, thrown-object physics in games). Frames verified with
// tubane_vr_check:
//   vecVelocity         world frame   (m/s)
//   vecAngularVelocity  DEVICE-LOCAL frame, axis * angle (rad/s)
#pragma once

#include <DirectXMath.h>

#include <cmath>
#include <cstring>

#include "tubane/ipc_layout.h"

namespace tubane::sandbox {

struct VelocityOptions {
    float smoothing  = 0.5f;   // EMA weight of the newest sample (1 = no smoothing)
    float maxJump    = 0.5f;   // m per frame: larger = teleport (reset / preset), velocity 0
    float maxTurnRad = 1.6f;   // rad per frame: larger = teleport
};

// Instantaneous velocities between two poses `dt` seconds apart.
// Returns false (and zeros) if the change looks like a teleport.
inline bool PoseDelta(const ipc::DevicePose& prev, const ipc::DevicePose& cur, double dt, const VelocityOptions& o,
                      double linear[3], double angularLocal[3]) {
    using namespace DirectX;
    for (int i = 0; i < 3; ++i) linear[i] = angularLocal[i] = 0.0;
    if (dt <= 1e-5) return true;

    const double dx = cur.position[0] - prev.position[0];
    const double dy = cur.position[1] - prev.position[1];
    const double dz = cur.position[2] - prev.position[2];
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > o.maxJump) return false;

    // Local-frame delta: cur = prev * dLocal  ->  dLocal = prev^-1 * cur.
    const XMVECTOR qPrev = XMQuaternionNormalize(XMVectorSet(static_cast<float>(prev.rotation[1]), static_cast<float>(prev.rotation[2]),
                                                             static_cast<float>(prev.rotation[3]), static_cast<float>(prev.rotation[0])));
    const XMVECTOR qCur = XMQuaternionNormalize(XMVectorSet(static_cast<float>(cur.rotation[1]), static_cast<float>(cur.rotation[2]),
                                                            static_cast<float>(cur.rotation[3]), static_cast<float>(cur.rotation[0])));
    // XMQuaternionMultiply(a, b) = "rotate by a, then by b" = b * a.
    XMFLOAT4 d;
    XMStoreFloat4(&d, XMQuaternionMultiply(qCur, XMQuaternionInverse(qPrev)));
    if (d.w < 0.0f) {  // shortest arc
        d = {-d.x, -d.y, -d.z, -d.w};
    }
    const double w     = std::fmin(1.0, static_cast<double>(d.w));
    const double angle = 2.0 * std::acos(w);
    if (angle > o.maxTurnRad) return false;

    for (int i = 0; i < 3; ++i) linear[i] = (i == 0 ? dx : i == 1 ? dy : dz) / dt;
    const double s = std::sqrt(std::fmax(0.0, 1.0 - w * w));  // sin(angle / 2)
    if (s > 1e-6) {
        const double k = angle / s / dt;
        angularLocal[0] = d.x * k;
        angularLocal[1] = d.y * k;
        angularLocal[2] = d.z * k;
    }
    return true;
}

// Per-slot estimator: call once per published frame with the poses about to
// be published; fills their velocity / angularVelocity fields.
class VelocityEstimator {
public:
    void Reset() { valid_ = false; }

    void Apply(ipc::DevicePose (&poses)[ipc::kMaxDevices], double now, bool enabled, const VelocityOptions& o = {}) {
        const double dt = valid_ ? now - lastTime_ : 0.0;
        for (uint32_t slot = 0; slot < ipc::kMaxDevices; ++slot) {
            ipc::DevicePose& p = poses[slot];
            double lin[3] = {}, ang[3] = {};
            if (enabled && valid_) {
                const bool moved = std::memcmp(prev_[slot].position, p.position, sizeof(p.position)) != 0 ||
                                   std::memcmp(prev_[slot].rotation, p.rotation, sizeof(p.rotation)) != 0;
                if (moved && PoseDelta(prev_[slot], p, dt, o, lin, ang)) {
                    // Smooth while moving; stop instantly when the pose stops changing.
                    for (int i = 0; i < 3; ++i) {
                        lin[i] = o.smoothing * lin[i] + (1.0 - o.smoothing) * lastLin_[slot][i];
                        ang[i] = o.smoothing * ang[i] + (1.0 - o.smoothing) * lastAng_[slot][i];
                    }
                }
            }
            for (int i = 0; i < 3; ++i) {
                p.velocity[i]        = lin[i];
                p.angularVelocity[i] = ang[i];
                lastLin_[slot][i]    = lin[i];
                lastAng_[slot][i]    = ang[i];
            }
            prev_[slot] = p;
        }
        lastTime_ = now;
        valid_    = true;
    }

private:
    bool            valid_ = false;
    double          lastTime_ = 0.0;
    ipc::DevicePose prev_[ipc::kMaxDevices]{};
    double          lastLin_[ipc::kMaxDevices][3]{};
    double          lastAng_[ipc::kMaxDevices][3]{};
};

}  // namespace tubane::sandbox
