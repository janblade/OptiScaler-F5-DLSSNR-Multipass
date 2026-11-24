#include "pch.h"
#include "FrameLimit.h"

#include "Config.h"
#include "State.h"
// #include "hooks/D3D11Hooks.h"

inline uint64_t FrameLimit::get_timestamp()
{
    static const int64_t freq = []()
    {
        LARGE_INTEGER f {};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1LL;
    }();

    LARGE_INTEGER counter {};
    QueryPerformanceCounter(&counter);
    const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
    const uint64_t f = static_cast<uint64_t>(freq);
    return (ticks / f) * 1'000'000'000ULL + ((ticks % f) * 1'000'000'000ULL) / f;
}

// https://learn.microsoft.com/en-us/windows/win32/sync/using-waitable-timer-objects
inline int FrameLimit::timer_sleep(int64_t hundred_ns)
{
    static HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due_time;

    due_time.QuadPart = -hundred_ns;

    if (!timer)
        return 1;

    if (!SetWaitableTimerEx(timer, &due_time, 0, NULL, NULL, NULL, 0))
        return 2;

    const DWORD waitMs = static_cast<DWORD>(std::clamp<int64_t>(hundred_ns / 10'000 + 2000, 1000, 120'000));
    if (WaitForSingleObject(timer, waitMs) != WAIT_OBJECT_0)
        return 3;

    return 0;
};

inline int FrameLimit::busywait_sleep(int64_t ns)
{
    auto current_time = get_timestamp();
    auto wait_until = current_time + ns;
    while (current_time < wait_until)
    {
        YieldProcessor();
        current_time = get_timestamp();
    }
    return 0;
}

inline int FrameLimit::combined_sleep(int64_t ns)
{
    if (ns <= 0)
        return 0;

    constexpr int64_t busywait_threshold = 2'000'000; // 2ms
    int status {};
    const uint64_t current_time = get_timestamp();
    if (ns <= busywait_threshold)
        status = busywait_sleep(ns);
    else
        status = timer_sleep((ns - busywait_threshold) / 100);

    const uint64_t elapsed = get_timestamp() - current_time;
    if (!status && elapsed < static_cast<uint64_t>(ns))
        status = busywait_sleep(ns - static_cast<int64_t>(elapsed));

    return status;
}

namespace
{
std::atomic<double> sleptMs = 0.0;
}

float FrameLimit::CapFps()
{
    auto& state = State::Instance();
    const float limit = Config::Instance()->FramerateLimit.value_or_default();
    const float dynamic = state.xefgDynamicCapFps.load(std::memory_order_relaxed);
    const auto fg = state.currentFG;

    // While XeFG is on, through its short pauses (UI composition or hudless changes pause it for 10 frames): switching
    // back to the FPS limit and again would resend every sleep mode twice a pause. Not when XeFG passes frames through
    // (real exclusive full screen) or generates none (1X, which the cap never picks): it would cap the game's own
    // frames.
    if (dynamic <= 0.0f || !UsesXeFGApi(state.activeFgOutput) || fg == nullptr || !fg->IsActive() ||
        fg->GenerationOff() || state.realExclusiveFullscreen)
    {
        return limit;
    }

    return limit > 0.0f ? std::min(limit, dynamic) : dynamic;
}

double FrameLimit::TakeSleptMs() { return sleptMs.exchange(0.0); }

void FrameLimit::sleep(uint32_t shownPerRendered)
{
    static std::mutex s_limitMutex;
    std::lock_guard<std::mutex> lock(s_limitMutex);
    static uint64_t next_frame_time = 0;

    if (auto fpsCap = CapFps(); fpsCap > 0.0f)
    {
        uint64_t min_interval_us = std::clamp((uint64_t) (1'000'000.0 / (double) fpsCap + 0.5), 0ULL, 100'000'000ULL);

        if (shownPerRendered > 1)
            min_interval_us *= shownPerRendered;

        const uint64_t target_interval_ns = min_interval_us * 1000ULL;
        const uint64_t current_time = get_timestamp();

        // Advance a periodic deadline rather than re-anchoring after sleep returns so OS wakeup jitter
        // never accumulates into steady FPS drift below the cap. Re-anchor on first frame or >1 interval hitch.
        if (next_frame_time == 0 || current_time >= next_frame_time + target_interval_ns)
        {
            next_frame_time = current_time + target_interval_ns;
            return;
        }

        // If the FPS cap rose or FG multiplier dropped, clamp stale deadline so we never sleep longer than
        // one current target interval.
        if (next_frame_time > current_time + target_interval_ns)
            next_frame_time = current_time + target_interval_ns;

        if (current_time < next_frame_time)
        {
            const uint64_t wait_ns = next_frame_time - current_time;
            if (auto res = combined_sleep(static_cast<int64_t>(wait_ns)); res)
                LOG_ERROR("Sleep command failed: {}", res);
        }

        const uint64_t after_time = get_timestamp();
        next_frame_time += target_interval_ns;
        if (after_time > next_frame_time + target_interval_ns)
            next_frame_time = after_time + target_interval_ns;

        if (after_time > current_time)
            sleptMs.fetch_add((double) (after_time - current_time) / 1'000'000.0);
    }
    else
    {
        next_frame_time = 0;
    }
}
