// Host check of native::DepthFinderCore, the API-neutral half of the depth finder: event sequences like the ones real games
// make (a D3D12 game with many command lists, an immediate-context game), no GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_depth_finder_core_smoke.cpp OptiScaler/native/DepthFinderCore.cpp
#include "../OptiScaler/native/DepthFinderCore.h"

#include <cstdio>
#include <string>
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

    printf(fails == 0 ? "all passed\n" : "FAILED (%d)\n", fails);
    return fails == 0 ? 0 : 1;
}
