#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace Dx11wDx12Sync
{
// This mutex will be used for preventing resize while present is running
inline std::shared_mutex& PresentResizeMutex()
{
    static std::shared_mutex mutex;
    return mutex;
}

// Non-zero while the bridge resizes. The menu overlay is drawn on frame generation's present thread (XeFG's own thread
// for its native presents) and torn down by the resize on the game's thread; the present skips it meanwhile.
inline std::atomic<int>& ResizeInProgress()
{
    static std::atomic<int> count { 0 };
    return count;
}

struct ScopedResize
{
    ScopedResize() { ++ResizeInProgress(); }
    ~ScopedResize() { --ResizeInProgress(); }
    ScopedResize(const ScopedResize&) = delete;
    ScopedResize& operator=(const ScopedResize&) = delete;
};
} // namespace Dx11wDx12Sync
