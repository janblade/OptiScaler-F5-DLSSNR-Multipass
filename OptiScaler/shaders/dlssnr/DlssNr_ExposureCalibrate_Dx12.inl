// "Tune for this scene", D3D12 side (DlssNr_ExposureCalibrate.h has the sweep, the availability rules and the why).
// Included by DlssNr_Dx12.cpp inside its anonymous namespace, right after g_frames.
//
// Nothing here costs anything until it is wanted: every resource is created when a run starts and parked for release
// when it ends. While no run is on and the menu is not looking, an evaluation only stamps the time
// (CalibrationIdleFrame); availability is worked out only while the menu polls ExposureCalibration() or a run is on.
//
// Per evaluation while it is wanted:
//   CalibrationBeginFrame   NR-stopped check, the menu's start, readbacks that are due, and the sweep's EV for this
//                           evaluation. The white point is pinned: the base white point frozen at the start times the
//                           step's Trim (CalibrationWhitePoint), used by the encode and resolve instead of the live
//                           exposure, so Automatic, Follow-game and Game exposure all hold still for the run; a
//                           drifting base aborts it.
//   CalibrationCopyInput    after the encode: the untouched frame into this evaluation's input copy
//   CalibrationMeasure      after the resolve: the edited frame into this evaluation's output copy, and on a measured
//                           evaluation the stats pass over output, previous output, input, previous input and the
//                           picture the model was shown
// Input and output are each kept twice, alternating, so the previous evaluation's copy is still there to compare with.
// The input has to be copied at the encode: when the game's colour is not UAV-capable the resolve writes into the
// encode's untouched copy.

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to);
ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, unsigned int height);
void ParkNrResource(ID3D12Resource*& res);

namespace Cal = DlssNrExposureCalibrate;

// The stats grid: 64x64 tiles, two RGBA32F texels each, side by side (Cal::ReduceGrid has the layout). 128 * 16 = 2048
// bytes a row, a multiple of the 256 a texture-to-buffer copy needs, so the readback is a flat array.
constexpr unsigned int kCalGridWidth = Cal::kGridTiles * 2;
constexpr unsigned int kCalGridHeight = Cal::kGridTiles;
constexpr unsigned int kCalRowBytes = Cal::kGridRowFloats * sizeof(float);
constexpr unsigned int kCalGridBytes = kCalRowBytes * kCalGridHeight;
static_assert(kCalRowBytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);
// Readbacks are read this many evaluations after they were recorded, so the GPU is certainly done with them. Frame
// generation keeps the GPU further behind than the 8 the frame statistics use (the retirement list waits 32). The ring
// holds every measured evaluation of that window: at most 4 measured per 12 (8 settle + 4), so 8 in any 16.
constexpr unsigned int kCalReadDelay = 16;
constexpr unsigned int kCalRing = 12;
// No evaluation for this long: NR stopped. A run is abandoned and a pending start dropped.
constexpr unsigned long long kCalStallMs = 2000;
// The menu counts as looking for this long after it last polled; availability is only worked out meanwhile.
constexpr unsigned long long kCalMenuMs = 1000;
// Proxy thresholds for damage: its peak channel above the shoulder, or below the floor.
constexpr float kCalShoulder = 0.95f;
constexpr float kCalFloor = 0.02f;

struct CalibrationState
{
    // Menu thread and render thread, under the mutex.
    std::mutex mutex;
    Cal::Sweep sweep;
    bool startRequested = false;
    uint32_t source = 3;                          // the panel a run was started from: 3 Automatic, 1 Game exposure
    Cal::Blocker blocker = Cal::Blocker::NrStopped; // why a run cannot start or go on, as of the last wanted evaluation
    const char* startError = "";                  // why the last start did not happen, "" if it did

    // Either thread, lock-free: read every evaluation.
    std::atomic<bool> active { false };                  // a start is pending, a run is on, or resources are held
    std::atomic<unsigned long long> lastEvaluationMs { 0 };
    std::atomic<unsigned long long> menuPolledMs { 0 };

