#include "input_devices.h"

#include <Xinput.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace tubane::sandbox {

namespace {

void ApplyRadialDeadzone(float& x, float& y, float deadzone) {
    const float mag = std::sqrt(x * x + y * y);
    if (mag <= deadzone) {
        x = y = 0.0f;
        return;
    }
    const float scaled = std::min((mag - deadzone) / (1.0f - deadzone), 1.0f);
    x = x / mag * scaled;
    y = y / mag * scaled;
}

// True if a DirectInput product GUID belongs to an XInput device (those are
// handled through XInput; their DirectInput view merges the triggers).
// XInput devices expose "IG_" in their raw-input HID device path.
bool IsXInputDevice(const GUID& product) {
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0) return false;
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) return false;

    for (const RAWINPUTDEVICELIST& dev : list) {
        if (dev.dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT size   = sizeof(info);
        if (GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICEINFO, &info, &size) == static_cast<UINT>(-1)) continue;
        if (MAKELONG(info.hid.dwVendorId, info.hid.dwProductId) != static_cast<LONG>(product.Data1)) continue;

        UINT nameLen = 0;
        GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, nullptr, &nameLen);
        std::wstring name(nameLen, L'\0');
        if (GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, name.data(), &nameLen) == static_cast<UINT>(-1)) continue;
        if (name.find(L"IG_") != std::wstring::npos) return true;
    }
    return false;
}

struct EnumContext {
    GUID         instance{};
    std::wstring name;
    bool         found = false;
};

BOOL CALLBACK EnumGameControllers(LPCDIDEVICEINSTANCEW inst, LPVOID user) {
    auto* ctx = static_cast<EnumContext*>(user);
    if (IsXInputDevice(inst->guidProduct)) return DIENUM_CONTINUE;
    ctx->instance = inst->guidInstance;
    ctx->name     = inst->tszProductName;
    ctx->found    = true;
    return DIENUM_STOP;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

bool KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool OurWindowIsForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace

// ===========================================================================
// GamepadSource
// ===========================================================================
bool GamepadSource::Init(HWND hwnd) {
    hwnd_ = hwnd;
    // DirectInput is optional: without it only XInput pads work.
    DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                       reinterpret_cast<void**>(dinput_.GetAddressOf()), nullptr);
    return true;
}

void GamepadSource::Shutdown() {
    CloseDirectInputDevice();
    dinput_.Reset();
    api_ = Api::None;
}

void GamepadSource::Poll(double now) {
    if (!enabled) {
        state_ = {};
        return;
    }

    bool ok = false;
    switch (api_) {
        case Api::XInput:      ok = PollXInput(); break;
        case Api::DirectInput: ok = PollDirectInput(); break;
        case Api::None:        break;
    }
    if (ok) return;

    // Disconnected (or never connected). Enumerating DirectInput can take tens
    // of milliseconds, so scan only at start-up and after a device change
    // notification (RequestRescan), debounced to let drivers settle.
    if (api_ != Api::None) {
        CloseDirectInputDevice();
        api_  = Api::None;
        name_.clear();
    }
    state_ = {};
    if (!rescanRequested_) return;
    if (nextScan_ < 0.0) {
        nextScan_ = now + 0.5;
        return;
    }
    if (now < nextScan_) return;
    rescanRequested_ = false;
    nextScan_        = -1.0;

    for (int i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE xs{};
        if (XInputGetState(static_cast<DWORD>(i), &xs) == ERROR_SUCCESS) {
            xinputIndex_ = i;
            api_         = Api::XInput;
            name_        = "XInput controller #" + std::to_string(i + 1);
            PollXInput();
            return;
        }
    }
    if (OpenDirectInputDevice()) {
        api_ = Api::DirectInput;
        PollDirectInput();
    }
}

