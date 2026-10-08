// Reuse detail between frames, D3D12 (menu: "Reuse detail between frames"; ini DetailReuse*).
//
// Every other frame skips the model -- all passes -- and moves the previous frame's detail (model answer minus model
// input, in the proxy domain at the working size) onto this frame's input instead; the resolve then composes it like
// any model answer. The cadence is dlssnr/DlssNrDetailReuse.h, the GPU work precompile/dlssnr_detail_reuse.hlsl, and
// what does not depend on the API (when it may run, the constants, the status) dlssnr/DlssNrDetailReuseHost.h.
// NR after SR only; frames that must see the real model (reset, settings change, Tune, frame hold) run it, and none of
// it runs while frame generation makes frames unless asked to (A/B testing): generated frames are built from real
// ones, and alternating full and reused frames can flicker under it.
//
// Included inside DlssNr_Dx12.cpp's anonymous namespace, after CreateScratch / ParkNrResource / Barrier. Dispatch calls
// BeforeModel and AfterModel around its pass loop and AfterResolve after the resolve, all under g_nrMutex. Its own
// state lives here; the textures exist only while it is on.
namespace DetailReuse
{
// What Dispatch knows about this frame: the shared facts, and the D3D12 objects.
struct Frame : DlssNrDetailReuse::HostFrame
{
    DlssNr_Dx12* pass = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12Device* device = nullptr;
    const DlssNrFrameInfo* info = nullptr;
    DXGI_FORMAT answerFormat = DXGI_FORMAT_UNKNOWN; // the model answers' format
    ID3D12Resource* modelInput = nullptr;           // readable
    ID3D12Resource* motion = nullptr;               // readable
    ID3D12Resource* depth = nullptr;                // readable
    ID3D12Resource* output = nullptr;               // the first model answer's texture, at rest (UAV)
};

// What BeforeModel decided.
struct Plan
{
    bool active = false;     // reuse runs on this route now (the bottleneck reuse is then off)
    bool reused = false;     // the model is skipped: the answer is in Frame::output, still writable
    bool resetModel = false; // start the model's history over (it last ran two frames ago, without composed vectors)
    ID3D12Resource* motion = nullptr; // the vectors the model evaluates with, and their subrect origin
    unsigned int motionBaseX = 0, motionBaseY = 0;
};

DlssNrDetailReuse::Host host { "DLSS-NR detail reuse" };
DlssNrDetailReuse::Cadence& cadence = host.cadence;
DlssNrDetailReuse::Decision& decision = host.decision;
DlssNrDetailReuseConstants& params = host.params;

// Every processed frame saves its detail; two sets alternate, one read this frame (cur), the other written.
ID3D12Resource* detail[2] = {};      // work size, RGBA16F: answer - input, a = valid
ID3D12Resource* colourDepth[2] = {}; // work size, RGBA32F: that frame's input colour, a = far-is-zero depth
                                     // (32-bit: far reversed-Z values fall below half precision's range)
unsigned int cur = 0;
ID3D12Resource* prevMotion = nullptr; // work size: the last reused frame's vectors as uv displacement
ID3D12Resource* composed = nullptr;   // the motion texture's allocation size: raw vectors over two frames
ID3D12Resource* estimate = nullptr;   // work size: moved detail with its trust (Fill and Steady)
ID3D12Resource* steadied = nullptr;   // work size, the answers' format: the steadied answer (Steady)
DXGI_FORMAT steadiedFormat = DXGI_FORMAT_UNKNOWN;
unsigned int workWidth = 0, workHeight = 0, composedWidth = 0, composedHeight = 0;
bool composedReadable = false, steadiedReadable = false; // this frame, between the calls
bool measureWithSteady = false; // a held frame whose steadiness pass carries the coverage measurement

// How much of a frame arrived with no detail to move, for DlssNrDetailReuse::MotionGuard: the Coverage pass sums the
// trust over a small grid, which is copied to a readback buffer and read a few frames later, when the GPU is certainly
// past it. Each slot keeps its own fence, signalled from FinishedPictureSubmitted (the real ExecuteCommandLists) like
// DlssNrGpuTime's samples: recording a copy is not executing it, and two queues could signal one shared fence out of
// order.
constexpr unsigned int kCoverageTiles = kDlssNrDetailReuseCoverageTiles;
constexpr unsigned int kCoverageRowBytes = kCoverageTiles * 2 * sizeof(float); // 32 texels of RG32F = 256 bytes exactly
constexpr unsigned int kCoverageBytes = kCoverageRowBytes * kCoverageTiles;
constexpr unsigned int kCoverageSlots = 8;
static_assert(kCoverageRowBytes % 256 == 0, "a texture-to-buffer copy needs rows a multiple of 256 bytes");

ID3D12Resource* coverage = nullptr; // kCoverageTiles^2, RG32F: (sum of 1 - trust, pixels measured) per tile

struct CoverageSlot
{
    ID3D12Resource* buffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    ID3D12CommandList* commands = nullptr; // identity only
    UINT64 value = 0;
    bool pending = false;   // a copy was recorded into it
    bool submitted = false; // and the list it was recorded on was executed
};

CoverageSlot coverageSlots[kCoverageSlots];
UINT64 coverageValue = 0;
// Coverage's own allocation failure: not host.AllocationFailed, which also turns off Fill and steadiness (they share
// the estimate texture, and losing fill is a visible loss for a measurement nobody asked for).
bool coverageFailed = false;

void ParkExtras()
{
    ParkNrResource(estimate);
    ParkNrResource(steadied);
    steadiedFormat = DXGI_FORMAT_UNKNOWN;
}

// The coverage grid and its readback slots. A slot with a copy in flight is parked, not released; its fence is kept,
// since a Signal recorded against it may still be outstanding (DlssNrGpuTime and Late::slots keep theirs for their
// object's lifetime too). Only Release lets the fences go.
void ParkCoverage()
{
    ParkNrResource(coverage);
    for (CoverageSlot& slot : coverageSlots)
    {
        ParkNrResource(slot.buffer);
        slot.commands = nullptr;
        slot.pending = slot.submitted = false;
    }
}

// Built the first frame the measurement is wanted; without them reuse simply never pauses.
bool MakeCoverage(ID3D12Device* device)
{
    coverage = CreateScratch(device, DXGI_FORMAT_R32G32_FLOAT, kCoverageTiles, kCoverageTiles);

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kCoverageBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    bool ok = coverage != nullptr;
    for (CoverageSlot& slot : coverageSlots)
    {
        ok = ok && SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&slot.buffer)));
        // A fence a park kept is reused; its value only ever goes up, so an old Signal cannot read as a new one.
        if (ok && slot.fence == nullptr)
            ok = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence)));
    }
    if (!ok)
    {
        ParkCoverage();
        coverageFailed = true;
        LOG_WARN("DLSS-NR detail reuse: could not allocate the coverage grid; it will not pause on fast motion");
    }
    return ok;
}

