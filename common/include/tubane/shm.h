// Win32 shared-memory wrapper + SeqLock helpers for tubane::ipc::SharedState.
//
// Usage (both driver and sandbox):
//     tubane::ipc::SharedMemory shm;
//     if (shm.Open(tubane::ipc::OpenMode::CreateOrOpen) == tubane::ipc::OpenResult::Ok) {
//         tubane::ipc::ControlBlock control;
//         if (tubane::ipc::ReadControl(*shm.State(), control)) { ... }
//     }
//
// Whichever process creates the mapping first seeds it with the defaults from
// default_state.h, so the driver works without a running sandbox and the
// sandbox starts from the same initial layout the driver uses.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstring>
#include <utility>

#include "tubane/default_state.h"
#include "tubane/ipc_layout.h"

namespace tubane::ipc {

// ---------------------------------------------------------------------------
// SeqLock primitives
// ---------------------------------------------------------------------------

// If a writer died in the middle of a write the counter stays odd forever and
// every reader would spin. The (single) writer calls this once on attach.
inline void SeqRecoverWriter(SeqLock& lock) {
    uint32_t s = lock.sequence.load(std::memory_order_relaxed);
    if (s & 1u) {
        lock.sequence.store(s + 1u, std::memory_order_release);
    }
}

// Single-writer update. `fn(Block&)` mutates the block in place.
template <class Block, class Fn>
inline void SeqWrite(SeqLock& lock, Block& block, Fn&& fn) {
    const uint32_t s = lock.sequence.load(std::memory_order_relaxed);
    lock.sequence.store(s + 1u, std::memory_order_relaxed);  // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    fn(block);
    lock.sequence.store(s + 2u, std::memory_order_release);  // even: consistent
}

// Lock-free snapshot. Returns false if no consistent copy could be taken
// within `maxAttempts` (writer stalled or crashed mid-write).
template <class Block>
inline bool SeqRead(const SeqLock& lock, const Block& block, Block& out, int maxAttempts = 256) {
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        const uint32_t s1 = lock.sequence.load(std::memory_order_acquire);
        if (s1 & 1u) {
            YieldProcessor();
            continue;
        }
        std::memcpy(&out, &block, sizeof(Block));
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = lock.sequence.load(std::memory_order_relaxed);
        if (s1 == s2) {
            return true;
        }
    }
    return false;
}

inline bool ReadControl(const SharedState& s, ControlBlock& out) { return SeqRead(s.controlLock, s.control, out); }
inline bool ReadDriver(const SharedState& s, DriverBlock& out)   { return SeqRead(s.driverLock, s.driver, out); }

template <class Fn> inline void WriteControl(SharedState& s, Fn&& fn) { SeqWrite(s.controlLock, s.control, std::forward<Fn>(fn)); }
template <class Fn> inline void WriteDriver(SharedState& s, Fn&& fn)  { SeqWrite(s.driverLock, s.driver, std::forward<Fn>(fn)); }

// ---------------------------------------------------------------------------
// SharedMemory
// ---------------------------------------------------------------------------
enum class OpenMode {
    CreateOrOpen,  // create the mapping if missing (driver, sandbox)
    OpenExisting,  // fail if nobody created it yet (diagnostic tools)
};

enum class OpenResult {
    Ok,
    NotFound,         // OpenExisting and no mapping exists
    MapFailed,        // CreateFileMapping / MapViewOfFile failed (see LastError())
    InitTimeout,      // creator never finished initialization
    VersionMismatch,  // other side was built with a different kProtocolVersion
};

inline const char* ToString(OpenResult r) {
    switch (r) {
        case OpenResult::Ok:              return "Ok";
        case OpenResult::NotFound:        return "NotFound";
        case OpenResult::MapFailed:       return "MapFailed";
        case OpenResult::InitTimeout:     return "InitTimeout";
        case OpenResult::VersionMismatch: return "VersionMismatch";
    }
    return "Unknown";
}

class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory() { Close(); }

    SharedMemory(const SharedMemory&)            = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    OpenResult Open(OpenMode mode, DWORD initTimeoutMs = 2000) {
        Close();

        if (mode == OpenMode::OpenExisting) {
            mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSharedMemoryName);
            if (!mapping_) {
                lastError_ = GetLastError();
                return lastError_ == ERROR_FILE_NOT_FOUND ? OpenResult::NotFound : OpenResult::MapFailed;
            }
        } else {
            mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          static_cast<DWORD>(sizeof(SharedState)), kSharedMemoryName);
            if (!mapping_) {
                lastError_ = GetLastError();
                return OpenResult::MapFailed;
            }
            created_ = (GetLastError() != ERROR_ALREADY_EXISTS);
        }

        void* view = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
        if (!view) {
            lastError_ = GetLastError();
            Close();
            return OpenResult::MapFailed;
        }
        state_ = static_cast<SharedState*>(view);

        if (created_) {
            // Fresh pages are zero-filled by the OS. Seed defaults, then
            // publish the magic last so other processes see a complete state.
            state_->version = kProtocolVersion;
            state_->size    = static_cast<uint32_t>(sizeof(SharedState));
            WriteControl(*state_, [](ControlBlock& c) { FillDefaultControl(c); });
            WriteDriver(*state_, [](DriverBlock& d) { FillDefaultDriver(d); });
            state_->magic.store(kMagic, std::memory_order_release);
            return OpenResult::Ok;
        }

        // Opened an existing mapping: wait for the creator to finish.
        const ULONGLONG deadline = GetTickCount64() + initTimeoutMs;
        while (state_->magic.load(std::memory_order_acquire) != kMagic) {
            if (GetTickCount64() > deadline) {
                Close();
                return OpenResult::InitTimeout;
            }
            Sleep(1);
        }
        if (state_->version != kProtocolVersion || state_->size != sizeof(SharedState)) {
            Close();
            return OpenResult::VersionMismatch;
        }
        return OpenResult::Ok;
    }

    void Close() {
        if (state_) {
            UnmapViewOfFile(state_);
            state_ = nullptr;
        }
        if (mapping_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        created_ = false;
    }

    bool         IsOpen() const    { return state_ != nullptr; }
    bool         Created() const   { return created_; }  // true if this process seeded the defaults
    DWORD        LastError() const { return lastError_; }
    SharedState* State() const     { return state_; }

private:
    HANDLE       mapping_   = nullptr;
    SharedState* state_     = nullptr;
    bool         created_   = false;
    DWORD        lastError_ = 0;
};

}  // namespace tubane::ipc