bool GamepadSource::PollXInput() {
    XINPUT_STATE xs{};
    if (xinputIndex_ < 0 || XInputGetState(static_cast<DWORD>(xinputIndex_), &xs) != ERROR_SUCCESS) return false;
    const XINPUT_GAMEPAD& g = xs.Gamepad;

    GamepadState s;
    s.lx = std::max(-1.0f, g.sThumbLX / 32767.0f);
    s.ly = std::max(-1.0f, g.sThumbLY / 32767.0f);
    s.rx = std::max(-1.0f, g.sThumbRX / 32767.0f);
    s.ry = std::max(-1.0f, g.sThumbRY / 32767.0f);
    s.lt = g.bLeftTrigger / 255.0f;
    s.rt = g.bRightTrigger / 255.0f;

    struct Map { WORD xi; uint32_t bit; };
    static constexpr Map kMap[] = {
        {XINPUT_GAMEPAD_A, kGpA}, {XINPUT_GAMEPAD_B, kGpB}, {XINPUT_GAMEPAD_X, kGpX}, {XINPUT_GAMEPAD_Y, kGpY},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, kGpLB}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, kGpRB},
        {XINPUT_GAMEPAD_BACK, kGpBack}, {XINPUT_GAMEPAD_START, kGpStart},
        {XINPUT_GAMEPAD_LEFT_THUMB, kGpLS}, {XINPUT_GAMEPAD_RIGHT_THUMB, kGpRS},
        {XINPUT_GAMEPAD_DPAD_UP, kGpUp}, {XINPUT_GAMEPAD_DPAD_DOWN, kGpDown},
        {XINPUT_GAMEPAD_DPAD_LEFT, kGpLeft}, {XINPUT_GAMEPAD_DPAD_RIGHT, kGpRight},
    };
    for (const Map& m : kMap) {
        if (g.wButtons & m.xi) s.buttons |= m.bit;
    }

    ApplyRadialDeadzone(s.lx, s.ly, deadzone);
    ApplyRadialDeadzone(s.rx, s.ry, deadzone);
    state_ = s;
    return true;
}

bool GamepadSource::OpenDirectInputDevice() {
    if (!dinput_) return false;
    EnumContext ctx;
    dinput_->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumGameControllers, &ctx, DIEDFL_ATTACHEDONLY);
    if (!ctx.found) return false;

    if (FAILED(dinput_->CreateDevice(ctx.instance, &device_, nullptr)) ||
        FAILED(device_->SetDataFormat(&c_dfDIJoystick2)) ||
        FAILED(device_->SetCooperativeLevel(hwnd_, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        CloseDirectInputDevice();
        return false;
    }

    // All axes -> -1000..1000.
    DIPROPRANGE range{};
    range.diph.dwSize       = sizeof(DIPROPRANGE);
    range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    range.diph.dwHow        = DIPH_DEVICE;
    range.lMin              = -1000;
    range.lMax              = 1000;
    device_->SetProperty(DIPROP_RANGE, &range.diph);

    DIDEVICEOBJECTINSTANCEW obj{};
    obj.dwSize = sizeof(obj);
    hasRx_ = SUCCEEDED(device_->GetObjectInfo(&obj, DIJOFS_RX, DIPH_BYOFFSET));
    hasRy_ = SUCCEEDED(device_->GetObjectInfo(&obj, DIJOFS_RY, DIPH_BYOFFSET));

    device_->Acquire();
    name_ = Narrow(ctx.name) + " (DirectInput)";
    return true;
}

void GamepadSource::CloseDirectInputDevice() {
    if (device_) device_->Unacquire();
    device_.Reset();
}

bool GamepadSource::PollDirectInput() {
    if (!device_) return false;
    if (FAILED(device_->Poll())) {
        if (FAILED(device_->Acquire())) return false;
        device_->Poll();
    }
    DIJOYSTATE2 js{};
    HRESULT hr = device_->GetDeviceState(sizeof(js), &js);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        if (FAILED(device_->Acquire())) return false;
        hr = device_->GetDeviceState(sizeof(js), &js);
    }
    if (FAILED(hr)) return false;

    // Common generic / PlayStation DirectInput layout:
    //   axes  X,Y = left stick, Z,Rz = right stick, Rx,Ry = analog L2/R2 (if present)
    //   btns  0 West(□/X) 1 South(×/A) 2 East(○/B) 3 North(△/Y) 4 L1 5 R1 6 L2 7 R2
    //         8 Select/Share 9 Start/Options 10 L3 11 R3
    auto btn = [&](int i) { return (js.rgbButtons[i] & 0x80) != 0; };
    GamepadState s;
    s.lx = js.lX / 1000.0f;
    s.ly = -js.lY / 1000.0f;
    s.rx = js.lZ / 1000.0f;
    s.ry = -js.lRz / 1000.0f;
    s.lt = hasRx_ ? std::clamp((js.lRx + 1000) / 2000.0f, 0.0f, 1.0f) : (btn(6) ? 1.0f : 0.0f);
    s.rt = hasRy_ ? std::clamp((js.lRy + 1000) / 2000.0f, 0.0f, 1.0f) : (btn(7) ? 1.0f : 0.0f);

    const uint32_t buttonBits[12] = {kGpX, kGpA, kGpB, kGpY, kGpLB, kGpRB, 0, 0, kGpBack, kGpStart, kGpLS, kGpRS};
    for (int i = 0; i < 12; ++i) {
        if (buttonBits[i] && btn(i)) s.buttons |= buttonBits[i];
    }

    const DWORD pov = js.rgdwPOV[0];
    if (LOWORD(pov) != 0xFFFF) {  // centered = 0xFFFFFFFF
        const DWORD deg = pov / 100;
        if (deg >= 315 || deg <= 45) s.buttons |= kGpUp;
        if (deg >= 45 && deg <= 135) s.buttons |= kGpRight;
        if (deg >= 135 && deg <= 225) s.buttons |= kGpDown;
        if (deg >= 225 && deg <= 315) s.buttons |= kGpLeft;
    }

    ApplyRadialDeadzone(s.lx, s.ly, deadzone);
    ApplyRadialDeadzone(s.rx, s.ry, deadzone);
    state_ = s;
    return true;
}

