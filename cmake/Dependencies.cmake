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
