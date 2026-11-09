// Sandbox side of the shared-memory link.
//
// The sandbox is the single writer of ControlBlock. It keeps its own copy as
// the source of truth (edited by the UI) and publishes it every frame.
#pragma once

#include <cstdint>

#include "tubane/shm.h"

namespace tubane::sandbox {

class IpcLink {
public:
    // Opens / creates the mapping and loads the current control block (poses
    // left by a previous sandbox run or the defaults seeded by the creator).
    bool Open();
    void Close();

    bool IsOpen() const { return shm_.IsOpen(); }
    ipc::OpenResult LastResult() const { return lastResult_; }
    unsigned long   LastWin32Error() const { return shm_.LastError(); }

    // PID of another live process that wrote the control block recently, or 0.
    uint32_t OtherWriterPid() const { return otherWriterPid_; }

    ipc::ControlBlock&       Control()       { return control_; }
    const ipc::ControlBlock& Control() const { return control_; }

    // Writes Control() to shared memory (heartbeat + pid filled in).
    void Publish();

    // Reads the driver block; `nowSeconds` is a monotonic clock.
    void PollDriver(double nowSeconds);

    const ipc::DriverBlock& Driver() const { return driver_; }
    // True while the driver heartbeat keeps changing (vrserver running).
    bool DriverAlive() const { return driverAlive_; }

private:
    ipc::SharedMemory shm_;
    ipc::OpenResult   lastResult_ = ipc::OpenResult::NotFound;
    ipc::ControlBlock control_{};
    ipc::DriverBlock  driver_{};
    uint64_t          heartbeat_ = 0;
    uint32_t          otherWriterPid_ = 0;

    uint64_t lastDriverHeartbeat_ = 0;
    double   lastDriverChange_    = -1.0;
    bool     driverAlive_         = false;
    bool     haveBaseline_        = false;
};

}  // namespace tubane::sandbox
