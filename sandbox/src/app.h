// tubane-sandbox application: UI panels, viewport, gizmo, IPC publishing.
#pragma once

#include <DirectXMath.h>
#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "anim_clip.h"
#include "camera.h"
#include "scene_file.h"
#include "spout_receiver.h"
#include "velocity.h"
#include "d3d11_context.h"
#include "hmd_viewer.h"
#include "input_devices.h"
#include "ipc_link.h"
#include "math_util.h"
#include "renderer.h"
#include "scene.h"

namespace tubane::sandbox {

// UI-side state of one Index controller; converted to ipc::ControllerInput
// every frame by ComposeInput().
struct ControllerUiState {
    static constexpr int kButtonCount = 7;  // System, A, B, Trigger, Thumbstick, Trackpad, Grip
    bool  latchClick[kButtonCount] = {};
    bool  latchTouch[kButtonCount] = {};
    bool  hold[kButtonCount]       = {};    // momentary, refreshed every frame
    float trigger     = 0.0f;
    float gripValue   = 0.0f;
    float gripForce   = 0.0f;
    float stick[2]    = {};
    bool  stickSpring = true;
    float pad[2]      = {};
    bool  padSpring   = true;
    float padForce    = 0.0f;
    float fingers[4]  = {};
    bool  autoFingers = true;  // index follows trigger, others follow grip
};

class App {
public:
    bool Init(D3D11Context* d3d, HWND hwnd);
    void Shutdown();

    // Forwarded from WM_INPUT (relative mouse motion).
    void OnRawMouseMove(long dx, long dy) { keyboardMouse_.OnRawMouseMove(dx, dy); }
    // Forwarded from WM_DEVICECHANGE (a gamepad may have been plugged in).
    void OnDeviceChange() { gamepad_.RequestRescan(); }

    // Builds the UI, renders the 3D viewport and publishes to shared memory.
    void Frame(float dt, double nowSeconds);

private:
    // UI
    void BuildDefaultLayout(ImGuiID dockspace);
    void DrawMenuBar();
    void DrawFileMenu();
    void DrawAnimationWindow();
    void DrawSpoutWindow();

    // File menu actions
    void SaveSceneTo(const std::filesystem::path& path);
    void LoadSceneFrom(const std::filesystem::path& path);
    void SaveSceneDialog(bool saveAs);
    void OpenSceneDialog();
    void OpenAnimationDialog();
    void ApplyTPose(Facing facing);
    void UpdateAnimation(float dt);
    void Notify(const std::string& message, bool error = false);
    void DrawViewport(float dt);
    void DrawViewportToolbar();
    void DrawDevicesPanel();
    void DrawInspectorPanel();
    void DrawInputPanel();
    void DrawDriverPanel();
    void DrawViewerPanel();
    void DrawHelpWindow();

    // Scene
    void RenderScene(DirectX::FXMMATRIX view, DirectX::CXMMATRIX proj);
    void DrawRoom();
    void DrawDevice(uint32_t slot);
    void DrawHmdFrustum();
    void DrawLabels(DirectX::FXMMATRIX viewProj, const ImVec2& origin, const ImVec2& size);
    int  PickDevice(const Ray& ray) const;
    void FocusSelected();

    // State helpers
    uint32_t VisibleTrackerCount() const;
    bool     SlotVisible(uint32_t slot) const;
    const char* SlotStatus(uint32_t slot) const;  // "", "(after restart)", ...
    void     ComposeInput(int hand, bool forceTrigger, const ExternalInput& gamepad, const ExternalInput& keyboard);
    void     DrawInputDevices();
    void     SetKeyboardCapture(bool capture);
    void     BumpConfigRevision();
    void     SyncConfigFromDriver();

    D3D11Context* d3d_ = nullptr;
    Renderer      renderer_;
    Camera        camera_;
    IpcLink       ipc_;
    HmdViewer     viewer_;
    HWND          hwnd_ = nullptr;

    // Physical input -> controller buttons / sticks (never poses).
    GamepadSource       gamepad_;
    KeyboardMouseSource keyboardMouse_;

    // Scene file / presets
    std::filesystem::path scenePath_;
    float                 bodyHeight_ = 1.70f;
    std::string           notifyText_;
    bool                  notifyError_ = false;
    double                notifyUntil_ = 0.0;
    double                now_ = 0.0;

    // Unity animation playback (drives poses of the checked devices)
    AnimClip              clip_;
    bool                  clipLoaded_ = false;
    std::filesystem::path clipPath_;
    bool                  showAnimWindow_ = false;
    bool                  animPlaying_ = false;
    bool                  animLoop_ = true;
    bool                  animApplyOnce_ = false;  // apply current time while paused (scrubbing)
    float                 animTime_ = 0.0f;
    float                 animSpeed_ = 1.0f;
    int                   animFacing_ = 0;          // Facing index
    AnimSampleOptions     animOptions_;
    bool                  animEnabled_[ipc::kMaxDevices] = {};
    bool                  animDriven_[ipc::kMaxDevices] = {};  // written by the clip in the last sample

    // Velocities sent with the poses (SteamVR pose prediction)
    VelocityEstimator     velocity_;
    bool                  sendVelocity_ = true;

    // Spout2 receiver
    SpoutReceiver         spout_;
    bool                  showSpoutWindow_ = false;
    std::string           spoutSender_;            // empty = active sender
    bool                  spoutNativeSize_ = false;
    bool                  spoutPopOut_ = false;

    // HMD Viewer panel
    bool viewerNativeSize_ = false;  // 1:1 pixels instead of fit-to-panel
    bool viewerPopOut_     = false;  // request: undock into its own OS window at native size

    int  selected_ = ipc::kSlotHmd;  // slot index, -1 = none
    int  gizmoOperation_ = 0;        // 0 = move, 1 = rotate, 2 = both
    bool gizmoLocal_ = false;
    bool snap_ = false;
    float snapMove_ = 0.05f;
    float snapAngle_ = 15.0f;

    bool showLabels_  = true;
    bool showFrustum_ = true;
    bool showHelp_    = false;
    bool showDemo_    = false;
    float roomWidth_  = 4.0f;
    float roomDepth_  = 4.0f;
    float roomHeight_ = 2.5f;

    enum class CameraDrag { None, Look, Pan, Orbit };
    CameraDrag cameraDrag_ = CameraDrag::None;

    ControllerUiState controllerUi_[2];
    int  inputTab_ = 0;          // requested Input tab (-1 = leave as is)
    int  spaceTriggerHand_ = -1; // hand whose trigger Space pulls this frame (-1 = none)

    // Euler cache for the inspector so editing does not jump at gimbal lock.
    int               eulerSlot_ = -1;
    DirectX::XMFLOAT4 eulerQuat_{};
    DirectX::XMFLOAT3 eulerDeg_{};

    uint32_t syncedDriverPid_ = 0;
    bool     firstFrame_ = true;
};

}  // namespace tubane::sandbox
