// Host check of LogAsyncRule.h: a sink that blocks must not hold the logging call, lines keep their order, a full queue
// drops the oldest and counts them, and the crash-path flush is bounded. No GPU, no game. spdlog is header-only:
// cl /std:c++20 /EHsc /W4 /DSPDLOG_HEADER_ONLY /DSPDLOG_NO_ATOMIC_LEVELS=0 /I external\spdlog\include tests\log_async_no_stall_smoke.cpp
#include "../OptiScaler/LogAsyncRule.h"

#include <spdlog/sinks/base_sink.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using namespace logasync;
using Clock = std::chrono::steady_clock;

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

// Stands in for a file on a drive that hiccups: the first line takes `firstDelayMs` (a blocked disk), the rest `eachMs`
class SlowSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    SlowSink(int firstDelayMs, int eachMs) : _first(firstDelayMs), _each(eachMs) {}

    std::vector<std::string> lines;
    int flushes = 0;

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(lines.empty() ? _first : _each));
        lines.emplace_back(msg.payload.data(), msg.payload.size());
    }
    void flush_() override { ++flushes; }

  private:
    int _first, _each;
};

// The lines without the flush marker the drain adds
static std::vector<std::string> Real(const std::vector<std::string>& lines)
{
    std::vector<std::string> out;
    for (const auto& line : lines)
        if (line.rfind(FlushMarkerSink::kPrefix, 0) != 0)
            out.push_back(line);
    return out;
}

