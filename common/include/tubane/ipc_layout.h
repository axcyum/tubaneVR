// tubane shared-memory layout.
//
// This header is the single source of truth for the data exchanged between
// tubane-driver (inside vrserver.exe) and tubane-sandbox / tools. It must stay
// a plain, standard-layout, fixed-size description: no pointers, no STL
// containers, explicit-width types only. Any change to the binary layout MUST
// bump kProtocolVersion.
//
// Ownership model (one writer per block, see docs/ipc.md):
//   SharedState::control  -> written by the sandbox (poses, input, config)
//   SharedState::driver   -> written by the driver  (status, render info)
// Each block is guarded by its own SeqLock so readers never observe a torn
// write without taking a lock.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace tubane::ipc {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
inline constexpr uint32_t kMagic           = 0x454E4254u;  // "TBNE" (little endian)
inline constexpr uint32_t kProtocolVersion = 1;

// "Local\" = per logon session namespace. vrserver.exe and the sandbox run in
// the same interactive session, so no extra privileges are needed.
inline constexpr wchar_t kSharedMemoryName[] = L"Local\\tubane_shared_state_v1";

inline constexpr uint32_t kMaxTrackers       = 8;
inline constexpr uint32_t kDefaultTrackers   = 3;
inline constexpr uint32_t kControllerCount   = 2;
inline constexpr uint32_t kMaxDevices        = 1 + kControllerCount + kMaxTrackers;  // HMD + L/R + trackers

// ---------------------------------------------------------------------------
// Device slots
// ---------------------------------------------------------------------------
// Fixed slot index of every virtual device inside SharedState arrays.
// Trackers occupy [kSlotFirstTracker, kSlotFirstTracker + kMaxTrackers).
enum DeviceSlot : uint32_t {
    kSlotHmd            = 0,
    kSlotLeftHand       = 1,
    kSlotRightHand      = 2,
    kSlotFirstTracker   = 3,
};

inline constexpr uint32_t TrackerSlot(uint32_t trackerIndex) { return kSlotFirstTracker + trackerIndex; }

// ---------------------------------------------------------------------------
// Pose
// ---------------------------------------------------------------------------
// Coordinate system == OpenVR "standing" space:
//   right-handed, +X right, +Y up, -Z forward, units = meters, radians.
// Quaternion order is (w, x, y, z) to match vr::HmdQuaternion_t.
enum PoseFlags : uint32_t {
    kPoseValid     = 1u << 0,  // pose data is meaningful
    kPoseConnected = 1u << 1,  // device should report deviceIsConnected = true
};

struct DevicePose {
    double   position[3];         // meters
    double   rotation[4];         // unit quaternion (w, x, y, z)
    double   velocity[3];         // m/s, world space
    double   angularVelocity[3];  // rad/s, world space
    uint32_t flags;               // PoseFlags
    uint32_t reserved;
};

// ---------------------------------------------------------------------------
// Controller input (Valve Index controller layout)
// ---------------------------------------------------------------------------
enum ButtonBits : uint32_t {
    kBtnSystemClick     = 1u << 0,
    kBtnSystemTouch     = 1u << 1,
    kBtnAClick          = 1u << 2,
    kBtnATouch          = 1u << 3,
    kBtnBClick          = 1u << 4,
    kBtnBTouch          = 1u << 5,
    kBtnTriggerClick    = 1u << 6,
    kBtnTriggerTouch    = 1u << 7,
    kBtnGripTouch       = 1u << 8,
    kBtnThumbstickClick = 1u << 9,
    kBtnThumbstickTouch = 1u << 10,
    kBtnTrackpadTouch   = 1u << 11,
};

struct ControllerInput {
    uint32_t buttons;        // ButtonBits
    float    trigger;        // 0..1
    float    gripValue;      // 0..1
    float    gripForce;      // 0..1
    float    thumbstickX;    // -1..1
    float    thumbstickY;    // -1..1
    float    trackpadX;      // -1..1
    float    trackpadY;      // -1..1
    float    trackpadForce;  // 0..1
    float    finger[4];      // index, middle, ring, pinky curl 0..1
    uint32_t reserved[3];
};

// ---------------------------------------------------------------------------
// Configuration (sandbox -> driver)
// ---------------------------------------------------------------------------
enum class ViewerEye : uint32_t {
    Left  = 0,
    Right = 1,
};

enum class ResolutionPreset : uint32_t {
    FHD  = 0,  // 1920 x 1080 per eye
    WQHD = 1,  // 2560 x 1440 per eye
};

struct Resolution {
    uint32_t width;
    uint32_t height;
};

