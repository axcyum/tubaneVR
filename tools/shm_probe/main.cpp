// tubane_shm_probe: inspect and drive tubane's shared memory from a console.
//
//   tubane_shm_probe status                 one-shot dump of driver + control blocks
//   tubane_shm_probe watch                  dump every 500 ms until Ctrl+C
//   tubane_shm_probe orbit                  animate HMD yaw + right hand circle until Ctrl+C
//   tubane_shm_probe set <slot> <x> <y> <z> [yawDeg]
//   tubane_shm_probe reset                  restore default poses
//   tubane_shm_probe resolution <FHD|WQHD>  request render resolution (restart SteamVR)
//   tubane_shm_probe trackers <0..8>        request tracker count     (restart SteamVR)
//   tubane_shm_probe trigger <left|right> <0..1>
//
// Commands that write act as the "sandbox" writer of the control block.
// Do NOT run them while tubane-sandbox is running (single-writer protocol).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "tubane/default_state.h"
#include "tubane/shm.h"

using namespace tubane::ipc;

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI OnCtrl(DWORD) {
    g_stop = true;
    return TRUE;
}

const char* SlotName(uint32_t slot) {
    static char buffer[32];
    switch (slot) {
        case kSlotHmd:       return "HMD";
        case kSlotLeftHand:  return "LeftHand";
        case kSlotRightHand: return "RightHand";
        default:
            std::snprintf(buffer, sizeof(buffer), "Tracker%u", slot - kSlotFirstTracker);
            return buffer;
    }
}

void PrintState(const SharedState& s) {
    DriverBlock  d{};
    ControlBlock c{};
    const bool okD = ReadDriver(s, d);
    const bool okC = ReadControl(s, c);

    std::printf("--- driver block %s ---\n", okD ? "" : "(read failed)");
    std::printf("  pid=%u heartbeat=%llu render=%ux%u preset=%s restartRequired=%u trackers=%u appliedRev=%u\n",
                d.driverProcessId, static_cast<unsigned long long>(d.driverHeartbeat), d.renderWidth, d.renderHeight,
                d.activeResolution == ResolutionPreset::WQHD ? "WQHD" : "FHD", d.restartRequired,
                d.activeTrackerCount, d.appliedRevision);

    std::printf("--- control block %s ---\n", okC ? "" : "(read failed)");
    std::printf("  sandboxHeartbeat=%llu rev=%u eye=%s preset=%s trackers=%u\n",
                static_cast<unsigned long long>(c.sandboxHeartbeat), c.config.revision,
                c.config.viewerEye == ViewerEye::Right ? "Right" : "Left",
                c.config.resolution == ResolutionPreset::WQHD ? "WQHD" : "FHD", c.config.trackerCount);
    for (uint32_t i = 0; i < kSlotFirstTracker + d.activeTrackerCount && i < kMaxDevices; ++i) {
        const DevicePose& p = c.poses[i];
        const uint32_t id = d.deviceIds[i];
        std::printf("  [%-9s] vrId=%-3s pos=(%+.3f %+.3f %+.3f) rot(wxyz)=(%+.3f %+.3f %+.3f %+.3f) flags=%u\n",
                    SlotName(i), id == 0xFFFFFFFFu ? "-" : std::to_string(id).c_str(), p.position[0], p.position[1],
                    p.position[2], p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3], p.flags);
    }
    for (int hand = 0; hand < 2; ++hand) {
        const ControllerInput& in = c.controllers[hand];
        std::printf("  [%s input] buttons=0x%03X trigger=%.2f grip=%.2f/%.2f stick=(%+.2f %+.2f) pad=(%+.2f %+.2f f%.2f) "
                    "fingers=(%.2f %.2f %.2f %.2f)\n",
                    hand ? "R" : "L", in.buttons, in.trigger, in.gripValue, in.gripForce, in.thumbstickX, in.thumbstickY,
                    in.trackpadX, in.trackpadY, in.trackpadForce, in.finger[0], in.finger[1], in.finger[2], in.finger[3]);
    }
}

void SetYaw(DevicePose& p, double yawRad) {
    p.rotation[0] = std::cos(yawRad * 0.5);
    p.rotation[1] = 0.0;
    p.rotation[2] = std::sin(yawRad * 0.5);
    p.rotation[3] = 0.0;
}

