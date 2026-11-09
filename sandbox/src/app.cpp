#include "app.h"

#include <ImGuizmo.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

#include "file_dialog.h"
#include "tubane/default_state.h"

namespace tubane::sandbox {

using namespace DirectX;

namespace {

constexpr const char* kWinViewport  = "Viewport";
constexpr const char* kWinDevices   = "Devices";
constexpr const char* kWinInspector = "Inspector";
constexpr const char* kWinInput     = "Controller Input";
constexpr const char* kWinDriver    = "Driver / Settings";
constexpr const char* kWinViewer    = "HMD Viewer";

struct ButtonDef {
    const char* name;
    uint32_t    clickBit;  // 0 = touch-only
    uint32_t    touchBit;
};
constexpr ButtonDef kButtons[ControllerUiState::kButtonCount] = {
    {"System",     ipc::kBtnSystemClick,     ipc::kBtnSystemTouch},
    {"A",          ipc::kBtnAClick,          ipc::kBtnATouch},
    {"B",          ipc::kBtnBClick,          ipc::kBtnBTouch},
    {"Trigger",    ipc::kBtnTriggerClick,    ipc::kBtnTriggerTouch},
    {"Thumbstick", ipc::kBtnThumbstickClick, ipc::kBtnThumbstickTouch},
    {"Trackpad",   0,                        ipc::kBtnTrackpadTouch},
    {"Grip",       0,                        ipc::kBtnGripTouch},
};
constexpr int kBtnIndexTrigger = 3;
constexpr int kBtnIndexStick   = 4;
constexpr int kBtnIndexPad     = 5;
constexpr int kBtnIndexGrip    = 6;

ImU32 ToImColor(const Color& c, float alphaScale = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a * alphaScale));
}

Color Scale(const Color& c, float s, float alpha) {
    return {std::min(c.r * s, 1.0f), std::min(c.g * s, 1.0f), std::min(c.b * s, 1.0f), alpha};
}

XMFLOAT3 Transform(FXMMATRIX m, float x, float y, float z) {
    XMFLOAT3 r;
    XMStoreFloat3(&r, XMVector3TransformCoord(XMVectorSet(x, y, z, 1.0f), m));
    return r;
}

// Wireframe of an oriented box.
void BoxLines(Renderer& r, FXMMATRIX world, const XMFLOAT3& c, const XMFLOAT3& h, const Color& color) {
    XMFLOAT3 p[8];
    for (int i = 0; i < 8; ++i) {
        p[i] = Transform(world, c.x + ((i & 1) ? h.x : -h.x), c.y + ((i & 2) ? h.y : -h.y), c.z + ((i & 4) ? h.z : -h.z));
    }
    const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) r.Line(p[e[0]], p[e[1]], color);
}

// Momentary button: true while held.
bool HoldButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::Button(label, size);
    return ImGui::IsItemActive();
}

// 2D analog pad. Returns true while dragged.
bool Pad2D(const char* id, float xy[2], float size, bool spring) {
    ImGuiIO&     io = ImGui::GetIO();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool   active = ImGui::IsItemActive();
    const ImVec2 center(p0.x + size * 0.5f, p0.y + size * 0.5f);
    const float  radius = size * 0.5f - 2.0f;

    if (active) {
        float x = (io.MousePos.x - center.x) / radius;
        float y = -(io.MousePos.y - center.y) / radius;
        const float len = std::sqrt(x * x + y * y);
        if (len > 1.0f) { x /= len; y /= len; }
        xy[0] = x;
        xy[1] = y;
    } else if (spring) {
        xy[0] = xy[1] = 0.0f;
    }

    dl->AddCircleFilled(center, radius, IM_COL32(40, 44, 52, 255), 48);
    dl->AddCircle(center, radius, IM_COL32(110, 116, 130, 255), 48, 1.5f);
    dl->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), IM_COL32(80, 84, 96, 255));
    dl->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), IM_COL32(80, 84, 96, 255));
    const ImVec2 dot(center.x + xy[0] * radius, center.y - xy[1] * radius);
    dl->AddCircleFilled(dot, 6.0f, active ? IM_COL32(255, 200, 80, 255) : IM_COL32(200, 200, 210, 255));
    return active;
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
bool App::Init(D3D11Context* d3d, HWND hwnd) {
    d3d_  = d3d;
    hwnd_ = hwnd;
    if (!renderer_.Create(d3d->Device())) {
        return false;
    }
    ipc_.Open();  // failure is shown in the Driver panel; the UI still works offline
    gamepad_.Init(hwnd);
    RegisterRawMouse(hwnd);
    return true;
}

void App::Shutdown() {
    SetKeyboardCapture(false);
    gamepad_.Shutdown();
    ipc_.Close();
    renderer_.Destroy();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void App::Frame(float dt, double now) {
    now_ = now;
    ipc_.PollDriver(now);
    SyncConfigFromDriver();
    UpdateAnimation(dt);
    if (showSpoutWindow_) spout_.Update(d3d_->Device(), d3d_->Context(), spoutSender_, now);

    // File shortcuts (not while typing or while keys drive the controllers).
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.WantTextInput && !keyboardMouse_.Capturing()) {
            if (ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveSceneDialog(io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_O, false)) OpenSceneDialog();
        }
    }
    viewer_.Update(d3d_->Device(), d3d_->Context(), ipc_.Driver(), ipc_.DriverAlive(), now);

    // Physical inputs. F1 toggles keyboard/mouse capture; Esc or focus loss
    // releases it (handled inside KeyboardMouseSource::Update).
    gamepad_.Poll(now);
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false) && !ImGui::GetIO().WantTextInput) {
        SetKeyboardCapture(!keyboardMouse_.Capturing());
    }
    keyboardMouse_.Update(hwnd_, dt);
    SetKeyboardCapture(keyboardMouse_.Capturing());  // sync ImGui flags if Update released it

    ImGuizmo::BeginFrame();

    DrawMenuBar();

    const ImGuiID dockspace = ImGui::GetID("tubane_dockspace");
    if (firstFrame_) {
        firstFrame_ = false;
        if (ImGui::DockBuilderGetNode(dockspace) == nullptr) {
            BuildDefaultLayout(dockspace);
        }
    }
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());

    for (auto& ui : controllerUi_) {
        std::fill(std::begin(ui.hold), std::end(ui.hold), false);
    }

    DrawDevicesPanel();
    DrawInspectorPanel();
    DrawInputPanel();
    DrawDriverPanel();
    DrawViewerPanel();
    spaceTriggerHand_ = -1;
    DrawViewport(dt);  // last: sees this frame's edits, renders the scene
    DrawHelpWindow();
    DrawAnimationWindow();
    DrawSpoutWindow();

    // Controller composition after all panels so UI holds, Space, gamepad and
    // keyboard/mouse combine (runs even if the Viewport window is hidden).
    ExternalInput gamepadInput{}, keyboardInput{};
    gamepad_.Fill(gamepadInput);
    keyboardMouse_.Fill(keyboardInput);
    ComposeInput(0, spaceTriggerHand_ == 0, gamepadInput, keyboardInput);
    ComposeInput(1, spaceTriggerHand_ == 1, gamepadInput, keyboardInput);
    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);

    // Velocities from this frame's pose changes (world linear, local angular).
    velocity_.Apply(ipc_.Control().poses, now_, sendVelocity_);
    ipc_.Publish();
}

