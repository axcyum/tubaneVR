// Virtual Valve Index controller (left or right).
//
// Uses SteamVR's bundled {indexcontroller} resources: render model and input
// profile, so bindings/UI look exactly like a real Index controller.
#pragma once

#include "device_base.h"

namespace tubane::driver {

enum class Hand { Left, Right };

class ControllerDevice final : public DeviceBase {
public:
    explicit ControllerDevice(Hand hand);

    void UpdateInput(const ipc::ControllerInput& input) override;
    void Deactivate() override;

protected:
    void SetupProperties(vr::PropertyContainerHandle_t container) override;

private:
    struct BoolInput   { vr::VRInputComponentHandle_t handle; uint32_t bit; };

    enum ScalarIndex : int {
        kTriggerValue, kGripValue, kGripForce,
        kThumbstickX, kThumbstickY,
        kTrackpadX, kTrackpadY, kTrackpadForce,
        kFingerIndex, kFingerMiddle, kFingerRing, kFingerPinky,
        kScalarCount
    };

    void CreateInputComponents(vr::PropertyContainerHandle_t container);

    const Hand hand_;

    BoolInput                    bools_[12]{};
    int                          boolCount_ = 0;
    vr::VRInputComponentHandle_t scalars_[kScalarCount]{};
    vr::VRInputComponentHandle_t haptic_ = vr::k_ulInvalidInputComponentHandle;

    // Last values sent to SteamVR; only changes are forwarded.
    ipc::ControllerInput last_{};
    bool                 hasLast_ = false;
};

}  // namespace tubane::driver
