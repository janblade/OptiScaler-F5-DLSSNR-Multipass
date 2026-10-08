#pragma once

// The command lists NR has recorded something on that the submit and reset hooks must hear about: its GPU timestamps
// (DlssNrGpuTime), Reuse's coverage readbacks and the finished-picture copy (DlssNr_Late.inl). Those hooks run on every
// ExecuteCommandLists and command list Reset in the process, on any thread and any queue (the game's workers, frame
// generation's presenter). NR holds g_nrMutex for its whole recording, so a hook that always took it would hold all of
// them up for that long. A hook first looks here, without a lock, and takes g_nrMutex only for a list that is watched.
//
// Lists are identities only (never dereferenced). Add is called while recording, before the list can be submitted; the
// hooks Remove a list once they have handed it to the trackers. If the table is ever full, Full() stays true and the
// hooks take the lock every time, as before.

#include <atomic>
#include <cstddef>

namespace DlssNrWatchedLists
{
inline constexpr size_t kCount = 64;

inline std::atomic<const void*> g_lists[kCount] {};
inline std::atomic<bool> g_full = false;

inline void Add(const void* list)
{
    if (list == nullptr)
        return;

    for (auto& entry : g_lists)
    {
        if (entry.load() == list)
            return;
    }

    for (auto& entry : g_lists)
    {
        const void* expected = nullptr;

        if (entry.compare_exchange_strong(expected, list))
            return;
    }

    g_full = true;
}

// Any of `lists` watched (or the table overflowed): the hook must take the lock.
inline bool Any(unsigned count, const void* const* lists)
{
    if (g_full.load())
        return true;

    if (lists == nullptr)
        return false;

    for (auto& entry : g_lists)
    {
        const void* watched = entry.load();

        if (watched == nullptr)
            continue;

        for (unsigned i = 0; i < count; ++i)
        {
            if (lists[i] == watched)
                return true;
        }
    }

    return false;
}

inline void Remove(unsigned count, const void* const* lists)
{
    if (lists == nullptr)
        return;

    for (unsigned i = 0; i < count; ++i)
    {
        if (lists[i] == nullptr)
            continue;

        for (auto& entry : g_lists)
        {
            const void* expected = lists[i];
            entry.compare_exchange_strong(expected, nullptr);
        }
    }
}
} // namespace DlssNrWatchedLists