void App::BuildDefaultLayout(ImGuiID dockspace) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, vp->WorkSize);

    ImGuiID center = dockspace;
    ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
    ImGuiID leftBottom  = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.55f, nullptr, &left);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.42f, nullptr, &right);

    ImGui::DockBuilderDockWindow(kWinViewport, center);
    ImGui::DockBuilderDockWindow(kWinDevices, left);
    ImGui::DockBuilderDockWindow(kWinInspector, leftBottom);
    ImGui::DockBuilderDockWindow(kWinInput, right);
    ImGui::DockBuilderDockWindow(kWinViewer, rightBottom);
    ImGui::DockBuilderDockWindow(kWinDriver, rightBottom);
    ImGui::DockBuilderFinish(dockspace);
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
void App::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;

    DrawFileMenu();

    if (ImGui::BeginMenu("Scene")) {
        if (ImGui::MenuItem("Reset all poses")) {
            for (uint32_t i = 0; i < ipc::kMaxDevices; ++i) ipc_.Control().poses[i] = ipc::DefaultPose(i);
        }
        if (ImGui::MenuItem("Release all inputs")) {
            for (auto& ui : controllerUi_) ui = ControllerUiState{};
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Reset camera")) camera_.Reset();
        if (ImGui::MenuItem("Focus selected", "F", false, selected_ >= 0)) FocusSelected();
        ImGui::Separator();
        ImGui::MenuItem("Device labels", nullptr, &showLabels_);
        ImGui::MenuItem("HMD frustum", nullptr, &showFrustum_);
        ImGui::MenuItem("Body links (IK)", nullptr, &showBodyLinks_);
        ImGui::Separator();
        ImGui::TextDisabled("Room size (m)");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::DragFloat("Width (X)", &roomWidth_, 0.05f, 1.0f, 20.0f, "%.2f");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::DragFloat("Depth (Z)", &roomDepth_, 0.05f, 1.0f, 20.0f, "%.2f");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::DragFloat("Height (Y)", &roomHeight_, 0.05f, 1.0f, 10.0f, "%.2f");
        ImGui::Separator();
        ImGui::MenuItem("ImGui demo", nullptr, &showDemo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Controls", nullptr, &showHelp_);
        ImGui::EndMenu();
    }

    if (now_ < notifyUntil_) {
        ImGui::SameLine(0.0f, 30.0f);
        ImGui::TextColored(notifyError_ ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(0.5f, 0.85f, 1, 1), "%s", notifyText_.c_str());
    }
    if (animPlaying_) {
        ImGui::SameLine(0.0f, 30.0f);
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.5f, 1), "[Anim %.1fs]", animTime_);
    }
    if (keyboardMouse_.Capturing()) {
        ImGui::SameLine(0.0f, 30.0f);
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "[KB/Mouse captured - Esc]");
    } else if (gamepad_.Connected()) {
        ImGui::SameLine(0.0f, 30.0f);
        ImGui::TextDisabled("[Gamepad]");
    }

    // Right-aligned connection status.
    const bool  alive = ipc_.DriverAlive();
    const char* text  = !ipc_.IsOpen() ? "IPC unavailable" : alive ? "SteamVR driver connected" : "SteamVR driver not running";
    const float width = ImGui::CalcTextSize(text).x + 24.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - width);
    ImGui::TextColored(alive ? ImVec4(0.4f, 0.9f, 0.4f, 1) : ImVec4(0.95f, 0.65f, 0.3f, 1), "%s", text);

    ImGui::EndMainMenuBar();
}

void App::DrawHelpWindow() {
    if (!showHelp_) return;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Controls", &showHelp_)) {
        ImGui::SeparatorText("Camera (Viewport)");
        ImGui::BulletText("RMB drag: look around");
        ImGui::BulletText("RMB + W/A/S/D: fly, Q/E: down/up, Shift: fast");
        ImGui::BulletText("RMB + wheel: change fly speed");
        ImGui::BulletText("MMB drag: pan,  wheel: dolly");
        ImGui::BulletText("Alt + LMB drag: orbit,  F: focus selected");
        ImGui::SeparatorText("Devices");
        ImGui::BulletText("LMB click: select device,  Esc: deselect");
        ImGui::BulletText("W: move gizmo,  E: rotate gizmo,  R: move + rotate");
        ImGui::BulletText("X: toggle world / local space");
        ImGui::BulletText("Hold Ctrl while dragging: snap");
        ImGui::BulletText("Hold Space (controller selected): pull trigger");
        ImGui::SeparatorText("Controller input");
        ImGui::BulletText("Gamepad (XInput / DirectInput): always active when connected");
        ImGui::BulletText("F1: capture keyboard & mouse for the controllers, Esc: release");
        ImGui::BulletText("Mappings: Controller Input panel");
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------
void App::DrawViewport(float dt) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::Begin(kWinViewport, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }

    ImGuiIO&     io     = ImGui::GetIO();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2       size   = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 16.0f);
    size.y = std::max(size.y, 16.0f);

    renderer_.EnsureTarget(static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y));
    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(renderer_.Texture())), size);
    // While keyboard/mouse is captured for the controllers, the viewport
    // ignores mouse and shortcuts entirely.
    const bool capturing    = keyboardMouse_.Capturing();
    const bool imageHovered = ImGui::IsItemHovered() && !capturing;
    if (capturing) cameraDrag_ = CameraDrag::None;

    // --- Camera -------------------------------------------------------------
    if (cameraDrag_ == CameraDrag::None && imageHovered) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) cameraDrag_ = CameraDrag::Look;
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) cameraDrag_ = CameraDrag::Pan;
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt) cameraDrag_ = CameraDrag::Orbit;
    }
    if ((cameraDrag_ == CameraDrag::Look && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) ||
        (cameraDrag_ == CameraDrag::Pan && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) ||
        (cameraDrag_ == CameraDrag::Orbit && !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        cameraDrag_ = CameraDrag::None;
    }

    CameraInput ci;
    ci.dt      = dt;
    ci.mouseDx = io.MouseDelta.x;
    ci.mouseDy = io.MouseDelta.y;
    ci.wheel   = imageHovered ? io.MouseWheel : 0.0f;
    ci.look    = cameraDrag_ == CameraDrag::Look;
    ci.pan     = cameraDrag_ == CameraDrag::Pan;
    ci.orbit   = cameraDrag_ == CameraDrag::Orbit;
    if (ci.look) {
        ci.forward = ImGui::IsKeyDown(ImGuiKey_W);
        ci.back    = ImGui::IsKeyDown(ImGuiKey_S);
        ci.left    = ImGui::IsKeyDown(ImGuiKey_A);
        ci.right   = ImGui::IsKeyDown(ImGuiKey_D);
        ci.up      = ImGui::IsKeyDown(ImGuiKey_E);
        ci.down    = ImGui::IsKeyDown(ImGuiKey_Q);
        ci.fast    = io.KeyShift;
    }
    camera_.Update(ci);

    // --- Shortcuts ----------------------------------------------------------
    const bool shortcutsActive = imageHovered && cameraDrag_ == CameraDrag::None && !io.WantTextInput;
    if (shortcutsActive) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmoOperation_ = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmoOperation_ = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmoOperation_ = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) gizmoLocal_ = !gizmoLocal_;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) FocusSelected();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) selected_ = -1;
    }
    const bool spaceTrigger = imageHovered && !io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_Space) &&
                              (selected_ == ipc::kSlotLeftHand || selected_ == ipc::kSlotRightHand);

    // --- Matrices -----------------------------------------------------------
    const float    aspect = size.x / size.y;
    const XMMATRIX view   = camera_.View();
    const XMMATRIX proj   = camera_.Projection(aspect);
    const XMMATRIX viewProj = view * proj;

    // --- Gizmo --------------------------------------------------------------
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::AllowAxisFlip(false);
    ImGuizmo::SetGizmoSizeClipSpace(0.16f);

    // No gizmo while keyboard/mouse is captured: clicks there drive the
    // controller triggers and must never move a pose.
    // Devices driven by a playing animation cannot be dragged either.
    const bool animLocked = selected_ >= 0 && animPlaying_ && animDriven_[selected_];
    if (!capturing && !animLocked && selected_ >= 0 && SlotVisible(static_cast<uint32_t>(selected_))) {
        ipc::DevicePose& pose = ipc_.Control().poses[selected_];
        XMFLOAT4X4 viewF, projF, model;
        XMStoreFloat4x4(&viewF, view);
        XMStoreFloat4x4(&projF, proj);
        XMStoreFloat4x4(&model, PoseMatrix(pose));

        ImGuizmo::OPERATION op = gizmoOperation_ == 0   ? ImGuizmo::TRANSLATE
                                 : gizmoOperation_ == 1 ? ImGuizmo::ROTATE
                                                        : (ImGuizmo::TRANSLATE | ImGuizmo::ROTATE);
        const bool  doSnap = snap_ != io.KeyCtrl;  // Ctrl inverts the snap toggle
        float       snapValues[3];
        if (gizmoOperation_ == 1) {
            snapValues[0] = snapValues[1] = snapValues[2] = snapAngle_;
        } else {
            snapValues[0] = snapValues[1] = snapValues[2] = snapMove_;
        }
        if (ImGuizmo::Manipulate(&viewF._11, &projF._11, op, gizmoLocal_ ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                                 &model._11, nullptr, doSnap ? snapValues : nullptr)) {
            XMVECTOR s, q, t;
            if (XMMatrixDecompose(&s, &q, &t, XMLoadFloat4x4(&model))) {
                XMFLOAT3 pos;
                XMStoreFloat3(&pos, t);
                SetPosition(pose, pos);
                SetRotation(pose, q);
            }
        }
    }

    // --- Toolbar overlay ----------------------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + 8.0f));
    DrawViewportToolbar();

    // --- Picking ------------------------------------------------------------
    if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !ImGui::IsAnyItemHovered() &&
        !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
        const float    nx = (io.MousePos.x - origin.x) / size.x * 2.0f - 1.0f;
        const float    ny = 1.0f - (io.MousePos.y - origin.y) / size.y * 2.0f;
        const XMMATRIX inv = XMMatrixInverse(nullptr, viewProj);
        const XMVECTOR nearP = XMVector3TransformCoord(XMVectorSet(nx, ny, 0.0f, 1.0f), inv);
        const XMVECTOR farP  = XMVector3TransformCoord(XMVectorSet(nx, ny, 1.0f, 1.0f), inv);
        Ray ray;
        XMStoreFloat3(&ray.origin, nearP);
        XMStoreFloat3(&ray.direction, XMVector3Normalize(XMVectorSubtract(farP, nearP)));
        selected_ = PickDevice(ray);
        if (selected_ == ipc::kSlotLeftHand) inputTab_ = 0;
        if (selected_ == ipc::kSlotRightHand) inputTab_ = 1;
    }

    // --- Labels + render ----------------------------------------------------
    if (showLabels_) DrawLabels(viewProj, origin, size);

    if (capturing) {
        const char*  text = "KEYBOARD / MOUSE -> CONTROLLERS   (Esc or F1 to release)";
        const ImVec2 ts   = ImGui::CalcTextSize(text);
        const ImVec2 pos(origin.x + (size.x - ts.x) * 0.5f, origin.y + size.y - ts.y - 16.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(pos.x - 10, pos.y - 6), ImVec2(pos.x + ts.x + 10, pos.y + ts.y + 6),
                          IM_COL32(140, 40, 40, 230), 4.0f);
        dl->AddText(pos, IM_COL32(255, 255, 255, 255), text);
    }

    if (spaceTrigger) spaceTriggerHand_ = selected_ == ipc::kSlotLeftHand ? 0 : 1;

    RenderScene(view, proj);
    ImGui::End();
}

