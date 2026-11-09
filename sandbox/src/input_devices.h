// Physical input sources mapped onto the virtual Index controllers' buttons,
// sticks and triggers. Poses (6DoF) are never touched here — they stay under
// manual (gizmo / inspector) control.
//
//   GamepadSource        XInput (Xbox-compatible) first, then any DirectInput
//                        game controller (generic USB / PlayStation layout)
//   KeyboardMouseSource  active only while "captured" (F1); WASD / mouse etc.
#pragma once

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

#include "tubane/ipc_layout.h"

namespace tubane::sandbox {

// Values produced by external sources for both hands. Only click bits and
// raw analog values are filled; touch bits are derived by the App.
struct ExternalInput {
    ipc::ControllerInput hand[2]{};  // [0] = left, [1] = right
};

// Normalized gamepad state (Xbox naming).
enum GamepadButton : uint32_t {
    kGpA = 1u << 0, kGpB = 1u << 1, kGpX = 1u << 2, kGpY = 1u << 3,
    kGpLB = 1u << 4, kGpRB = 1u << 5, kGpBack = 1u << 6, kGpStart = 1u << 7,
    kGpLS = 1u << 8, kGpRS = 1u << 9,
    kGpUp = 1u << 10, kGpDown = 1u << 11, kGpLeft = 1u << 12, kGpRight = 1u << 13,
};

struct GamepadState {
    float    lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, +Y = up
    float    lt = 0, rt = 0;                  // 0..1
    uint32_t buttons = 0;                     // GamepadButton bits
};

class GamepadSource {
public:
    enum class Api { None, XInput, DirectInput };

    bool Init(HWND hwnd);
    void Shutdown();
    void Poll(double nowSeconds);
    // Writes this source's values into a fresh (zeroed) ExternalInput.
    void Fill(ExternalInput& out) const;

    // Call on WM_DEVICECHANGE: the next Poll (after a short debounce) looks
    // for a newly attached controller.
    void RequestRescan() { rescanRequested_ = true; }

    bool               Connected() const { return api_ != Api::None; }
    Api                ApiInUse() const { return api_; }
    const std::string& Name() const { return name_; }
    const GamepadState& State() const { return state_; }

    bool  enabled  = true;
    float deadzone = 0.15f;

private:
    bool PollXInput();
    bool PollDirectInput();
    bool OpenDirectInputDevice();
    void CloseDirectInputDevice();

    HWND hwnd_ = nullptr;
    Api  api_  = Api::None;
    std::string  name_;
    GamepadState state_;
    int          xinputIndex_ = -1;
    bool         rescanRequested_ = true;  // scan once at start-up
    double       nextScan_    = 0.0;       // < 0 = debounce not started

    Microsoft::WRL::ComPtr<IDirectInput8W>       dinput_;
    Microsoft::WRL::ComPtr<IDirectInputDevice8W> device_;
    bool hasRx_ = false, hasRy_ = false;  // analog L2/R2 (PlayStation layout)
};

class KeyboardMouseSource {
public:
    // Capture hides and locks the cursor; keys and mouse then drive the
    // controllers instead of the sandbox UI. Released by Esc / F1 / focus loss.
    void SetCaptured(HWND hwnd, bool capture);
    bool Capturing() const { return capturing_; }

    // Called from WM_INPUT with relative mouse motion.
    void OnRawMouseMove(long dx, long dy);

    void Update(HWND hwnd, float dt);
    void Fill(ExternalInput& out) const;

    float sensitivity = 1.0f;

private:
    bool  capturing_ = false;
    long  accumX_ = 0, accumY_ = 0;
    float lookX_ = 0.0f, lookY_ = 0.0f;  // smoothed right-stick from mouse
    ExternalInput state_{};
};

// Registers the window for raw mouse input (WM_INPUT).
void RegisterRawMouse(HWND hwnd);

}  // namespace tubane::sandbox