int Usage() {
    std::fprintf(stderr,
                 "usage: tubane_shm_probe <status|watch|orbit|reset|set|resolution|trackers|trigger> [...]\n"
                 "  set <slot> <x> <y> <z> [yawDeg]   slot: 0=HMD 1=Left 2=Right 3..=Trackers\n"
                 "  resolution <FHD|WQHD>\n"
                 "  trackers <0..%u>\n"
                 "  trigger <left|right> <0..1>\n",
                 kMaxTrackers);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return Usage();
    }
    const std::string cmd = argv[1];
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    SharedMemory shm;
    const bool   readOnly = (cmd == "status" || cmd == "watch");
    const OpenResult r    = shm.Open(readOnly ? OpenMode::OpenExisting : OpenMode::CreateOrOpen);
    if (r != OpenResult::Ok) {
        std::fprintf(stderr, "shared memory: %s (win32 error %lu)%s\n", ToString(r), shm.LastError(),
                     r == OpenResult::NotFound ? " - is SteamVR running with the tubane driver?" : "");
        return 1;
    }
    SharedState& s = *shm.State();
    if (shm.Created()) {
        std::fprintf(stderr, "note: driver not running; mapping created by this probe and freed on exit.\n");
    }

    if (cmd == "status") {
        PrintState(s);
        return 0;
    }
    if (cmd == "watch") {
        while (!g_stop) {
            std::printf("\x1b[2J\x1b[H");
            PrintState(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return 0;
    }
    if (cmd == "reset") {
        WriteControl(s, [](ControlBlock& c) {
            for (uint32_t i = 0; i < kMaxDevices; ++i) c.poses[i] = DefaultPose(i);
            std::memset(c.controllers, 0, sizeof(c.controllers));
        });
        std::printf("poses reset\n");
        return 0;
    }
    if (cmd == "set") {
        if (argc < 6) return Usage();
        const uint32_t slot = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
        if (slot >= kMaxDevices) return Usage();
        const double x = std::atof(argv[3]), y = std::atof(argv[4]), z = std::atof(argv[5]);
        const double yaw = argc > 6 ? std::atof(argv[6]) * 3.14159265358979323846 / 180.0 : 0.0;
        WriteControl(s, [&](ControlBlock& c) {
            DevicePose& p = c.poses[slot];
            p.position[0] = x; p.position[1] = y; p.position[2] = z;
            SetYaw(p, yaw);
            p.flags = kPoseValid | kPoseConnected;
        });
        std::printf("%s -> (%.3f %.3f %.3f) yaw=%.1f deg\n", SlotName(slot), x, y, z, yaw * 180.0 / 3.14159265358979323846);
        return 0;
    }
    if (cmd == "resolution" || cmd == "trackers") {
        if (argc < 3) return Usage();
        const std::string arg = argv[2];
        WriteControl(s, [&](ControlBlock& c) {
            if (cmd == "resolution") {
                c.config.resolution = (arg == "WQHD" || arg == "wqhd") ? ResolutionPreset::WQHD : ResolutionPreset::FHD;
            } else {
                c.config.trackerCount = static_cast<uint32_t>(std::strtoul(arg.c_str(), nullptr, 10));
            }
            ++c.config.revision;
        });
        std::printf("requested %s=%s; restart SteamVR to apply\n", cmd.c_str(), arg.c_str());
        return 0;
    }
    if (cmd == "trigger") {
        if (argc < 4) return Usage();
        const int    hand  = (std::strcmp(argv[2], "right") == 0) ? 1 : 0;
        const float  value = static_cast<float>(std::atof(argv[3]));
        WriteControl(s, [&](ControlBlock& c) {
            ControllerInput& in = c.controllers[hand];
            in.trigger = value;
            if (value > 0.0f)  in.buttons |= kBtnTriggerTouch; else in.buttons &= ~kBtnTriggerTouch;
            if (value >= 0.9f) in.buttons |= kBtnTriggerClick; else in.buttons &= ~kBtnTriggerClick;
        });
        std::printf("%s trigger = %.2f\n", hand ? "right" : "left", value);
        return 0;
    }
    if (cmd == "orbit") {
        std::printf("orbiting; Ctrl+C to stop (poses stay where they are)\n");
        const auto start = std::chrono::steady_clock::now();
        while (!g_stop) {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            WriteControl(s, [&](ControlBlock& c) {
                ++c.sandboxHeartbeat;
                SetYaw(c.poses[kSlotHmd], 0.6 * std::sin(t * 0.8));
                DevicePose& rh = c.poses[kSlotRightHand];
                rh.position[0] = 0.20 + 0.15 * std::cos(t * 2.0);
                rh.position[1] = 1.10 + 0.15 * std::sin(t * 2.0);
                rh.position[2] = -0.30;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(11));
        }
        return 0;
    }
    return Usage();
}
