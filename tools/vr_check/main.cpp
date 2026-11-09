// tubane_vr_check: end-to-end verification through SteamVR.
//
//   tubane_vr_check            run all checks
//   tubane_vr_check input      controller input: shared memory -> driver -> SteamVR -> IVRInput
//   tubane_vr_check velocity   which frame SteamVR uses for DriverPose velocities
//   tubane_vr_check motion     sandbox velocity estimator -> driver -> SteamVR (world values)
//   tubane_vr_check skeleton   finger curls -> driver hand skeleton -> SteamVR finger curl summary
//   tubane_vr_check skeleton-dump <out.h>
//                              regenerate driver/src/hand_reference_poses.h from SteamVR
//
// The tool temporarily becomes the writer of the shared ControlBlock, so
// tubane-sandbox must not be running. The original block is restored at exit.
// Requires SteamVR running with the tubane driver.
#include <windows.h>

#include <openvr.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "tubane/shm.h"
#include "velocity.h"  // sandbox/src - the exact estimator the sandbox uses

using namespace tubane::ipc;

namespace {

SharedState* g_state = nullptr;

void Sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string ExeDir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    return s.substr(0, s.find_last_of("\\/") + 1);
}

bool SandboxRunning() {
    ControlBlock a{}, b{};
    ReadControl(*g_state, a);
    Sleep_ms(400);
    ReadControl(*g_state, b);
    return a.sandboxHeartbeat != b.sandboxHeartbeat;
}

// ===========================================================================
// Input check
// ===========================================================================
struct ActionValues {
    bool  active = false;
    bool  a = false, b = false, triggerClick = false, stickClick = false, padTouch = false;
    float trigger = 0, grip = 0;
    float stick[2] = {}, pad[2] = {};
};

struct Actions {
    vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t    a, b, trigger, triggerClick, grip, stick, stickClick, pad, padTouch;
    vr::VRInputValueHandle_t hand[2] = {vr::k_ulInvalidInputValueHandle, vr::k_ulInvalidInputValueHandle};
    vr::VRActionHandle_t     skeleton[2] = {vr::k_ulInvalidActionHandle, vr::k_ulInvalidActionHandle};

    bool Init() {
        auto* in = vr::VRInput();
        const std::string manifest = ExeDir() + "actions.json";
        if (in->SetActionManifestPath(manifest.c_str()) != vr::VRInputError_None) {
            std::printf("SetActionManifestPath failed (%s)\n", manifest.c_str());
            return false;
        }
        in->GetActionSetHandle("/actions/check", &set);
        in->GetActionHandle("/actions/check/in/a", &a);
        in->GetActionHandle("/actions/check/in/b", &b);
        in->GetActionHandle("/actions/check/in/trigger", &trigger);
        in->GetActionHandle("/actions/check/in/trigger_click", &triggerClick);
        in->GetActionHandle("/actions/check/in/grip", &grip);
        in->GetActionHandle("/actions/check/in/thumbstick", &stick);
        in->GetActionHandle("/actions/check/in/thumbstick_click", &stickClick);
        in->GetActionHandle("/actions/check/in/trackpad", &pad);
        in->GetActionHandle("/actions/check/in/trackpad_touch", &padTouch);
        in->GetActionHandle("/actions/check/in/skeleton_left", &skeleton[0]);
        in->GetActionHandle("/actions/check/in/skeleton_right", &skeleton[1]);
        in->GetInputSourceHandle("/user/hand/left", &hand[0]);
        in->GetInputSourceHandle("/user/hand/right", &hand[1]);
        return true;
    }

    void Update() const {
        vr::VRActiveActionSet_t active{};
        active.ulActionSet = set;
        vr::VRInput()->UpdateActionState(&active, sizeof(active), 1);
    }

