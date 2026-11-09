cmake_minimum_required(VERSION 3.24)

project(tubane
    VERSION 0.1.0
    DESCRIPTION "Fully virtualized SteamVR environment (virtual HMD / controllers / trackers)"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "tubane currently supports Windows (x64) only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "tubane must be built for x64 (SteamVR loads 64-bit drivers only).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT so driver_tubane.dll does not depend on the VC++ redistributable
# being present in vrserver's environment.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(TUBANE_BUILD_DRIVER  "Build the SteamVR driver (driver_tubane.dll)" ON)
option(TUBANE_BUILD_SANDBOX "Build the 3D sandbox GUI (tubane_sandbox.exe)" ON)
option(TUBANE_BUILD_TOOLS   "Build developer tools (shm_probe)"           ON)

include(cmake/Dependencies.cmake)

add_subdirectory(common)

if(TUBANE_BUILD_DRIVER)
    add_subdirectory(driver)
endif()

if(TUBANE_BUILD_SANDBOX)
    add_subdirectory(sandbox)
endif()

if(TUBANE_BUILD_TOOLS)
    add_subdirectory(tools/shm_probe)
    add_subdirectory(tools/vr_check)
endif()