inline constexpr Resolution ResolutionOf(ResolutionPreset preset) {
    return preset == ResolutionPreset::WQHD ? Resolution{2560, 1440} : Resolution{1920, 1080};
}

// Persistent settings (resolution, trackerCount) live in SteamVR's settings
// file so they survive restarts. The sandbox requests a change by writing the
// new values and incrementing `revision`; the driver notices the new revision,
// stores the values into steamvr.vrsettings and reports `restartRequired`.
// Values present at driver start-up are never applied implicitly — only a
// revision change triggers an update, so stale defaults cannot overwrite the
// user's saved settings.
struct ControlConfig {
    uint32_t         revision;          // bumped by the sandbox on every user change
    ViewerEye        viewerEye;         // live: eye copied into the shared frame (Step 3)
    ResolutionPreset resolution;        // persistent: per-eye render target (restart required)
    uint32_t         trackerCount;      // persistent: trackers spawned at Init (restart required)
    uint32_t         reserved[4];
};

enum DisplayMode : uint32_t {
    kDisplayExtended = 0,  // SteamVR "Headset Window" on the desktop (no shared texture)
    kDisplayDirect   = 1,  // IVRDriverDirectModeComponent -> shared texture for the sandbox viewer
};

// Shared frame texture (driver -> sandbox), see docs/ipc.md "映像経路":
//   format  DXGI_FORMAT_R8G8B8A8_TYPELESS (holds sRGB-encoded 8-bit RGBA; view it as *_UNORM)
//   flags   D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
//   sync    keyed mutex, key 0 on both sides, never blocking (AcquireSync(0, 0))
//   size    DriverBlock::renderWidth x renderHeight

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------
// Written by the sandbox, read by the driver.
struct ControlBlock {
    uint64_t        sandboxHeartbeat;   // incremented every sandbox frame; 0 = no sandbox yet
    uint32_t        sandboxProcessId;
    uint32_t        reserved0;
    ControlConfig   config;
    DevicePose      poses[kMaxDevices];
    ControllerInput controllers[kControllerCount];  // [0] = left, [1] = right
};

// Written by the driver, read by the sandbox.
struct DriverBlock {
    uint64_t driverHeartbeat;     // incremented by the driver pose thread
    uint32_t driverProcessId;     // vrserver.exe PID
    uint32_t activeTrackerCount;  // trackers actually spawned
    uint32_t renderWidth;         // per-eye render target currently in use
    uint32_t renderHeight;
    ResolutionPreset activeResolution;  // preset in effect for this SteamVR session
    uint32_t restartRequired;     // 1 = saved settings differ from the running session
    uint32_t appliedRevision;     // last ControlConfig::revision the driver processed
    uint32_t displayMode;         // DisplayMode: how the HMD image is delivered
    uint64_t adapterLuid;         // GPU adapter LUID the compositor renders on (direct mode)
    uint64_t frameIndex;          // incremented per frame written to the shared texture
    wchar_t  frameTextureName[64];// name of the shared frame texture (NT handle), empty = none
    uint32_t deviceIds[kMaxDevices]; // OpenVR tracked-device index per slot, 0xFFFFFFFF = none
};

// ---------------------------------------------------------------------------
// SeqLock — single writer, many readers, wait-free for the writer.
// ---------------------------------------------------------------------------
// The counter is odd while a write is in progress. Readers retry until they
// observe the same even value before and after copying the payload.
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "cross-process atomics require a lock-free std::atomic<uint32_t>");

struct alignas(64) SeqLock {
    std::atomic<uint32_t> sequence{0};
    uint32_t              padding[15]{};
};

// ---------------------------------------------------------------------------
// Root object mapped into both processes
// ---------------------------------------------------------------------------
struct SharedState {
    // Header — written once by whichever process creates the mapping.
    std::atomic<uint32_t> magic;    // kMagic once initialization finished
    uint32_t              version;  // kProtocolVersion
    uint32_t              size;     // sizeof(SharedState)
    uint32_t              reserved[13];

    SeqLock      controlLock;
    ControlBlock control;

    SeqLock      driverLock;
    DriverBlock  driver;
};

static_assert(std::is_standard_layout_v<SharedState>, "SharedState must be standard layout");
static_assert(std::is_trivially_copyable_v<ControlBlock>, "ControlBlock must be trivially copyable");
static_assert(std::is_trivially_copyable_v<DriverBlock>,  "DriverBlock must be trivially copyable");
static_assert(offsetof(SharedState, controlLock) % 64 == 0, "SeqLock must be cache-line aligned");

}  // namespace tubane::ipc