    ActionValues Read(int h) const {
        ActionValues v;
        auto* in = vr::VRInput();
        auto digital = [&](vr::VRActionHandle_t action, bool& out) {
            vr::InputDigitalActionData_t d{};
            in->GetDigitalActionData(action, &d, sizeof(d), hand[h]);
            out = d.bActive && d.bState;
            v.active = v.active || d.bActive;
        };
        auto analog = [&](vr::VRActionHandle_t action, float* out, int n) {
            vr::InputAnalogActionData_t d{};
            in->GetAnalogActionData(action, &d, sizeof(d), hand[h]);
            out[0] = d.x;
            if (n > 1) out[1] = d.y;
            v.active = v.active || d.bActive;
        };
        digital(a, v.a);
        digital(b, v.b);
        digital(triggerClick, v.triggerClick);
        digital(stickClick, v.stickClick);
        digital(padTouch, v.padTouch);
        analog(trigger, &v.trigger, 1);
        analog(grip, &v.grip, 1);
        analog(stick, v.stick, 2);
        analog(pad, v.pad, 2);
        return v;
    }
};

void SetInput(int hand, const ControllerInput& in) {
    WriteControl(*g_state, [&](ControlBlock& c) {
        std::memset(c.controllers, 0, sizeof(c.controllers));
        c.controllers[hand] = in;
    });
}

// Waits for the driver (RunFrame) and SteamVR to pick up the new state.
ActionValues Settle(const Actions& actions, int hand) {
    ActionValues v;
    for (int i = 0; i < 15; ++i) {
        Sleep_ms(20);
        actions.Update();
        v = actions.Read(hand);
    }
    return v;
}

bool Near(float a, float b, float eps = 0.03f) { return std::fabs(a - b) <= eps; }

struct InputCase {
    const char* name;
    std::function<void(ControllerInput&)> set;
    std::function<bool(const ActionValues&)> check;
};