    // Render thread only.
    Cal::Frame frame {};
    float frameWhitePoint = 0.0f; // the pinned white point of this evaluation, 0 when nothing runs
    bool measurePending = false;  // this evaluation should be measured and has not been yet
    float measureWhitePoint = 1.0f;
    bool logged = true;
    unsigned int current = 0;
    ID3D12Resource* input[2] = {};
    ID3D12Resource* output[2] = {};
    bool inputReadable[2] = {};  // NON_PIXEL_SHADER_RESOURCE, else COPY_DEST
    bool outputReadable[2] = {};
    ID3D12Resource* grid = nullptr;
    ID3D12Resource* readback[kCalRing] = {};
    bool slotBusy[kCalRing] = {};
    Cal::Ticket slotTicket[kCalRing] = {};
    unsigned long long slotDue[kCalRing] = {};
};

CalibrationState g_cal;

ID3D12Resource* CreateCopyOf(ID3D12Device* device, ID3D12Resource* like)
{
    D3D12_RESOURCE_DESC desc = like->GetDesc();
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.Alignment = 0;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* res = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&res))))
        return nullptr;
    return res;
}

void CalibrationReleaseResources()
{
    for (unsigned int i = 0; i < 2; ++i)
    {
        ParkNrResource(g_cal.input[i]);
        ParkNrResource(g_cal.output[i]);
        g_cal.inputReadable[i] = g_cal.outputReadable[i] = false;
    }

    ParkNrResource(g_cal.grid);

    for (unsigned int i = 0; i < kCalRing; ++i)
    {
        ParkNrResource(g_cal.readback[i]);
        g_cal.slotBusy[i] = false;
    }
}

bool CalibrationCreateResources(ID3D12Device* device)
{
    g_cal.grid = CreateScratch(device, DXGI_FORMAT_R32G32B32A32_FLOAT, kCalGridWidth, kCalGridHeight);

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kCalGridBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    bool ok = g_cal.grid != nullptr;

    for (unsigned int i = 0; ok && i < kCalRing; ++i)
        ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&g_cal.readback[i])));

    if (!ok)
        CalibrationReleaseResources();

    return ok;
}

// A readback slot -> one Stats (Cal::ReduceGrid). An unmappable slot is an empty sample, which the sweep drops.
Cal::Stats CalibrationReduce(ID3D12Resource* readback)
{
    void* mapped = nullptr;
    D3D12_RANGE range { 0, kCalGridBytes };

    if (FAILED(readback->Map(0, &range, &mapped)) || mapped == nullptr)
    {
        Cal::Stats empty {};
        empty.detailBand = NAN;
        return empty;
    }

    const Cal::Stats s = Cal::ReduceGrid(static_cast<const float*>(mapped));
    D3D12_RANGE none { 0, 0 };
    readback->Unmap(0, &none);
    return s;
}

std::string CalibrationStopText(const Cal::Sweep& s)
{
    std::string text = Cal::AbortText(s.AbortReason());

    if (s.AbortReason() == Cal::Abort::Unavailable)
        text = text + ": " + Cal::BlockerText(s.StopBlocker());

    return text;
}

void CalibrationLogResult()
{
    const Cal::Sweep& s = g_cal.sweep;
    const auto& steps = s.Steps();
    const float neutral = s.Config().neutralTrim;

    if (s.AbortReason() != Cal::Abort::None)
    {
        LOG_INFO("DLSS-NR calibrate: stopped after {} of {} steps: {}",
                 std::count_if(steps.begin(), steps.end(), [](const Cal::StepResult& r) { return r.samples > 0; }),
                 steps.size(), CalibrationStopText(s));
    }

    for (size_t i = 0; i < steps.size(); ++i)
    {
        const Cal::StepResult& r = steps[i];

        if (r.samples == 0)
            continue;

        LOG_INFO("DLSS-NR calibrate: {:+.1f} EV (trim {:.3f}) n {} | raw {:.5f} band {:.5f} (out {:.5f} in {:.5f}) | "
                 "flicker {:.5f} (out {:.5f} in {:.5f}) | shoulder {:.4f} floor {:.4f} | score raw {:.3f} band {:.3f}",
                 Cal::Tidy(r.ev), Cal::TrimForEv(r.ev, neutral), r.samples, r.detailRaw,
                 r.DetailOf(Cal::Detail::BandPass), r.detailBand, r.inputBand, r.Flicker(), r.outputChange,
                 r.inputChange, r.shoulder, r.floor, s.Score(i, Cal::Detail::Raw), s.Score(i, Cal::Detail::BandPass));
    }

    if (s.Finished())
        LOG_INFO("DLSS-NR calibrate: current {:+.2f} EV, best raw {:+.1f} EV, best band {:+.1f} EV, result {:+.2f} EV{}",
                 Cal::Tidy(s.CurrentEv()), Cal::Tidy(s.BestEv(Cal::Detail::Raw)),
                 Cal::Tidy(s.BestEv(Cal::Detail::BandPass)), Cal::Tidy(s.ResultEv()),
                 s.Unsure()   ? " (unsure: detail varied less than flicker, keeps the current value)"
                 : s.AtEdge() ? " (at the edge of the range: the real best may lie beyond, keeps the current value)"
                 : s.Changed() ? "" : " (flat: keeps the current value)");
}