void App::DrawViewportToolbar() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.09f, 0.11f, 0.85f));
    ImGui::BeginChild("##toolbar", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    const char* ops[] = {"Move (W)", "Rotate (E)", "Both (R)"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(ops[i], gizmoOperation_ == i)) gizmoOperation_ = i;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::RadioButton("World", !gizmoLocal_)) gizmoLocal_ = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Local", gizmoLocal_)) gizmoLocal_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &snap_);
    if (snap_ || ImGui::GetIO().KeyCtrl) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::DragFloat("##snapmove", &snapMove_, 0.005f, 0.001f, 1.0f, "%.3f m");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        ImGui::DragFloat("##snapangle", &snapAngle_, 0.5f, 1.0f, 90.0f, "%.0f deg");
    }
    ImGui::TextDisabled("Fly %.1f m/s", camera_.FlySpeed());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Scene rendering
// ---------------------------------------------------------------------------
void App::RenderScene(FXMMATRIX view, CXMMATRIX proj) {
    renderer_.Begin(d3d_->Context(), view * proj, {0.075f, 0.08f, 0.095f, 1.0f});
    DrawRoom();
    const uint32_t count = ipc::kSlotFirstTracker + VisibleTrackerCount();
    for (uint32_t slot = 0; slot < count; ++slot) DrawDevice(slot);
    if (showBodyLinks_) DrawBodyLinks();
    if (showFrustum_) DrawHmdFrustum();
    renderer_.End();
}

void App::DrawRoom() {
    // Large faint floor grid (1 m).
    const Color faint{0.22f, 0.24f, 0.28f, 1.0f};
    for (int i = -10; i <= 10; ++i) {
        const float v = static_cast<float>(i);
        renderer_.Line({v, 0.0f, -10.0f}, {v, 0.0f, 10.0f}, faint);
        renderer_.Line({-10.0f, 0.0f, v}, {10.0f, 0.0f, v}, faint);
    }

    // Room floor grid (0.5 m) inside the play area.
    const float hw = roomWidth_ * 0.5f, hd = roomDepth_ * 0.5f, h = roomHeight_;
    const Color minor{0.30f, 0.34f, 0.40f, 1.0f};
    const float y = 0.001f;
    for (float x = -std::floor(hw / 0.5f) * 0.5f; x <= hw + 1e-4f; x += 0.5f) {
        renderer_.Line({x, y, -hd}, {x, y, hd}, minor);
    }
    for (float z = -std::floor(hd / 0.5f) * 0.5f; z <= hd + 1e-4f; z += 0.5f) {
        renderer_.Line({-hw, y, z}, {hw, y, z}, minor);
    }

    // Room box (wireframe "箱").
    const Color edge{0.35f, 0.75f, 0.85f, 1.0f};
    const XMFLOAT3 c[8] = {{-hw, 0, -hd}, {hw, 0, -hd}, {hw, 0, hd}, {-hw, 0, hd},
                           {-hw, h, -hd}, {hw, h, -hd}, {hw, h, hd}, {-hw, h, hd}};
    for (int i = 0; i < 4; ++i) {
        renderer_.Line(c[i], c[(i + 1) % 4], edge);
        renderer_.Line(c[i + 4], c[(i + 1) % 4 + 4], edge);
        renderer_.Line(c[i], c[i + 4], edge);
    }

    // World axes at the origin and the forward (-Z) arrow.
    const float y2 = 0.002f;
    renderer_.Line({0, y2, 0}, {0.5f, y2, 0}, {0.95f, 0.3f, 0.3f, 1});
    renderer_.Line({0, 0, 0}, {0, 0.5f, 0}, {0.35f, 0.9f, 0.3f, 1});
    renderer_.Line({0, y2, 0}, {0, y2, 0.5f}, {0.3f, 0.5f, 1.0f, 1});
    const Color fwd{0.9f, 0.9f, 0.9f, 0.9f};
    renderer_.Line({0, y2, -0.6f}, {0, y2, -1.1f}, fwd);
    renderer_.Line({0, y2, -1.1f}, {-0.08f, y2, -0.98f}, fwd);
    renderer_.Line({0, y2, -1.1f}, {0.08f, y2, -0.98f}, fwd);
}

void App::DrawDevice(uint32_t slot) {
    const ipc::DevicePose& pose = ipc_.Control().poses[slot];
    const DeviceInfo       info = DescribeSlot(slot);
    const DeviceModel&     model = ModelFor(info.kind, slot);
    const XMMATRIX         world = PoseMatrix(pose);

    const bool  selected  = static_cast<int>(slot) == selected_;
    const bool  pending   = SlotStatus(slot)[0] != '\0';
    const bool  connected = (pose.flags & ipc::kPoseConnected) != 0;
    const float alpha     = (pending || !connected) ? 0.35f : 1.0f;
    const float boost     = selected ? 1.25f : 1.0f;

    for (const BoxPart& part : model.parts) {
        renderer_.Box(world, part.center, part.size, Scale(info.color, part.shade * boost, alpha));
    }
    renderer_.Axes(world, info.kind == DeviceKind::Hmd ? 0.16f : 0.10f, alpha);

    if (selected) {
        const XMFLOAT3 h{model.pickHalfExtents.x + 0.01f, model.pickHalfExtents.y + 0.01f, model.pickHalfExtents.z + 0.01f};
        BoxLines(renderer_, world, model.pickCenter, h, {1.0f, 0.85f, 0.2f, 1.0f});
    }

    // Drop line to the floor for depth perception.
    const XMFLOAT3 p = PositionOf(pose);
    renderer_.Line(p, {p.x, 0.0f, p.z}, {0.5f, 0.5f, 0.55f, 0.35f * alpha});
}

// White leg links between the body trackers, like the VRIK (Final IK) gizmo:
// waist -> foot, or waist -> knee -> foot when the knee (thigh) tracker exists.
// Tracker index order follows DescribeSlot: 0 waist, 1/2 feet, 6/7 knees.
void App::DrawBodyLinks() {
    constexpr uint32_t kWaist = 0, kFootL = 1, kFootR = 2, kKneeL = 6, kKneeR = 7;
    auto has = [&](uint32_t index) { return SlotVisible(ipc::kSlotFirstTracker + index); };
    auto pos = [&](uint32_t index) { return PositionOf(ipc_.Control().poses[ipc::kSlotFirstTracker + index]); };
    if (!has(kWaist)) return;

    const Color    white{0.95f, 0.95f, 0.95f, 0.9f};
    const XMFLOAT3 waist = pos(kWaist);
    const uint32_t legs[2][2] = {{kFootL, kKneeL}, {kFootR, kKneeR}};
    for (const auto& leg : legs) {
        const bool foot = has(leg[0]);
        const bool knee = has(leg[1]);
        if (knee) {
            const XMFLOAT3 k = pos(leg[1]);
            renderer_.Line(waist, k, white);
            if (foot) renderer_.Line(k, pos(leg[0]), white);
        } else if (foot) {
            renderer_.Line(waist, pos(leg[0]), white);
        }
    }
}

