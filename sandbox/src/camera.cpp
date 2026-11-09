#include "camera.h"

#include <algorithm>
#include <cmath>

namespace tubane::sandbox {

using namespace DirectX;

namespace {
constexpr float kLookSensitivity = 0.0035f;  // rad per pixel
constexpr float kMaxPitch        = 1.55f;
}  // namespace

void Camera::Reset() {
    LookAt({1.5f, 1.75f, 1.9f}, {0.0f, 0.9f, 0.0f});
    flySpeed_ = 2.0f;
}

XMFLOAT3 Camera::Forward() const {
    const float cp = std::cos(pitch_);
    return {-std::sin(yaw_) * cp, std::sin(pitch_), -std::cos(yaw_) * cp};
}

XMFLOAT3 Camera::Right() const {
    return {std::cos(yaw_), 0.0f, -std::sin(yaw_)};
}

XMFLOAT3 Camera::Up() const {
    XMFLOAT3 up;
    const XMFLOAT3 f = Forward();
    const XMFLOAT3 r = Right();
    XMStoreFloat3(&up, XMVector3Cross(XMLoadFloat3(&r), XMLoadFloat3(&f)));
    return up;
}

void Camera::LookAt(const XMFLOAT3& eye, const XMFLOAT3& target) {
    position_ = eye;
    const XMVECTOR d = XMVectorSubtract(XMLoadFloat3(&target), XMLoadFloat3(&eye));
    XMFLOAT3 dir;
    XMStoreFloat3(&dir, XMVector3Normalize(d));
    orbitDistance_ = std::max(0.1f, XMVectorGetX(XMVector3Length(d)));
    pitch_ = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    yaw_   = std::atan2(-dir.x, -dir.z);
}

void Camera::Focus(const XMFLOAT3& target, float distance) {
    const XMFLOAT3 f = Forward();
    position_ = {target.x - f.x * distance, target.y - f.y * distance, target.z - f.z * distance};
    orbitDistance_ = distance;
}

void Camera::Update(const CameraInput& in) {
    if (in.look || in.orbit) {
        XMFLOAT3 pivot{};
        if (in.orbit) {
            const XMFLOAT3 f = Forward();
            pivot = {position_.x + f.x * orbitDistance_, position_.y + f.y * orbitDistance_,
                     position_.z + f.z * orbitDistance_};
        }
        yaw_   -= in.mouseDx * kLookSensitivity;
        pitch_ -= in.mouseDy * kLookSensitivity;
        pitch_  = std::clamp(pitch_, -kMaxPitch, kMaxPitch);
        if (in.orbit) {
            Focus(pivot, orbitDistance_);
        }
    }

    const XMFLOAT3 f = Forward();
    const XMFLOAT3 r = Right();
    const XMFLOAT3 u = Up();

    if (in.pan) {
        const float scale = 0.0015f * std::max(orbitDistance_, 0.5f);
        position_.x += (-r.x * in.mouseDx + u.x * in.mouseDy) * scale;
        position_.y += (-r.y * in.mouseDx + u.y * in.mouseDy) * scale;
        position_.z += (-r.z * in.mouseDx + u.z * in.mouseDy) * scale;
    }

    if (in.look) {
        // Wheel while flying changes the fly speed.
        if (in.wheel != 0.0f) {
            flySpeed_ = std::clamp(flySpeed_ * std::pow(1.2f, in.wheel), 0.1f, 50.0f);
        }
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (in.forward) z += 1.0f;
        if (in.back)    z -= 1.0f;
        if (in.right)   x += 1.0f;
        if (in.left)    x -= 1.0f;
        if (in.up)      y += 1.0f;
        if (in.down)    y -= 1.0f;
        const float speed = flySpeed_ * (in.fast ? 4.0f : 1.0f) * in.dt;
        position_.x += (f.x * z + r.x * x) * speed;
        position_.y += (f.y * z + r.y * x + y) * speed;
        position_.z += (f.z * z + r.z * x) * speed;
    } else if (in.wheel != 0.0f) {
        const float step = 0.15f * std::max(orbitDistance_, 0.3f) * in.wheel;
        position_.x += f.x * step;
        position_.y += f.y * step;
        position_.z += f.z * step;
        orbitDistance_ = std::max(0.1f, orbitDistance_ - step);
    }
}

XMMATRIX Camera::View() const {
    const XMFLOAT3 f = Forward();
    return XMMatrixLookToRH(XMLoadFloat3(&position_), XMLoadFloat3(&f), XMVectorSet(0, 1, 0, 0));
}

XMMATRIX Camera::Projection(float aspect) const {
    return XMMatrixPerspectiveFovRH(fovY_, std::max(aspect, 0.01f), nearZ_, farZ_);
}

}  // namespace tubane::sandbox
