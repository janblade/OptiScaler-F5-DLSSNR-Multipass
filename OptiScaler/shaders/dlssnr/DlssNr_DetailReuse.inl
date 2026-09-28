// Reuse detail between frames, D3D12 (menu: "Reuse detail between frames"; ini DetailReuse*).
//
// Every other frame skips the model -- all passes -- and moves the previous frame's detail (model answer minus model
// input, in the proxy domain at the working size) onto this frame's input instead; the resolve then composes it like
// any model answer. The cadence is dlssnr/DlssNrDetailReuse.h, the GPU work precompile/dlssnr_detail_reuse.hlsl.
// NR after SR only; frames that must see the real model (reset, settings change, Tune, frame hold) run it, and none of
// it runs while frame generation makes frames unless asked to (A/B testing): generated frames are built from real
// ones, and alternating full and reused frames can flicker under it.
//
// Included inside DlssNr_Dx12.cpp's anonymous namespace, after CreateScratch / ParkNrResource / Barrier. Dispatch calls
// BeforeModel and AfterModel around its pass loop and AfterResolve after the resolve, all under g_nrMutex. Its own
// state lives here; the textures exist only while it is on.
namespace DetailReuse
{
// What Dispatch knows about this frame.
struct Frame
{
    DlssNr_Dx12* pass = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12Device* device = nullptr;
    const Config* cfg = nullptr;
    const DlssNrFrameInfo* info = nullptr;
    DXGI_FORMAT answerFormat = DXGI_FORMAT_UNKNOWN; // the model answers' format
    unsigned int workWidth = 0, workHeight = 0;
    unsigned int motionWidth = 0, motionHeight = 0, motionBaseX = 0, motionBaseY = 0;
    unsigned int motionAllocWidth = 0, motionAllocHeight = 0; // the motion texture itself
    unsigned int depthWidth = 0, depthHeight = 0, depthBaseX = 0, depthBaseY = 0;
    bool depthInverted = false;
    float mvScaleX = 1.0f, mvScaleY = 1.0f; // the game's own scale
    ID3D12Resource* modelInput = nullptr;   // readable
    ID3D12Resource* motion = nullptr;       // readable
    ID3D12Resource* depth = nullptr;        // readable
    ID3D12Resource* output = nullptr;       // the first model answer's texture, at rest (UAV)
    bool modelReset = false;                // the model starts over this frame
    bool blocked = false;                   // Tune or frame hold: the real model must run
    unsigned long long frameNumber = 0;     // the present counter, or NR's own frame count
    unsigned long long revision = 0;        // changes when anything that changes the model's answer changes
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

DlssNrDetailReuse::Cadence cadence;
DlssNrDetailReuse::Decision decision;
DlssNrDetailReuseConstants params {};

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
bool allocFailed = false, extrasFailed = false;
unsigned int failedWidth = 0, failedHeight = 0; // the working size an allocation failed at (retried at another)
bool modelSkipped = false; // the model was skipped on the last NR frame
bool activeLast = false;
unsigned int gatedFrames = 0; // NR frames in a row with reuse held off for now (frame generation, low frame rate)
DlssNrDetailReuse::FrameRateGate rateGate;
std::chrono::steady_clock::time_point lastNrFrame {};
constexpr const char* kBelowMinimumFps = "off below the minimum frame rate";
bool composedReadable = false, steadiedReadable = false; // this frame, between the calls
std::string why;
double gpuRecent[16] = {}; // the last 16 whole-pass GPU times: full and reused frames cost differently
unsigned int gpuRecentCount = 0;

// What the menu reads, published at the end of each BeforeModel under a lock of its own: g_nrMutex is held through
// the whole of NR's recording, and the menu must not wait on it every frame.
std::mutex publishedMutex;
DlssNr::DetailReuseInfo published;
unsigned long long publishedPresent = 0; // State::frameCount when it was published
// When NR stops reaching BeforeModel (NR turned off, the proxy path, the game stops upscaling), the status reads as not
// active after this many presents.
constexpr unsigned long long kStalePresents = 30;
// Frame generation pauses briefly and often (menu, loading, some games toggle it): the textures stay through this many
// NR frames of it before they are released.
constexpr unsigned int kKeepGatedFrames = 300;

// Frame generation is built from two real frames; reused frames next to full ones flicker under it (NBA 2K27 with
// OptiFG, 2026-09-28), and frame generation fills in frames better than moved NR detail does. Returns the reason, or
// null when none is seen: OptiScaler's own (active, not paused), the game's DLSS-G seen through NGX, or one owned by
// an external module.
const char* FrameGenerationInUse()
{
    auto& state = State::Instance();
    if (state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused())
        return "off while frame generation is on";
    // A DLSS-G evaluate within the last 30 presents that generates frames (or does not say how many: DLSS-G builds
    // without multi frame generation leave the count out). PresentsSince reads "never" as endlessly long ago.
    if (state.dlssgLastEvaluateGenerates &&
        DlssNr::PresentsSince(state.frameCount, state.dlssgLastEvaluateFrame) <= 30)
        return "off while the game's frame generation is on";
    if (state.externalFrameGeneration)
        return "off while an external frame generation owns the frames";
    return nullptr;
}

void ParkExtras()
{
    ParkNrResource(estimate);
    ParkNrResource(steadied);
    steadiedFormat = DXGI_FORMAT_UNKNOWN;
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
    workWidth = workHeight = 0;
    cadence.Drop();
}

// An allocation failed at this working size: not retried until the size changes or the option is switched off and on.
void AllocationFailed(const Frame& f, bool& latch)
{
    latch = true;
    failedWidth = f.workWidth;
    failedHeight = f.workHeight;
}

// Allocates or parks the textures for this frame. Returns whether reuse can run. keep: reuse is held off for now
// (frame generation), so the textures stay.
bool Prepare(const Frame& f, bool wanted, bool keep, float steady, float fill)
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
            AllocationFailed(f, allocFailed);
            LOG_ERROR("DLSS-NR detail reuse: could not allocate its history textures; every frame runs the model");
            return false;
        }
        workWidth = f.workWidth;
        workHeight = f.workHeight;
        cur = 0;
        gpuRecentCount = 0;
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
            AllocationFailed(f, allocFailed);
            LOG_ERROR("DLSS-NR detail reuse: could not allocate its motion texture; every frame runs the model");
            return false;
        }
        composedWidth = wantWidth;
        composedHeight = wantHeight;
    }

    // The moved detail with its trust, for Fill on reused frames and Steady on full ones; the steadied answer for
    // Steady only.
    const bool needEstimate = steady > 0.0f || fill > 0.0f;
    if (needEstimate && estimate == nullptr && !extrasFailed)
    {
        estimate = CreateScratch(f.device, DXGI_FORMAT_R16G16B16A16_FLOAT, f.workWidth, f.workHeight);
        if (estimate == nullptr)
        {
            AllocationFailed(f, extrasFailed);
            LOG_WARN("DLSS-NR detail reuse: could not allocate the estimate texture; no fill and no steadiness");
        }
    }
    else if (!needEstimate && estimate != nullptr)
    {
        ParkNrResource(estimate);
    }
    if (steady > 0.0f && estimate != nullptr && !extrasFailed &&
        (steadied == nullptr || steadiedFormat != f.answerFormat))
    {
        ParkNrResource(steadied);
        steadied = CreateScratch(f.device, f.answerFormat, f.workWidth, f.workHeight);
        steadiedFormat = steadied != nullptr ? f.answerFormat : DXGI_FORMAT_UNKNOWN;
        if (steadied == nullptr)
        {
            AllocationFailed(f, extrasFailed);
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

// The moved detail with its trust into `estimate`, then `mode` (Fill or Steady) from it into `target`.
bool EstimateThen(const Frame& f, DlssNrDetailReuseMode mode, ID3D12Resource* second, ID3D12Resource* target)
{
    MakeHistoryReadable(f.cmdList, true);
    params.Mode = DlssNrDetailReuse_Estimate;
    bool ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput, detail[cur],
                                          colourDepth[cur], f.motion, f.depth, estimate, nullptr);
    MakeHistoryReadable(f.cmdList, false);
    if (!ok)
        return false;
    Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    params.Mode = mode;
    ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput, second, estimate,
                                     nullptr, f.depth, target, nullptr);
    Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return ok;
}