void App::DrawHmdFrustum() {
    const ipc::DevicePose& pose  = ipc_.Control().poses[ipc::kSlotHmd];
    const XMMATRIX         world = PoseMatrix(pose);
    const ipc::Resolution  res   = ipc::ResolutionOf(ipc_.Control().config.resolution);
    // Matches HmdDevice::GetProjectionRaw: 90 deg horizontal, vertical from aspect.
    const float d  = 0.6f;
    const float hx = 1.0f * d;
    const float hy = d * static_cast<float>(res.height) / static_cast<float>(res.width);
    const XMFLOAT3 o  = Transform(world, 0, 0, 0);
    const XMFLOAT3 c0 = Transform(world, -hx, -hy, -d);
    const XMFLOAT3 c1 = Transform(world, hx, -hy, -d);
    const XMFLOAT3 c2 = Transform(world, hx, hy, -d);
    const XMFLOAT3 c3 = Transform(world, -hx, hy, -d);
    const Color col{0.95f, 0.95f, 1.0f, 0.35f};
    renderer_.Line(o, c0, col);
    renderer_.Line(o, c1, col);
    renderer_.Line(o, c2, col);
    renderer_.Line(o, c3, col);
    renderer_.Line(c0, c1, col);
    renderer_.Line(c1, c2, col);
    renderer_.Line(c2, c3, col);
    renderer_.Line(c3, c0, col);
}

void App::DrawLabels(FXMMATRIX viewProj, const ImVec2& origin, const ImVec2& size) {
    ImDrawList*    dl    = ImGui::GetWindowDrawList();
    const uint32_t count = ipc::kSlotFirstTracker + VisibleTrackerCount();
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    for (uint32_t slot = 0; slot < count; ++slot) {
        const XMFLOAT3 p = PositionOf(ipc_.Control().poses[slot]);
        XMFLOAT4 clip;
        XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(p.x, p.y + 0.09f, p.z, 1.0f), viewProj));
        if (clip.w <= 0.01f) continue;
        const float sx = origin.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x;
        const float sy = origin.y + (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * size.y;

        const DeviceInfo info = DescribeSlot(slot);
        char text[96];
        std::snprintf(text, sizeof(text), "%s %s", info.name.c_str(), SlotStatus(slot));
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 pos(sx - ts.x * 0.5f, sy - ts.y);
        const bool   sel = static_cast<int>(slot) == selected_;
        dl->AddRectFilled(ImVec2(pos.x - 4, pos.y - 2), ImVec2(pos.x + ts.x + 4, pos.y + ts.y + 2),
                          sel ? IM_COL32(70, 60, 20, 220) : IM_COL32(15, 17, 22, 170), 3.0f);
        dl->AddText(pos, sel ? IM_COL32(255, 220, 90, 255) : ToImColor(info.color), text);
    }
    dl->PopClipRect();
}

int App::PickDevice(const Ray& ray) const {
    int   best  = -1;
    float bestT = 1e30f;
    const uint32_t count = ipc::kSlotFirstTracker + VisibleTrackerCount();
    for (uint32_t slot = 0; slot < count; ++slot) {
        const DeviceInfo   info  = DescribeSlot(slot);
        const DeviceModel& model = ModelFor(info.kind, slot);
        // Slightly inflated pick volume so small devices are easy to hit.
        const XMFLOAT3 h{model.pickHalfExtents.x + 0.02f, model.pickHalfExtents.y + 0.02f, model.pickHalfExtents.z + 0.02f};
        const float t = RayBoxDistance(ray, PoseMatrix(ipc_.Control().poses[slot]), model.pickCenter, h);
        if (t >= 0.0f && t < bestT) {
            bestT = t;
            best  = static_cast<int>(slot);
        }
    }
    return best;
}

