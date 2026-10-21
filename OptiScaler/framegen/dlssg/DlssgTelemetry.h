#pragma once

#include <atomic>

// What the game's own DLSS-G is doing, for the overlay's line under "Override DLSSG Ratio". Written on the game's
// threads by the Streamline hooks and read by the overlay, so plain atomics. A setting proves nothing about MFG: these
// are the counts Streamline itself reported. Not filled while OptiScaler's own frame generation stands in for DLSS-G.
namespace DlssgTelemetry
{

struct Counts
{
    std::atomic_bool optionsSeen { false };
    std::atomic_bool active { false };         // the last slDLSSGSetOptions sent left DLSS-G on
    std::atomic<unsigned int> requested { 1 }; // generated frames the game asked for (1 = 2X)
    std::atomic<unsigned int> sent { 1 };      // generated frames sent on, after any override
    std::atomic<unsigned int> result { 0 };    // sl::Result of that call (0 = ok)
    std::atomic_bool stateSeen { false };
    std::atomic<unsigned int> presented { 0 }; // numFramesActuallyPresented at the last slDLSSGGetState
    std::atomic<unsigned int> maxPresented { 0 };
};

inline Counts g_counts;

inline const Counts& Get() { return g_counts; }

inline void RecordSetOptions(unsigned int requested, unsigned int sent, bool active, unsigned int result)
{
    g_counts.requested.store(requested, std::memory_order_relaxed);
    g_counts.sent.store(sent, std::memory_order_relaxed);
    g_counts.result.store(result, std::memory_order_relaxed);
    g_counts.active.store(active, std::memory_order_relaxed);
    g_counts.optionsSeen.store(true, std::memory_order_release);
}

inline void RecordState(unsigned int presented)
{
    g_counts.presented.store(presented, std::memory_order_relaxed);

    auto seenMax = g_counts.maxPresented.load(std::memory_order_relaxed);
    while (presented > seenMax &&
           !g_counts.maxPresented.compare_exchange_weak(seenMax, presented, std::memory_order_relaxed))
    {
    }

    g_counts.stateSeen.store(true, std::memory_order_release);
}

} // namespace DlssgTelemetry