static double Ms(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main()
{
    // The rule: auto + file = worker thread, explicit false = synchronous, explicit true = worker that blocks
    CHECK(ChooseMode(std::nullopt, true) == Mode::AsyncDropOldest);
    CHECK(ChooseMode(std::nullopt, false) == Mode::Sync);
    CHECK(ChooseMode(false, true) == Mode::Sync);
    CHECK(ChooseMode(true, true) == Mode::AsyncBlock);
    CHECK(ChooseMode(true, false) == Mode::AsyncBlock);

    // What the drop-oldest queue costs in memory (it is allocated up front): about 13 MB at most
    printf("queue: %zu slots of %zu bytes = %.1f MB\n", kDropOldestQueueSize + 1, sizeof(spdlog::details::async_msg),
           (kDropOldestQueueSize + 1) * sizeof(spdlog::details::async_msg) / 1048576.0);
    static_assert((kDropOldestQueueSize + 1) * sizeof(spdlog::details::async_msg) < 16u * 1048576u);

    // A sink blocked for 500 ms does not hold 1,000 calls (auto mode)
    {
        auto slow = std::make_shared<SlowSink>(500, 0);
        auto setup = MakeLogger("t_auto", { slow }, ChooseMode(std::nullopt, true));
        setup.logger->set_level(spdlog::level::debug);
        setup.logger->flush_on(spdlog::level::trace);

        const auto start = Clock::now();

        for (int i = 0; i < 1000; ++i)
            setup.logger->debug("line {}", i);

        const double took = Ms(start, Clock::now());
        printf("auto: 1000 calls took %.1f ms\n", took);
        CHECK(took < 100.0);

        // They all arrive, in order, and the drain sees the flush
        CHECK(FlushAndDrain(setup, std::chrono::milliseconds(3000)));
        CHECK(Real(slow->lines).size() == 1000);
        bool ordered = true;
        const auto got = Real(slow->lines);
        for (size_t i = 0; i < got.size(); ++i)
            ordered = ordered && got[i] == "line " + std::to_string(i);
        CHECK(ordered);
    }

    // An explicit false keeps today's synchronous behaviour: the call waits for the sink, line by line
    {
        auto slow = std::make_shared<SlowSink>(200, 0);
        auto setup = MakeLogger("t_sync", { slow }, ChooseMode(false, true));
        setup.logger->flush_on(spdlog::level::trace);

        const auto start = Clock::now();
        setup.logger->info("one");
        CHECK(Ms(start, Clock::now()) >= 190.0);
        CHECK(slow->lines.size() == 1); // already written when the call returns
        CHECK(setup.pool == nullptr);
    }

    // A full queue drops the oldest, counts them, and the newest survive; one report per burst
    {
        auto slow = std::make_shared<SlowSink>(300, 0);
        auto setup = MakeLogger("t_drop", { slow }, Mode::AsyncDropOldest);
        const size_t total = setup.queueSize + 5000;

        const auto start = Clock::now();

        for (size_t i = 0; i < total; ++i)
            setup.logger->info("n {}", i);

        CHECK(Ms(start, Clock::now()) < 250.0); // never waited for the 300 ms sink

        CHECK(FlushAndDrain(setup, std::chrono::milliseconds(5000)));
        const size_t dropped = setup.pool->overrun_counter();
        printf("drop: %zu queued, %zu dropped, %zu written\n", total, dropped, slow->lines.size());
        CHECK(dropped > 0);
        CHECK(slow->lines.size() + dropped == total + 1); // every line and the marker line is written or counted
        CHECK(slow->lines.size() > 2 && slow->lines[slow->lines.size() - 2] == "n " + std::to_string(total - 1)); // the newest line is never the one dropped

        DropReporter reporter;
        CHECK(reporter.Poll(0, 0.0) == 0);
        CHECK(reporter.Poll(dropped, 1000.0) == dropped);
        CHECK(reporter.Poll(dropped, 9000.0) == 0);     // nothing new
        CHECK(reporter.Poll(dropped + 7, 2000.0) == 0); // rising, but inside the interval
        CHECK(reporter.Poll(dropped + 7, 7000.0) == 7); // the burst, said once
        CHECK(reporter.Poll(dropped + 7, 20000.0) == 0);
    }

    // The crash path: the flush-and-wait is bounded when the sink is stuck, and complete when it is merely slow
    {
        auto stuck = std::make_shared<SlowSink>(1500, 0);
        auto setup = MakeLogger("t_stuck", { stuck }, Mode::AsyncDropOldest);
        setup.logger->info("a");
        const auto start = Clock::now();
        CHECK(!FlushAndDrain(setup, std::chrono::milliseconds(200)));
        const double took = Ms(start, Clock::now());
        printf("stuck: gave up after %.0f ms\n", took);
        CHECK(took < 600.0);

        auto slowish = std::make_shared<SlowSink>(0, 2);
        auto setup2 = MakeLogger("t_slowish", { slowish }, Mode::AsyncDropOldest);
        for (int i = 0; i < 100; ++i)
            setup2.logger->info("s {}", i);
        CHECK(FlushAndDrain(setup2, std::chrono::milliseconds(2000)));
        CHECK(Real(slowish->lines).size() == 100);
    }

    // A block-policy logger with a full queue is not asked to take another message on the crash path
    {
        auto stuck = std::make_shared<SlowSink>(1000, 0);
        auto pool = std::make_shared<spdlog::details::thread_pool>(kBlockQueueSize, 1);
        auto setup = MakeLogger("t_block", { stuck }, Mode::AsyncBlock, pool);
        setup.logger->info("first"); // the worker takes it and sits in the sink
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (size_t i = 0; i < kBlockQueueSize - 1; ++i)
            setup.logger->info("x");
        const auto start = Clock::now();
        CHECK(!FlushAndDrain(setup, std::chrono::milliseconds(200)));
        CHECK(Ms(start, Clock::now()) < 150.0);
    }

    // Rebuilding the logger: the old setup is parked, not destroyed, so the caller never joins its stuck worker; the new
    // logger works at once and the drop reporter starts again from 0
    {
        auto stuck = std::make_shared<SlowSink>(1500, 0);
        auto* oldSetup = new Setup(MakeLogger("t_old", { stuck }, Mode::AsyncDropOldest));

        for (int i = 0; i < 100; ++i)
            oldSetup->logger->info("old {}", i);

        std::vector<Setup*> retired;
        DropReporter reporter;
        CHECK(reporter.Poll(40, 0.0) == 40);

        const auto start = Clock::now();
        auto fresh = std::make_shared<SlowSink>(0, 0);
        auto newSetup = MakeLogger("t_new", { fresh }, Mode::AsyncDropOldest);
        Retire(retired, oldSetup);
        Retire(retired, nullptr); // the first build has no previous setup
        reporter.Reset();
        newSetup.logger->info("new line");
        const double took = Ms(start, Clock::now());
        printf("rebuild: switching took %.1f ms\n", took);
        CHECK(took < 100.0);
        CHECK(retired.size() == 1 && retired[0] == oldSetup);
        CHECK(FlushAndDrain(newSetup, std::chrono::milliseconds(1000)));
        CHECK(Real(fresh->lines).size() == 1);

        // After the reset a new logger's first overflow is reported, not swallowed by the old count
        CHECK(reporter.Poll(3, 100000.0) == 3);
    }

    // The exit rule: a worker-backed logger drains once; a synchronous one has nothing to drain
    CHECK(ShouldDrainAtExit(Mode::AsyncDropOldest, false));
    CHECK(ShouldDrainAtExit(Mode::AsyncBlock, false));
    CHECK(!ShouldDrainAtExit(Mode::Sync, false));
    CHECK(!ShouldDrainAtExit(Mode::AsyncDropOldest, true));

    // The unhandled-exception filter chain
    {
        using Filter = int (*)(int);
        static Filter self = +[](int v) { return v; };
        static Filter gameA = +[](int v) { return v + 1; };
        static Filter gameB = +[](int v) { return v + 2; };

        FilterChain<Filter> chain(self);
        CHECK(chain.Next() == nullptr);

        // Ours went in over the game's first filter: it is chained after ours
        chain.Adopt(gameA);
        CHECK(chain.Next() == gameA);

        // The game sets another one later: the previous one is returned (as the API does), the new one is chained
        CHECK(chain.Replace(gameB) == gameA);
        CHECK(chain.Next() == gameB);
        CHECK(chain.Next()(10) == 12); // the game's answer is what comes back

        // Our own filter handed in (a game restoring "the previous") is never chained to itself
        CHECK(chain.Replace(self) == gameB);
        CHECK(chain.Next() == gameB);
        chain.Adopt(self);
        CHECK(chain.Next() == gameB);

        // The game clears its filter
        CHECK(chain.Replace(nullptr) == gameB);
        CHECK(chain.Next() == nullptr);

        // A crash inside the handler: only the first entry on a thread does the work
        {
            FilterChain<Filter>::Entry outer;
            CHECK(outer.First());
            {
                FilterChain<Filter>::Entry inner;
                CHECK(!inner.First());
            }
            FilterChain<Filter>::Entry again;
            CHECK(!again.First()); // the outer one is still handling
        }
        FilterChain<Filter>::Entry afterwards;
        CHECK(afterwards.First()); // the flag is back once the handler returned
    }

    printf(fails == 0 ? "PASS\n" : "FAILED %d\n", fails);
    return fails == 0 ? 0 : 1;
}