void App::FocusSelected() {
    if (selected_ < 0) return;
    camera_.Focus(PositionOf(ipc_.Control().poses[selected_]), 1.0f);
}

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------
void App::DrawDevicesPanel() {
    if (!ImGui::Begin(kWinDevices)) {
        ImGui::End();
        return;
    }
    const uint32_t count = ipc::kSlotFirstTracker + VisibleTrackerCount();
    const auto&    driver = ipc_.Driver();
    for (uint32_t slot = 0; slot < count; ++slot) {
        const DeviceInfo info = DescribeSlot(slot);
        ImGui::PushID(static_cast<int>(slot));
        ImGui::ColorButton("##c", ImVec4(info.color.r, info.color.g, info.color.b, 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(10, 10));
        ImGui::SameLine();
        char label[128];
        std::snprintf(label, sizeof(label), "%s %s", info.name.c_str(), SlotStatus(slot));
        if (ImGui::Selectable(label, selected_ == static_cast<int>(slot), ImGuiSelectableFlags_AllowDoubleClick)) {
            selected_ = static_cast<int>(slot);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) FocusSelected();
            if (slot == ipc::kSlotLeftHand) inputTab_ = 0;
            if (slot == ipc::kSlotRightHand) inputTab_ = 1;
        }
        if (ipc_.DriverAlive() && driver.deviceIds[slot] != 0xFFFFFFFFu) {
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 40.0f);
            ImGui::TextDisabled("#%u", driver.deviceIds[slot]);
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button("Reset all poses")) {
        for (uint32_t i = 0; i < ipc::kMaxDevices; ++i) ipc_.Control().poses[i] = ipc::DefaultPose(i);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("double-click = focus");
    ImGui::End();
}

void App::DrawInspectorPanel() {
    if (!ImGui::Begin(kWinInspector)) {
        ImGui::End();
        return;
    }
    if (selected_ < 0 || !SlotVisible(static_cast<uint32_t>(selected_))) {
        ImGui::TextDisabled("No device selected.");
        ImGui::TextDisabled("Click a device in the viewport or the Devices list.");
        ImGui::End();
        return;
    }

    const uint32_t   slot = static_cast<uint32_t>(selected_);
    ipc::DevicePose& pose = ipc_.Control().poses[slot];
    const DeviceInfo info = DescribeSlot(slot);

    ImGui::TextColored(ImVec4(info.color.r, info.color.g, info.color.b, 1.0f), "%s", info.name.c_str());
    ImGui::TextDisabled("slot %u   OpenVR device %s", slot,
                        (ipc_.DriverAlive() && ipc_.Driver().deviceIds[slot] != 0xFFFFFFFFu)
                            ? std::to_string(ipc_.Driver().deviceIds[slot]).c_str()
                            : "-");
    ImGui::Separator();

    // Position
    XMFLOAT3 pos = PositionOf(pose);
    if (ImGui::DragFloat3("Position (m)", &pos.x, 0.005f, -50.0f, 50.0f, "%.3f")) {
        SetPosition(pose, pos);
    }

    // Rotation as Euler (cached so the values stay stable while dragging).
    const XMFLOAT4 q = RotationOf(pose);
    const bool cacheValid = eulerSlot_ == selected_ && std::fabs(q.x - eulerQuat_.x) < 1e-5f &&
                            std::fabs(q.y - eulerQuat_.y) < 1e-5f && std::fabs(q.z - eulerQuat_.z) < 1e-5f &&
                            std::fabs(q.w - eulerQuat_.w) < 1e-5f;
    if (!cacheValid) {
        eulerSlot_ = selected_;
        eulerQuat_ = q;
        eulerDeg_  = EulerDegFromQuaternion(XMLoadFloat4(&q));
    }
    if (ImGui::DragFloat3("Pitch/Yaw/Roll", &eulerDeg_.x, 0.5f, -360.0f, 360.0f, "%.1f deg")) {
        SetRotation(pose, QuaternionFromEulerDeg(eulerDeg_));
        eulerQuat_ = RotationOf(pose);
    }
    HelpMarker("Degrees. Pitch = about X, Yaw = about Y, Roll = about Z (applied roll -> pitch -> yaw).");

    ImGui::Text("Quaternion  w %+.4f  x %+.4f  y %+.4f  z %+.4f", pose.rotation[0], pose.rotation[1], pose.rotation[2],
                pose.rotation[3]);

    ImGui::Spacing();
    bool connected = (pose.flags & ipc::kPoseConnected) != 0;
    bool valid     = (pose.flags & ipc::kPoseValid) != 0;
    if (ImGui::Checkbox("Connected", &connected)) {
        pose.flags = connected ? (pose.flags | ipc::kPoseConnected) : (pose.flags & ~ipc::kPoseConnected);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Tracking valid", &valid)) {
        pose.flags = valid ? (pose.flags | ipc::kPoseValid) : (pose.flags & ~ipc::kPoseValid);
    }
    HelpMarker("Connected off: SteamVR shows the device as disconnected.\n"
               "Tracking valid off: reported as out of range (pose not valid).");

    ImGui::Spacing();
    if (ImGui::Button("Reset pose")) pose = ipc::DefaultPose(slot);
    ImGui::SameLine();
    if (ImGui::Button("Zero rotation")) SetRotation(pose, XMQuaternionIdentity());
    ImGui::SameLine();
    if (ImGui::Button("Focus (F)")) FocusSelected();
    ImGui::End();
}

void App::DrawInputPanel() {
    if (!ImGui::Begin(kWinInput)) {
        ImGui::End();
        return;
    }
    DrawInputDevices();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("hands")) {
        for (int hand = 0; hand < 2; ++hand) {
            const ImGuiTabItemFlags flags = (inputTab_ == hand) ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(hand == 0 ? "Left hand" : "Right hand", nullptr, flags)) continue;
            ControllerUiState& ui = controllerUi_[hand];
            ImGui::PushID(hand);

            ImGui::SeparatorText("Buttons");
            if (ImGui::BeginTable("buttons", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Button");
                ImGui::TableSetupColumn("Hold");
                ImGui::TableSetupColumn("Click");
                ImGui::TableSetupColumn("Touch");
                ImGui::TableHeadersRow();
                for (int b = 0; b < ControllerUiState::kButtonCount; ++b) {
                    ImGui::PushID(b);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kButtons[b].name);
                    ImGui::TableNextColumn();
                    ui.hold[b] = HoldButton("press", ImVec2(-FLT_MIN, 0));
                    ImGui::TableNextColumn();
                    if (kButtons[b].clickBit) ImGui::Checkbox("##click", &ui.latchClick[b]);
                    else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    ImGui::Checkbox("##touch", &ui.latchTouch[b]);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Hold = momentary. Click/Touch = latched. Space in viewport = trigger.");

            ImGui::SeparatorText("Analog");
            ImGui::SliderFloat("Trigger", &ui.trigger, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Grip value", &ui.gripValue, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Grip force", &ui.gripForce, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Trackpad force", &ui.padForce, 0.0f, 1.0f, "%.2f");

            const float padSize = 110.0f;
            ImGui::BeginGroup();
            ImGui::TextUnformatted("Thumbstick");
            Pad2D("##stick", ui.stick, padSize, ui.stickSpring);
            ImGui::Checkbox("Spring##stick", &ui.stickSpring);
            ImGui::Text("%+.2f %+.2f", ui.stick[0], ui.stick[1]);
            ImGui::EndGroup();
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::BeginGroup();
            ImGui::TextUnformatted("Trackpad");
            Pad2D("##pad", ui.pad, padSize, ui.padSpring);
            ImGui::Checkbox("Spring##pad", &ui.padSpring);
            ImGui::Text("%+.2f %+.2f", ui.pad[0], ui.pad[1]);
            ImGui::EndGroup();

            ImGui::SeparatorText("Fingers (hand skeleton)");

            // Gesture presets (VRChat gesture names). They switch to manual curls.
            struct Gesture { const char* name; float thumb, index, middle, ring, pinky; };
            static constexpr Gesture kGestures[] = {
                {"Open", 0, 0, 0, 0, 0},       {"Fist", 1, 1, 1, 1, 1},     {"Point", 1, 0, 1, 1, 1},
                {"Victory", 1, 0, 0, 1, 1},    {"Rock'n'roll", 1, 0, 1, 1, 0}, {"Hand gun", 0, 0, 1, 1, 1},
                {"Thumbs up", 0, 1, 1, 1, 1},
            };
            for (size_t g = 0; g < std::size(kGestures); ++g) {
                if (g % 4 != 0) ImGui::SameLine();
                if (ImGui::Button(kGestures[g].name)) {
                    const Gesture& gs = kGestures[g];
                    ui.autoFingers   = false;
                    ui.thumbOverride = true;
                    ui.thumb         = gs.thumb;
                    ui.fingers[0] = gs.index;
                    ui.fingers[1] = gs.middle;
                    ui.fingers[2] = gs.ring;
                    ui.fingers[3] = gs.pinky;
                }
            }

            ImGui::Checkbox("Auto (index = trigger, others = grip)", &ui.autoFingers);
            ImGui::BeginDisabled(ui.autoFingers);
            const char* fingerNames[4] = {"Index", "Middle", "Ring", "Pinky"};
            for (int f = 0; f < 4; ++f) ImGui::SliderFloat(fingerNames[f], &ui.fingers[f], 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();

            ImGui::Checkbox("Thumb override", &ui.thumbOverride);
            HelpMarker("Off: like a real Index controller, the thumb curls when it touches A / B / thumbstick / "
                       "trackpad and closes fully when one is pressed.\nOn: the thumb uses the slider below.");
            ImGui::BeginDisabled(!ui.thumbOverride);
            ImGui::SliderFloat("Thumb", &ui.thumb, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::TextDisabled("Drives /input/skeleton (finger tracking, hand gestures) and /input/finger/*.");

            ImGui::SeparatorText("Sent to driver");
            const ipc::ControllerInput& in = ipc_.Control().controllers[hand];
            ImGui::Text("buttons 0x%03X  trigger %.2f  grip %.2f", in.buttons, in.trigger, in.gripValue);

            ImGui::PopID();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    inputTab_ = -1;
    ImGui::End();
}

void App::DrawDriverPanel() {
    if (!ImGui::Begin(kWinDriver)) {
        ImGui::End();
        return;
    }
    const ipc::DriverBlock& d = ipc_.Driver();

    ImGui::SeparatorText("Status");
    if (!ipc_.IsOpen()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Shared memory: %s (win32 error %lu)", ipc::ToString(ipc_.LastResult()),
                           ipc_.LastWin32Error());
    } else if (ipc_.DriverAlive()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "Driver connected");
        ImGui::Text("vrserver pid %u   heartbeat %llu", d.driverProcessId, static_cast<unsigned long long>(d.driverHeartbeat));
        ImGui::Text("Render target %u x %u per eye (%s)", d.renderWidth, d.renderHeight,
                    d.activeResolution == ipc::ResolutionPreset::WQHD ? "WQHD" : "FHD");
        ImGui::Text("Trackers spawned: %u", d.activeTrackerCount);
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1), "Driver not running");
        ImGui::TextWrapped("Start SteamVR with the tubane driver registered (tools\\register_driver.ps1). "
                           "Edits made now are applied as soon as the driver connects.");
    }
    if (ipc_.OtherWriterPid() != 0) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Another sandbox (pid %u) was running at start-up!", ipc_.OtherWriterPid());
        HelpMarker("Only one process may write poses. Close the other instance (or tubane_shm_probe write commands).");
    }

    ImGui::SeparatorText("Settings");
    ipc::ControlConfig& cfg = ipc_.Control().config;

    int res = cfg.resolution == ipc::ResolutionPreset::WQHD ? 1 : 0;
    if (ImGui::Combo("Resolution", &res, "FHD (1920 x 1080)\0WQHD (2560 x 1440)\0")) {
        cfg.resolution = res == 1 ? ipc::ResolutionPreset::WQHD : ipc::ResolutionPreset::FHD;
        BumpConfigRevision();
    }
    HelpMarker("Per-eye render target of the virtual HMD. Saved to steamvr.vrsettings; restart SteamVR to apply.");

    int trackers = static_cast<int>(cfg.trackerCount);
    if (ImGui::SliderInt("Trackers", &trackers, 0, static_cast<int>(ipc::kMaxTrackers))) {
        cfg.trackerCount = static_cast<uint32_t>(trackers);
        BumpConfigRevision();
    }
    HelpMarker("Number of Vive Trackers spawned. Saved to steamvr.vrsettings; restart SteamVR to apply.");

    int eye = cfg.viewerEye == ipc::ViewerEye::Right ? 1 : 0;
    ImGui::TextUnformatted("Viewer eye");
    ImGui::SameLine();
    if (ImGui::RadioButton("Left", eye == 0)) { cfg.viewerEye = ipc::ViewerEye::Left; BumpConfigRevision(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Right", eye == 1)) { cfg.viewerEye = ipc::ViewerEye::Right; BumpConfigRevision(); }

    if (ipc_.DriverAlive()) {
        if (d.appliedRevision != cfg.revision) {
            ImGui::TextDisabled("Waiting for driver to apply settings...");
        } else if (d.restartRequired) {
            ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "Saved. Restart SteamVR to apply resolution / tracker count.");
        }
    }

    ImGui::Checkbox("Send velocities (pose prediction)", &sendVelocity_);
    HelpMarker("Linear and angular velocity computed from how devices move each frame. SteamVR uses them to "
               "predict poses and games use them e.g. for throwing. Off = devices report zero velocity.");

    ImGui::SeparatorText("GPU");
    ImGui::TextWrapped("Sandbox adapter: %ls", d3d_->AdapterName());
    ImGui::TextDisabled("LUID 0x%016llX", static_cast<unsigned long long>(d3d_->AdapterLuid()));
    ImGui::End();
}

void App::DrawViewerPanel() {
    // The viewer always gets its own OS window when floating (never merged
    // into the main window), so "Pop out" yields an independent window.
    ImGuiWindowClass windowClass;
    windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&windowClass);

    const ipc::DriverBlock& d = ipc_.Driver();
    if (viewerPopOut_) {
        viewerPopOut_ = false;
        const float w = static_cast<float>(viewer_.HasFrame() ? viewer_.Width() : ipc::ResolutionOf(ipc_.Control().config.resolution).width);
        const float h = static_cast<float>(viewer_.HasFrame() ? viewer_.Height() : ipc::ResolutionOf(ipc_.Control().config.resolution).height);
        const ImGuiViewport* main = ImGui::GetMainViewport();
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(main->Pos.x + 60.0f, main->Pos.y + 60.0f), ImGuiCond_Always);
        // Window = native-resolution image + title bar + toolbar + status line.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float chrome = ImGui::GetFrameHeight() + ImGui::GetFrameHeightWithSpacing() * 2.0f +
                             style.WindowPadding.y * 2.0f + style.ItemSpacing.y;
        ImGui::SetNextWindowSize(ImVec2(w + style.WindowPadding.x * 2.0f, h + chrome), ImGuiCond_Always);
        viewerNativeSize_ = true;
    }

    if (!ImGui::Begin(kWinViewer, nullptr, viewerNativeSize_ ? ImGuiWindowFlags_HorizontalScrollbar : 0)) {
        ImGui::End();
        return;
    }

    ipc::ControlConfig& cfg = ipc_.Control().config;

    // --- Toolbar ------------------------------------------------------------
    ImGui::TextUnformatted("Eye");
    ImGui::SameLine();
    if (ImGui::RadioButton("Left##eye", cfg.viewerEye == ipc::ViewerEye::Left)) {
        cfg.viewerEye = ipc::ViewerEye::Left;
        BumpConfigRevision();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Right##eye", cfg.viewerEye == ipc::ViewerEye::Right)) {
        cfg.viewerEye = ipc::ViewerEye::Right;
        BumpConfigRevision();
    }
    ImGui::SameLine(0.0f, 16.0f);
    int res = cfg.resolution == ipc::ResolutionPreset::WQHD ? 1 : 0;
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("WQHD 2560x1440").x + ImGui::GetFrameHeight() * 2.0f);
    if (ImGui::Combo("##res", &res, "FHD 1920x1080\0WQHD 2560x1440\0")) {
        cfg.resolution = res == 1 ? ipc::ResolutionPreset::WQHD : ipc::ResolutionPreset::FHD;
        BumpConfigRevision();
    }

    // --- Second row: view options + status ------------------------------------
    ImGui::Checkbox("1:1 pixels", &viewerNativeSize_);
    ImGui::SameLine();
    if (ImGui::Button("Pop out")) viewerPopOut_ = true;
    ImGui::SetItemTooltip("Open the viewer as its own window at the render resolution");
    ImGui::SameLine(0.0f, 16.0f);
    if (viewer_.HasFrame()) {
        ImGui::Text("%u x %u   %.0f fps   frame %llu", viewer_.Width(), viewer_.Height(), viewer_.Fps(),
                    static_cast<unsigned long long>(d.frameIndex));
    } else {
        ImGui::TextDisabled("%s", viewer_.Status());
    }
    if (ipc_.DriverAlive() && d.restartRequired) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "  restart SteamVR to apply the new resolution");
    }
    if (ipc_.DriverAlive() && d.displayMode == ipc::kDisplayDirect && d.adapterLuid != 0 &&
        d.adapterLuid != d3d_->AdapterLuid()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "GPU mismatch: SteamVR renders on another adapter (LUID 0x%llX)",
                           static_cast<unsigned long long>(d.adapterLuid));
    }

    // --- Image ----------------------------------------------------------------
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (viewer_.HasFrame()) {
        const float tw = static_cast<float>(viewer_.Width());
        const float th = static_cast<float>(viewer_.Height());
        ImVec2 size(tw, th);
        if (!viewerNativeSize_) {
            const float scale = std::min(avail.x / tw, avail.y / th);
            size = ImVec2(std::max(1.0f, tw * scale), std::max(1.0f, th * scale));
            // Center inside the panel.
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f,
                                       ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - size.y) * 0.5f)));
        }
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(viewer_.Texture())), size);
    } else {
        ImGui::Dummy(ImVec2(avail.x, std::max(avail.y, 40.0f)));
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(mn, mx, IM_COL32(10, 10, 12, 255));
        const char* text = "No HMD image";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        ImGui::GetWindowDrawList()->AddText(ImVec2((mn.x + mx.x - ts.x) * 0.5f, (mn.y + mx.y - ts.y) * 0.5f),
                                            IM_COL32(120, 120, 130, 255), text);
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------
uint32_t App::VisibleTrackerCount() const {
    const uint32_t requested = std::min(ipc_.Control().config.trackerCount, ipc::kMaxTrackers);
    if (ipc_.DriverAlive()) return std::max(requested, std::min(ipc_.Driver().activeTrackerCount, ipc::kMaxTrackers));
    return requested;
}

bool App::SlotVisible(uint32_t slot) const {
    return slot < ipc::kSlotFirstTracker + VisibleTrackerCount();
}

const char* App::SlotStatus(uint32_t slot) const {
    if (slot < ipc::kSlotFirstTracker || !ipc_.DriverAlive()) return "";
    const uint32_t index = slot - ipc::kSlotFirstTracker;
    if (index >= ipc_.Driver().activeTrackerCount) return "(after restart)";
    if (index >= ipc_.Control().config.trackerCount) return "(removed on restart)";
    return "";
}

void App::SetKeyboardCapture(bool capture) {
    keyboardMouse_.SetCaptured(hwnd_, capture);
    ImGuiIO& io = ImGui::GetIO();
    // While captured the UI must not react to the (hidden, pinned) mouse.
    if (keyboardMouse_.Capturing()) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    } else {
        io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
    }
}