void GamepadSource::Fill(ExternalInput& out) const {
    if (!enabled || !Connected()) return;
    const GamepadState& s = state_;
    ipc::ControllerInput& L = out.hand[0];
    ipc::ControllerInput& R = out.hand[1];
    auto on = [&](uint32_t bit) { return (s.buttons & bit) != 0; };

    // Left hand
    L.thumbstickX = s.lx;
    L.thumbstickY = s.ly;
    L.trigger     = s.lt;
    if (on(kGpLB))   L.gripValue = 1.0f;
    if (on(kGpX))    L.buttons |= ipc::kBtnAClick;
    if (on(kGpY))    L.buttons |= ipc::kBtnBClick;
    if (on(kGpLS))   L.buttons |= ipc::kBtnThumbstickClick;
    if (on(kGpBack)) L.buttons |= ipc::kBtnSystemClick;
    // D-pad -> left trackpad
    const float padX = (on(kGpRight) ? 1.0f : 0.0f) - (on(kGpLeft) ? 1.0f : 0.0f);
    const float padY = (on(kGpUp) ? 1.0f : 0.0f) - (on(kGpDown) ? 1.0f : 0.0f);
    if (padX != 0.0f || padY != 0.0f) {
        const float len = std::sqrt(padX * padX + padY * padY);
        L.trackpadX = padX / len;
        L.trackpadY = padY / len;
        L.buttons |= ipc::kBtnTrackpadTouch;
    }

    // Right hand
    R.thumbstickX = s.rx;
    R.thumbstickY = s.ry;
    R.trigger     = s.rt;
    if (on(kGpRB))    R.gripValue = 1.0f;
    if (on(kGpA))     R.buttons |= ipc::kBtnAClick;
    if (on(kGpB))     R.buttons |= ipc::kBtnBClick;
    if (on(kGpRS))    R.buttons |= ipc::kBtnThumbstickClick;
    if (on(kGpStart)) R.buttons |= ipc::kBtnSystemClick;
}

