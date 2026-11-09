// Free-fly camera for the sandbox viewport.
//
//   RMB drag            look around
//   RMB + W/A/S/D/Q/E   fly (Shift = fast, wheel while RMB = change speed)
//   MMB drag            pan
//   Wheel               dolly forward / backward
//   Alt + LMB drag      orbit around the focus point
//   F                   focus selected device (handled by App via Focus())
#pragma once

#include <DirectXMath.h>

namespace tubane::sandbox {

struct CameraInput {
    float dt = 0.0f;
    float mouseDx = 0.0f, mouseDy = 0.0f;
    float wheel = 0.0f;
    bool  look = false;    // RMB held (started inside the viewport)
    bool  pan = false;     // MMB held
    bool  orbit = false;   // Alt + LMB held
    bool  forward = false, back = false, left = false, right = false, up = false, down = false;
    bool  fast = false;
};

class Camera {
public:
    Camera() { Reset(); }

    void Reset();
    void Update(const CameraInput& in);

    // Moves the camera so that `target` is centered at `distance` meters.
    void Focus(const DirectX::XMFLOAT3& target, float distance);
    void LookAt(const DirectX::XMFLOAT3& eye, const DirectX::XMFLOAT3& target);

    DirectX::XMMATRIX View() const;
    DirectX::XMMATRIX Projection(float aspect) const;

    DirectX::XMFLOAT3 Position() const { return position_; }
    DirectX::XMFLOAT3 Forward() const;
    DirectX::XMFLOAT3 Right() const;
    DirectX::XMFLOAT3 Up() const;

    float FovYRadians() const { return fovY_; }
    float NearZ() const { return nearZ_; }
    float FarZ() const { return farZ_; }
    float FlySpeed() const { return flySpeed_; }

private:
    DirectX::XMFLOAT3 position_{};
    float yaw_   = 0.0f;  // radians, 0 = looking toward -Z, positive = turn left
    float pitch_ = 0.0f;  // radians, positive = look up
    float orbitDistance_ = 3.0f;
    float flySpeed_ = 2.0f;  // m/s

    float fovY_  = 60.0f * 3.14159265f / 180.0f;
    float nearZ_ = 0.02f;
    float farZ_  = 200.0f;
};

}  // namespace tubane::sandbox