// Every finished copy: the share of that frame with no detail to move, to the guard. Called once a frame, before a new
// copy is recorded, so a slot is never read while the GPU may still be writing it.
void CollectCoverage()
{
    for (CoverageSlot& slot : coverageSlots)
    {
        if (!slot.pending || !slot.submitted || slot.buffer == nullptr || slot.fence == nullptr)
            continue;
        const UINT64 completed = slot.fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < slot.value)
            continue;

        void* mapped = nullptr;
        D3D12_RANGE range { 0, kCoverageBytes };
        if (SUCCEEDED(slot.buffer->Map(0, &range, &mapped)) && mapped != nullptr)
        {
            double dropped = 0.0, pixels = 0.0;
            const auto* row = static_cast<const char*>(mapped);
            for (unsigned int y = 0; y < kCoverageTiles; ++y, row += kCoverageRowBytes)
            {
                const auto* tile = reinterpret_cast<const float*>(row);
                for (unsigned int x = 0; x < kCoverageTiles; ++x)
                {
                    // A tile whose numbers are not finite is left out rather than poisoning the frame's share.
                    if (std::isfinite(tile[2 * x]) && std::isfinite(tile[2 * x + 1]))
                    {
                        dropped += tile[2 * x];
                        pixels += tile[2 * x + 1];
                    }
                }
            }
            D3D12_RANGE none { 0, 0 };
            slot.buffer->Unmap(0, &none);
            if (pixels > 0.0)
                host.RecordDropped((float) std::clamp(dropped / pixels, 0.0, 1.0));
        }
        slot.pending = slot.submitted = false;
        slot.commands = nullptr;
    }
}