void App::DrawInputDevices() {
    auto mappingTable = [](const char* id, std::initializer_list<std::pair<const char*, const char*>> rows) {
        if (ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& [from, to] : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(from);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", to);
            }
            ImGui::EndTable();
        }
    };

    // --- Gamepad --------------------------------------------------------------
    if (ImGui::CollapsingHeader("Gamepad", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enabled##gp", &gamepad_.enabled);
        ImGui::SameLine();
        if (gamepad_.Connected()) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "%s", gamepad_.Name().c_str());
        } else {
            ImGui::TextDisabled(gamepad_.enabled ? "not connected (detected when plugged in)" : "disabled");
        }
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Stick deadzone", &gamepad_.deadzone, 0.0f, 0.5f, "%.2f");
        if (gamepad_.Connected()) {
            const GamepadState& s = gamepad_.State();
            ImGui::TextDisabled("L (%+.2f %+.2f) R (%+.2f %+.2f) LT %.2f RT %.2f btn 0x%04X", s.lx, s.ly, s.rx, s.ry, s.lt,
                                s.rt, s.buttons);
        }
        if (ImGui::TreeNode("Gamepad mapping")) {
            mappingTable("gpmap", {
                {"Left stick / Right stick", "L / R thumbstick"},
                {"LT / RT", "L / R trigger"},
                {"LB / RB", "L / R grip"},
                {"X / Y", "Left A / B"},
                {"A / B", "Right A / B"},
                {"L3 / R3", "L / R thumbstick click"},
                {"Back / Start", "L / R system"},
                {"D-pad", "Left trackpad"},
            });
            ImGui::TextDisabled("DirectInput pads use the common layout\n(□/X=West ×/A=South ○/B=East △/Y=North).");
            ImGui::TreePop();
        }
    }

    // --- Keyboard & mouse -------------------------------------------------------
    if (ImGui::CollapsingHeader("Keyboard & Mouse", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (keyboardMouse_.Capturing()) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Capturing - Esc or F1 to release");
        } else if (ImGui::Button("Start capture (F1)")) {
            SetKeyboardCapture(true);
        }
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Mouse sensitivity", &keyboardMouse_.sensitivity, 0.1f, 5.0f, "%.1f");
        if (ImGui::TreeNode("Keyboard & mouse mapping")) {
            mappingTable("kmmap", {
                {"W A S D", "Left thumbstick"},
                {"Mouse move / Arrow keys", "Right thumbstick"},
                {"Right click / Left click", "L / R trigger"},
                {"Q / E", "L / R grip"},
                {"Z / X", "Left A / B"},
                {"Space / F", "Right A / B"},
                {"Left Ctrl / Middle click", "L / R thumbstick click"},
                {"Tab / Enter", "L / R system"},
            });
            ImGui::TreePop();
        }
        ImGui::TextDisabled("Poses (6DoF) are never moved by these inputs.");
    }
}

// ---------------------------------------------------------------------------
// File menu
// ---------------------------------------------------------------------------
void App::Notify(const std::string& message, bool error) {
    notifyText_  = message;
    notifyError_ = error;
    notifyUntil_ = now_ + 4.0;
}

