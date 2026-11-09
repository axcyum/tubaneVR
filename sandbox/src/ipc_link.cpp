#include "ipc_link.h"

#include <windows.h>

#include "tubane/default_state.h"

namespace tubane::sandbox {

namespace {

bool ProcessAlive(uint32_t pid) {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

}  // namespace

bool IpcLink::Open() {
    lastResult_ = shm_.Open(ipc::OpenMode::CreateOrOpen);
    if (lastResult_ != ipc::OpenResult::Ok) {
        ipc::FillDefaultControl(control_);
        return false;
    }
    ipc::SharedState& state = *shm_.State();
    ipc::SeqRecoverWriter(state.controlLock);  // a previous sandbox may have died mid-write

    if (!ipc::ReadControl(state, control_)) {
        ipc::FillDefaultControl(control_);
    }
    heartbeat_ = control_.sandboxHeartbeat;

    const uint32_t self = GetCurrentProcessId();
    if (control_.sandboxProcessId != 0 && control_.sandboxProcessId != self && ProcessAlive(control_.sandboxProcessId)) {
        otherWriterPid_ = control_.sandboxProcessId;
    }
    return true;
}

void IpcLink::Close() {
    shm_.Close();
}

void IpcLink::Publish() {
    if (!shm_.IsOpen()) return;
    ++heartbeat_;
    control_.sandboxHeartbeat = heartbeat_;
    control_.sandboxProcessId = GetCurrentProcessId();
    ipc::WriteControl(*shm_.State(), [&](ipc::ControlBlock& c) { c = control_; });
}

void IpcLink::PollDriver(double now) {
    if (!shm_.IsOpen()) {
        driverAlive_ = false;
        return;
    }
    ipc::DriverBlock d{};
    if (!ipc::ReadDriver(*shm_.State(), d)) {
        return;  // keep previous snapshot
    }
    driver_ = d;
    if (!haveBaseline_) {
        // First sample: a stale heartbeat left by a dead driver must not count as "alive".
        haveBaseline_        = true;
        lastDriverHeartbeat_ = d.driverHeartbeat;
    } else if (d.driverHeartbeat != lastDriverHeartbeat_) {
        lastDriverHeartbeat_ = d.driverHeartbeat;
        lastDriverChange_    = now;
    }
    driverAlive_ = lastDriverChange_ >= 0.0 && (now - lastDriverChange_) < 1.0 && d.driverProcessId != 0;
}

}  // namespace tubane::sandbox