// The grid into a free readback slot. Recorded right after a Coverage dispatch, with the grid still a UAV.
void CopyCoverage(ID3D12GraphicsCommandList* cmdList)
{
    CoverageSlot* free = nullptr;
    for (CoverageSlot& slot : coverageSlots)
        if (!slot.pending && slot.buffer != nullptr && slot.fence != nullptr)
        {
            free = &slot;
            break;
        }
    if (free == nullptr) // every slot in flight: this frame is not measured, which the guard reads as no reading
        return;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = coverage;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = free->buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kCoverageTiles;
    dst.PlacedFootprint.Footprint.Height = kCoverageTiles;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kCoverageRowBytes;

    Barrier(cmdList, coverage, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, coverage, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The list the hook will report is the real one, not a Streamline proxy: DlssNrGpuTime::Start and Late::Arm both
    // translate it the same way, and without it no slot ever matches and the measurement quietly dies.
    free->commands = cmdList;
    ID3D12GraphicsCommandList* real = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, cmdList, (IUnknown**) &real))
        free->commands = real;
    DlssNrWatchedLists::Add(free->commands); // the submit/reset hooks must hear about this list
    free->pending = true;
    free->submitted = false;
}

// A command list a copy was recorded on was reset without being executed: the slot is free again, and no fence was
// ever signalled for it. Called from FinishedPictureResetCommandList, as DlssNrGpuTime::ResetRecording is.
void ResetRecording(ID3D12CommandList* cmdList)
{
    for (CoverageSlot& slot : coverageSlots)
        if (slot.pending && !slot.submitted && slot.commands == cmdList)
        {
            slot.pending = false;
            slot.commands = nullptr;
        }
}

// The command list a copy was recorded on has been executed: its slot's fence may be signalled. Called from
// FinishedPictureSubmitted, under g_nrMutex, after the game's own ExecuteCommandLists.
void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (queue == nullptr || lists == nullptr)
        return;
    for (CoverageSlot& slot : coverageSlots)
    {
        if (!slot.pending || slot.submitted || slot.fence == nullptr)
            continue;
        for (UINT i = 0; i < count; ++i)
            if (lists[i] == slot.commands)
            {
                slot.value = ++coverageValue;
                slot.submitted = true;
                // A failed signal would leave the slot waiting for ever: give it up instead, unread.
                if (FAILED(queue->Signal(slot.fence.Get(), slot.value)))
                    slot.pending = slot.submitted = false;
                break;
            }
    }
}

void ParkComposed()
{
    ParkNrResource(composed);
    composedWidth = composedHeight = 0;
}

void ParkAll()
{
    for (unsigned int i = 0; i < 2; ++i)
    {
        ParkNrResource(detail[i]);
        ParkNrResource(colourDepth[i]);
    }
    ParkNrResource(prevMotion);
    ParkComposed();
    ParkExtras();
    ParkCoverage();
    workWidth = workHeight = 0;
    cadence.Drop();
}