void App::DrawFileMenu() {
    if (!ImGui::BeginMenu("File")) return;

    if (ImGui::MenuItem("Open scene...", "Ctrl+O")) OpenSceneDialog();
    if (ImGui::MenuItem("Save scene", "Ctrl+S")) SaveSceneDialog(false);
    if (ImGui::MenuItem("Save scene as...", "Ctrl+Shift+S")) SaveSceneDialog(true);
    if (!scenePath_.empty()) ImGui::TextDisabled("  %s", scenePath_.filename().string().c_str());
    ImGui::Separator();

    if (ImGui::BeginMenu("T-pose")) {
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat("Body height", &bodyHeight_, 0.01f, 1.0f, 2.2f, "%.2f m");
        ImGui::Separator();
        const Facing facings[] = {Facing::North, Facing::East, Facing::South, Facing::West};
        for (Facing f : facings) {
            char label[48];
            std::snprintf(label, sizeof(label), "Face %s", FacingLabel(f));
            if (ImGui::MenuItem(label)) ApplyTPose(f);
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();

    if (ImGui::MenuItem("Load Unity animation (.anim)...")) OpenAnimationDialog();
    ImGui::MenuItem("Animation player", nullptr, &showAnimWindow_);
    ImGui::Separator();
    ImGui::MenuItem("Spout2 receiver", nullptr, &showSpoutWindow_);
    ImGui::Separator();
    if (ImGui::MenuItem("Exit")) DestroyWindow(hwnd_);
    ImGui::EndMenu();
}

void App::SaveSceneTo(const std::filesystem::path& path) {
    SceneData scene;
    std::copy(std::begin(ipc_.Control().poses), std::end(ipc_.Control().poses), std::begin(scene.poses));
    const ipc::ControlConfig& cfg = ipc_.Control().config;
    scene.trackerCount = cfg.trackerCount;
    scene.resolution   = cfg.resolution;
    scene.viewerEye    = cfg.viewerEye;
    scene.roomWidth    = roomWidth_;
    scene.roomDepth    = roomDepth_;
    scene.roomHeight   = roomHeight_;

    std::string error;
    if (SaveScene(path, scene, error)) {
        scenePath_ = path;
        Notify("Saved " + path.filename().string());
    } else {
        Notify("Save failed: " + error, true);
    }
}

void App::LoadSceneFrom(const std::filesystem::path& path) {
    SceneData scene;
    std::copy(std::begin(ipc_.Control().poses), std::end(ipc_.Control().poses), std::begin(scene.poses));
    std::string error;
    if (!LoadScene(path, scene, error)) {
        Notify("Load failed: " + error, true);
        return;
    }
    animPlaying_ = false;  // a playing clip would immediately overwrite the loaded poses
    std::copy(std::begin(scene.poses), std::end(scene.poses), std::begin(ipc_.Control().poses));
    roomWidth_  = scene.roomWidth;
    roomDepth_  = scene.roomDepth;
    roomHeight_ = scene.roomHeight;

    ipc::ControlConfig& cfg = ipc_.Control().config;
    if (cfg.trackerCount != scene.trackerCount || cfg.resolution != scene.resolution || cfg.viewerEye != scene.viewerEye) {
        cfg.trackerCount = scene.trackerCount;
        cfg.resolution   = scene.resolution;
        cfg.viewerEye    = scene.viewerEye;
        BumpConfigRevision();  // driver saves them; restart needed for count / resolution
    }
    scenePath_ = path;
    Notify("Loaded " + path.filename().string());
}

void App::SaveSceneDialog(bool saveAs) {
    if (!saveAs && !scenePath_.empty()) {
        SaveSceneTo(scenePath_);
        return;
    }
    const auto path = SaveFileDialog(hwnd_, L"tubane scene (*.tubane.json)\0*.tubane.json\0JSON (*.json)\0*.json\0", L"json",
                                     L"Save scene", scenePath_.empty() ? std::filesystem::path(L"scene.tubane.json") : scenePath_);
    if (path) SaveSceneTo(*path);
}

void App::OpenSceneDialog() {
    const auto path = OpenFileDialog(hwnd_, L"tubane scene (*.tubane.json)\0*.tubane.json\0JSON (*.json)\0*.json\0All files\0*.*\0",
                                     L"Open scene");
    if (path) LoadSceneFrom(*path);
}

void App::ApplyTPose(Facing facing) {
    animPlaying_ = false;
    BuildTPose(facing, bodyHeight_, ipc_.Control().poses);
    char msg[64];
    std::snprintf(msg, sizeof(msg), "T-pose facing %s", FacingLabel(facing));
    Notify(msg);
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------
void App::OpenAnimationDialog() {
    const auto path = OpenFileDialog(hwnd_, L"Unity AnimationClip (*.anim)\0*.anim\0All files\0*.*\0", L"Load Unity animation");
    if (!path) return;
    AnimClip    clip;
    std::string error;
    if (!LoadUnityAnim(*path, clip, error)) {
        Notify("Animation: " + error, true);
        showAnimWindow_ = true;
        return;
    }
    clip_       = std::move(clip);
    clipLoaded_ = true;
    clipPath_   = *path;
    animTime_   = 0.0f;
    animPlaying_ = false;
    for (uint32_t slot = 0; slot < ipc::kMaxDevices; ++slot) {
        animEnabled_[slot] = clip_.bindings[slot].hasPosition || clip_.bindings[slot].hasRotation ||
                             (slot == ipc::kSlotHmd && clip_.hmdEstimated);
    }
    animApplyOnce_  = true;
    showAnimWindow_ = true;
    Notify("Loaded animation " + clip_.name);
}

void App::UpdateAnimation(float dt) {
    if (!clipLoaded_) return;
    if (animPlaying_) {
        animTime_ += dt * animSpeed_;
        const float duration = std::max(clip_.duration, 1e-3f);
        if (animTime_ > duration || animTime_ < 0.0f) {
            if (animLoop_) {
                animTime_ = std::fmod(animTime_, duration);
                if (animTime_ < 0.0f) animTime_ += duration;
            } else {
                animTime_    = std::clamp(animTime_, 0.0f, duration);
                animPlaying_ = false;
            }
        }
    } else if (!animApplyOnce_) {
        return;
    }
    animApplyOnce_ = false;

    animOptions_.yaw = FacingYawRadians(static_cast<Facing>(animFacing_));
    ipc::DevicePose sampled[ipc::kMaxDevices];
    std::copy(std::begin(ipc_.Control().poses), std::end(ipc_.Control().poses), std::begin(sampled));
    bool driven[ipc::kMaxDevices] = {};
    SampleClip(clip_, animTime_, animOptions_, sampled, driven);
    for (uint32_t slot = 0; slot < ipc::kMaxDevices; ++slot) {
        animDriven_[slot] = driven[slot] && animEnabled_[slot];
        if (animDriven_[slot]) {
            ipc::DevicePose& p = ipc_.Control().poses[slot];
            std::copy(std::begin(sampled[slot].position), std::end(sampled[slot].position), std::begin(p.position));
            std::copy(std::begin(sampled[slot].rotation), std::end(sampled[slot].rotation), std::begin(p.rotation));
        }
    }
}

void App::DrawAnimationWindow() {
    if (!showAnimWindow_) return;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation Player", &showAnimWindow_)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Load .anim...")) OpenAnimationDialog();
    if (!clipLoaded_) {
        ImGui::TextDisabled("No clip loaded. Unity .anim files saved as text (Asset Serialization: Force Text).");
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::Text("%s  (%s, %.2f s)", clip_.name.c_str(), clip_.kind == AnimClip::Kind::Humanoid ? "Humanoid" : "Generic",
                clip_.duration);

    // Transport
    if (ImGui::Button(animPlaying_ ? "Pause" : "Play")) animPlaying_ = !animPlaying_;
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        animPlaying_   = false;
        animTime_      = 0.0f;
        animApplyOnce_ = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &animLoop_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("Speed", &animSpeed_, 0.01f, -4.0f, 4.0f, "%.2fx");
    if (ImGui::SliderFloat("Time", &animTime_, 0.0f, std::max(clip_.duration, 0.001f), "%.2f s")) animApplyOnce_ = true;

    // Placement
    ImGui::SeparatorText("Placement");
    bool changed = false;
    const char* facings[] = {"N (-Z)", "E (+X)", "S (+Z)", "W (-X)"};
    ImGui::SetNextItemWidth(120.0f);
    changed |= ImGui::Combo("Facing", &animFacing_, facings, 4);
    ImGui::SetNextItemWidth(120.0f);
    changed |= ImGui::DragFloat("Scale", &animOptions_.scale, 0.005f, 0.1f, 3.0f, "%.3f");
    changed |= ImGui::DragFloat3("Offset (m)", &animOptions_.offset.x, 0.01f, -20.0f, 20.0f, "%.2f");
    if (clip_.hmdEstimated) {
        ImGui::SetNextItemWidth(120.0f);
        changed |= ImGui::DragFloat("Head above body", &animOptions_.headOffset, 0.005f, 0.0f, 1.5f, "%.3f");
        HelpMarker("Humanoid clips have no head goal. The HMD is placed this far above the body center (RootT), "
                   "in the clip's units (before Scale).");
    }
    if (changed && !animPlaying_) animApplyOnce_ = true;

    // Device bindings
    ImGui::SeparatorText("Devices");
    if (ImGui::BeginTable("animdev", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (uint32_t slot = 0; slot < ipc::kMaxDevices; ++slot) {
            const DeviceBinding& b = clip_.bindings[slot];
            const bool bound = b.hasPosition || b.hasRotation || (slot == ipc::kSlotHmd && clip_.hmdEstimated);
            if (!bound) continue;
            ImGui::PushID(static_cast<int>(slot));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##on", &animEnabled_[slot]) && !animPlaying_) animApplyOnce_ = true;
            ImGui::SameLine();
            ImGui::TextUnformatted(DescribeSlot(slot).name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", b.source.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s%s", (b.hasPosition || slot == ipc::kSlotHmd) ? "pos " : "",
                                (b.hasRotation || slot == ipc::kSlotHmd) ? "rot" : "");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Unchecked devices keep their manual pose. Animated devices can't be dragged while playing.");
    for (const std::string& w : clip_.warnings) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", w.c_str());
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Spout2 receiver window
// ---------------------------------------------------------------------------
void App::DrawSpoutWindow() {
    if (!showSpoutWindow_) {
        if (spout_.HasFrame()) spout_.Reset();
        return;
    }
    ImGuiWindowClass windowClass;
    windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&windowClass);
    if (spoutPopOut_ && spout_.HasFrame()) {
        spoutPopOut_ = false;
        const ImGuiStyle& style = ImGui::GetStyle();
        const float chrome = ImGui::GetFrameHeight() + ImGui::GetFrameHeightWithSpacing() * 2.0f + style.WindowPadding.y * 2.0f;
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->Pos.x + 100.0f, ImGui::GetMainViewport()->Pos.y + 100.0f),
                                ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(spout_.Width() + style.WindowPadding.x * 2.0f, spout_.Height() + chrome),
                                 ImGuiCond_Always);
        spoutNativeSize_ = true;
    } else {
        ImGui::SetNextWindowSize(ImVec2(640, 420), ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin("Spout2 Receiver", &showSpoutWindow_, spoutNativeSize_ ? ImGuiWindowFlags_HorizontalScrollbar : 0)) {
        ImGui::End();
        return;
    }

    // Sender selection
    const std::vector<std::string> senders = SpoutReceiver::ListSenders();
    const std::string preview = spoutSender_.empty() ? "Active sender" : spoutSender_;
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo("Sender", preview.c_str())) {
        if (ImGui::Selectable("Active sender", spoutSender_.empty())) spoutSender_.clear();
        for (const std::string& s : senders) {
            if (ImGui::Selectable(s.c_str(), s == spoutSender_)) spoutSender_ = s;
        }
        if (senders.empty()) ImGui::TextDisabled("(no Spout senders running)");
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("1:1 pixels##spout", &spoutNativeSize_);
    ImGui::SameLine();
    if (ImGui::Button("Pop out##spout")) spoutPopOut_ = true;

    if (spout_.HasFrame()) {
        ImGui::Text("%s   %u x %u", spout_.ConnectedSender().c_str(), spout_.Width(), spout_.Height());
    } else {
        ImGui::TextDisabled("%s", spout_.Status().c_str());
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (spout_.HasFrame()) {
        const float tw = static_cast<float>(spout_.Width()), th = static_cast<float>(spout_.Height());
        ImVec2 size(tw, th);
        if (!spoutNativeSize_) {
            const float scale = std::min(avail.x / tw, avail.y / th);
            size = ImVec2(std::max(1.0f, tw * scale), std::max(1.0f, th * scale));
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f,
                                       ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - size.y) * 0.5f)));
        }
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(spout_.Texture())), size);
    } else {
        ImGui::Dummy(ImVec2(avail.x, std::max(avail.y, 40.0f)));
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(mn, mx, IM_COL32(10, 10, 12, 255));
        const char*  text = "No Spout2 input";
        const ImVec2 ts   = ImGui::CalcTextSize(text);
        ImGui::GetWindowDrawList()->AddText(ImVec2((mn.x + mx.x - ts.x) * 0.5f, (mn.y + mx.y - ts.y) * 0.5f),
                                            IM_COL32(120, 120, 130, 255), text);
    }
    ImGui::End();
}

