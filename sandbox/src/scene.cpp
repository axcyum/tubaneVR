#include "scene.h"

#include <cstdio>

namespace tubane::sandbox {

DeviceInfo DescribeSlot(uint32_t slot) {
    switch (slot) {
        case ipc::kSlotHmd:       return {slot, DeviceKind::Hmd, "HMD", {0.92f, 0.92f, 0.95f, 1.0f}};
        case ipc::kSlotLeftHand:  return {slot, DeviceKind::Controller, "Index Controller L", {0.30f, 0.62f, 1.00f, 1.0f}};
        case ipc::kSlotRightHand: return {slot, DeviceKind::Controller, "Index Controller R", {1.00f, 0.45f, 0.35f, 1.0f}};
        default: break;
    }
    static constexpr const char* kRoleNames[] = {"Waist", "Left Foot", "Right Foot", "Chest",
                                                 "Left Elbow", "Right Elbow", "Left Knee", "Right Knee"};
    const uint32_t index = slot - ipc::kSlotFirstTracker;
    char name[64];
    std::snprintf(name, sizeof(name), "Tracker %u (%s)", index, index < 8 ? kRoleNames[index] : "None");
    return {slot, DeviceKind::Tracker, name, {0.95f, 0.80f, 0.25f, 1.0f}};
}

const DeviceModel& ModelFor(DeviceKind kind, uint32_t slot) {
    // HMD: body + darker visor on the front (-Z) + strap ring.
    static const DeviceModel kHmd{
        {
            {{0.0f, 0.0f, 0.0f}, {0.19f, 0.11f, 0.09f}, 1.0f},
            {{0.0f, 0.0f, -0.05f}, {0.17f, 0.085f, 0.02f}, 0.25f},
            {{0.0f, 0.0f, 0.09f}, {0.16f, 0.03f, 0.10f}, 0.55f},
        },
        {0.0f, 0.0f, 0.0f},
        {0.10f, 0.06f, 0.09f},
    };
    // Index controller: handle along -Z with a head near the front, palm side
    // toward the inside of the hand (+X for left, -X for right).
    static const DeviceModel kLeft{
        {
            {{0.0f, -0.01f, 0.02f}, {0.035f, 0.045f, 0.13f}, 1.0f},
            {{0.0f, 0.02f, -0.06f}, {0.045f, 0.03f, 0.05f}, 0.75f},
            {{0.018f, 0.0f, 0.02f}, {0.006f, 0.05f, 0.12f}, 0.35f},  // strap
        },
        {0.0f, 0.0f, 0.0f},
        {0.03f, 0.035f, 0.09f},
    };
    static const DeviceModel kRight{
        {
            {{0.0f, -0.01f, 0.02f}, {0.035f, 0.045f, 0.13f}, 1.0f},
            {{0.0f, 0.02f, -0.06f}, {0.045f, 0.03f, 0.05f}, 0.75f},
            {{-0.018f, 0.0f, 0.02f}, {0.006f, 0.05f, 0.12f}, 0.35f},
        },
        {0.0f, 0.0f, 0.0f},
        {0.03f, 0.035f, 0.09f},
    };
    // Vive Tracker: puck + front pin (-Z) marking the LED/forward side.
    static const DeviceModel kTracker{
        {
            {{0.0f, 0.0f, 0.0f}, {0.095f, 0.035f, 0.095f}, 1.0f},
            {{0.0f, 0.0f, -0.055f}, {0.02f, 0.02f, 0.02f}, 0.45f},
            {{0.0f, -0.025f, 0.0f}, {0.03f, 0.015f, 0.03f}, 0.4f},
        },
        {0.0f, 0.0f, 0.0f},
        {0.05f, 0.03f, 0.06f},
    };

    switch (kind) {
        case DeviceKind::Hmd:        return kHmd;
        case DeviceKind::Controller: return slot == ipc::kSlotLeftHand ? kLeft : kRight;
        case DeviceKind::Tracker:    return kTracker;
    }
    return kTracker;
}

}  // namespace tubane::sandbox