// The pinned white point of this evaluation, or 0 when no run is on.
float CalibrationWhitePoint() { return g_cal.frameWhitePoint; }

// Whether this evaluation needs CalibrationBeginFrame: a start is pending, a run is on, resources are held, or the
// menu is looking (it shows availability). Lock-free; everything else is CalibrationIdleFrame.
bool CalibrationWanted()
{
    return g_cal.active.load(std::memory_order_acquire) ||
           GetTickCount64() - g_cal.menuPolledMs.load(std::memory_order_relaxed) < kCalMenuMs;
}

// An evaluation while nothing is wanted: only the clock the NR-stopped check reads.
void CalibrationIdleFrame() { g_cal.lastEvaluationMs.store(GetTickCount64(), std::memory_order_relaxed); }

// Everything availability depends on, from this evaluation. Only called while wanted (it parses the Trim anchors).
Cal::Situation CalibrationSituation(const Config& cfg, bool usingAutoExposure, bool isHdrBuffer, bool finishedPicture,
                                    bool gameExposureNow)
{
    Cal::Situation s;
    s.source = cfg.DlssNrWhitePointSource.value_or_default();
    s.proxyMode = cfg.DlssNrUseProxy.value_or_default();
    s.holdFrame = cfg.DlssNrHoldFrame.value_or_default();
    s.finishedPicture = finishedPicture;
    s.hdr = isHdrBuffer;
    s.autoRunning = usingAutoExposure;
    s.followOn = DlssNr::FollowGameOn(cfg);
    s.following = g_nr.followingGame;
    s.followDisagreementEv = FollowDisagreementEv();
    s.gameExposureNow = gameExposureNow;
    s.gameExposureReading = g_nr.gameExposure > 1e-6f;

    if (s.source == 1)
        s.anchors = !DlssNrTrim::Parse(cfg.DlssNrGameExposureTrimAnchors.value_or_default()).empty() ||
                    cfg.DlssNrGameExposureTrimPreview.value_or_default();
    else
        s.anchors = !DlssNrTrim::Parse(cfg.DlssNrAutoExposureTrimAnchors.value_or_default()).empty() ||
                    cfg.DlssNrAutoExposureTrimPreview.value_or_default();

    return s;
}

// The base white point the tuned source's Trim multiplies, 0 without a reading: Game exposure's is the game's own
// (PreExposure / exposure), Automatic's is AutoBaseWhitePoint.
float CalibrationBase(const Config& cfg)
{
    if (cfg.DlssNrWhitePointSource.value_or_default() == 1)
        return g_nr.gameExposure > 1e-6f ? g_nr.gamePreExposure / g_nr.gameExposure : 0.0f;

    return AutoBaseWhitePoint();
}

