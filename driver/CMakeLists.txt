# driver_tubane.dll — SteamVR server driver.
#
# After every build the complete, ready-to-register driver folder is
# assembled at:  <build dir>/package/tubane/
#   driver.vrdrivermanifest
#   resources/settings/default.vrsettings
#   bin/win64/driver_tubane.dll (+ .pdb)
# Register it once with tools/register_driver.ps1.

add_library(driver_tubane SHARED
    src/driver_main.cpp
    src/server_provider.cpp
    src/server_provider.h
    src/device_base.cpp
    src/device_base.h
    src/hmd_device.cpp
    src/hmd_device.h
    src/direct_mode.cpp
    src/direct_mode.h
    src/driver_ipc.h
    src/controller_device.cpp
    src/controller_device.h
    src/hand_skeleton.cpp
    src/hand_skeleton.h
    src/hand_reference_poses.h
    src/tracker_device.cpp
    src/tracker_device.h
    src/settings.cpp
    src/settings.h
    src/driver_log.h)

target_link_libraries(driver_tubane PRIVATE tubane::common tubane::openvr_headers d3d11 dxgi d3dcompiler)

if(MSVC)
    target_compile_options(driver_tubane PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()

set(TUBANE_DRIVER_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package/tubane" CACHE INTERNAL "")

# Output straight into the package layout (no config subdirectory), so the
# registered path never changes between Debug and Release builds.
set_target_properties(driver_tubane PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY                "${TUBANE_DRIVER_PACKAGE_DIR}/bin/win64"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${TUBANE_DRIVER_PACKAGE_DIR}/bin/win64"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${TUBANE_DRIVER_PACKAGE_DIR}/bin/win64"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${TUBANE_DRIVER_PACKAGE_DIR}/bin/win64"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${TUBANE_DRIVER_PACKAGE_DIR}/bin/win64")

# Copy manifest + resources next to the DLL on every build.
add_custom_command(TARGET driver_tubane POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/package/tubane"
            "${TUBANE_DRIVER_PACKAGE_DIR}"
    COMMENT "Assembling SteamVR driver package in ${TUBANE_DRIVER_PACKAGE_DIR}")
