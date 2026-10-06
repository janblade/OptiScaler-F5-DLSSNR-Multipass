// Not built with the precompiled header: no Direct3D in here, so tests/nr_depth_finder_core_smoke.cpp compiles it alone.
#include "DepthFinderCore.h"

#include <algorithm>
#include <format>
#include <intrin.h>
#include <iterator>

namespace native
{

namespace
{
thread_local int g_syntheticUpscalerDepth = 0;
}

SyntheticUpscalerCallScope::SyntheticUpscalerCallScope()
{
    ++g_syntheticUpscalerDepth;
}

SyntheticUpscalerCallScope::~SyntheticUpscalerCallScope()
{
    --g_syntheticUpscalerDepth;
}

void DepthFinderCore::Start(LogFn log)
{
    _log = std::move(log);
    _active = true;
}

void DepthFinderCore::NoteUpscalerCall()
{
    // Our own synthetic call: the finder must not see it as the game having an upscaler, or it stands down the very pipeline
    // that is feeding the call.
    if (g_syntheticUpscalerDepth > 0)
        return;

    // One relaxed store on every upscaler call: EndPresent decides what it means.
    _lastUpscalerCall.store(std::max<uint64_t>(_presentsNow.load(std::memory_order_relaxed), 1),
                            std::memory_order_relaxed);
}

uint64_t DepthFinderCore::NextInstanceId()
{
    static std::atomic<uint64_t> next { 1 };
    return next.fetch_add(1, std::memory_order_relaxed);
}

namespace
{
// A few contexts per thread, found without the lock: a thread records one command list at a time, and a few more when it
// interleaves. Entries say which core and which epoch they were made for.
struct CachedContext
{
    uint64_t instance = 0;
    uint64_t context = 0;
    uint64_t epoch = 0;
    void* state = nullptr;
};

constexpr size_t kContextCacheSize = 8;
thread_local CachedContext g_contextCache[kContextCacheSize];
} // namespace

DepthFinderCore::ContextState& DepthFinderCore::ContextLocked(uint64_t context)
{
    auto& state = _contexts[context];
    state.lastSeen = _presents;
    const uint64_t epoch = _epoch.load(std::memory_order_relaxed);

    // A stand-down forgets what a context knew of its viewport, as dropping its entry used to.
    if (state.epoch != epoch)
    {
        state.epoch = epoch;
        state.viewportWidth.store(0.0f, std::memory_order_relaxed);
        state.lastRealViewport.store(0.0f, std::memory_order_relaxed);
    }

    return state;
}

DepthFinderCore::ContextState& DepthFinderCore::ContextForDraw(uint64_t context)
{
    CachedContext& entry = g_contextCache[((context >> 4) ^ (context >> 9)) % kContextCacheSize];

    if (entry.instance == _instanceId && entry.context == context &&
        entry.epoch == _cacheEpoch.load(std::memory_order_acquire))
        return *static_cast<ContextState*>(entry.state);

    std::lock_guard lock(_mutex);

    ContextState& state = ContextLocked(context);
    entry.instance = _instanceId;
    entry.context = context;
    entry.epoch = _cacheEpoch.load(std::memory_order_relaxed);
    entry.state = &state;
    return state;
}

void DepthFinderCore::Bind(ContextState& context, Stats* stats)
{
    if (context.stats == stats)
        return;

    if (context.stats != nullptr)
    {
        auto& bound = context.stats->bound;
        const auto found = std::find(bound.begin(), bound.end(), &context);

        if (found != bound.end())
        {
            *found = bound.back();
            bound.pop_back();
        }

        UpdateStamping(*context.stats);
    }

    context.stats = stats;
    context.stampDraws.store(false, std::memory_order_relaxed);

    if (stats != nullptr)
    {
        stats->bound.push_back(&context);
        UpdateStamping(*stats);
    }
}

void DepthFinderCore::UpdateStamping(Stats& stats)
{
    const bool shared = stats.bound.size() > 1;

    for (ContextState* context : stats.bound)
        context->stampDraws.store(shared, std::memory_order_relaxed);
}

void DepthFinderCore::FoldBuffer(Stats& stats)
{
    for (ContextState* context : stats.bound)
        Fold(*context);
}

void DepthFinderCore::RetireIdleContexts()
{
    // Free what was retired long enough ago that no thread can still be inside a draw on it: enough presents and enough time.
    const auto now = std::chrono::steady_clock::now();

    std::erase_if(_retired,
                  [this, now](const RetiredContext& retired)
                  { return _presents - retired.present > kRetireGrace && now - retired.when >= kRetireGraceTime; });

    bool any = false;

    for (auto it = _contexts.begin(); it != _contexts.end();)
    {
        const ContextState& state = it->second;
        const uint64_t lastUsed = std::max(state.lastSeen, state.lastDrawPresent.load(std::memory_order_relaxed));

        if (state.stats != nullptr || _presents - lastUsed <= kRetireAfter)
        {
            ++it;
            continue;
        }

        _retiredDraws += state.drawcalls.load(std::memory_order_relaxed) - state.indirectCalls.load(std::memory_order_relaxed);
        _retiredIndirect += state.indirectEvents.load(std::memory_order_relaxed);

        auto next = std::next(it);
        _retired.push_back({ _presents, now, _contexts.extract(it) });
        it = next;
        any = true;
    }

    // Threads find their contexts through the map again, never through a cached pointer to a retired one.
    if (any)
        _cacheEpoch.fetch_add(1, std::memory_order_release);
}

void DepthFinderCore::Fold(ContextState& context)
{
    const uint64_t vertices = context.vertices.load(std::memory_order_relaxed);
    const uint64_t drawcalls = context.drawcalls.load(std::memory_order_relaxed);
    const uint64_t indirectCalls = context.indirectCalls.load(std::memory_order_relaxed);
    const uint64_t realDraws = context.realDraws.load(std::memory_order_relaxed);

    // Draws made while the context is bound to no buffer we know are not counted, as before.
    if (Stats* stats = context.stats)
    {
        const uint64_t addVertices = vertices - context.foldedVertices;
        const uint32_t addDrawcalls = (uint32_t) (drawcalls - context.foldedDrawcalls);
        const uint32_t addIndirect = (uint32_t) (indirectCalls - context.foldedIndirectCalls);

        for (DrawStats* s : { &stats->total, &stats->current })
        {
            s->vertices += addVertices;
            s->drawcalls += addDrawcalls;
            s->drawcallsIndirect += addIndirect;
        }

        // The viewport is the newest real draw's among every context that drew into the buffer, whichever folds first. A
        // context alone on the buffer stamped nothing; every other context that drew into it was folded when it left, so what
        // this one drew since is the newest.
        if (realDraws != context.foldedRealDraws)
        {
            const uint64_t stamp = context.stampDraws.load(std::memory_order_relaxed)
                                       ? context.lastRealDrawStamp.load(std::memory_order_relaxed)
                                       : __rdtsc();

            if (stamp >= stats->current.viewportStamp)
            {
                stats->current.lastViewportWidth = context.lastRealViewport.load(std::memory_order_relaxed);
                stats->current.viewportStamp = stamp;
            }
        }
    }

    context.foldedVertices = vertices;
    context.foldedDrawcalls = drawcalls;
    context.foldedIndirectCalls = indirectCalls;
    context.foldedRealDraws = realDraws;
}

void DepthFinderCore::Pending(const ContextState& context, DrawStats& pending) const
{
    pending.vertices = context.vertices.load(std::memory_order_relaxed) - context.foldedVertices;
    pending.drawcalls = (uint32_t) (context.drawcalls.load(std::memory_order_relaxed) - context.foldedDrawcalls);
    pending.drawcallsIndirect =
        (uint32_t) (context.indirectCalls.load(std::memory_order_relaxed) - context.foldedIndirectCalls);
}

void DepthFinderCore::CountEvents(uint64_t& draws, uint64_t& indirect) const
{
    std::lock_guard lock(_mutex);
    draws = _retiredDraws;
    indirect = _retiredIndirect;

    for (const auto& context : _contexts)
    {
        draws += context.second.drawcalls.load(std::memory_order_relaxed) -
                 context.second.indirectCalls.load(std::memory_order_relaxed);
        indirect += context.second.indirectEvents.load(std::memory_order_relaxed);
    }
}

// The draw path's one way to a context's counts: under the lock when the adapter's contexts are shared (one id written by
// several threads), through the thread's cache without it otherwise.
template <typename Count> void DepthFinderCore::WithContext(uint64_t context, Count&& count)
{
    if (_sharedContexts.load(std::memory_order_relaxed))
    {
        std::lock_guard lock(_mutex);
        count(ContextLocked(context));
    }
    else
    {
        count(ContextForDraw(context));
    }
}

void DepthFinderCore::OnDraw(uint64_t context, uint64_t vertices, uint32_t instances)
{
    // Acquire pairs with the release of whatever made the finder active: the hook then sees the settings made before it
    // (_sharedContexts). Free on x86.
    if (!_active.load(std::memory_order_acquire))
        return;

    WithContext(context,
                [this, vertices, instances](ContextState& state)
                {
                    Bump(state.vertices, vertices * instances);
                    Bump(state.drawcalls, 1);
                    state.lastDrawPresent.store(_presentsNow.load(std::memory_order_relaxed), std::memory_order_relaxed);

                    // A fullscreen rectangle (two triangles) does not update the viewport the last real draw used.
                    if (!(vertices == 6 && instances == 1))
                    {
                        Bump(state.realDraws, 1);
                        state.lastRealViewport.store(state.viewportWidth.load(std::memory_order_relaxed),
                                                     std::memory_order_relaxed);

                        if (state.stampDraws.load(std::memory_order_relaxed))
                            state.lastRealDrawStamp.store(__rdtsc(), std::memory_order_relaxed);
                    }
                });
}

void DepthFinderCore::OnIndirect(uint64_t context, uint32_t maxCount)
{
    if (!_active.load(std::memory_order_acquire))
        return;

    WithContext(context,
                [this, maxCount](ContextState& state)
                {
                    state.lastDrawPresent.store(_presentsNow.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    Bump(state.indirectEvents, 1);
                    Bump(state.drawcalls, maxCount);
                    Bump(state.indirectCalls, maxCount);
                    Bump(state.realDraws, 1);
                    state.lastRealViewport.store(state.viewportWidth.load(std::memory_order_relaxed),
                                                 std::memory_order_relaxed);

                    if (state.stampDraws.load(std::memory_order_relaxed))
                        state.lastRealDrawStamp.store(__rdtsc(), std::memory_order_relaxed);
                });
}

void DepthFinderCore::OnViewport(uint64_t context, float width)
{
    // Only the main viewport matters, as in ReShade's add-on.
    if (!_active.load(std::memory_order_acquire))
        return;

    WithContext(context, [width](ContextState& state) { state.viewportWidth.store(width, std::memory_order_relaxed); });
}

SnapshotRequest DepthFinderCore::OnDepthBound(uint64_t context, bool hadDepth, const DepthBuffer* bound)
{
    SnapshotRequest request;

    if (!_active.load(std::memory_order_relaxed))
        return request;

    std::lock_guard lock(_mutex);

    Stats* boundStats = nullptr;

    if (bound != nullptr && bound->id != 0)
    {
        auto& stats = _stats[bound->id];
        stats.width = bound->width;
        stats.height = bound->height;
        stats.format = bound->format;
        stats.resource = bound->id;
        stats.readOnlyDepth = bound->readOnlyDepth;
        boundStats = &stats;
    }

    // The picked buffer is often never cleared again in the frame it was drawn (Witcher 3 clears at the start of the pass), so
    // the clear never offers a snapshot. The moment the context moves off it is the other chance: the busiest stretch of the
    // frame is copied there, in the state the view says the buffer is in.
    ContextState& state = ContextLocked(context);
    Stats* previous = state.stats;

    // Every draw into the buffer it leaves, from any context, is in its counts before they are judged.
    if (previous != nullptr)
        FoldBuffer(*previous);
    else
        Fold(state);

    if (previous != nullptr && previous != boundStats && _snapshotsWanted.load(std::memory_order_relaxed) &&
        _pick.valid && previous->resource != 0 && _pick.id == previous->resource && previous->current.drawcalls != 0)
    {
        if (previous->current.vertices >= _snapshotFloor)
        {
            request.take = true;
            request.id = previous->resource;
            request.readOnlyDepth = previous->readOnlyDepth;
            request.stretchVertices = previous->current.vertices;
            request.where = "unbind";
        }

        previous->current = DrawStats {};
    }

    Bind(state, boundStats);

    _counters.binds.fetch_add(1, std::memory_order_relaxed);

    if (hadDepth)
    {
        _counters.bindsWithDepth.fetch_add(1, std::memory_order_relaxed);

        if (boundStats == nullptr)
            _counters.bindsUnknownDepth.fetch_add(1, std::memory_order_relaxed);
    }

    return request;
}

SnapshotRequest DepthFinderCore::OnDepthClear(uint64_t context, const DepthBuffer& buffer, float value)
{
    SnapshotRequest request;

    if (!_active.load(std::memory_order_relaxed) || buffer.id == 0)
        return request;

    std::lock_guard lock(_mutex);

    ContextLocked(context);

    auto& stats = _stats[buffer.id];

    // Every draw into the buffer so far, from any context bound to it, counts before the clear.
    FoldBuffer(stats);
    stats.width = buffer.width;
    stats.height = buffer.height;
    stats.format = buffer.format;

    // Reversed-Z games clear to 0.0 (or anything but 1.0). The first clear of a frame decides afresh; any later one in the same
    // frame can only add reversed, as before.
    if (!stats.clearedThisFrame)
        stats.reversed = value != 1.0f;
    else if (value != 1.0f)
        stats.reversed = true;

    stats.clearedThisFrame = true;

    // A clear with no work before it (the start of a frame) means nothing.
    if (stats.current.drawcalls == 0)
        return request;

    const DrawStats stretch = stats.current;
    stats.current = DrawStats {};

    // A clear after a render into a small viewport (a mirror, a portal) is not the scene; ReShade's rule.
    const bool real = stretch.lastViewportWidth > 1024.0f || stretch.lastViewportWidth == 0.0f || _pictureWidth <= 1024.0f;

    if (!real)
        return request;

    // The busiest stretch of the frame is the one to snapshot; ties go to the later one, so a scene drawn first into a shadow
    // map and then for real picks the real one.
    if (stretch.vertices >= _snapshotFloor)
    {
        stats.bestClear = (int32_t) stats.clears;

        // The overlay's copy: only of the buffer picked last frame, and only at the busiest stretch, so the copy that is left
        // at the end of the frame is the scene's.
        if (_snapshotsWanted.load(std::memory_order_relaxed) && _pick.valid && _pick.id == buffer.id)
        {
            request.take = true;
            request.id = buffer.id;
            request.readOnlyDepth = false; // a clear needs depth-write
            request.stretchVertices = stretch.vertices;
            request.where = "clear";
        }
    }

    ++stats.clears;
    return request;
}

// A context that ends while still bound to the picked buffer never unbinds it, so the stretch drawn since its last clear
// (Cyberpunk draws the world after the clear and ends the list there) is copied here.
SnapshotRequest DepthFinderCore::OnContextEnd(uint64_t context)
{
    SnapshotRequest request;

    if (!_active.load(std::memory_order_relaxed))
        return request;

    std::lock_guard lock(_mutex);

    const auto found = _contexts.find(context);

    if (found == _contexts.end())
        return request;

    found->second.lastSeen = _presents;

    if (found->second.stats == nullptr)
    {
        Fold(found->second);
        return request;
    }

    Stats* stats = found->second.stats;
    FoldBuffer(*stats);

    if (_snapshotsWanted.load(std::memory_order_relaxed) && _pick.valid && stats->resource != 0 &&
        _pick.id == stats->resource && stats->current.drawcalls != 0)
    {
        if (stats->current.vertices >= _snapshotFloor)
        {
            request.take = true;
            request.id = stats->resource;
            request.readOnlyDepth = stats->readOnlyDepth;
            request.stretchVertices = stats->current.vertices;
            request.where = "close";
        }

        stats->current = DrawStats {};
    }

    Bind(found->second, nullptr);
    return request;
}

SnapshotRequest DepthFinderCore::FlushForPresent(uint64_t context)
{
    SnapshotRequest request;

    if (!_active.load(std::memory_order_relaxed))
        return request;

    std::lock_guard lock(_mutex);

    const auto found = _contexts.find(context);

    if (found == _contexts.end())
        return request;

    found->second.lastSeen = _presents;

    if (found->second.stats == nullptr)
    {
        Fold(found->second);
        return request;
    }

    Stats* stats = found->second.stats;
    FoldBuffer(*stats);

    if (_snapshotsWanted.load(std::memory_order_relaxed) && _pick.valid && stats->resource != 0 &&
        _pick.id == stats->resource && stats->current.drawcalls != 0 && stats->current.vertices >= _snapshotFloor)
    {
        request.take = true;
        request.id = stats->resource;
        request.readOnlyDepth = stats->readOnlyDepth;
        request.stretchVertices = stats->current.vertices;
        request.where = "present";
        stats->current = DrawStats {};
    }

    return request;
}

uint64_t DepthFinderCore::BeginPresent(uint32_t pictureWidth, uint32_t pictureHeight)
{
    (void) pictureHeight;

    std::lock_guard lock(_mutex);

    _pictureWidth = (float) pictureWidth;

    // What every bound context drew since its last event counts in this frame (an unbound one's draws count nowhere, and are
    // dropped at its next bind).
    for (auto& stats : _stats)
        FoldBuffer(stats.second);

    // Lists are recorded in no fixed order, so "the busiest stretch so far" picked a different copy from frame to frame (the
    // world one, or the first-person weapon's) and the preview flickered. Every stretch that draws a fair share of what the
    // pick drew last frame is copied instead; the one that runs last on the GPU is left, the same every frame.
    _snapshotFloor = 0;

    if (_pick.valid)
    {
        const auto picked = _stats.find(_pick.id);

        if (picked != _stats.end())
            _snapshotFloor = picked->second.total.vertices * 2 / 100;
    }

    _frameCandidates.clear();
    _frameCandidates.reserve(_stats.size());

    for (auto it = _stats.begin(); it != _stats.end();)
    {
        auto& stats = it->second;

        if (stats.total.drawcalls == 0 && stats.clears == 0)
        {
            // Not touched this frame: forget it, and any context still pointing at it.
            for (ContextState* context : stats.bound)
                context->stats = nullptr;

            it = _stats.erase(it);
            continue;
        }

        GenericDepthSelect::Candidate c;
        c.id = it->first;
        c.width = stats.width;
        c.height = stats.height;
        c.format = stats.format;
        c.vertices = stats.total.vertices;
        c.drawcalls = stats.total.drawcalls;
        c.drawcallsIndirect = stats.total.drawcallsIndirect;
        c.clears = stats.clears;
        c.bestClear = stats.bestClear;
        c.reversed = stats.reversed;
        _frameCandidates.push_back(c);

        // The next frame starts from nothing; the bound pointer stays valid because the entry stays.
        stats.total = DrawStats {};
        stats.current = DrawStats {};
        stats.clears = 0;
        stats.bestClear = -1;
        stats.clearedThisFrame = false;
        ++it;
    }

    if (_presents % kLogEveryFrames == 0)
        RetireIdleContexts();

    ++_frames;
    return _presents;
}

bool DepthFinderCore::EndPresent(uint32_t pictureWidth, uint32_t pictureHeight, uint32_t warmupFrames)
{
    {
        std::lock_guard lock(_mutex);

        ++_presents;
        _presentsNow.store(_presents, std::memory_order_relaxed);

        const uint64_t last = _lastUpscalerCall.load(std::memory_order_relaxed);
        const bool calling = last != 0 && _presents - last < kQuietPresents;

        if (calling != _upscalerSeen.load())
        {
            _upscalerSeen = calling;

            if (calling)
                Say("Depth finder: the game is calling an upscaler; the finder stands down while it does");
            else
                Say(std::format("Depth finder: no upscaler call for {} presents; the finder is watching again",
                                kQuietPresents));
        }

        if (calling)
        {
            // Stand down: stop counting, drop the tracking and the pick, and start the warm-up over for when it wakes.
            _active = false;
            _armed = false;

            // Forget the tracking: the contexts stay (the draw path holds pointers to them) but are bound to nothing, drop
            // what they have drawn, and look their viewports up afresh.
            for (auto& context : _contexts)
            {
                context.second.stats = nullptr;
                Fold(context.second);
            }

            _epoch.fetch_add(1, std::memory_order_relaxed);
            _cacheEpoch.fetch_add(1, std::memory_order_release);
            _stats.clear();
            _pick = GenericDepthSelect::Pick {};
            _pickSeen = false;
            _selector.Reset();
            _warmupStart = _presents;
            return true;
        }

        // Counting restarts here; the draws of the frame in which it woke are not seen, which is fine.
        _active = true;

        // Counting has gone on through the warm-up, but nothing is picked or reported until it is over.
        if (_presents - _warmupStart <= warmupFrames)
        {
            _armed = false;
            return false;
        }

        _armed = true;
    }

    const auto pick = _selector.Update(_frameCandidates, pictureWidth, pictureHeight);

    const bool pickChanged = (pick.valid ? pick.id : 0) != _lastLoggedPick;
    const bool dueForLog = _frames - _lastLoggedFrame >= kLogEveryFrames || _lastLoggedFrame == 0;

    if (pickChanged || dueForLog)
    {
        LogCandidates(_frameCandidates, pick, pictureWidth, pictureHeight);
        _lastLoggedFrame = _frames;
        _lastLoggedPick = pick.valid ? pick.id : 0;
    }

    // A held pick can be of a buffer that drew nothing this frame: say whether it did.
    const bool seen = pick.valid && std::any_of(_frameCandidates.begin(), _frameCandidates.end(),
                                                [&pick](const auto& c) { return c.id == pick.id; });

    std::lock_guard lock(_mutex);
    _pick = pick;
    _pickSeen = seen;
    return false;
}

DepthFinderCore::Diagnostic DepthFinderCore::Diagnose(uint64_t context) const
{
    std::lock_guard lock(_mutex);
    Diagnostic d;
    d.wanted = _snapshotsWanted.load(std::memory_order_relaxed);

    const auto found = _contexts.find(context);

    if (found == _contexts.end())
        return d;

    d.hasContext = true;

    if (found->second.stats == nullptr)
        return d;

    d.hasBoundBuffer = true;
    d.boundResource = found->second.stats->resource;

    d.currentVertices = found->second.stats->current.vertices;
    d.currentDrawcalls = found->second.stats->current.drawcalls;

    for (const ContextState* context : found->second.stats->bound)
    {
        DrawStats pending;
        Pending(*context, pending);
        d.currentVertices += pending.vertices;
        d.currentDrawcalls += pending.drawcalls;
    }

    return d;
}

GenericDepthSelect::Pick DepthFinderCore::CurrentPick() const
{
    std::lock_guard lock(_mutex);
    return _pick;
}

bool DepthFinderCore::PickSeenThisFrame() const
{
    std::lock_guard lock(_mutex);
    return _pick.valid && _pickSeen;
}

uint64_t DepthFinderCore::WarmupStart() const
{
    std::lock_guard lock(_mutex);
    return _warmupStart;
}

uint64_t DepthFinderCore::SnapshotFloor() const
{
    std::lock_guard lock(_mutex);
    return _snapshotFloor;
}

void DepthFinderCore::LogCandidates(const std::vector<GenericDepthSelect::Candidate>& frame,
                                    const GenericDepthSelect::Pick& pick, uint32_t pictureWidth, uint32_t pictureHeight)
{
    auto sorted = frame;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return GenericDepthSelect::Score(a) > GenericDepthSelect::Score(b); });

