// Driver-side shared-memory access.
//
// The driver is the single *process* writing DriverBlock, but three threads
// update it (pose thread heartbeat, RunFrame status, compositor Present frame
// counter). SeqLock only supports one writer at a time, so every write goes
// through WriteDriver(), which serializes writers with a mutex.
#pragma once

#include <mutex>
#include <utility>

#include "tubane/shm.h"

namespace tubane::driver {

class DriverIpc {
public:
    ipc::OpenResult Open() {
        const ipc::OpenResult r = shm_.Open(ipc::OpenMode::CreateOrOpen);
        if (r == ipc::OpenResult::Ok) {
            ipc::SeqRecoverWriter(shm_.State()->driverLock);  // previous vrserver may have died mid-write
        }
        return r;
    }
    void Close() { shm_.Close(); }

    bool          IsOpen() const    { return shm_.IsOpen(); }
    bool          Created() const   { return shm_.Created(); }
    unsigned long LastError() const { return shm_.LastError(); }

    // Lock-free snapshot of the sandbox-written block (any thread).
    bool ReadControl(ipc::ControlBlock& out) const {
        return shm_.IsOpen() && ipc::ReadControl(*shm_.State(), out);
    }

    // Serialized in-place update of DriverBlock (any thread).
    template <class Fn>
    void WriteDriver(Fn&& fn) {
        if (!shm_.IsOpen()) return;
        std::lock_guard lock(writeMutex_);
        ipc::WriteDriver(*shm_.State(), std::forward<Fn>(fn));
    }

private:
    ipc::SharedMemory shm_;
    std::mutex        writeMutex_;
};

}  // namespace tubane::driver
