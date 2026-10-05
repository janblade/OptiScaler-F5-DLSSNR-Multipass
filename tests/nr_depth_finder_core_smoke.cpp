// Host check of native::DepthFinderCore, the API-neutral half of the depth finder: event sequences like the ones real games
// make (a D3D12 game with many command lists, an immediate-context game), no GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_depth_finder_core_smoke.cpp OptiScaler/native/DepthFinderCore.cpp
#include "../OptiScaler/native/DepthFinderCore.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace native;

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

static constexpr uint32_t W = 1920, H = 1080;
static constexpr uint32_t kWarmup = 2;

static const DepthBuffer kScene { 0xA, W, H, 40, false };
static const DepthBuffer kShadow { 0xB, 2048, 2048, 40, false };
static const DepthBuffer kSceneReadOnly { 0xA, W, H, 40, true };

// One frame the way a single-context game makes it: a shadow pass, then the scene (cleared to 0: reversed-Z), and a present.
// `snapshots` collects what the core asked to be copied.
static void SingleContextFrame(DepthFinderCore& core, std::vector<SnapshotRequest>& snapshots, bool unbindAtEnd = true)
{
    const uint64_t ctx = 1;

    auto take = [&](const SnapshotRequest& r)
    {
        if (r.take)
            snapshots.push_back(r);
    };

    take(core.OnDepthBound(ctx, true, &kShadow));
    core.OnViewport(ctx, 2048.0f);
    for (int i = 0; i < 30; ++i)
        core.OnDraw(ctx, 3000, 1);

    take(core.OnDepthBound(ctx, true, &kScene));
    core.OnViewport(ctx, (float) W);
    take(core.OnDepthClear(ctx, kScene, 0.0f));

    for (int i = 0; i < 200; ++i)
        core.OnDraw(ctx, 6000, 1);

    // A fullscreen pass: not counted as a viewport update, small.
    core.OnDraw(ctx, 6, 1);

    if (unbindAtEnd)
        take(core.OnDepthBound(ctx, false, nullptr));

    core.BeginPresent(W, H);
    core.EndPresent(W, H, kWarmup);
}

// Runs `body(context)` for each context of a frame: on its own thread each (the way a game records command lists in
// parallel), or one after the other on this one.
template <typename Body> static void ForEachContext(int contexts, bool parallel, Body body)
{
    if (!parallel)
    {
        for (int c = 0; c < contexts; ++c)
            body(c);

        return;
    }

    std::vector<std::thread> threads;

    for (int c = 0; c < contexts; ++c)
        threads.emplace_back(body, c);

    for (auto& t : threads)
        t.join();
}

// What a game with several command lists does in a frame: every list binds the shadow map and draws into it, then binds the
// scene (the first one clears it), draws, makes a fullscreen pass and an indirect draw, and closes. The draws of the lists run
// on a thread of their own each when `parallel`; the events around them are made in the same order from this thread.
// `snapshots` collects what the core asked to be copied (buffer, where, stretch), to be compared in sorted order.
static void MultiContextFrames(DepthFinderCore& core, int contexts, bool parallel, int frames,
                               std::vector<std::tuple<uint64_t, std::string, uint64_t>>& snapshots)
{
    auto take = [&](const SnapshotRequest& r)
    {
        if (r.take)
            snapshots.emplace_back(r.id, r.where, r.stretchVertices);
    };

    for (int frame = 0; frame < frames; ++frame)
    {
        for (int c = 0; c < contexts; ++c)
            take(core.OnDepthBound(100 + c, true, &kShadow));

        ForEachContext(contexts, parallel,
                       [&](int c)
                       {
                           core.OnViewport(100 + c, 2048.0f);
                           for (int i = 0; i < 30; ++i)
                               core.OnDraw(100 + c, 3000 + c, 1);
                       });

        for (int c = 0; c < contexts; ++c)
            take(core.OnDepthBound(100 + c, true, &kScene));

        take(core.OnDepthClear(100, kScene, 0.0f));

        ForEachContext(contexts, parallel,
                       [&](int c)
                       {
                           core.OnViewport(100 + c, (float) W);
                           for (int i = 0; i < 200; ++i)
                               core.OnDraw(100 + c, 6000, 1 + c % 3);
                           core.OnDraw(100 + c, 6, 1);
                           core.OnIndirect(100 + c, 4);
                       });

        for (int c = 0; c < contexts; ++c)
            take(core.OnContextEnd(100 + c));

        core.BeginPresent(W, H);
        core.EndPresent(W, H, kWarmup);
    }
}

