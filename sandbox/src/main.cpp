// tubane-sandbox entry point: Win32 window, D3D11, Dear ImGui main loop.
#include <windows.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <chrono>
#include <string>

#include "app.h"
#include "d3d11_context.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

tubane::sandbox::D3D11Context g_d3d;
UINT g_resizeWidth = 0, g_resizeHeight = 0;

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return true;
    }
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_resizeWidth  = LOWORD(lParam);
                g_resizeHeight = HIWORD(lParam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // Alt is used for orbit; no window menu
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::string ExeDirectory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    return s.substr(0, s.find_last_of("\\/") + 1);
}

void LoadFonts(ImGuiIO& io) {
    // Segoe UI for Latin text, Yu Gothic merged in for Japanese glyphs.
    // ImGui 1.92 loads glyphs on demand, so no glyph ranges are needed.
    ImFont* base = nullptr;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
        base = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    }
    if (!base) {
        io.Fonts->AddFontDefault();
    }
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\YuGothM.ttc") != INVALID_FILE_ATTRIBUTES) {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\YuGothM.ttc", 17.0f, &cfg);
    }
}

void ApplyStyle(float dpiScale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.GrabRounding      = 3.0f;
    style.TabRounding       = 3.0f;
    style.WindowBorderSize  = 1.0f;
    style.FramePadding      = ImVec2(6, 4);
    style.ItemSpacing       = ImVec2(8, 6);
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.11f, 0.12f, 0.14f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.29f, 0.38f, 1.0f);
    c[ImGuiCol_Header]        = ImVec4(0.20f, 0.36f, 0.48f, 0.65f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.44f, 0.58f, 0.80f);
    c[ImGuiCol_Tab]           = ImVec4(0.15f, 0.20f, 0.25f, 1.0f);
    c[ImGuiCol_TabSelected]   = ImVec4(0.20f, 0.36f, 0.48f, 1.0f);
    style.ScaleAllSizes(dpiScale);
    style.FontScaleDpi = dpiScale;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"tubane_sandbox";
    RegisterClassExW(&wc);

    const int width  = static_cast<int>(1600 * dpiScale);
    const int height = static_cast<int>(940 * dpiScale);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"tubane sandbox", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                width, height, nullptr, nullptr, instance, nullptr);
    if (!hwnd || !g_d3d.Create(hwnd)) {
        MessageBoxW(nullptr, L"Failed to create a Direct3D 11 device.", L"tubane sandbox", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, showCmd == SW_HIDE ? SW_SHOWDEFAULT : showCmd);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    static const std::string iniPath = ExeDirectory() + "tubane_sandbox.ini";
    io.IniFilename = iniPath.c_str();

    ApplyStyle(dpiScale);
    LoadFonts(io);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_d3d.Device(), g_d3d.Context());

    tubane::sandbox::App app;
    if (!app.Init(&g_d3d)) {
        MessageBoxW(nullptr, L"Failed to initialize the renderer (shader compilation).", L"tubane sandbox", MB_ICONERROR);
        return 1;
    }

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto       last  = start;
    bool       running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (g_resizeWidth != 0 && g_resizeHeight != 0) {
            g_d3d.Resize(g_resizeWidth, g_resizeHeight);
            g_resizeWidth = g_resizeHeight = 0;
        }
        if (IsIconic(hwnd)) {
            // Keep publishing (heartbeat) at a low rate while minimized.
            Sleep(10);
        }

        const auto  now = Clock::now();
        const float dt  = std::chrono::duration<float>(now - last).count();
        last = now;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.Frame(dt, std::chrono::duration<double>(now - start).count());

        ImGui::Render();
        const float clear[4] = {0.06f, 0.065f, 0.075f, 1.0f};
        g_d3d.BeginFrame(clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_d3d.Present(true);
    }

    app.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_d3d.Destroy();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
