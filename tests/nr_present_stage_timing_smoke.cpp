// Host check of native/PresentStageTiming.h: window max/average, report cadence and the stall rate limit. No GPU, no
// game. cl /std:c++20 /EHsc /W4 tests/nr_present_stage_timing_smoke.cpp
#include "../OptiScaler/native/PresentStageTiming.h"

#include <cstdio>
#include <cstring>

using namespace native::presenttiming;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main()
{
    // Cadence: a summary on the 300th present, not before; max and average over the window
    {
        StageTiming t;
        int summaries = 0;

        for (uint32_t i = 1; i <= kReportEveryPresents; ++i)
        {
            t.Record(Stage::Present, i == 10 ? 30.0 : 2.0);
            t.Record(Stage::Hook, i == 10 ? 31.0 : 3.0);
            auto out = t.EndPresent(i * 10.0);
            CHECK(!out.stall);

            if (out.summary)
            {
                CHECK(i == kReportEveryPresents);
                ++summaries;
            }
        }

        CHECK(summaries == 1);
        CHECK(t.SummaryLine().find("300 presents") == 0);
        CHECK(t.SummaryLine().find("present max 30.0 avg 2.09 ms") != std::string::npos);
        CHECK(t.SummaryLine().find("whole hook max 31.0 avg 3.09 ms") != std::string::npos);
        // Stages never recorded are left out
        CHECK(t.SummaryLine().find("bridge present") == std::string::npos);

        // Restart: the next window counts from zero
        t.Restart();
        t.Record(Stage::Present, 1.0);
        CHECK(!t.EndPresent(5000.0).summary);
    }

    // A stall (>50 ms): one line with the breakdown, then at most one per second
    {
        StageTiming t;
        t.Record(Stage::NativeMotion, 1.0);
        t.Record(Stage::BridgePresent, 400.0);
        t.Record(Stage::Hook, 410.0);
        auto out = t.EndPresent(1000.0);
        CHECK(out.stall);
        CHECK(t.StallLine().find("bridge present 400.0 ms") != std::string::npos);
        CHECK(t.StallLine().find("whole hook 410.0 ms") != std::string::npos);

        t.Record(Stage::Hook, 200.0);
        CHECK(!t.EndPresent(1500.0).stall); // inside the second
        t.Record(Stage::Hook, 200.0);
        CHECK(t.EndPresent(2000.0).stall); // a second after the first
        t.Record(Stage::Hook, 50.0);
        CHECK(!t.EndPresent(4000.0).stall); // 50 ms is not over 50 ms
    }

    // The frame generation instance: its own present is the present
    {
        StageTiming t;
        t.Record(Stage::FrameGenerationPresent, 300.0);
        auto out = t.EndPresent(0.0); // a clock that starts at 0 still logs the first stall
        CHECK(out.stall);
        CHECK(t.StallLine().find("frame generation present 300.0 ms") != std::string::npos);
    }

    // The wait for frame generation's mutex is its own stage, in the stall line and the summary
    {
        StageTiming t;
        t.Record(Stage::BridgePresent, 400.0);
        t.Record(Stage::FrameGenerationMutexWait, 380.0);
        t.Record(Stage::Hook, 410.0);
        auto out = t.EndPresent(0.0);
        CHECK(out.stall);
        CHECK(t.StallLine().find("bridge present 400.0 ms") != std::string::npos);
        CHECK(t.StallLine().find("frame generation mutex wait 380.0 ms") != std::string::npos);

        // Alone it does not make a stall: the stall is what the present took
        StageTiming u;
        u.Record(Stage::FrameGenerationMutexWait, 380.0);
        CHECK(!u.EndPresent(0.0).stall);

        StageTiming w;
        int summaries = 0;

        for (uint32_t i = 1; i <= kReportEveryPresents; ++i)
        {
            w.Record(Stage::FrameGenerationMutexWait, i == 5 ? 90.0 : 0.0);
            summaries += w.EndPresent(i * 10.0).summary ? 1 : 0;
        }

        CHECK(summaries == 1);
        CHECK(w.SummaryLine().find("frame generation mutex wait max 90.0 avg 0.30 ms") != std::string::npos);
    }

    // The per-thread hand-off from FGPresent to the Vulkan present hook: set each call, taken (and cleared) once
    {
        CHECK(TakeFrameGenerationMutexWait() == 0.0);
        NoteFrameGenerationMutexWait(12.5);
        CHECK(PeekFrameGenerationMutexWait() == 12.5);
        CHECK(PeekFrameGenerationMutexWait() == 12.5);
        NoteFrameGenerationMutexWait(0.0); // a later FGPresent that took no lock
        CHECK(PeekFrameGenerationMutexWait() == 0.0);
        NoteFrameGenerationMutexWait(7.0);
        CHECK(TakeFrameGenerationMutexWait() == 7.0);
        CHECK(TakeFrameGenerationMutexWait() == 0.0); // a frame whose present never reached FGPresent
    }

    printf(fails == 0 ? "PASS\n" : "FAILED\n");
    return fails == 0 ? 0 : 1;
}