// Allocates or parks the textures for this frame. Returns whether reuse can run. keep: reuse is held off for now
// (frame generation, the frame rate), so the textures stay.
bool Prepare(const Frame& f, bool wanted, bool keep, float steady, float fill, bool measure)
{
    // Everything that follows the working size, rebuilt only when that changes.
    if (wanted && (detail[0] == nullptr || workWidth != f.workWidth || workHeight != f.workHeight))
    {
        ParkAll();
        bool allocated = true;
        for (unsigned int i = 0; i < 2; ++i)
        {
            detail[i] = CreateScratch(f.device, DXGI_FORMAT_R16G16B16A16_FLOAT, f.workWidth, f.workHeight);
            colourDepth[i] = CreateScratch(f.device, DXGI_FORMAT_R32G32B32A32_FLOAT, f.workWidth, f.workHeight);
            allocated = allocated && detail[i] != nullptr && colourDepth[i] != nullptr;
        }
        prevMotion = CreateScratch(f.device, DXGI_FORMAT_R32G32_FLOAT, f.workWidth, f.workHeight);
        allocated = allocated && prevMotion != nullptr;
        if (!allocated)
        {
            ParkAll();
            host.AllocationFailed(f, false);
            LOG_ERROR("DLSS-NR detail reuse: could not allocate its history textures; every frame runs the model");
            return false;
        }
        workWidth = f.workWidth;
        workHeight = f.workHeight;
        cur = 0;
        host.TexturesRebuilt();
        LOG_INFO("DLSS-NR detail reuse: on, model {}x{}", f.workWidth, f.workHeight);
    }
    else if (!wanted)
    {
        if (detail[0] != nullptr && !keep)
            ParkAll();
        return false;
    }

    // The composed vectors go to the model in the motion subrect's place, so they are sized like the motion texture's
    // allocation: a render-size change moves the subrect inside it without a rebuild.
    const unsigned int wantWidth = std::max(f.motionAllocWidth, f.motionWidth);
    const unsigned int wantHeight = std::max(f.motionAllocHeight, f.motionHeight);
    if (composed == nullptr || composedWidth != wantWidth || composedHeight != wantHeight)
    {
        ParkComposed();
        composed = CreateScratch(f.device, DXGI_FORMAT_R32G32_FLOAT, wantWidth, wantHeight);
        if (composed == nullptr)
        {
            ParkAll();
            host.AllocationFailed(f, false);
            LOG_ERROR("DLSS-NR detail reuse: could not allocate its motion texture; every frame runs the model");
            return false;
        }
        composedWidth = wantWidth;
        composedHeight = wantHeight;
    }

    // The coverage grid, only while the pause on fast motion is on. A failure here disables the measurement alone.
    if (measure && coverage == nullptr && !coverageFailed)
        MakeCoverage(f.device);
    else if (!measure && coverage != nullptr)
        ParkCoverage();
    const bool measuring = measure && coverage != nullptr;

    // The moved detail with its trust, for Fill on reused frames, Steady on full ones and the coverage measurement;
    // the steadied answer for Steady only.
    const bool needEstimate = steady > 0.0f || fill > 0.0f || measuring;
    if (needEstimate && estimate == nullptr && !host.ExtrasFailed())
    {
        estimate = CreateScratch(f.device, DXGI_FORMAT_R16G16B16A16_FLOAT, f.workWidth, f.workHeight);
        if (estimate == nullptr)
        {
            host.AllocationFailed(f, true);
            LOG_WARN("DLSS-NR detail reuse: could not allocate the estimate texture; no fill and no steadiness");
        }
    }
    else if (!needEstimate && estimate != nullptr)
    {
        ParkNrResource(estimate);
    }
    if (steady > 0.0f && estimate != nullptr && !host.ExtrasFailed() &&
        (steadied == nullptr || steadiedFormat != f.answerFormat))
    {
        ParkNrResource(steadied);
        steadied = CreateScratch(f.device, f.answerFormat, f.workWidth, f.workHeight);
        steadiedFormat = steadied != nullptr ? f.answerFormat : DXGI_FORMAT_UNKNOWN;
        if (steadied == nullptr)
        {
            host.AllocationFailed(f, true);
            LOG_WARN("DLSS-NR detail reuse: could not allocate the steadiness texture; full frames stay as the model made them");
        }
    }
    else if (steady <= 0.0f && steadied != nullptr)
    {
        ParkNrResource(steadied);
        steadiedFormat = DXGI_FORMAT_UNKNOWN;
    }
    return true;
}

void MakeHistoryReadable(ID3D12GraphicsCommandList* cmdList, bool readable)
{
    const auto from = readable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto to = readable ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Barrier(cmdList, detail[cur], from, to);
    Barrier(cmdList, colourDepth[cur], from, to);
}

// How much of the frame the estimate has no detail for, into the grid and on towards the CPU. The estimate must be
// readable; it is left as it was found.
void MeasureCoverage(const Frame& f)
{
    if (coverage == nullptr)
        return;
    params.Mode = DlssNrDetailReuse_Coverage;
    // One 8x8 thread group per tile of the grid, whatever the working size.
    const unsigned int threads = kCoverageTiles * 8;
    if (f.pass->DispatchDetailReuse(f.cmdList, params, threads, threads, f.modelInput, estimate, nullptr, nullptr,
                                    nullptr, coverage, nullptr))
        CopyCoverage(f.cmdList);
}

