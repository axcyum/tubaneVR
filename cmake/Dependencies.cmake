# Third-party dependencies, fetched automatically at configure time.
include(FetchContent)

# --- OpenVR SDK -------------------------------------------------------------
# Only the headers are needed: a server driver talks to vrserver exclusively
# through interfaces handed over in HmdDriverFactory / VR_INIT_SERVER_DRIVER_CONTEXT,
# so openvr_api.lib is NOT linked. SOURCE_SUBDIR points at a directory without
# a CMakeLists.txt so that OpenVR's own (old) CMake project is never configured.
set(TUBANE_OPENVR_VERSION "v2.5.1" CACHE STRING "OpenVR SDK git tag to fetch")

FetchContent_Declare(openvr
    GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
    GIT_TAG        ${TUBANE_OPENVR_VERSION}
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  headers)
FetchContent_MakeAvailable(openvr)

add_library(openvr_headers INTERFACE)
add_library(tubane::openvr_headers ALIAS openvr_headers)
target_include_directories(openvr_headers SYSTEM INTERFACE "${openvr_SOURCE_DIR}/headers")

if(TUBANE_BUILD_SANDBOX)
    # --- Dear ImGui (docking branch) ------------------------------------------
    # Neither ImGui nor ImGuizmo ship a usable CMake project; we compile their
    # sources into static libraries ourselves (SOURCE_SUBDIR skips theirs).
    set(TUBANE_IMGUI_VERSION "v1.92.9b-docking" CACHE STRING "Dear ImGui git tag to fetch")
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        ${TUBANE_IMGUI_VERSION}
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _tubane_no_cmake)
    FetchContent_MakeAvailable(imgui)

    add_library(imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp")
    add_library(tubane::imgui ALIAS imgui)
    target_include_directories(imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
    target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
    target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

    # --- ImGuizmo ---------------------------------------------------------------
    # No release tags are maintained upstream, so a commit is pinned.
    set(TUBANE_IMGUIZMO_COMMIT "105e4fde45e4e74aae8973d98750820ef7aca7e2" CACHE STRING "ImGuizmo commit to fetch")
    FetchContent_Declare(imguizmo
        GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
        GIT_TAG        ${TUBANE_IMGUIZMO_COMMIT}
        SOURCE_SUBDIR  _tubane_no_cmake)
    FetchContent_MakeAvailable(imguizmo)

    # --- nlohmann/json (scene files) --------------------------------------------
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(nlohmann_json)

    add_library(imguizmo STATIC "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
    add_library(tubane::imguizmo ALIAS imguizmo)
    target_include_directories(imguizmo SYSTEM PUBLIC "${imguizmo_SOURCE_DIR}/src")
    target_link_libraries(imguizmo PUBLIC imgui)
endif()