int RunInputCheck() {
    std::printf("\n=== Controller input: shared memory -> driver -> SteamVR -> IVRInput ===\n");
    Actions actions;
    if (!actions.Init()) return 1;

    const std::vector<InputCase> cases = {
        {"A click", [](ControllerInput& c) { c.buttons = kBtnAClick | kBtnATouch; },
         [](const ActionValues& v) { return v.a && !v.b; }},
        {"B click", [](ControllerInput& c) { c.buttons = kBtnBClick | kBtnBTouch; },
         [](const ActionValues& v) { return v.b && !v.a; }},
        {"Trigger 0.50", [](ControllerInput& c) { c.trigger = 0.5f; c.buttons = kBtnTriggerTouch; },
         [](const ActionValues& v) { return Near(v.trigger, 0.5f) && !v.triggerClick; }},
        {"Trigger 1.00 + click", [](ControllerInput& c) { c.trigger = 1.0f; c.buttons = kBtnTriggerTouch | kBtnTriggerClick; },
         [](const ActionValues& v) { return Near(v.trigger, 1.0f) && v.triggerClick; }},
        {"Grip 0.80", [](ControllerInput& c) { c.gripValue = 0.8f; c.buttons = kBtnGripTouch; },
         [](const ActionValues& v) { return Near(v.grip, 0.8f); }},
        {"Thumbstick (+0.60, -0.40)", [](ControllerInput& c) { c.thumbstickX = 0.6f; c.thumbstickY = -0.4f; c.buttons = kBtnThumbstickTouch; },
         [](const ActionValues& v) { return Near(v.stick[0], 0.6f) && Near(v.stick[1], -0.4f); }},
        {"Thumbstick click", [](ControllerInput& c) { c.buttons = kBtnThumbstickClick | kBtnThumbstickTouch; },
         [](const ActionValues& v) { return v.stickClick; }},
        {"Trackpad (-0.50, +0.50) touch", [](ControllerInput& c) { c.trackpadX = -0.5f; c.trackpadY = 0.5f; c.buttons = kBtnTrackpadTouch; },
         [](const ActionValues& v) { return v.padTouch && Near(v.pad[0], -0.5f) && Near(v.pad[1], 0.5f); }},
        {"Released", [](ControllerInput&) {},
         [](const ActionValues& v) { return !v.a && !v.b && v.trigger < 0.05f && v.grip < 0.05f && !v.padTouch; }},
    };

    // While the SteamVR dashboard is open it owns all controller input.
    if (vr::VROverlay() && vr::VROverlay()->IsDashboardVisible()) {
        std::printf("SteamVR dashboard is open - it captures controller input. Close it "
                    "(vrcmd --hidedashboard) and retry.\n");
    }

    // SteamVR loads the action manifest / default bindings asynchronously.
    bool ready = false;
    for (int i = 0; i < 100 && !ready; ++i) {
        Sleep_ms(50);
        actions.Update();
        ready = actions.Read(0).active || actions.Read(1).active;
    }
    std::printf("bindings %s\n", ready ? "active" : "NOT active after 5 s");

    int failures = 0;
    bool anyActive = false;
    for (int hand = 0; hand < 2; ++hand) {
        std::printf("\n[%s hand]\n", hand == 0 ? "Left" : "Right");
        for (const InputCase& tc : cases) {
            ControllerInput in{};
            tc.set(in);
            SetInput(hand, in);
            const ActionValues v = Settle(actions, hand);
            anyActive = anyActive || v.active;
            const bool ok = v.active && tc.check(v);
            failures += ok ? 0 : 1;
            std::printf("  %-32s %s   a=%d b=%d trg=%.2f%s grip=%.2f stick=(%+.2f %+.2f)%s pad=(%+.2f %+.2f)%s%s\n", tc.name,
                        ok ? "PASS" : "FAIL", v.a, v.b, v.trigger, v.triggerClick ? "(click)" : "", v.grip, v.stick[0],
                        v.stick[1], v.stickClick ? "(click)" : "", v.pad[0], v.pad[1], v.padTouch ? "(touch)" : "",
                        v.active ? "" : "  [action inactive]");
        }
        SetInput(hand, ControllerInput{});
    }
    if (!anyActive) {
        std::printf("\nAll actions inactive: this application does not have input focus\n"
                    "(another VR application such as SteamVR Home owns it). Close it and retry.\n");
    }
    std::printf("\nInput check: %s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

// ===========================================================================
// Velocity frame check
// ===========================================================================
int RunVelocityCheck() {
    std::printf("\n=== Pose velocity: which frame does SteamVR use? ===\n");
    // HMD pitched +90 deg about X (looking straight up): its local +Y axis
    // points to world +Z. Linear velocity +X, angular velocity +Y.
    const double s = std::sqrt(0.5);
    WriteControl(*g_state, [&](ControlBlock& c) {
        DevicePose& p = c.poses[kSlotHmd];
        p.position[0] = 0.0; p.position[1] = 1.5; p.position[2] = 0.0;
        p.rotation[0] = s; p.rotation[1] = s; p.rotation[2] = 0.0; p.rotation[3] = 0.0;
        p.velocity[0] = 0.5; p.velocity[1] = 0.0; p.velocity[2] = 0.0;
        p.angularVelocity[0] = 0.0; p.angularVelocity[1] = 1.0; p.angularVelocity[2] = 0.0;
        p.flags = kPoseValid | kPoseConnected;
    });
    Sleep_ms(300);

    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0.0f, poses,
                                                    vr::k_unMaxTrackedDeviceCount);
    const vr::TrackedDevicePose_t& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
    const auto& v = hmd.vVelocity.v;
    const auto& w = hmd.vAngularVelocity.v;
    std::printf("  sent:     velocity (+0.50 +0.00 +0.00)  angular (+0.00 +1.00 +0.00)  [world axes]\n");
    std::printf("  received: velocity (%+.2f %+.2f %+.2f)  angular (%+.2f %+.2f %+.2f)  valid=%d\n", v[0], v[1], v[2],
                w[0], w[1], w[2], hmd.bPoseIsValid);

    const bool linWorld = std::fabs(v[0] - 0.5f) < 0.05f && std::fabs(v[1]) < 0.05f && std::fabs(v[2]) < 0.05f;
    const bool angWorld = std::fabs(w[1] - 1.0f) < 0.05f && std::fabs(w[0]) < 0.05f && std::fabs(w[2]) < 0.05f;
    const bool angLocal = std::fabs(std::fabs(w[2]) - 1.0f) < 0.05f && std::fabs(w[0]) < 0.05f && std::fabs(w[1]) < 0.05f;
    std::printf("  linear velocity frame : %s\n", linWorld ? "WORLD" : "unexpected");
    std::printf("  angular velocity frame: %s\n", angWorld ? "WORLD" : angLocal ? "LOCAL (device)" : "unexpected");
    return (linWorld && (angWorld || angLocal)) ? 0 : 1;
}

// ===========================================================================
// Skeleton reference dump (generates driver/src/hand_reference_poses.h)
// ===========================================================================
bool WaitSkeletonActive(const Actions& actions) {
    for (int i = 0; i < 100; ++i) {
        Sleep_ms(50);
        actions.Update();
        vr::InputSkeletalActionData_t l{}, r{};
        vr::VRInput()->GetSkeletalActionData(actions.skeleton[0], &l, sizeof(l));
        vr::VRInput()->GetSkeletalActionData(actions.skeleton[1], &r, sizeof(r));
        if (l.bActive && r.bActive) return true;
    }
    return false;
}

int RunSkeletonDump(const std::string& outPath) {
    std::printf("\n=== Skeleton reference poses -> %s ===\n", outPath.c_str());
    Actions actions;
    if (!actions.Init()) return 1;
    if (!WaitSkeletonActive(actions)) {
        std::printf("skeleton actions not active (dashboard open? driver without skeleton component?)\n");
        return 1;
    }

    struct Dump { const char* name; int hand; vr::EVRSkeletalReferencePose pose; };
    const Dump dumps[] = {
        {"kLeftOpenHand", 0, vr::VRSkeletalReferencePose_OpenHand},
        {"kLeftFist", 0, vr::VRSkeletalReferencePose_Fist},
        {"kRightOpenHand", 1, vr::VRSkeletalReferencePose_OpenHand},
        {"kRightFist", 1, vr::VRSkeletalReferencePose_Fist},
    };

    FILE* f = nullptr;
    if (fopen_s(&f, outPath.c_str(), "w") != 0 || !f) {
        std::printf("cannot write %s\n", outPath.c_str());
        return 1;
    }
    std::fprintf(f,
                 "// GENERATED by `tubane_vr_check skeleton-dump` - do not edit by hand.\n"
                 "//\n"
                 "// SteamVR's reference poses for the 31-bone OpenVR hand skeleton\n"
                 "// (IVRInput::GetSkeletalReferenceTransforms, VRSkeletalTransformSpace_Parent),\n"
                 "// i.e. the open hand / fist SteamVR itself defines for hand skeletons.\n"
                 "// Bone order: 0 root, 1 wrist, 2-5 thumb, 6-10 index, 11-15 middle,\n"
                 "// 16-20 ring, 21-25 pinky, 26-30 aux (thumb..pinky).\n"
                 "#pragma once\n\n#include <openvr_driver.h>\n\nnamespace tubane::driver::hand_reference {\n\n"
                 "inline constexpr uint32_t kBoneCount = 31;\n\n");

    for (const Dump& d : dumps) {
        uint32_t count = 0;
        vr::VRInput()->GetBoneCount(actions.skeleton[d.hand], &count);
        vr::VRBoneTransform_t bones[31] = {};
        const vr::EVRInputError err = vr::VRInput()->GetSkeletalReferenceTransforms(
            actions.skeleton[d.hand], vr::VRSkeletalTransformSpace_Parent, d.pose, bones, 31);
        std::printf("  %-16s bones=%u err=%d\n", d.name, count, static_cast<int>(err));
        if (err != vr::VRInputError_None || count != 31) {
            std::fclose(f);
            return 1;
        }
        std::fprintf(f, "inline constexpr vr::VRBoneTransform_t %s[kBoneCount] = {\n", d.name);
        for (const vr::VRBoneTransform_t& b : bones) {
            std::fprintf(f, "    {{%.7ff, %.7ff, %.7ff, %.7ff}, {%.7ff, %.7ff, %.7ff, %.7ff}},\n", b.position.v[0],
                         b.position.v[1], b.position.v[2], b.position.v[3], b.orientation.w, b.orientation.x,
                         b.orientation.y, b.orientation.z);
        }
        std::fprintf(f, "};\n\n");
    }
    std::fprintf(f, "}  // namespace tubane::driver::hand_reference\n");
    std::fclose(f);
    std::printf("written.\n");
    return 0;
}

// ===========================================================================
// Skeleton check: finger curls -> driver bones -> SteamVR summary curls
// ===========================================================================
int RunSkeletonCheck() {
    std::printf("\n=== Skeleton: finger curls -> driver hand skeleton -> SteamVR ===\n");
    Actions actions;
    if (!actions.Init()) return 1;
    if (!WaitSkeletonActive(actions)) {
        std::printf("skeleton actions not active (dashboard open?)\n");
        return 1;
    }

    // expected curl per finger: 0 = open (< 0.25), 1 = curled (> 0.75), 2 = half (0.25..0.75)
    struct SkelCase {
        const char* name;
        float       fingers[4];
        uint32_t    buttons;
        float       thumbOverride;  // < 0 = auto (from touches)
        int         expect[5];      // thumb, index, middle, ring, pinky
    };
    const SkelCase cases[] = {
        {"Open hand", {0, 0, 0, 0}, 0, -1, {0, 0, 0, 0, 0}},
        {"Point (index open, others curled)", {0, 1, 1, 1}, 0, -1, {0, 0, 1, 1, 1}},
        {"Index curled only", {1, 0, 0, 0}, 0, -1, {0, 1, 0, 0, 0}},
        {"Fist + thumb pressed (A)", {1, 1, 1, 1}, kBtnAClick | kBtnATouch, -1, {1, 1, 1, 1, 1}},
        {"Half curl", {0.5f, 0.5f, 0.5f, 0.5f}, 0, -1, {0, 2, 2, 2, 2}},
        {"Thumbs up (override thumb 0)", {1, 1, 1, 1}, kBtnAClick | kBtnATouch, 0.0f, {0, 1, 1, 1, 1}},
        {"Thumb only (override thumb 1)", {0, 0, 0, 0}, 0, 1.0f, {1, 0, 0, 0, 0}},
    };
    const char* fingerNames[5] = {"thumb", "index", "middle", "ring", "pinky"};

    int failures = 0;
    for (int hand = 0; hand < 2; ++hand) {
        std::printf("\n[%s hand]\n", hand == 0 ? "Left" : "Right");
        for (const SkelCase& sc : cases) {
            ControllerInput in{};
            std::copy(std::begin(sc.fingers), std::end(sc.fingers), in.finger);
            in.buttons = sc.buttons;
            if (sc.thumbOverride >= 0.0f) {
                in.thumbCurl   = sc.thumbOverride;
                in.fingerFlags = kFingerThumbOverride;
            }
            SetInput(hand, in);
            Settle(actions, hand);

            vr::VRSkeletalSummaryData_t s{};
            const vr::EVRInputError err =
                vr::VRInput()->GetSkeletalSummaryData(actions.skeleton[hand], vr::VRSummaryType_FromAnimation, &s);
            bool ok = err == vr::VRInputError_None;
            for (int f = 0; f < 5; ++f) {
                const float c = s.flFingerCurl[f];
                const bool  fine = sc.expect[f] == 0 ? c < 0.25f : sc.expect[f] == 1 ? c > 0.75f : (c > 0.25f && c < 0.75f);
                ok = ok && fine;
            }
            failures += ok ? 0 : 1;
            std::printf("  %-36s %s  curls:", sc.name, ok ? "PASS" : "FAIL");
            for (int f = 0; f < 5; ++f) std::printf(" %s=%.2f", fingerNames[f], s.flFingerCurl[f]);
            std::printf("%s\n", err == vr::VRInputError_None ? "" : "  [summary error]");
        }
        SetInput(hand, ControllerInput{});
    }
    std::printf("\nSkeleton check: %s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

// ===========================================================================
// Motion check: sandbox velocity estimation end to end
// ===========================================================================
int RunMotionCheck() {
    std::printf("\n=== Motion: sandbox velocity estimator -> driver -> SteamVR ===\n");
    using namespace DirectX;
    // Device pitched up 90 deg, turning about WORLD +Y at 1 rad/s while
    // moving along +X at 0.5 m/s. SteamVR should report world-frame values.
    const XMVECTOR pitch90 = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), XM_PIDIV2);
    tubane::sandbox::VelocityEstimator estimator;
    DevicePose poses[kMaxDevices];
    {
        ControlBlock c{};
        ReadControl(*g_state, c);
        std::copy(std::begin(c.poses), std::end(c.poses), std::begin(poses));
    }

    const auto start = std::chrono::steady_clock::now();
    float sum[6] = {};
    int   samples = 0;
    for (int frame = 0; frame < 90; ++frame) {  // ~1 s at 90 Hz
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        XMFLOAT4 q;
        XMStoreFloat4(&q, XMQuaternionMultiply(pitch90, XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), static_cast<float>(t))));
        DevicePose& p = poses[kSlotHmd];
        p.position[0] = -0.25 + 0.5 * t; p.position[1] = 1.5; p.position[2] = 0.0;
        p.rotation[0] = q.w; p.rotation[1] = q.x; p.rotation[2] = q.y; p.rotation[3] = q.z;
        p.flags = kPoseValid | kPoseConnected;
        estimator.Apply(poses, t, true);
        WriteControl(*g_state, [&](ControlBlock& c) { std::copy(std::begin(poses), std::end(poses), std::begin(c.poses)); });

        if (frame >= 30) {  // after the estimator / SteamVR settled
            vr::TrackedDevicePose_t tp[vr::k_unMaxTrackedDeviceCount];
            vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0.0f, tp,
                                                            vr::k_unMaxTrackedDeviceCount);
            for (int i = 0; i < 3; ++i) {
                sum[i] += tp[0].vVelocity.v[i];
                sum[3 + i] += tp[0].vAngularVelocity.v[i];
            }
            ++samples;
        }
        Sleep_ms(11);
    }
    for (float& s : sum) s /= static_cast<float>(samples);
    std::printf("  expected: velocity (+0.50 +0.00 +0.00)  angular (+0.00 +1.00 +0.00)  [world]\n");
    std::printf("  received: velocity (%+.2f %+.2f %+.2f)  angular (%+.2f %+.2f %+.2f)  (mean of %d samples)\n", sum[0],
                sum[1], sum[2], sum[3], sum[4], sum[5], samples);
    const bool ok = std::fabs(sum[0] - 0.5f) < 0.08f && std::fabs(sum[1]) < 0.08f && std::fabs(sum[2]) < 0.08f &&
                    std::fabs(sum[3]) < 0.15f && std::fabs(sum[4] - 1.0f) < 0.15f && std::fabs(sum[5]) < 0.15f;
    std::printf("Motion check: %s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";

    SharedMemory shm;
    if (shm.Open(OpenMode::OpenExisting) != OpenResult::Ok) {
        std::printf("shared memory not found - start SteamVR with the tubane driver first\n");
        return 2;
    }
    g_state = shm.State();
    DriverBlock driver{};
    ReadDriver(*g_state, driver);
    if (driver.driverProcessId == 0) {
        std::printf("tubane driver is not running\n");
        return 2;
    }
    if (SandboxRunning()) {
        std::printf("tubane-sandbox is running - close it first (this tool writes the control block)\n");
        return 2;
    }

    ControlBlock original{};
    ReadControl(*g_state, original);
    SeqRecoverWriter(g_state->controlLock);

    vr::EVRInitError err = vr::VRInitError_None;
    // Input actions are only active for the application that has input focus,
    // i.e. the scene application. Connecting as a scene app takes focus from
    // SteamVR Home for the duration of the check (it comes back on exit).
    const bool needsFocus = (mode == "all" || mode == "input" || mode == "skeleton" || mode == "skeleton-dump");
    vr::VR_Init(&err, needsFocus ? vr::VRApplication_Scene : vr::VRApplication_Background);
    if (err != vr::VRInitError_None) {
        std::printf("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 2;
    }

    int result = 0;
    if (mode == "all" || mode == "input") result |= RunInputCheck();
    if (mode == "all" || mode == "velocity") result |= RunVelocityCheck();
    if (mode == "all" || mode == "motion") result |= RunMotionCheck();
    if (mode == "all" || mode == "skeleton") result |= RunSkeletonCheck();
    if (mode == "skeleton-dump") result |= RunSkeletonDump(argc > 2 ? argv[2] : "hand_reference_poses.h");

    // Restore what the sandbox (or defaults) had before.
    WriteControl(*g_state, [&](ControlBlock& c) { c = original; });
    vr::VR_Shutdown();
    return result;
}