// The moved detail with its trust into `estimate`, then `mode` (Fill or Steady) from it into `target`. measure: also
// read off how much of the frame had no detail to move, while the estimate is readable anyway.
bool EstimateThen(const Frame& f, DlssNrDetailReuseMode mode, ID3D12Resource* second, ID3D12Resource* target,
                  bool measure)
{
    MakeHistoryReadable(f.cmdList, true);
    params.Mode = DlssNrDetailReuse_Estimate;
    bool ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput, detail[cur],
                                          colourDepth[cur], f.motion, f.depth, estimate, nullptr);
    MakeHistoryReadable(f.cmdList, false);
    if (!ok)
        return false;
    Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (measure)
        MeasureCoverage(f);
    params.Mode = mode;
    ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput, second, estimate,
                                     nullptr, f.depth, target, nullptr);
    Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return ok;
}

// A full frame while reuse is paused: nothing is reused, but the measurement carries on so the guard sees the picture
// calm down. Estimate into `estimate`, then the grid; the answer is untouched.
void MeasureOnly(const Frame& f)
{
    if (coverage == nullptr || estimate == nullptr)
        return;
    MakeHistoryReadable(f.cmdList, true);
    params.Mode = DlssNrDetailReuse_Estimate;
    const bool ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput, detail[cur],
                                                colourDepth[cur], f.motion, f.depth, estimate, nullptr);
    MakeHistoryReadable(f.cmdList, false);
    if (!ok)
        return;
    Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    MeasureCoverage(f);
    Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