// Before the white point is resolved, while CalibrationWanted().
void CalibrationBeginFrame(const Config& cfg, ID3D12Device* device, unsigned int width, unsigned int height,
                           const Cal::Situation& situation, float baseWhitePoint)
{
    std::lock_guard<std::mutex> lock(g_cal.mutex);
    const unsigned long long now = GetTickCount64();
    const unsigned long long last = g_cal.lastEvaluationMs.exchange(now, std::memory_order_relaxed);

    // NR stopped for a while (a loading screen, NR toggled off) with the menu closed: what the run measured before is
    // of another moment, and a start asked for back then is not wanted now.
    if (last != 0 && now - last > kCalStallMs)
    {
        g_cal.sweep.Abandon(Cal::Abort::NrOff);
        g_cal.startRequested = false;
    }

    g_cal.blocker = Cal::Availability(situation);

    // A measured evaluation that never reached the stats pass (a failed evaluate, another path): its ticket is
    // returned empty, so the sweep does not wait for it.
    if (g_cal.measurePending)
    {
        g_cal.measurePending = false;
        Cal::Stats dropped {};
        dropped.detailBand = NAN;
        g_cal.sweep.AddStats(g_cal.frame.ticket, dropped);
    }

    for (unsigned int i = 0; i < kCalRing; ++i)
    {
        if (g_cal.slotBusy[i] && g_frames >= g_cal.slotDue[i])
        {
            g_cal.slotBusy[i] = false;
            g_cal.sweep.AddStats(g_cal.slotTicket[i], CalibrationReduce(g_cal.readback[i]));
        }
    }

    const Cal::Context ctx { width, height, situation.source, true, baseWhitePoint, g_cal.blocker };

    if (g_cal.startRequested && !g_cal.sweep.Running())
    {
        g_cal.startRequested = false;
        g_cal.startError = "";

        if (g_cal.blocker != Cal::Blocker::None)
        {
            g_cal.startError = Cal::BlockerText(g_cal.blocker);
        }
        else if (!(baseWhitePoint > 0.0f))
        {
            g_cal.startError = "the exposure has no reading yet";
        }
        else if (g_cal.grid == nullptr && !CalibrationCreateResources(device))
        {
            g_cal.startError = "could not allocate the calibration buffers";
        }
        else
        {
            // Each source tunes its own slider: Automatic around its 5x neutral, Game exposure around 1x.
            g_cal.source = situation.source;
            if (situation.source == 1)
                g_cal.sweep.Start(Cal::EvForTrim(cfg.DlssNrWhitePointTrim.value_or_default(), Cal::kGameExposureNeutralTrim),
                                  Cal::GameExposureSettings(), ctx);
            else
                g_cal.sweep.Start(Cal::EvForTrim(DlssNr::AutoTrimEffective(cfg)), Cal::Settings {}, ctx);
            // One scale for every step and every run, whatever the slider was at (Sweep::MeasureWhitePoint).
            g_cal.measureWhitePoint = g_cal.sweep.MeasureWhitePoint();
            g_cal.logged = false;
            LOG_INFO("DLSS-NR calibrate: started at {:+.2f} EV, {} steps, measuring at white point {:.4g}",
                     Cal::Tidy(g_cal.sweep.CurrentEv()), g_cal.sweep.StepCount(), g_cal.measureWhitePoint);
        }
    }

    g_cal.frame = g_cal.sweep.NextFrame(ctx);
    g_cal.measurePending = g_cal.frame.measure;
    g_cal.frameWhitePoint = g_cal.frame.override ? g_cal.sweep.WhitePointFor(g_cal.frame.ev) : 0.0f;

    if (g_cal.frame.override)
        g_cal.current ^= 1u;

    if (!g_cal.sweep.Running() && !g_cal.logged)
    {
        g_cal.logged = true;
        CalibrationLogResult();
    }

    // Everything back once the run is over and nothing is still in flight to a readback.
    const bool inFlight = std::any_of(std::begin(g_cal.slotBusy), std::end(g_cal.slotBusy), [](bool b) { return b; });

    if (!g_cal.sweep.Running() && g_cal.grid != nullptr && !inFlight)
        CalibrationReleaseResources();

    if (!g_cal.sweep.Running() && !g_cal.startRequested && g_cal.grid == nullptr)
        g_cal.active.store(false, std::memory_order_release);
}

bool CalibrationActive() { return g_cal.frame.override && g_cal.grid != nullptr; }

// Ends everything at NR shutdown: no run, no pending start, no pin left behind for whatever initialises next.
void CalibrationShutdown()
{
    std::lock_guard<std::mutex> lock(g_cal.mutex);
    g_cal.sweep.Abandon(Cal::Abort::NrOff);
    g_cal.startRequested = false;
    g_cal.frame = {};
    g_cal.frameWhitePoint = 0.0f;
    g_cal.measurePending = false;
    g_cal.logged = true;
    CalibrationReleaseResources();
    g_cal.active.store(false, std::memory_order_release);
}

