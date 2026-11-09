// Logging into SteamVR's vrserver.txt (Steam\logs\vrserver.txt).
#pragma once

#include <openvr_driver.h>

#include <cstdarg>
#include <cstdio>

namespace tubane::driver {

inline void Log(const char* fmt, ...) {
    char    body[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    if (vr::IVRDriverLog* log = vr::VRDriverLog()) {
        char line[1100];
        std::snprintf(line, sizeof(line), "[tubane] %s\n", body);
        log->Log(line);
    }
}

}  // namespace tubane::driver