Plan BeforeModel(const Frame& f)
{
    Plan plan;
    plan.motion = f.motion;
    plan.motionBaseX = f.motionBaseX;
    plan.motionBaseY = f.motionBaseY;
    composedReadable = steadiedReadable = false;
    // Anything the GPU has finished measuring, before this frame records more: the guard reads it in Gate.
    CollectCoverage();

    DlssNrDetailReuse::HostFrame facts = f;
    facts.beforeUpscale = f.info->BeforeUpscale;
    facts.finishedPicture = f.info->FinishedPicture;
    facts.present = State::Instance().frameCount;
    const DlssNrDetailReuse::Wanted want = host.Gate(
        facts, [&]() -> const char* { return f.pass->DetailReuseReady() ? nullptr : "its shader could not be built"; });
    plan.active = Prepare(f, want.wanted, want.keep, want.steady, want.fill, want.measure);
    host.Decide(facts, plan.active, want);

    if (decision.kind == DlssNrDetailReuse::Kind::Reuse)
    {
        if (want.fill > 0.0f && estimate != nullptr)
        {
            // Moved detail with its trust first; Fill composes it and fills where it was dropped.
            plan.reused = EstimateThen(f, DlssNrDetailReuse_Fill, estimate, f.output, want.measure);
        }
        else
        {
            // Fill is off. The frame is still measured when asked for, in a pass of its own, so the reuse below stays
            // exactly what it was: Fill at no strength is nearly the same but not bit for bit (it reads the detail back
            // through a half-float texture and lacks Reproject's overflow guard).
            if (want.measure)
                MeasureOnly(f);

            MakeHistoryReadable(f.cmdList, true);
            params.Mode = DlssNrDetailReuse_Reproject;
            plan.reused = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput,
                                                      detail[cur], colourDepth[cur], f.motion, f.depth, f.output,
                                                      nullptr);
            MakeHistoryReadable(f.cmdList, false);
        }

        if (plan.reused)
        {
            // Kept for the next full frame, which composes it with its own vectors for the model's history. Without
            // it that frame cannot compose, so the cadence starts over and the model's history is reset instead.
            params.Mode = DlssNrDetailReuse_SaveMotion;
            if (!f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.motion, nullptr, nullptr,
                                             f.motion, nullptr, prevMotion, nullptr))
                cadence.Drop();
        }
        else
        {
            // Nothing was written: run the model this frame and start the cadence over.
            cadence.ReuseFailed();
            static bool warnedReuse = false;
            if (!warnedReuse)
            {
                warnedReuse = true;
                LOG_WARN("DLSS-NR detail reuse: the reuse pass could not be recorded; running the model");
            }
        }
    }

    // A full frame while reuse is paused: keep measuring, so the guard knows when the picture has calmed down. With
    // steadiness on, AfterModel's own Estimate carries the measurement instead of running a second identical one.
    measureWithSteady = false;
    if (!plan.reused && plan.active && want.measure && want.hold && decision.historyUsable)
    {
        if (params.Steady > 0.0f && estimate != nullptr && steadied != nullptr)
            measureWithSteady = true;
        else
            MeasureOnly(f);
    }

    // A full frame after a reused frame: the model last ran two frames ago, so it gets the vectors over both.
    if (!plan.reused && decision.kind == DlssNrDetailReuse::Kind::Full && decision.composeMotion)
    {
        Barrier(f.cmdList, prevMotion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        params.Mode = DlssNrDetailReuse_Compose;
        const bool ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.motionWidth, f.motionHeight, f.motion,
                                                    nullptr, nullptr, f.motion, prevMotion, composed, nullptr);
        Barrier(f.cmdList, prevMotion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (ok)
        {
            Barrier(f.cmdList, composed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            composedReadable = true;
            plan.motion = composed;
            plan.motionBaseX = plan.motionBaseY = 0;
        }
    }

    plan.resetModel = host.Finish(facts, plan.active, plan.reused, composedReadable);
    return plan;
}

// After the model passes, or the reuse. succeeded: finalAnswer holds a valid, readable answer. May replace it with
// the steadied one.
void AfterModel(const Frame& f, const Plan& plan, bool succeeded, ID3D12Resource*& finalAnswer)
{
    if (composedReadable)
        Barrier(f.cmdList, composed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    composedReadable = false;

    // A full frame with last frame's history: pull the model's new detail toward the moved previous detail, as far as
    // that is trusted, so this frame and the reused one next to it differ less.
    if (!plan.reused && succeeded && finalAnswer != nullptr && decision.historyUsable && params.Steady > 0.0f &&
        estimate != nullptr && steadied != nullptr &&
        EstimateThen(f, DlssNrDetailReuse_Steady, finalAnswer, steadied, measureWithSteady))
    {
        Barrier(f.cmdList, steadied, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        steadiedReadable = true;
        finalAnswer = steadied;
    }

    // Every processed frame keeps its detail (answer - input), input colour and depth for the next frame. Not a reused
    // frame painted by the debug view: its colours are not detail.
    if (decision.captureHistory)
    {
        bool captured = false;
        if (succeeded && finalAnswer != nullptr && !(plan.reused && params.DebugView != 0))
        {
            const unsigned int write = 1u - cur;
            params.Mode = DlssNrDetailReuse_Capture;
            captured = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput,
                                                   finalAnswer, f.depth, nullptr, nullptr, detail[write],
                                                   colourDepth[write]);
            if (captured)
                cur = write;
        }
        cadence.Captured(captured);
    }
}

// After the resolve has read the answer: the steadied one goes back to rest.
void AfterResolve(ID3D12GraphicsCommandList* cmdList)
{
    if (steadiedReadable)
        Barrier(cmdList, steadied, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    steadiedReadable = false;
}

void RecordGpuTime(double ms) { host.RecordGpuTime(ms); }

DlssNr::DetailReuseInfo Status() { return host.Status(); }

// For the menu, without g_nrMutex.
DlssNr::DetailReuseInfo Published() { return host.Published(State::Instance().frameCount); }

// Shutdown: the GPU is idle, so everything is released outright.
void Release()
{
    for (ID3D12Resource** res : { &detail[0], &detail[1], &colourDepth[0], &colourDepth[1], &prevMotion, &composed,
                                  &estimate, &steadied, &coverage })
    {
        if (*res != nullptr)
        {
            (*res)->Release();
            *res = nullptr;
        }
    }
    for (CoverageSlot& slot : coverageSlots)
    {
        if (slot.buffer != nullptr)
        {
            slot.buffer->Release();
            slot.buffer = nullptr;
        }
        slot.fence.Reset();
        slot.commands = nullptr;
        slot.value = 0;
        slot.pending = slot.submitted = false;
    }
    coverageValue = 0;
    coverageFailed = false;
    measureWithSteady = false;
    workWidth = workHeight = composedWidth = composedHeight = 0;
    steadiedFormat = DXGI_FORMAT_UNKNOWN;
    composedReadable = steadiedReadable = false;
    cur = 0;
    host.Reset();
}
} // namespace DetailReuse