void App::BumpConfigRevision() {
    ++ipc_.Control().config.revision;
}

void App::SyncConfigFromDriver() {
    // Adopt the driver's active settings once per driver session, unless a
    // change requested earlier is still waiting for a SteamVR restart.
    if (!ipc_.DriverAlive()) return;
    const ipc::DriverBlock& d = ipc_.Driver();
    if (d.driverProcessId == syncedDriverPid_) return;
    syncedDriverPid_ = d.driverProcessId;
    ipc::ControlConfig& cfg = ipc_.Control().config;
    if (!d.restartRequired && cfg.revision == d.appliedRevision) {
        cfg.resolution   = d.activeResolution;
        cfg.trackerCount = d.activeTrackerCount;
    }
}

void App::ComposeInput(int hand, bool forceTrigger, const ExternalInput& gamepad, const ExternalInput& keyboard) {
    ControllerUiState&    ui = controllerUi_[hand];
    ipc::ControllerInput  in{};

    // 1) Manual UI
    for (int b = 0; b < ControllerUiState::kButtonCount; ++b) {
        const bool click = kButtons[b].clickBit && (ui.latchClick[b] || ui.hold[b]);
        const bool touch = ui.latchTouch[b] || (!kButtons[b].clickBit && ui.hold[b]);
        if (click) in.buttons |= kButtons[b].clickBit;
        if (touch) in.buttons |= kButtons[b].touchBit;
    }
    in.trigger       = (forceTrigger || ui.hold[kBtnIndexTrigger]) ? 1.0f : ui.trigger;
    const bool gripHeld = ui.hold[kBtnIndexGrip];
    in.gripValue     = gripHeld ? 1.0f : ui.gripValue;
    in.gripForce     = gripHeld ? std::max(ui.gripForce, 0.5f) : ui.gripForce;
    in.thumbstickX   = ui.stick[0];
    in.thumbstickY   = ui.stick[1];
    in.trackpadX     = ui.pad[0];
    in.trackpadY     = ui.pad[1];
    in.trackpadForce = ui.padForce;
    (void)kBtnIndexStick;
    (void)kBtnIndexPad;

    // 2) Gamepad + keyboard/mouse: buttons OR'ed, analog values take whichever
    //    source is deflected furthest.
    auto stronger = [](float a, float b) { return std::fabs(b) > std::fabs(a) ? b : a; };
    for (const ExternalInput* ext : {&gamepad, &keyboard}) {
        const ipc::ControllerInput& e = ext->hand[hand];
        in.buttons      |= e.buttons;
        in.trigger       = stronger(in.trigger, e.trigger);
        in.gripValue     = stronger(in.gripValue, e.gripValue);
        in.gripForce     = stronger(in.gripForce, e.gripForce);
        in.trackpadForce = stronger(in.trackpadForce, e.trackpadForce);
        if (std::fabs(e.thumbstickX) + std::fabs(e.thumbstickY) > std::fabs(in.thumbstickX) + std::fabs(in.thumbstickY)) {
            in.thumbstickX = e.thumbstickX;
            in.thumbstickY = e.thumbstickY;
        }
        if (std::fabs(e.trackpadX) + std::fabs(e.trackpadY) > std::fabs(in.trackpadX) + std::fabs(in.trackpadY)) {
            in.trackpadX = e.trackpadX;
            in.trackpadY = e.trackpadY;
        }
    }

    // 3) Derived touch / click bits (a pressed button is also touched).
    for (const ButtonDef& def : kButtons) {
        if (def.clickBit && (in.buttons & def.clickBit)) in.buttons |= def.touchBit;
    }
    if (in.trigger >= 0.95f) in.buttons |= ipc::kBtnTriggerClick | ipc::kBtnTriggerTouch;
    if (in.trigger > 0.01f) in.buttons |= ipc::kBtnTriggerTouch;
    if (in.gripValue > 0.01f) in.buttons |= ipc::kBtnGripTouch;
    if (std::fabs(in.thumbstickX) > 0.01f || std::fabs(in.thumbstickY) > 0.01f) in.buttons |= ipc::kBtnThumbstickTouch;
    if (std::fabs(in.trackpadX) > 0.01f || std::fabs(in.trackpadY) > 0.01f || in.trackpadForce > 0.0f) {
        in.buttons |= ipc::kBtnTrackpadTouch;
    }

    if (ui.autoFingers) {
        in.finger[0] = in.trigger;
        in.finger[1] = in.finger[2] = in.finger[3] = in.gripValue;
    } else {
        std::copy(std::begin(ui.fingers), std::end(ui.fingers), in.finger);
    }
    if (ui.thumbOverride) {
        in.thumbCurl = ui.thumb;
        in.fingerFlags |= ipc::kFingerThumbOverride;
    }

    ipc_.Control().controllers[hand] = in;
}

}  // namespace tubane::sandbox