// ===========================================================================
// KeyboardMouseSource
// ===========================================================================
void RegisterRawMouse(HWND hwnd) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // generic desktop
    rid.usUsage     = 0x02;  // mouse
    rid.dwFlags     = 0;     // only while our window has focus
    rid.hwndTarget  = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void KeyboardMouseSource::SetCaptured(HWND hwnd, bool capture) {
    if (capture == capturing_) return;
    capturing_ = capture;
    accumX_ = accumY_ = 0;
    lookX_ = lookY_ = 0.0f;
    state_ = {};
    if (capture) {
        // Pin the (hidden) cursor at the window center; raw input still
        // delivers relative motion.
        RECT rc{};
        GetClientRect(hwnd, &rc);
        POINT c{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
        ClientToScreen(hwnd, &c);
        SetCursorPos(c.x, c.y);
        const RECT clip{c.x, c.y, c.x + 1, c.y + 1};
        ClipCursor(&clip);
        while (ShowCursor(FALSE) >= 0) {
        }
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {
        }
    }
}

void KeyboardMouseSource::OnRawMouseMove(long dx, long dy) {
    if (!capturing_) return;
    accumX_ += dx;
    accumY_ += dy;
}

void KeyboardMouseSource::Update(HWND hwnd, float dt) {
    if (!capturing_) return;
    if (!OurWindowIsForeground() || KeyDown(VK_ESCAPE)) {
        SetCaptured(hwnd, false);
        return;
    }

    ExternalInput s{};
    ipc::ControllerInput& L = s.hand[0];
    ipc::ControllerInput& R = s.hand[1];

    // Left stick: WASD
    float lx = (KeyDown('D') ? 1.0f : 0.0f) - (KeyDown('A') ? 1.0f : 0.0f);
    float ly = (KeyDown('W') ? 1.0f : 0.0f) - (KeyDown('S') ? 1.0f : 0.0f);
    if (lx != 0.0f && ly != 0.0f) {
        lx *= 0.70710678f;
        ly *= 0.70710678f;
    }
    L.thumbstickX = lx;
    L.thumbstickY = ly;

    // Right stick: mouse motion (smoothed), arrow keys override.
    const float safeDt = std::max(dt, 1e-3f);
    const float targetX = std::clamp(accumX_ / safeDt * sensitivity / 1200.0f, -1.0f, 1.0f);
    const float targetY = std::clamp(-accumY_ / safeDt * sensitivity / 1200.0f, -1.0f, 1.0f);
    accumX_ = accumY_ = 0;
    const float alpha = 1.0f - std::exp(-safeDt / 0.06f);
    lookX_ += (targetX - lookX_) * alpha;
    lookY_ += (targetY - lookY_) * alpha;
    if (std::fabs(lookX_) < 0.01f) lookX_ = 0.0f;
    if (std::fabs(lookY_) < 0.01f) lookY_ = 0.0f;
    const float ax = (KeyDown(VK_RIGHT) ? 1.0f : 0.0f) - (KeyDown(VK_LEFT) ? 1.0f : 0.0f);
    const float ay = (KeyDown(VK_UP) ? 1.0f : 0.0f) - (KeyDown(VK_DOWN) ? 1.0f : 0.0f);
    R.thumbstickX = ax != 0.0f ? ax : lookX_;
    R.thumbstickY = ay != 0.0f ? ay : lookY_;

    // Triggers / grips
    if (KeyDown(VK_RBUTTON)) L.trigger = 1.0f;
    if (KeyDown(VK_LBUTTON)) R.trigger = 1.0f;
    if (KeyDown('Q'))        L.gripValue = 1.0f;
    if (KeyDown('E'))        R.gripValue = 1.0f;

    // Buttons
    if (KeyDown('Z'))          L.buttons |= ipc::kBtnAClick;
    if (KeyDown('X'))          L.buttons |= ipc::kBtnBClick;
    if (KeyDown(VK_LCONTROL))  L.buttons |= ipc::kBtnThumbstickClick;
    if (KeyDown(VK_TAB))       L.buttons |= ipc::kBtnSystemClick;
    if (KeyDown(VK_SPACE))     R.buttons |= ipc::kBtnAClick;
    if (KeyDown('F'))          R.buttons |= ipc::kBtnBClick;
    if (KeyDown(VK_MBUTTON))   R.buttons |= ipc::kBtnThumbstickClick;
    if (KeyDown(VK_RETURN))    R.buttons |= ipc::kBtnSystemClick;

    state_ = s;
}

void KeyboardMouseSource::Fill(ExternalInput& out) const {
    if (!capturing_) return;
    out = state_;
}

}  // namespace tubane::sandbox
