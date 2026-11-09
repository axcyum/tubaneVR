# tubane_sandbox.exe — 3D sandbox GUI (D3D11 + Dear ImGui + ImGuizmo).
add_executable(tubane_sandbox WIN32
    src/main.cpp
    src/app.cpp
    src/app.h
    src/camera.cpp
    src/camera.h
    src/d3d11_context.cpp
    src/d3d11_context.h
    src/hmd_viewer.cpp
    src/hmd_viewer.h
    src/input_devices.cpp
    src/input_devices.h
    src/ipc_link.cpp
    src/ipc_link.h
    src/renderer.cpp
    src/renderer.h
    src/scene.cpp
    src/scene.h
    src/math_util.h)

target_link_libraries(tubane_sandbox PRIVATE tubane::common tubane::imgui tubane::imguizmo
    xinput dinput8 dxguid)

if(MSVC)
    target_compile_options(tubane_sandbox PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()

set_target_properties(tubane_sandbox PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:tubane_sandbox>")
