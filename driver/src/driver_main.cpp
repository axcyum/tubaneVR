// Entry point loaded by vrserver.exe.
//
// SteamVR looks for <driver dir>/bin/win64/driver_<name>.dll and calls the
// exported HmdDriverFactory for each interface it wants.
#include <openvr_driver.h>

#include <cstring>

#include "server_provider.h"

namespace {
tubane::driver::ServerProvider g_serverProvider;
}

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
    if (std::strcmp(interfaceName, vr::IServerTrackedDeviceProvider_Version) == 0) {
        return static_cast<vr::IServerTrackedDeviceProvider*>(&g_serverProvider);
    }
    if (returnCode) {
        *returnCode = vr::VRInitError_Init_InterfaceNotFound;
    }
    return nullptr;
}