DlssNr::DetailReuseInfo Status();

// The status the menu reads, under its own lock (see publishedMutex).
void Publish()
{
    auto status = Status();
    std::lock_guard<std::mutex> lock(publishedMutex);
    published = std::move(status);
    publishedPresent = State::Instance().frameCount;
}

Plan BeforeModel(const Frame& f)
{
    const Config& cfg = *f.cfg;
    Plan plan;
    plan.motion = f.motion;
    plan.motionBaseX = f.motionBaseX;
    plan.motionBaseY = f.motionBaseY;
    composedReadable = steadiedReadable = false;

    const bool on = cfg.DlssNrDetailReuse.value_or_default();
    // A failed allocation is retried once the option is switched off and on, or at another working size.
    if ((allocFailed || extrasFailed) && (!on || f.workWidth != failedWidth || f.workHeight != failedHeight))
        allocFailed = extrasFailed = false;
    // Under frame generation it runs only when asked to (A/B testing).
    const char* fg = FrameGenerationInUse();
    const bool withFg = fg != nullptr && cfg.DlssNrDetailReuseWithFg.value_or_default();
    // NR runs once per rendered frame, so the time between two calls is the rendered frame rate, frame generation
    // or not. Measured whether or not reuse runs, so the gate knows when to let it back.
    const auto now = std::chrono::steady_clock::now();
    const double sinceLast = lastNrFrame.time_since_epoch().count() != 0
                                 ? std::chrono::duration<double>(now - lastNrFrame).count()
                                 : 0.0;
    lastNrFrame = now;
    const bool fastEnough = rateGate.Update(sinceLast, cfg.DlssNrDetailReuseMinFps.value_or_default());
    const char* whyNot = nullptr;
    if (f.info->BeforeUpscale)
        whyNot = "unavailable while NR runs before SR";
    else if (f.info->FinishedPicture)
        whyNot = "unavailable in Finished Picture";
    else if (fg != nullptr && !withFg)
        whyNot = fg;
    else if (!fastEnough)
        whyNot = kBelowMinimumFps;
    else if (allocFailed)
        whyNot = "its history textures could not be allocated";
    else if (on && !f.pass->DetailReuseReady())
        whyNot = "its shader could not be built";
    why = on && whyNot != nullptr ? whyNot : "";
    {
        static std::string loggedWhy;
        const std::string state = !why.empty() ? why
                                  : on && withFg ? "available, kept on with frame generation"
                                                 : "available";
        if (state != loggedWhy)
        {
            LOG_INFO("DLSS-NR detail reuse: {} (rendered frame rate {:.0f} fps)", state, rateGate.Fps());
            loggedWhy = state;
        }
    }

    const float steady = std::clamp(cfg.DlssNrDetailReuseSteady.value_or_default(), 0.0f, 1.0f);
    const float fill = std::clamp(cfg.DlssNrDetailReuseFill.value_or_default(), 0.0f, 1.0f);
    // Held off for now (frame generation, which often pauses and resumes, or the frame rate): the textures stay a while.
    const bool heldOff = on && whyNot != nullptr && (whyNot == fg || whyNot == kBelowMinimumFps);
    gatedFrames = heldOff ? gatedFrames + 1 : 0;
    plan.active = Prepare(f, on && whyNot == nullptr, heldOff && gatedFrames <= kKeepGatedFrames, steady, fill);

    DlssNrDetailReuse::FrameFacts facts;
    facts.enabled = plan.active;
    facts.reset = f.modelReset;
    facts.blocked = f.blocked;
    // Without frame generation NR runs on every present, so any skipped present is a gap. With it, the present counter
    // can also count generated frames (up to 3 per real one with multi frame generation).
    facts.frame = f.frameNumber;
    facts.maxStep = withFg ? 4 : 1;
    facts.revision = f.revision;
    decision = cadence.Next(facts);

    params = {};
    params.WorkWidth = f.workWidth;
    params.WorkHeight = f.workHeight;
    params.MotionWidth = f.motionWidth;
    params.MotionHeight = f.motionHeight;
    params.MotionBaseX = f.motionBaseX;
    params.MotionBaseY = f.motionBaseY;
    params.DepthWidth = f.depthWidth;
    params.DepthHeight = f.depthHeight;
    params.DepthBaseX = f.depthBaseX;
    params.DepthBaseY = f.depthBaseY;
    params.DepthInverted = f.depthInverted ? 1u : 0u;
    // The game's own scale: raw times it is pixels of the motion subrect, which the shader divides by its size.
    params.MvScaleX = f.mvScaleX;
    params.MvScaleY = f.mvScaleY;
    params.DepthTolerance = kDlssNrDetailReuseDepthTolerance;
    params.ClipGamma = kDlssNrDetailReuseClipGamma;
    params.ClipFalloff = kDlssNrDetailReuseClipFalloff;
    params.SigmaFloor = kDlssNrDetailReuseSigmaFloor;
    params.Steady = steady;
    params.FillStrength = fill;
    // Scaled with the working size, like the colour box: the same reach on screen at any model resolution.
    params.FillRadius = kDlssNrDetailReuseFillRadius1080p *
                        std::clamp(std::sqrt((float) f.workWidth * (float) f.workHeight / (1920.0f * 1080.0f)), 0.5f,
                                   2.0f);
    params.DebugView = cfg.DlssNrDetailReuseDebug.value_or_default() ? 1u : 0u;

    if (decision.kind == DlssNrDetailReuse::Kind::Reuse)
    {
        if (fill > 0.0f && estimate != nullptr)
        {
            // Moved detail with its trust first; Fill composes it and fills where it was dropped.
            plan.reused = EstimateThen(f, DlssNrDetailReuse_Fill, estimate, f.output);
        }
        else
        {
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

    // The model last ran two frames ago but gets no composed vectors now: reuse was switched off, frame generation
    // came on, the size changed, or composing failed. Its history would be moved by one frame of motion; start it over.
    if (!plan.reused && modelSkipped && !composedReadable && !f.modelReset)
    {
        plan.resetModel = true;
        static bool loggedRealign = false;
        if (!loggedRealign)
        {
            loggedRealign = true;
            LOG_INFO("DLSS-NR detail reuse: model history reset after a reused frame (no composed vectors)");
        }
    }
    modelSkipped = plan.reused;
    activeLast = plan.active;
    Publish();
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
        EstimateThen(f, DlssNrDetailReuse_Steady, finalAnswer, steadied))
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

void RecordGpuTime(double ms) { gpuRecent[gpuRecentCount++ % 16] = ms; }

DlssNr::DetailReuseInfo Status()
{
    DlssNr::DetailReuseInfo status { cadence.Full(), cadence.Reused(), cadence.Fallback(), why };
    const unsigned int count = std::min(gpuRecentCount, 16u);
    for (unsigned int i = 0; i < count; ++i)
    {
        status.lightMs = i == 0 ? gpuRecent[i] : std::min(status.lightMs, gpuRecent[i]);
        status.heavyMs = i == 0 ? gpuRecent[i] : std::max(status.heavyMs, gpuRecent[i]);
        status.averageMs += gpuRecent[i] / count;
    }
    status.active = activeLast;
    status.baseFps = rateGate.Fps();
    return status;
}

// For the menu, without g_nrMutex.
DlssNr::DetailReuseInfo Published()
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    DlssNr::DetailReuseInfo status = published;
    const unsigned long long present = State::Instance().frameCount;
    if (present > publishedPresent && present - publishedPresent > kStalePresents)
        status.active = false;
    return status;
}

// Shutdown: the GPU is idle, so everything is released outright.
void Release()
{
    for (ID3D12Resource** res : { &detail[0], &detail[1], &colourDepth[0], &colourDepth[1], &prevMotion, &composed,
                                  &estimate, &steadied })
    {
        if (*res != nullptr)
        {
            (*res)->Release();
            *res = nullptr;
        }
    }
    workWidth = workHeight = composedWidth = composedHeight = 0;
    steadiedFormat = DXGI_FORMAT_UNKNOWN;
    allocFailed = extrasFailed = modelSkipped = activeLast = composedReadable = steadiedReadable = false;
    failedWidth = failedHeight = gatedFrames = 0;
    cur = 0;
    cadence.Drop();
    std::lock_guard<std::mutex> lock(publishedMutex);
    published = {};
}
} // namespace DetailReuse
