#pragma once

// Where does a present spend its time? Per-stage max and average over a window of presents, and one line when a single
// present runs long, so a stall in a log names the stage that holds it. Header-only and free of D3D, Vulkan and spdlog:
// a host test can include it. One instance per present thread; the caller reads the clock and does the logging.

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace native::presenttiming
{
// What hkvkQueuePresentKHR times (Present = the original vkQueuePresentKHR, the hidden one when bridged), and what
// FGHooks::FGPresent times (FrameGenerationPresent = o_FGSCPresent, XeFG's own present). FrameGenerationMutexWait is the
// time FGPresent waited for frame generation's mutex before it; on the game's present thread that wait sits inside
// BridgePresent, so a stall there says whether it was our work or the wait for the other present.
enum class Stage : uint32_t
{
    NativeMotion,
    LowLatency,
    Present,
    BridgePresent,
    Hook,
    FrameGenerationPresent,
    FrameGenerationMutexWait,
    Count
};

inline const char* StageName(Stage stage)
{
    switch (stage)
    {
    case Stage::NativeMotion:
        return "native motion";
    case Stage::LowLatency:
        return "low latency";
    case Stage::Present:
        return "present";
    case Stage::BridgePresent:
        return "bridge present";
    case Stage::Hook:
        return "whole hook";
    case Stage::FrameGenerationPresent:
        return "frame generation present";
    case Stage::FrameGenerationMutexWait:
        return "frame generation mutex wait";
    default:
        return "?";
    }
}

inline constexpr uint32_t kReportEveryPresents = 300;
inline constexpr double kStallMs = 50.0;
inline constexpr double kStallLogIntervalMs = 1000.0;

// How long this thread's last FGPresent waited for frame generation's mutex. FGPresent sets it on every call (0 when it
// took no lock); the game's present hook takes it after the bridge's present (the same thread) and records it as a stage.
inline double& ThreadMutexWaitMs()
{
    thread_local double waitMs = 0.0;
    return waitMs;
}

inline void NoteFrameGenerationMutexWait(double ms)
{
    ThreadMutexWaitMs() = ms;
}

// Read and keep: FGPresent's own timing
inline double PeekFrameGenerationMutexWait()
{
    return ThreadMutexWaitMs();
}

// Read and clear: a frame whose present never reached FGPresent must not report the last frame's wait
inline double TakeFrameGenerationMutexWait()
{
    const double ms = ThreadMutexWaitMs();
    ThreadMutexWaitMs() = 0.0;
    return ms;
}

class StageTiming
{
  public:
    // This present's time in `stage`, ms. Stages not recorded count as 0 for that present.
    void Record(Stage stage, double ms)
    {
        _current[Index(stage)] = ms;
    }

    struct Outcome
    {
        bool summary = false; // 300 presents passed: SummaryLine() has the line; call Restart() after logging it
        bool stall = false;   // this present ran over kStallMs and the last stall line is at least a second old
    };

    // The present is over: `nowMs` is any monotonic clock in ms. Folds the recorded times into the window.
    Outcome EndPresent(double nowMs)
    {
        Outcome out;

        for (size_t i = 0; i < N; ++i)
        {
            _sum[i] += _current[i];

            if (_current[i] > _max[i])
                _max[i] = _current[i];
        }

        ++_count;

        // What "a present" lasts: the whole hook, or for the frame generation instance its own present
        const double total = _current[Index(Stage::Hook)] > _current[Index(Stage::FrameGenerationPresent)]
                                 ? _current[Index(Stage::Hook)]
                                 : _current[Index(Stage::FrameGenerationPresent)];

        if (total > kStallMs && (_lastStallLogMs < 0.0 || nowMs - _lastStallLogMs >= kStallLogIntervalMs))
        {
            out.stall = true;
            _lastStallLogMs = nowMs;
            _stallLine = Format("stall", _current, nullptr);
        }

        out.summary = _count >= kReportEveryPresents;

        if (out.summary)
            _summaryLine = BuildSummary();

        _current.fill(0.0);
        return out;
    }

    // The line for Outcome::stall: each stage's time in this present
    const std::string& StallLine() const
    {
        return _stallLine;
    }

    // The line for Outcome::summary
    const std::string& SummaryLine() const
    {
        return _summaryLine;
    }

    void Restart()
    {
        _sum.fill(0.0);
        _max.fill(0.0);
        _count = 0;
    }

  private:
    static constexpr size_t N = static_cast<size_t>(Stage::Count);
    using Row = std::array<double, N>;

    static size_t Index(Stage stage)
    {
        return static_cast<size_t>(stage);
    }

    // Stages that never recorded anything are left out
    static std::string Format(const char* what, const Row& values, const Row* averages)
    {
        std::string line = what;
        char buffer[96];

        for (size_t i = 0; i < N; ++i)
        {
            if (values[i] <= 0.0)
                continue;

            if (averages != nullptr)
                snprintf(buffer, sizeof(buffer), "; %s max %.1f avg %.2f ms", StageName(static_cast<Stage>(i)),
                         values[i], (*averages)[i]);
            else
                snprintf(buffer, sizeof(buffer), "; %s %.1f ms", StageName(static_cast<Stage>(i)), values[i]);

            line += buffer;
        }

        return line;
    }

    std::string BuildSummary() const
    {
        Row average {};

        for (size_t i = 0; i < N; ++i)
            average[i] = _count > 0 ? _sum[i] / static_cast<double>(_count) : 0.0;

        char head[48];
        snprintf(head, sizeof(head), "%u presents", _count);
        return Format(head, _max, &average);
    }

    Row _current {};
    Row _sum {};
    Row _max {};
    uint32_t _count = 0;
    double _lastStallLogMs = -1.0;
    std::string _stallLine;
    std::string _summaryLine;
};
} // namespace native::presenttiming
