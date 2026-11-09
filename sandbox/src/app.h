// tubane-sandbox application: UI panels, viewport, gizmo, IPC publishing.
#pragma once

#include <DirectXMath.h>
#include <imgui.h>

#include <cstdint>

#include "camera.h"
#include "d3d11_context.h"
#include "hmd_viewer.h"
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
    bool Init(D3D11Context* d3d);
    void Shutdown();

    // Builds the UI, renders the 3D viewport and publishes to shared memory.
    void Frame(float dt, double nowSeconds);

private:
    // UI
    void BuildDefaultLayout(ImGuiID dockspace);
    void DrawMenuBar();
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
    void     ComposeInput(int hand, bool forceTrigger);
    void     BumpConfigRevision();
    void     SyncConfigFromDriver();

    D3D11Context* d3d_ = nullptr;
    Renderer      renderer_;
    Camera        camera_;
    IpcLink       ipc_;
    HmdViewer     viewer_;

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

    // Euler cache for the inspector so editing does not jump at gimbal lock.
    int               eulerSlot_ = -1;
    DirectX::XMFLOAT4 eulerQuat_{};
    DirectX::XMFLOAT3 eulerDeg_{};

    uint32_t syncedDriverPid_ = 0;
    bool     firstFrame_ = true;
};

}  // namespace tubane::sandbox