int main()
{
    std::vector<std::string> log;

    // The scene's buffer is picked after the warm-up, with the reversed-Z hint from the clear value.
    {
        DepthFinderCore core;
        core.Start([&](const std::string& line) { log.push_back(line); });
        core.SetSnapshotsWanted(true);

        std::vector<SnapshotRequest> snapshots;

        for (int frame = 0; frame < 3; ++frame)
        {
            SingleContextFrame(core, snapshots);
            CHECK(!core.Armed() || frame >= 2);
        }

        for (int frame = 0; frame < 3; ++frame)
            SingleContextFrame(core, snapshots);

        const auto pick = core.CurrentPick();
        CHECK(pick.valid && pick.id == 0xA);
        CHECK(pick.width == W && pick.height == H);
        CHECK(pick.reversed);
        CHECK(core.Armed());
        CHECK(!log.empty());
    }

    // Once it is picked, a frame asks for its copy: the scene is cleared mid-frame (copy at the clear) and the scene stretch
    // drawn after the clear is copied when the context moves off the buffer ("unbind").
    {
        DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(true);

        std::vector<SnapshotRequest> snapshots;

        for (int frame = 0; frame < 6; ++frame)
            SingleContextFrame(core, snapshots);

        snapshots.clear();
        SingleContextFrame(core, snapshots);

        // The shadow map is never the pick, so it never asks. The scene is cleared once per frame with nothing drawn before
        // it (the clear at the start of its pass) and then drawn into: the one request is the stretch at the unbind.
        CHECK(snapshots.size() == 1);

        if (!snapshots.empty())
        {
            CHECK(snapshots[0].id == 0xA);
            CHECK(std::string(snapshots[0].where) == "unbind");
            CHECK(snapshots[0].stretchVertices == 200 * 6000 + 6);
            CHECK(!snapshots[0].readOnlyDepth);
        }
    }

    // A context that ends while still bound (a D3D12 command list closing) asks for the stretch at its end, in the state the
    // view says (read-only here).
    {
        DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(true);

        std::vector<SnapshotRequest> ignored;

        for (int frame = 0; frame < 6; ++frame)
            SingleContextFrame(core, ignored);

        // A new context, as a command list is: bind, draw, close.
        const uint64_t list = 77;
        auto request = core.OnDepthBound(list, true, &kSceneReadOnly);
        CHECK(!request.take);
        for (int i = 0; i < 50; ++i)
            core.OnDraw(list, 9000, 1);

        request = core.OnContextEnd(list);
        CHECK(request.take);
        CHECK(request.id == 0xA);
        CHECK(request.readOnlyDepth);
        CHECK(std::string(request.where) == "close");
        CHECK(request.stretchVertices == 50 * 9000);

        // Closed: a second end with nothing bound asks for nothing.
        CHECK(!core.OnContextEnd(list).take);
    }

    // Nothing is asked for while no one wants the copies.
    {
        DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(false);

        std::vector<SnapshotRequest> snapshots;

        for (int frame = 0; frame < 8; ++frame)
            SingleContextFrame(core, snapshots);

        CHECK(snapshots.empty());
        CHECK(core.CurrentPick().valid);
    }

    // The scene's stretch must draw a fair share of what the pick drew last frame: a tiny stretch is not copied.
    {
        DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(true);

        std::vector<SnapshotRequest> ignored;

        for (int frame = 0; frame < 6; ++frame)
            SingleContextFrame(core, ignored); // a frame draws about 1.2M vertices: the floor is 2% of that

        // A new context draws 100 vertices into the scene buffer and closes: under the floor, no copy.
        const uint64_t list = 78;
        core.OnDepthBound(list, true, &kScene);
        core.OnDraw(list, 100, 1);
        CHECK(!core.OnContextEnd(list).take);
    }

    // A game with an upscaler of its own: the finder stands down, drops its pick, and watches again after the quiet time.
    {
        DepthFinderCore core;
        core.Start({});
        std::vector<SnapshotRequest> ignored;

        for (int frame = 0; frame < 6; ++frame)
            SingleContextFrame(core, ignored);

        CHECK(core.CurrentPick().valid);

        core.NoteUpscalerCall();
        bool stoodDown = false;

        core.BeginPresent(W, H);
        stoodDown = core.EndPresent(W, H, kWarmup);

        CHECK(stoodDown);
        CHECK(core.GameCallsUpscaler());
        CHECK(!core.Active());
        CHECK(!core.CurrentPick().valid);
        CHECK(!core.Armed());

        // Counting is off while it is down: events do nothing.
        CHECK(!core.OnDepthBound(1, true, &kScene).take);

        // 120 presents with no call: it wakes, and the warm-up starts over.
        bool woke = false;

        for (int i = 0; i < 125 && !woke; ++i)
        {
            core.BeginPresent(W, H);
            core.EndPresent(W, H, kWarmup);
            woke = !core.GameCallsUpscaler();
        }

        CHECK(woke);
        CHECK(core.Active());

        for (int frame = 0; frame < 6; ++frame)
            SingleContextFrame(core, ignored);

        CHECK(core.CurrentPick().valid && core.CurrentPick().id == 0xA);
    }

    // A game that keeps calling an upscaler is never picked in.
    {
        DepthFinderCore core;
        core.Start({});
        std::vector<SnapshotRequest> ignored;

        for (int frame = 0; frame < 40; ++frame)
        {
            core.NoteUpscalerCall();
            SingleContextFrame(core, ignored);
        }

        CHECK(core.GameCallsUpscaler());
        CHECK(!core.CurrentPick().valid);
    }

    // A D3D11-style immediate context: bound once at the start and never rebound, never cleared again. Nothing but
    // FlushForPresent (called once per presented frame, before BeginPresent, as the game's menu driver does) can ever offer a
    // copy of it, and it must keep offering one every frame without losing track of the binding.
    {
        DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(true);

        const uint64_t ctx = 1;
        std::vector<SnapshotRequest> ignored;

        core.OnDepthBound(ctx, true, &kShadow);
        core.OnViewport(ctx, 2048.0f);
        for (int i = 0; i < 30; ++i)
            core.OnDraw(ctx, 3000, 1);

        core.OnDepthBound(ctx, true, &kScene); // bound once, for good
        core.OnViewport(ctx, (float) W);
        core.OnDepthClear(ctx, kScene, 0.0f); // cleared once, for good

        // DepthFinderCore's own warm-up (kWarmup presents) and the Selector's own (a candidate must have been seen a couple
        // of Selector updates, which only run once DepthFinderCore is armed) both have to clear before a pick is valid; eight
        // frames is enough for both, matching the pattern the other tests in this file use.
        for (int frame = 0; frame < 8; ++frame)
        {
            for (int i = 0; i < 200; ++i)
                core.OnDraw(ctx, 6000, 1);

            // FlushForPresent runs before BeginPresent, the same order the D3D11 driver uses.
            const auto flushed = core.FlushForPresent(ctx);

            if (frame >= 6) // picked and armed by then
                CHECK(flushed.take && flushed.id == 0xA && std::string(flushed.where) == "present");

            core.BeginPresent(W, H);
            core.EndPresent(W, H, kWarmup);
        }

        CHECK(core.CurrentPick().valid && core.CurrentPick().id == 0xA);

        // Cleared once, before the first frame: the reversed-Z it showed then still holds for every frame after.
        CHECK(core.CurrentPick().reversed);

        // The binding is never lost: a further frame with no new draw offers nothing (current is empty), and one more draw
        // and flush offers one again.
        CHECK(!core.FlushForPresent(ctx).take);

        for (int i = 0; i < 200; ++i)
            core.OnDraw(ctx, 6000, 1);

        CHECK(core.FlushForPresent(ctx).take);
    }

    // A frame whose clear the hooks do not see keeps the buffer's reversed-Z; the next clear decides again, both ways.
    {
        DepthFinderCore core;
        core.Start({});
        std::vector<SnapshotRequest> ignored;

        for (int frame = 0; frame < 6; ++frame)
            SingleContextFrame(core, ignored);

        CHECK(core.CurrentPick().reversed);

        const uint64_t ctx = 1;

        for (int frame = 0; frame < 3; ++frame)
        {
            core.OnDepthBound(ctx, true, &kScene);
            core.OnViewport(ctx, (float) W);
            for (int i = 0; i < 200; ++i)
                core.OnDraw(ctx, 6000, 1);
            core.OnDepthBound(ctx, false, nullptr);
            core.BeginPresent(W, H);
            core.EndPresent(W, H, kWarmup);

            CHECK(core.CurrentPick().valid && core.CurrentPick().id == 0xA);
            CHECK(core.CurrentPick().reversed);
        }

        core.OnDepthBound(ctx, true, &kScene);
        core.OnDepthClear(ctx, kScene, 1.0f);
        for (int i = 0; i < 200; ++i)
            core.OnDraw(ctx, 6000, 1);
        core.OnDepthBound(ctx, false, nullptr);
        core.BeginPresent(W, H);
        core.EndPresent(W, H, kWarmup);

        CHECK(!core.CurrentPick().reversed);
    }

    // Our own synthetic upscaler call does not stand the finder down, nested or not; a call outside the scope still does.
    {
        DepthFinderCore core;
        core.Start({});
        std::vector<SnapshotRequest> ignored;

        for (int frame = 0; frame < 6; ++frame)
            SingleContextFrame(core, ignored);

        for (int frame = 0; frame < 3; ++frame)
        {
            {
                SyntheticUpscalerCallScope outer;
                core.NoteUpscalerCall();
                {
                    SyntheticUpscalerCallScope inner;
                    core.NoteUpscalerCall();
                }
                core.NoteUpscalerCall();
            }

            SingleContextFrame(core, ignored);
            CHECK(!core.GameCallsUpscaler());
            CHECK(core.CurrentPick().valid);
        }

        core.NoteUpscalerCall();
        core.BeginPresent(W, H);
        CHECK(core.EndPresent(W, H, kWarmup));
        CHECK(core.GameCallsUpscaler());
    }

    // Lists recorded on several threads at once give the same candidates, pick, copy requests and log as the same events made on
    // one thread: the per-draw counting takes no lock, and what it counts reaches the buffers at the lists' own events.
    {
        constexpr int kContexts = 8;
        constexpr int kFrames = 10;

        std::vector<std::string> serialLog;
        std::vector<std::tuple<uint64_t, std::string, uint64_t>> serialSnapshots;

        DepthFinderCore serial;
        serial.Start([&](const std::string& line) { serialLog.push_back(line); });
        serial.SetSnapshotsWanted(true);
        MultiContextFrames(serial, kContexts, false, kFrames, serialSnapshots);

        CHECK(serial.CurrentPick().valid && serial.CurrentPick().id == 0xA);
        CHECK(!serialSnapshots.empty());
        CHECK(!serialLog.empty());

        std::sort(serialSnapshots.begin(), serialSnapshots.end());

        for (int run = 0; run < 3; ++run)
        {
            std::vector<std::string> parallelLog;
            std::vector<std::tuple<uint64_t, std::string, uint64_t>> parallelSnapshots;

            DepthFinderCore parallel;
            parallel.Start([&](const std::string& line) { parallelLog.push_back(line); });
            parallel.SetSnapshotsWanted(true);
            MultiContextFrames(parallel, kContexts, true, kFrames, parallelSnapshots);

            std::sort(parallelSnapshots.begin(), parallelSnapshots.end());

            const auto a = serial.CurrentPick();
            const auto b = parallel.CurrentPick();
            CHECK(a.valid == b.valid && a.id == b.id && a.width == b.width && a.height == b.height &&
                  a.reversed == b.reversed);
            CHECK(serialSnapshots == parallelSnapshots);
            CHECK(serialLog == parallelLog);
            CHECK(serial.SnapshotFloor() == parallel.SnapshotFloor());
        }
    }

    // Lists that interleave their events: whatever any list drew into a buffer counts at a clear or an unbind made by another,
    // as when every draw was added to the buffer at once. The expected values are the locked core's (the test passes on it).
    {
        DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(true);

        std::vector<SnapshotRequest> ignored;

        for (int frame = 0; frame < 6; ++frame)
            SingleContextFrame(core, ignored); // the floor is 2% of 1.2M vertices: 24000

        // List 200 draws the scene and stays open; list 201, bound to the shadow map, clears the scene buffer.
        core.OnDepthBound(200, true, &kScene);
        core.OnViewport(200, (float) W);
        for (int i = 0; i < 200; ++i)
            core.OnDraw(200, 6000, 1);

        core.OnDepthBound(201, true, &kShadow);
        const auto cleared = core.OnDepthClear(201, kScene, 0.0f);
        CHECK(cleared.take && cleared.id == 0xA && std::string(cleared.where) == "clear");
        CHECK(cleared.stretchVertices == 200 * 6000);

        core.OnContextEnd(200);
        core.OnContextEnd(201);
        core.BeginPresent(W, H);
        core.EndPresent(W, H, kWarmup);

        // Lists 300 and 301 both draw into the scene; 301 moves off it first, with only together enough to be copied.
        core.OnDepthBound(300, true, &kScene);
        core.OnDepthBound(301, true, &kScene);
        core.OnViewport(300, (float) W);
        core.OnViewport(301, (float) W);
        for (int i = 0; i < 3; ++i)
            core.OnDraw(300, 6000, 1);
        for (int i = 0; i < 2; ++i)
            core.OnDraw(301, 6000, 1);

        const auto left = core.OnDepthBound(301, false, nullptr);
        CHECK(left.take && left.id == 0xA && std::string(left.where) == "unbind");
        CHECK(left.stretchVertices == 5 * 6000);

        // The stretch was taken whole at the unbind: the other list's close has nothing left to copy.
        CHECK(!core.OnContextEnd(300).take);
        core.BeginPresent(W, H);
        core.EndPresent(W, H, kWarmup);
    }

    // An adapter whose one context id is drawn on from several threads (D3D11's deferred contexts go through the immediate
    // context's hooks): with shared contexts every draw counts.
    {
        DepthFinderCore core;
        core.SetSharedContexts(true);
        core.Start({});
        core.SetSnapshotsWanted(true);

        std::vector<SnapshotRequest> ignored;

        for (int frame = 0; frame < 6; ++frame)
            SingleContextFrame(core, ignored);

        core.OnDepthBound(1, true, &kScene);
        core.OnViewport(1, (float) W);
        ForEachContext(4, true,
                       [&](int)
                       {
                           for (int i = 0; i < 20000; ++i)
                               core.OnDraw(1, 100, 1);
                       });

        const auto left = core.OnDepthBound(1, false, nullptr);
        CHECK(left.take && left.stretchVertices == 4ull * 20000 * 100);
    }

    // Lists a game makes once and lets go are retired after a while, and the finder keeps working: a list id seen again later
    // (a new list at a reused address) counts as before.
    {
        DepthFinderCore core;
        core.Start({});
        core.SetSnapshotsWanted(true);

        std::vector<SnapshotRequest> ignored;

        for (int list = 0; list < 64; ++list)
        {
            core.OnDepthBound(1000 + list, true, &kShadow);
            core.OnDraw(1000 + list, 3000, 1);
            core.OnContextEnd(1000 + list);
        }

        for (int frame = 0; frame < 1400; ++frame)
            SingleContextFrame(core, ignored);

        CHECK(core.CurrentPick().valid && core.CurrentPick().id == 0xA);

        core.OnDepthBound(1000, true, &kScene);
        for (int i = 0; i < 50; ++i)
            core.OnDraw(1000, 9000, 1);
        const auto closed = core.OnContextEnd(1000);
        CHECK(closed.take && closed.stretchVertices == 50 * 9000);
    }

    printf(fails == 0 ? "all passed\n" : "FAILED (%d)\n", fails);
    return fails == 0 ? 0 : 1;
}
