#include "pch.h"

#include "DlssNr_ExposureAutoScan.h"
#include "DlssNr_ExposureAuto.h"
#include "DlssNr_ExposureMatch.h"
#include "DlssNr_ExposureScanRecord.h"
#include "DlssNr_GameDefaults.h"
#include "DlssNr_SuperiorScaling.h"

#include <shaders/dlssnr/DlssNr_ExposureAdapt.h>

#include <Config.h>
#include <State.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace DlssNr
{
namespace ExposureScan
{
namespace
{
using Core::Tracked;

constexpr unsigned int kMatchEveryRows = 50; // ~5 s at the recording's ~10 rows a second
constexpr size_t kMaxHistoryRows = 6000;     // ~10 min: a search gives up after 5

// A found value that reads invalid for a moment (a frame the readback missed, the match briefly gone) is held this long
// before the picture lets go of it: letting go and taking over again is two visible moves for one bad sample.
constexpr float kHoldSeconds = 0.5f;

// The matcher's run, on a thread of its own over a copy of the history. Detached, and owned jointly with that thread:
// nothing ever waits for it, so a match still running when the game exits cannot hold the exit up (a std::async
// future's destructor would block at static teardown, on a thread ExitProcess has already ended). The process exiting
// ends the thread; OptiScaler is not unloaded while the game runs.
struct MatchJob
{
    std::atomic<bool> done { false };
    DlssNrExposureMatch::Verdict verdict;
};

struct AutoState
{
    DlssNrExposureAuto::Lifecycle life;
    DlssNrExposureAuto::Phase said = DlssNrExposureAuto::Phase::Watching;    // what the log said last
    DlssNrExposureAuto::Phase entered = DlssNrExposureAuto::Phase::Watching; // what was let go of for
    DlssNrExposureAuto::PreExposureMotion pre;
    bool started = false;
    std::chrono::steady_clock::time_point start;
    unsigned long long frames = 0; // NR frames Auto has seen (the scan's own count stops while nothing is held)
    unsigned int adoptions = 0;    // likely candidates taken, ever (rank 2 and up)
    unsigned int adoptionsSeen = 0;
    unsigned int search = 0; // the search the history belongs to (Lifecycle::SearchNumber)
    unsigned int notD3D12 = 0; // NR frames in a row the game's presents were not D3D12 (kNotD3D12Frames)

    // One row per recording row (DlssNrExposureScanRecord::Due) with a meter reading: the meter, and each slot's value.
    // A slot's column belongs to the owner of generation owner[slot], from row first[slot] on.
    std::vector<float> meter;
    std::vector<std::vector<float>> values;
    std::vector<size_t> first;
    std::vector<uint32_t> owner;
    unsigned int rowsSinceMatch = 0;

    std::shared_ptr<MatchJob> matching;
    unsigned int matchingSearch = 0;
    std::vector<size_t> matchingSlots;    // the slot of each column the running match was given
    std::vector<uint32_t> matchingOwner;  // and its owner then

    // What was found: the slot, its owner, its units and Automatic's level against it
    int found = -1;
    uint32_t foundOwner = 0;
    DlssNrExposureMatch::Fit fit;
    float level = 0.0f;

    // The level from the best-measured find this session (the most rows), and its kind. A re-find of the same kind --
    // the game remade its exposure on a quality change -- is judged on a short history: Cyberpunk 2077 measured 1.5 on
    // 250 rows where its first find had 1.06 on 850, half a stop darker for having changed DLSS quality. Kept, the
    // brightness does not depend on when the exposure was found.
    float bestLevel = 0.0f;
    size_t bestLevelRows = 0;
    DlssNrExposureMatch::Kind bestLevelKind = DlssNrExposureMatch::Kind::Unsure;

    // The found exposure as the picture uses it, worked out once per NR frame (FoundExposure reads it), with the
    // pre-exposure of the frame it goes with. A match that moves with our meter's unadapted reading (lag 1 row or less,
    // or leading it) is the game's per-frame target, before its own eye adaptation: Cyberpunk 2077's jumps up to ~1 EV between frames when
    // the camera pans and stops, a flash if used as it is. That one is eased as Automatic's own reading is
    // (DlssNr_ExposureAdapt.h, the same seconds); a match that trails the meter is the game's eased value already and is
    // used as it is.
    float now = 0.0f;
    float nowPre = 1.0f;
    std::chrono::steady_clock::time_point nowAt;
    std::chrono::steady_clock::time_point validAt;
};

AutoState g_auto;

const char* PhaseName(DlssNrExposureAuto::Phase p)
{
    using DlssNrExposureAuto::Phase;

    switch (p)
    {
    case Phase::Watching:
        return "watching";
    case Phase::Done:
        return "not needed: the game hands its exposure over";
    case Phase::Searching:
        return "searching";
    case Phase::Found:
        return "found the game's exposure";
    case Phase::Baked:
        return "the picture is already exposed";
    case Phase::GaveUp:
        return "found nothing, Automatic";
    case Phase::NotHere:
        return "D3D12 games only for now, Automatic";
    }

    return "";
}

// Whether the game renders with D3D12: the scan watches D3D12 resource creation, so only there can it find the game's
// exposure. NR runs on D3D12 for D3D11 and Vulkan games too (the bridges, the native path's private device), where
// `api` reads D3D12 (VirtualUpscalerDriver sets it): what the game presents with tells them apart. GTA V (D3D11, native
// NR) searched OptiScaler's own device (2026-10-21).
bool GameRendersD3D12()
{
    const State& state = State::Instance();

    if (state.swapchainInteropApi != SwapchainInteropApi::None || state.swapchainApi == API::DX11 ||
        state.swapchainApi == API::Vulkan)
        return false;

    return state.api == API::DX12 || state.api == API::NotSelected;
}

// NR frames in a row that GameRendersD3D12 must say no before Auto stands down (NotHere lets go of every candidate, and
// a D3D12 game makes most of them at load). `swapchainApi` is whatever presented last: a D3D12 game that also presents
// a D3D11 swapchain (a launcher, an intro video, D3D11On12) reads D3D11 on those presents, and alternating presents
// keep resetting the count. ~2 s at 60 fps.
constexpr unsigned int kNotD3D12Frames = 120;

void ClearHistoryLocked()
{
    g_auto.meter.clear();
    g_auto.values.clear();
    g_auto.first.clear();
    g_auto.owner.clear();
    g_auto.rowsSinceMatch = 0;
    g_auto.found = -1;
}

void AppendRowLocked(float meter)
{
    // A row without a meter reading says nothing to the matcher (Automatic not running this frame)
    if (g_auto.meter.size() >= kMaxHistoryRows || !(meter > 0.0f) || !std::isfinite(meter))
        return;

    const std::vector<Tracked>& tracked = Core::ListLocked();
    const size_t row = g_auto.meter.size();
    g_auto.meter.push_back(meter);

    const size_t slots = tracked.size();
    g_auto.values.resize(slots);
    g_auto.first.resize(slots, row);
    g_auto.owner.resize(slots, 0);

    for (size_t i = 0; i < slots; ++i)
    {
        const Tracked& t = tracked[i];
        std::vector<float>& column = g_auto.values[i];

        // A new owner starts its own history here
        if (g_auto.owner[i] != t.generation)
        {
            g_auto.owner[i] = t.generation;
            g_auto.first[i] = row;
        }

        column.resize(row, 0.0f);
        column.push_back(t.alive && std::isfinite(t.latest) ? t.latest : 0.0f);
    }

    g_auto.rowsSinceMatch++;
}

// The match started some rows ago, when done: Found (with the slot still held by the owner it judged), Baked, None or
// not sure
DlssNrExposureAuto::Verdict CollectMatchLocked()
{
    using DlssNrExposureAuto::Verdict;

    if (!g_auto.matching || !g_auto.matching->done.load(std::memory_order_acquire))
        return Verdict::NotSure;

    const DlssNrExposureMatch::Verdict v = g_auto.matching->verdict;
    g_auto.matching.reset();

    if (g_auto.matchingSearch != g_auto.search)
        return Verdict::NotSure;

    switch (v.what)
    {
    case DlssNrExposureMatch::What::Found:
    {
        const std::vector<Tracked>& tracked = Core::ListLocked();

        if (v.index < 0 || (size_t) v.index >= g_auto.matchingSlots.size())
            return Verdict::NotSure;

        const size_t slot = g_auto.matchingSlots[(size_t) v.index];

        if (slot >= tracked.size() || slot >= g_auto.values.size() || !tracked[slot].alive ||
            tracked[slot].generation != g_auto.matchingOwner[(size_t) v.index])
            return Verdict::NotSure;

        // Automatic's level against the found value, over the rows the match judged
        const size_t from = std::min(g_auto.first[slot], g_auto.meter.size());
        const std::vector<float> meter(g_auto.meter.begin() + from, g_auto.meter.end());
        const std::vector<float> value(g_auto.values[slot].begin() + std::min(from, g_auto.values[slot].size()),
                                       g_auto.values[slot].end());
        const float level = DlssNrExposureMatch::AnchorScale(v.fit, meter, value);

        if (!(level > 0.0f) || !std::isfinite(level))
            return Verdict::NotSure;

        g_auto.found = (int) slot;
        g_auto.foundOwner = tracked[slot].generation;
        g_auto.fit = v.fit;
        g_auto.level = level;

        const bool sameKind = g_auto.bestLevel > 0.0f && g_auto.bestLevelKind == v.fit.kind;

        if (!sameKind || v.rows >= g_auto.bestLevelRows)
        {
            g_auto.bestLevel = level;
            g_auto.bestLevelRows = v.rows;
            g_auto.bestLevelKind = v.fit.kind;
        }
        else
        {
            LOG_INFO("DLSS-NR exposure scan (Auto): found again on {} rows (level {:.4g}); keeping the level measured on "
                     "{} rows: {:.4g}",
                     v.rows, level, g_auto.bestLevelRows, g_auto.bestLevel);
            g_auto.level = g_auto.bestLevel;
        }

        static const char* kKinds[] = { "unsure",    "steady",     "not a float", "a timer",    "noisy",
                                        "unrelated", "multiplier", "divisor",     "stop value", "scene luminance" };
        LOG_INFO("DLSS-NR exposure scan (Auto): candidate {} ({}{}) is the game's exposure: a {}, corr {:.3f}, slope "
                 "{:.3f}, lag {} rows, over {} rows and {:.1f} EV; Automatic's level against it {:.4g}",
                 slot + 1, tracked[slot].shape, Core::ApiSuffix(tracked[slot].api), kKinds[(int) v.fit.kind],
                 v.fit.corr, v.fit.slope, v.fit.lag, v.rows, v.meterTravelEv, level);
        return Verdict::Found;
    }
    case DlssNrExposureMatch::What::Baked:
        return Verdict::Baked;
    case DlssNrExposureMatch::What::None:
        return Verdict::None;
    default:
        return Verdict::NotSure;
    }
}

void StartMatchLocked()
{
    if (g_auto.matching || g_auto.rowsSinceMatch < kMatchEveryRows || g_auto.meter.empty())
        return;

    // Only the columns of candidates held now, by the owner the history has: the rest cannot be found
    const std::vector<Tracked>& tracked = Core::ListLocked();
    std::vector<std::vector<float>> values;
    std::vector<size_t> first;
    std::vector<size_t> slots;
    std::vector<uint32_t> owners;

    for (size_t i = 0; i < g_auto.values.size() && i < tracked.size(); ++i)
    {
        if (!tracked[i].alive || tracked[i].generation != g_auto.owner[i])
            continue;

        values.push_back(g_auto.values[i]);
        values.back().resize(g_auto.meter.size(), 0.0f);
        first.push_back(g_auto.first[i]);
        slots.push_back(i);
        owners.push_back(g_auto.owner[i]);
    }

    g_auto.rowsSinceMatch = 0;

    if (values.empty())
        return;

    auto job = std::make_shared<MatchJob>();

    try
    {
        std::thread(
            [job, meter = g_auto.meter, values = std::move(values), first = std::move(first)]
            {
                job->verdict = DlssNrExposureMatch::MatchEach(meter, values, first);
                job->done.store(true, std::memory_order_release);
            })
            .detach();
    }
    catch (const std::system_error&)
    {
        return; // no thread to be had: the next row tries again
    }

    g_auto.matching = std::move(job);
    g_auto.matchingSearch = g_auto.search;
    g_auto.matchingSlots = std::move(slots);
    g_auto.matchingOwner = std::move(owners);
}

bool MatchAliveLocked()
{
    const std::vector<Tracked>& tracked = Core::ListLocked();
    return g_auto.found >= 0 && (size_t) g_auto.found < tracked.size() && tracked[g_auto.found].alive &&
           tracked[g_auto.found].generation == g_auto.foundOwner;
}

// On entering a phase: let go of every candidate, or of all but the match. Candidates taken after that (in Found or
// GaveUp) are held for the next search without being copied (CopiesLocked).
void LetGoOnEntryLocked()
{
    const DlssNrExposureAuto::Phase now = g_auto.life.Now();

    if (now == g_auto.entered)
        return;

    g_auto.entered = now;

    if (!g_auto.life.LetsGo() && !g_auto.life.CopiesMatchOnly())
        return;

    const std::vector<Tracked>& tracked = Core::ListLocked();

    for (size_t i = 0; i < tracked.size(); ++i)
    {
        if (tracked[i].alive && !(g_auto.life.CopiesMatchOnly() && (int) i == g_auto.found))
            Core::LetGoLocked(i);
    }
}

// The found exposure for this frame (AutoState::now)
void FoundNowLocked(const FrameExposure& frame, std::chrono::steady_clock::time_point now)
{
    float target = 0.0f;

    if (g_auto.life.Now() == DlssNrExposureAuto::Phase::Found && MatchAliveLocked())
        target = g_auto.level *
                 DlssNrExposureMatch::AsExposure(g_auto.fit, Core::ListLocked()[(size_t) g_auto.found].latest);

    if (!DlssNrExposureAdapt::Valid(target))
    {
        // One bad sample while still found: hold the last value a moment
        const bool hold = g_auto.life.Now() == DlssNrExposureAuto::Phase::Found && g_auto.now > 0.0f &&
                          std::chrono::duration<float>(now - g_auto.validAt).count() < kHoldSeconds;

        if (!hold)
            g_auto.now = 0.0f;
    }
    else if (g_auto.fit.lag > 1)
    {
        g_auto.now = target;
        g_auto.validAt = now;
    }
    else
    {
        const Config& cfg = *Config::Instance();
        const float dt = g_auto.now > 0.0f ? std::chrono::duration<float>(now - g_auto.nowAt).count() : 0.0f;
        const float brighter = DlssNrExposureAdapt::Blend(
            dt, DlssNrExposureAdapt::Seconds(cfg.DlssNrAutoExposureAdaptBrighterSeconds.value_or_default(),
                                             DlssNrExposureAdapt::kDefaultBrighterSeconds));
        const float darker = DlssNrExposureAdapt::Blend(
            dt, DlssNrExposureAdapt::Seconds(cfg.DlssNrAutoExposureAdaptDarkerSeconds.value_or_default()));
        g_auto.now = DlssNrExposureAdapt::Ease(g_auto.now, target, brighter, darker, g_auto.now <= 0.0f);
        g_auto.validAt = now;
    }

    if (g_auto.now > 0.0f && DlssNrExposureAdapt::Valid(target))
        g_auto.nowPre = frame.pre > 0.0f && std::isfinite(frame.pre) ? frame.pre : 1.0f;

    g_auto.nowAt = now;
}
} // namespace

namespace AutoScan
{
bool On()
{
    const auto& cfg = *Config::Instance();
    return !DlssNr::SuperiorScaling::Enabled(cfg) &&
           cfg.DlssNrWhitePointSource.value_or_default() == DlssNrExposureDefault::kSourceAuto;
}

void FrameLocked(const FrameExposure& frame)
{
    using namespace DlssNrExposureAuto;

    if (!On())
        return;

    const auto now = std::chrono::steady_clock::now();

    if (!g_auto.started)
    {
        g_auto.started = true;
        g_auto.start = now;
    }

    g_auto.pre.Note(frame.pre);

    if (g_auto.life.CopiesAll() && DlssNrExposureScanRecord::Due(g_auto.frames))
        AppendRowLocked(frame.meter);

    g_auto.frames++;

    // What the game hands over, as Game exposure judges it: a texture counts once it has read clean, and while it has
    // not yet (nor been given up on) the scan keeps what it holds
    const DlssNrExposureDefault::Offer& offer = DlssNr::GameExposureOffer();
    Frame f;
    f.seconds = std::chrono::duration<float>(now - g_auto.start).count();
    f.handed = offer.Confirmed();
    f.handedPending = offer.Pending();
    f.preExposure = g_auto.pre.Moving();
    g_auto.notD3D12 = GameRendersD3D12() ? 0 : std::min(g_auto.notD3D12 + 1, kNotD3D12Frames);
    f.supported = g_auto.notD3D12 < kNotD3D12Frames;
    f.verdict = CollectMatchLocked();
    f.matchAlive = MatchAliveLocked();
    f.newCandidates = g_auto.adoptions != g_auto.adoptionsSeen;
    g_auto.adoptionsSeen = g_auto.adoptions;

    g_auto.life.OnFrame(f);

    if (g_auto.life.SearchNumber() != g_auto.search)
    {
        g_auto.search = g_auto.life.SearchNumber();
        ClearHistoryLocked();
    }

    if (g_auto.life.Now() == Phase::Searching)
        StartMatchLocked();

    LetGoOnEntryLocked();
    FoundNowLocked(frame, now);

    if (g_auto.life.Now() != g_auto.said)
    {
        g_auto.said = g_auto.life.Now();

        if (g_auto.said == Phase::GaveUp)
            LOG_INFO("DLSS-NR exposure scan (Auto): {}; candidates the game makes from here start a new search after "
                     "{:.0f} s",
                     PhaseName(g_auto.said), g_auto.life.RetrySeconds());
        else if (g_auto.said == Phase::Done && f.preExposure && !f.handed)
            LOG_INFO("DLSS-NR exposure scan (Auto): not needed: the game's pre-exposure moves with the light");
        else
            LOG_INFO("DLSS-NR exposure scan (Auto): {}", PhaseName(g_auto.said));
    }
}

bool ClosedLocked(int rank) { return On() && (g_auto.life.Closed() || (rank < 2 && !g_auto.life.CopiesAll())); }

void AdoptedLocked(int rank)
{
    // A bigger buffer (rank 1) is not worth a new search on its own: Starfield-like games keep making them
    if (rank >= 2)
        g_auto.adoptions++;
}

bool CopiesLocked(size_t slot)
{
    if (!On())
        return true;

    if (g_auto.life.CopiesAll())
        return true;

    return g_auto.life.CopiesMatchOnly() && (int) slot == g_auto.found;
}

bool SearchingLocked() { return !On() || g_auto.life.CopiesAll(); }
} // namespace AutoScan

std::string AutoStatus()
{
    if (!AutoScan::On())
        return {};

    std::lock_guard<std::mutex> lock(Core::Mutex());

    // Before NR's first frame in a game that is not D3D12, say so rather than "watching"; from then on the lifecycle
    // decides (kNotD3D12Frames)
    if (g_auto.frames == 0 && !GameRendersD3D12())
        return PhaseName(DlssNrExposureAuto::Phase::NotHere);

    // Done for a pre-exposure that moves and no texture handed over: Automatic stays in force (as the log says)
    if (g_auto.life.Now() == DlssNrExposureAuto::Phase::Done && g_auto.pre.Moving() &&
        !DlssNr::GameExposureOffer().Confirmed())
        return "not needed: the game's pre-exposure moves with the light, Automatic";

    std::string line = PhaseName(g_auto.life.Now());
    const std::vector<Tracked>& tracked = Core::ListLocked();

    if (g_auto.life.Now() == DlssNrExposureAuto::Phase::Found && g_auto.found >= 0 &&
        (size_t) g_auto.found < tracked.size())
        line += " (" + tracked[g_auto.found].shape + ")";

    // Searching with nothing held does not move on by itself: say what it waits for
    if (g_auto.life.Now() == DlssNrExposureAuto::Phase::Searching &&
        std::none_of(tracked.begin(), tracked.end(), [](const Tracked& t) { return t.alive; }))
        line += ": nothing to search yet, the game makes its buffers at a loading screen";
    else if (g_auto.life.Now() == DlssNrExposureAuto::Phase::Searching && Core::WaitingLocked() > 0)
        line += " (" + std::to_string(Core::WaitingLocked()) + " more waiting their turn)";

    return line;
}

bool FoundExposure(float* multiplier, float* preExposure)
{
    if (multiplier == nullptr || !AutoScan::On())
        return false;

    std::lock_guard<std::mutex> lock(Core::Mutex());

    if (g_auto.life.Now() != DlssNrExposureAuto::Phase::Found || !MatchAliveLocked() ||
        !DlssNrExposureAdapt::Valid(g_auto.now))
        return false;

    *multiplier = g_auto.now;

    if (preExposure != nullptr)
        *preExposure = g_auto.nowPre;

    return true;
}
} // namespace ExposureScan
} // namespace DlssNr
