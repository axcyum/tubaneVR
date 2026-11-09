#include "controller_device.h"

#include <cmath>
#include <iterator>
#include <type_traits>

#include "driver_log.h"

namespace tubane::driver {

ControllerDevice::ControllerDevice(Hand hand)
    : DeviceBase(hand == Hand::Left ? ipc::kSlotLeftHand : ipc::kSlotRightHand,
                 hand == Hand::Left ? "TUBANE-KNU-L" : "TUBANE-KNU-R",
                 vr::TrackedDeviceClass_Controller),
      hand_(hand) {}

void ControllerDevice::SetupProperties(vr::PropertyContainerHandle_t container) {
    auto*      props = vr::VRProperties();
    const bool left  = hand_ == Hand::Left;

    SetCommonProperties(container, "Valve", left ? "Knuckles Left" : "Knuckles Right",
                        left ? "valve/index_controllerTUBANE-L" : "valve/index_controllerTUBANE-R");

    props->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32,
                            left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
    props->SetStringProperty(container, vr::Prop_ControllerType_String, "knuckles");
    props->SetStringProperty(container, vr::Prop_InputProfilePath_String,
                             "{indexcontroller}/input/index_controller_profile.json");
    props->SetStringProperty(container, vr::Prop_RenderModelName_String,
                             left ? "{indexcontroller}valve_controller_knu_1_0_left"
                                  : "{indexcontroller}valve_controller_knu_1_0_right");
    props->SetInt32Property(container, vr::Prop_ControllerHandSelectionPriority_Int32, 0);

    CreateInputComponents(container);
}

void ControllerDevice::CreateInputComponents(vr::PropertyContainerHandle_t container) {
    auto* input = vr::VRDriverInput();

    struct BoolDef { const char* path; uint32_t bit; };
    static constexpr BoolDef kBools[] = {
        {"/input/system/click",     ipc::kBtnSystemClick},
        {"/input/system/touch",     ipc::kBtnSystemTouch},
        {"/input/a/click",          ipc::kBtnAClick},
        {"/input/a/touch",          ipc::kBtnATouch},
        {"/input/b/click",          ipc::kBtnBClick},
        {"/input/b/touch",          ipc::kBtnBTouch},
        {"/input/trigger/click",    ipc::kBtnTriggerClick},
        {"/input/trigger/touch",    ipc::kBtnTriggerTouch},
        {"/input/grip/touch",       ipc::kBtnGripTouch},
        {"/input/thumbstick/click", ipc::kBtnThumbstickClick},
        {"/input/thumbstick/touch", ipc::kBtnThumbstickTouch},
        {"/input/trackpad/touch",   ipc::kBtnTrackpadTouch},
    };
    static_assert(std::size(kBools) <= std::extent_v<decltype(bools_)>);

    boolCount_ = 0;
    for (const BoolDef& def : kBools) {
        BoolInput& b = bools_[boolCount_++];
        b.bit        = def.bit;
        input->CreateBooleanComponent(container, def.path, &b.handle);
    }

    struct ScalarDef { ScalarIndex index; const char* path; vr::EVRScalarUnits units; };
    static constexpr ScalarDef kScalars[] = {
        {kTriggerValue,  "/input/trigger/value",  vr::VRScalarUnits_NormalizedOneSided},
        {kGripValue,     "/input/grip/value",     vr::VRScalarUnits_NormalizedOneSided},
        {kGripForce,     "/input/grip/force",     vr::VRScalarUnits_NormalizedOneSided},
        {kThumbstickX,   "/input/thumbstick/x",   vr::VRScalarUnits_NormalizedTwoSided},
        {kThumbstickY,   "/input/thumbstick/y",   vr::VRScalarUnits_NormalizedTwoSided},
        {kTrackpadX,     "/input/trackpad/x",     vr::VRScalarUnits_NormalizedTwoSided},
        {kTrackpadY,     "/input/trackpad/y",     vr::VRScalarUnits_NormalizedTwoSided},
        {kTrackpadForce, "/input/trackpad/force", vr::VRScalarUnits_NormalizedOneSided},
        {kFingerIndex,   "/input/finger/index",   vr::VRScalarUnits_NormalizedOneSided},
        {kFingerMiddle,  "/input/finger/middle",  vr::VRScalarUnits_NormalizedOneSided},
        {kFingerRing,    "/input/finger/ring",    vr::VRScalarUnits_NormalizedOneSided},
        {kFingerPinky,   "/input/finger/pinky",   vr::VRScalarUnits_NormalizedOneSided},
    };
    for (const ScalarDef& def : kScalars) {
        input->CreateScalarComponent(container, def.path, &scalars_[def.index], vr::VRScalarType_Absolute,
                                     def.units);
    }

    input->CreateHapticComponent(container, "/output/haptic", &haptic_);

    // TODO(Step 2/3): skeletal input via CreateSkeletonComponent
    // ("/input/skeleton/left|right", "/skeleton/hand/left|right") once the
    // sandbox can author finger curls -> bone transforms.

    hasLast_ = false;
}

void ControllerDevice::UpdateInput(const ipc::ControllerInput& in) {
    if (!IsActive()) {
        return;
    }
    auto* input = vr::VRDriverInput();

    for (int i = 0; i < boolCount_; ++i) {
        const bool now = (in.buttons & bools_[i].bit) != 0;
        const bool was = (last_.buttons & bools_[i].bit) != 0;
        if (!hasLast_ || now != was) {
            input->UpdateBooleanComponent(bools_[i].handle, now, 0.0);
        }
    }

    const float values[kScalarCount] = {
        in.trigger,   in.gripValue,  in.gripForce,
        in.thumbstickX, in.thumbstickY,
        in.trackpadX, in.trackpadY,  in.trackpadForce,
        in.finger[0], in.finger[1],  in.finger[2], in.finger[3],
    };
    const float previous[kScalarCount] = {
        last_.trigger,   last_.gripValue,  last_.gripForce,
        last_.thumbstickX, last_.thumbstickY,
        last_.trackpadX, last_.trackpadY,  last_.trackpadForce,
        last_.finger[0], last_.finger[1],  last_.finger[2], last_.finger[3],
    };
    for (int i = 0; i < kScalarCount; ++i) {
        if (!hasLast_ || std::fabs(values[i] - previous[i]) > 1e-5f) {
            input->UpdateScalarComponent(scalars_[i], values[i], 0.0);
        }
    }

    last_    = in;
    hasLast_ = true;
}

void ControllerDevice::Deactivate() {
    hasLast_ = false;
    DeviceBase::Deactivate();
}

}  // namespace tubane::driver