// Copies `from` (in `fromState`) into one of the pair, leaving the copy readable by a shader.
void CalibrationCopy(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, ID3D12Resource*& copy, bool& readable,
                     ID3D12Resource* from, D3D12_RESOURCE_STATES fromState)
{
    // A copy from before a resize (the frame, or the encode's copy, was rebuilt) is not the same shape any more, and
    // CopyResource between different shapes is invalid. Replace it.
    if (copy != nullptr)
    {
        const D3D12_RESOURCE_DESC have = copy->GetDesc();
        const D3D12_RESOURCE_DESC want = from->GetDesc();

        if (have.Width != want.Width || have.Height != want.Height || have.Format != want.Format ||
            have.MipLevels != want.MipLevels || have.DepthOrArraySize != want.DepthOrArraySize ||
            have.SampleDesc.Count != want.SampleDesc.Count)
        {
            ParkNrResource(copy);
            readable = false;
        }
    }

    if (copy == nullptr)
    {
        copy = CreateCopyOf(device, from);
        readable = false;

        if (copy == nullptr)
            return;
    }

    if (readable)
        Barrier(cmdList, copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    Barrier(cmdList, from, fromState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(copy, from);
    Barrier(cmdList, from, D3D12_RESOURCE_STATE_COPY_SOURCE, fromState);
    Barrier(cmdList, copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    readable = true;
}

// After the encode: the untouched frame (the encode's copy, readable by shaders).
void CalibrationCopyInput(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, ID3D12Resource* untouched)
{
    if (!CalibrationActive() || untouched == nullptr)
        return;

    const unsigned int i = g_cal.current;
    CalibrationCopy(cmdList, device, g_cal.input[i], g_cal.inputReadable[i], untouched,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// After the resolve: `edited` is the finished frame, in COPY_SOURCE (the caller transitions the game's resource).
// `modelInput` is the picture the model was shown -- the full-size proxy, or its shrink when the model runs reduced --
// readable by shaders; the stats pass samples it at the frame's coordinates scaled to its own size.
// `width` x `height` is the frame, which may be smaller than the resources.
void CalibrationMeasure(DlssNr_Dx12* pass, ID3D12GraphicsCommandList* cmdList, ID3D12Device* device,
                        ID3D12Resource* edited, ID3D12Resource* modelInput, unsigned int width, unsigned int height)
{
    if (!CalibrationActive() || edited == nullptr)
        return;

    const unsigned int i = g_cal.current;
    const unsigned int prev = i ^ 1u;
    CalibrationCopy(cmdList, device, g_cal.output[i], g_cal.outputReadable[i], edited,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);

    if (!g_cal.measurePending)
        return;

    const bool haveAll = g_cal.outputReadable[i] && g_cal.outputReadable[prev] && g_cal.inputReadable[i] &&
                         g_cal.inputReadable[prev] && modelInput != nullptr;

    unsigned int slot = kCalRing;
    for (unsigned int s = 0; s < kCalRing; ++s)
    {
        if (!g_cal.slotBusy[s])
        {
            slot = s;
            break;
        }
    }

    if (!haveAll || slot == kCalRing)
        return; // left pending: the next CalibrationBeginFrame returns the ticket empty

    // The stats shader reads its own layout over the first fields (dlssnr_detail_stats.hlsl's cbuffer): Mode unused,
    // WhitePoint = the measuring white point, Width/Height = the frame, TransferStrength/ColourStrength = the damage
    // thresholds.
    DlssNrConstants params {};
    params.Mode = 0;
    params.WhitePoint = g_cal.measureWhitePoint;
    params.Width = width;
    params.Height = height;
    params.TransferStrength = kCalShoulder;
    params.ColourStrength = kCalFloor;

    if (!pass->DispatchDetailStats(cmdList, params, g_cal.output[i], g_cal.output[prev], g_cal.input[i],
                                   g_cal.input[prev], modelInput, g_cal.grid))
        return;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = g_cal.grid;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_cal.readback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kCalGridWidth;
    dst.PlacedFootprint.Footprint.Height = kCalGridHeight;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kCalRowBytes;

    Barrier(cmdList, g_cal.grid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, g_cal.grid, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    std::lock_guard<std::mutex> lock(g_cal.mutex);
    g_cal.slotBusy[slot] = true;
    g_cal.slotTicket[slot] = g_cal.frame.ticket;
    g_cal.slotDue[slot] = g_frames + kCalReadDelay;
    g_cal.measurePending = false;
}