    Say(std::format("Depth finder: frame {}, picture {}x{}, {} depth buffer(s) in use{}", _frames, pictureWidth,
                    pictureHeight, sorted.size(), pick.valid ? "" : ", none qualifies"));

    uint64_t draws = 0, indirect = 0;
    CountEvents(draws, indirect);

    const int bundleSame = _counters.bundleSameDraw.load();
    Say(std::format("Depth finder:   hooks so far: {} depth views created, {} binds of render targets ({} with a depth "
                    "buffer, {} of those unknown to us), {} draws, {} indirect, {} bundle executions (bundle draw code is "
                    "the direct list's: {}), {} dispatches",
                    _counters.depthViewsCreated.load(), _counters.binds.load(), _counters.bindsWithDepth.load(),
                    _counters.bindsUnknownDepth.load(), draws, indirect,
                    _counters.bundles.load(), bundleSame < 0 ? "unknown" : bundleSame ? "yes" : "no",
                    _counters.dispatches.load()));

    const size_t shown = std::min<size_t>(sorted.size(), 8);

    for (size_t i = 0; i < shown; ++i)
    {
        const auto& c = sorted[i];
        Say(std::format("Depth finder:   {}{:X}  {}x{}  format {}  {} vertices, {} draws ({} indirect), {} clear(s), best "
                        "clear {}{}",
                        pick.valid && pick.id == c.id ? "-> " : "   ", c.id, c.width, c.height, c.format, c.vertices,
                        c.drawcalls, c.drawcallsIndirect, c.clears, c.bestClear, c.reversed ? ", reversed-Z" : ""));
    }
}

} // namespace native
