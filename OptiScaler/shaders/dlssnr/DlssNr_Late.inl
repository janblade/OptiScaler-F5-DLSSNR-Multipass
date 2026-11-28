// Included inside namespace DlssNr. All entry points share g_nrMutex.
namespace Late
{
using Microsoft::WRL::ComPtr;
struct Slot
{
    ComPtr<ID3D12Resource> depth, motion, linear, encoded, residual;
    ComPtr<ID3D12Fence> fence;
    // Signalled to `ready` on the producer queue right after `fence`; a present on another queue waits on it on the GPU.
    // Apart from `fence` because the watchdog may signal it from the CPU (DlssNr_CrossQueueGate.h).
    ComPtr<ID3D12Fence> gate;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12CommandQueue> producerQueue; // real queue that signals `ready`; held so its identity stays valid
    ID3D12CommandList* producer = nullptr; // identity only; never dereferenced
    DlssNrFrameInfo frame {};
    uint64_t ready = 0, done = 0, serial = 0;
    bool pending = false, submitted = false, expired = false, residualOnly = false, sceneLinear = true;
    bool gated = false; // the producer queue signalled `gate` for this capture
};
std::array<Slot, 4> slots;
ComPtr<ID3D12Device> device;
uint64_t serial = 0, successes = 0, stillRendering = 0, lastApplicationSuccesses = 0;
uint64_t lastAppliedEpoch = 0; // capture epoch of the last picture NR ran on; 0: none yet (FinishedHistoryGap)
bool loggedCrossQueue = false;
std::string status = "Waiting for a finished picture.";
bool reset = true;
// Native input's own history reset (ApplyNativeInput, ApplyVirtualUpscalerInput), apart from `reset` above: the swap chain
// path cancels, and so sets `reset`, on every present while Finished Picture is off, which is how Optical F5Low's
// "NR + frame generation" mode runs. Shared, it reset NR's history on every frame there: NR's edits flickered.
bool nativeReset = true;
// Capture epoch of the last picture NR ran on with a Vulkan game's own guides (ApplyFinishedGameGuides); 0: none yet
// (FinishedHistoryGap), and again after a frame NR did not run on.
uint64_t gameGuidesApplied = 0;

// Native input's HDR10 scratch pair (no slot to keep it in): NR's linear scRGB picture and the PQ result in the picture's
// own format. Recreated when the picture's size or format changes. A replaced buffer is kept until the frame number has
// moved kRetireFrames past the frame it was replaced in: lists recorded before that may still be reading it on the GPU,
// however many resizes come in between (dragging a window edge resizes every frame). Released with the NR state (Shutdown).
struct NativeHdr
{
    struct Retired
    {
        ComPtr<ID3D12Resource> resource;
        unsigned long long epoch = 0; // the frame it was replaced in
    };

    // Well past the frames a swap chain keeps in flight (three at most here).
    static constexpr unsigned long long kRetireFrames = 8;

    ComPtr<ID3D12Resource> linear, encoded;
    std::vector<Retired> retired;
};
NativeHdr nativeHdr;

std::atomic<bool> tracking { false };
inline constexpr GUID colorSpaceKey = { 0x34a31e7b, 0x84c5, 0x44ef, { 0xa7, 0x4d, 0x6b, 0xd3, 0x60, 0x8c, 0xe5, 0x22 } };

void Say(const char* message)
{
    if (status != message)
    {
        status = message;
        LOG_INFO("DLSS-NR finished picture: {}", message);
    }
}

// Never destroyed: its thread may still be watching at unload, and joining it there would hold the loader lock.
CrossQueueGate::Watchdog& Gate()
{
    static auto* watchdog = new CrossQueueGate::Watchdog(std::chrono::milliseconds(1500));
    return *watchdog;
}

// [DlssNr] FinishedGpuWait: may this present queue a GPU wait on the game's render queue? Called on every present
// that reaches ApplyFinishedColor with Finished Picture on; `captured` = a capture of the game's frame is waiting.
// Skyrim (v0.1.40-pre1 tester): the device was reset on the first frames of a DLSS-G context made again after the
// upscaler was rebuilt, with the present waiting on the gate. Mode 2 keeps the wait off for a settle after frame
// generation turns on/off/pauses or the upscaler changes; mode 3 keeps it off while OptiScaler's frame generation is
// attached. Without the wait, frames still rendering at present are skipped, as before #362.
bool GpuWaitAllowed(bool captured)
{
    const auto mode = Config::Instance()->DlssNrFinishedGpuWait.value_or_default();
    if (mode == 0)
        return false;
    if (mode == 1)
        return true;
    auto& state = State::Instance();
    auto* fg = state.currentFG;
    if (mode == 3)
        return fg == nullptr;
    struct Seen
    {
        const void* fg = nullptr;
        uint32_t upscaler = 0; // the upscaler object's serial, not its address (a rebuild can reuse the address)
        bool generating = false;
    };
    static Seen seen {};
    static uint32_t presentsSince = 0;
    static uint32_t uncaptured = 0; // presents in a row with nothing captured (loading screen, upscaler gone)
    static bool settleLogged = false;
    static std::chrono::steady_clock::time_point changedAt {};
    const Seen now { fg, state.currentFeature != nullptr ? state.currentFeature->CreatedSerial() : 0u,
                     fg && fg->IsActive() && !fg->IsPaused() };
    // A long stretch without captures counts as a change for as long as it lasts, so the settle starts when captures
    // come back: frame generation and the upscaler can both be rebuilt inside it without this function seeing either
    constexpr uint32_t kGapPresents = 30;
    uncaptured = captured ? 0 : (uncaptured < UINT32_MAX ? uncaptured + 1 : uncaptured);
    if (now.fg != seen.fg || now.upscaler != seen.upscaler || now.generating != seen.generating ||
        uncaptured >= kGapPresents)
    {
        // Once per settle: a state that flips often keeps the wait off without a line each time
        if (!settleLogged && (presentsSince != 0 || seen.fg || seen.upscaler))
        {
            settleLogged = true;
            LOG_INFO("DLSS-NR finished picture: frame generation or the upscaler changed; the present stops waiting "
                     "on the GPU for a moment (frames still rendering are skipped meanwhile)");
        }
        seen = now;
        presentsSince = 0;
        changedAt = std::chrono::steady_clock::now();
    }
    if (presentsSince < UINT32_MAX)
        ++presentsSince;
    constexpr uint32_t kSettlePresents = 120;
    constexpr auto kSettleTime = std::chrono::seconds(2);
    const bool settled = presentsSince > kSettlePresents && std::chrono::steady_clock::now() - changedAt >= kSettleTime;
    if (settled)
        settleLogged = false;
    return settled;
}

// Slots whose gate the watchdog freed: their guide copy may still be pending and their fence now runs out of order, so
// they are never used again (pending without a submission: Acquire, Cancel, the reset hook and the resize wait skip
// them). Costs one of the four slots, once per session.
void QuarantineAbandoned()
{
    for (const auto serial : Gate().TakeAbandoned())
        for (auto& slot : slots)
            if (slot.serial == serial)
            {
                slot.pending = true;
                slot.submitted = false;
                slot.expired = true;
                slot.gated = false;
                slot.producer = nullptr;
                reset = true;
                LOG_WARN("DLSS-NR finished picture: the game's render queue did not finish a frame within 1.5 s of its "
                         "present; the present no longer waits for it this session (frames still rendering at present "
                         "are skipped)");
            }
}

bool Finished(const Slot& slot)
{
    return !slot.fence || (slot.fence->GetCompletedValue() != UINT64_MAX &&
                          slot.fence->GetCompletedValue() >= slot.done);
}

// The pair for a picture of this size and format, in frame `epoch` (native::PresentEpoch). False when it could not be made.
bool EnsureNativeHdr(const D3D12_RESOURCE_DESC& desc, unsigned long long epoch)
{
    // Buffers replaced long enough ago that no list still in flight can read them. A frame number that went backwards (a
    // new frame source) keeps them a while longer, which only costs memory.
    std::erase_if(nativeHdr.retired, [epoch](const NativeHdr::Retired& old)
                  { return epoch >= old.epoch && epoch - old.epoch > NativeHdr::kRetireFrames; });

    const auto ensure = [&](ComPtr<ID3D12Resource>& resource, DXGI_FORMAT format)
    {
        if (resource)
        {
            const auto have = resource->GetDesc();

            if (have.Width == desc.Width && have.Height == desc.Height && have.Format == format)
                return true;

            nativeHdr.retired.push_back({ std::move(resource), epoch });
        }

        resource.Attach(CreateScratch(device.Get(), format, (unsigned) desc.Width, desc.Height));
        return resource != nullptr;
    };

    return ensure(nativeHdr.linear, DXGI_FORMAT_R16G16B16A16_FLOAT) && ensure(nativeHdr.encoded, desc.Format);
}

ID3D12CommandQueue* RealQueue(ID3D12CommandQueue* queue)
{
    ID3D12CommandQueue* real = nullptr;
    return Util::CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &real) ? real : queue;
}

void Cancel()
{
    // Submitted copies may still be in flight; their fences still protect reuse.
    for (auto& slot : slots)
    {
        slot.expired = true;
        if (slot.submitted)
            slot.pending = false;
    }
    reset = true;
    lastAppliedEpoch = 0;
}

// Presents OptiScaler's own frame generation shows per game frame: 1 + the generated ones while it generates.
uint32_t PresentsPerGameFrame()
{
    const auto fg = State::Instance().currentFG;
    return native::lowlatency::ShownPerRendered(fg != nullptr && fg->IsGenerating(),
                                                fg != nullptr ? fg->GetInterpolatedFrameCount() : 0);
}

bool Clone(ComPtr<ID3D12Resource>& copy, ID3D12Resource* source, const D3D12_RESOURCE_DESC& want)
{
    // Mips and array slices are fine: the copy keeps only the top level (CopyFinishedGuide).
    if (want.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || want.SampleDesc.Count != 1)
        return false;
    if (copy)
    {
        const auto have = copy->GetDesc();
        if (have.Width != want.Width || have.Height != want.Height ||
            have.Format != TypedGuideFormat(want.Format))
            copy.Reset(); // caller checked the previous GPU fence
    }
    if (!copy)
        copy.Attach(CreateGuideClone(device.Get(), source, true));
    return copy != nullptr;
}

Slot* Acquire(ID3D12GraphicsCommandList* cmd)
{
    if (!cmd || State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
    { Say("This option needs a native DirectX 12 game."); return nullptr; }
    if (StreamlinePicture::PauseEvaluation())
    {
        Cancel();
        Say("Paused: native frame generation has no application-picture handoff.");
        return nullptr;
    }
    ComPtr<ID3D12Device> currentDevice;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&currentDevice))))
        return nullptr;
    if (device && device != currentDevice)
    {
        Say("The graphics device changed. Restart the game to use this option.");
        return nullptr;
    }
    device = currentDevice;
    tracking.store(true);
    QuarantineAbandoned(); // before a slot whose gate the watchdog freed can look finished
    // Only the lightweight submission hook is needed, including when FG is disabled.
    ResTrack_Dx12::HookLateNrQueue(device.Get());
    Slot* next = nullptr;
    for (auto& slot : slots)
        if (!slot.pending && Finished(slot)) { next = &slot; break; }
    if (!next)
    {
        // This frame gets no capture: an older frame's pending one must not be composed onto its picture.
        // Captures of this frame (a game may evaluate more than once) stay.
        const auto now = State::Instance().frameCount;
        for (auto& slot : slots)
        {
            if (slot.submitted && slot.pending && slot.frame.SubmissionEpoch < now)
            {
                reset |= slot.frame.Reset; // dropped unapplied: keep the game's reset flag
                slot.pending = false;
            }
        }
        Say("Waiting for the previous picture to finish.");
        return nullptr;
    }
    auto& slot = *next;
    if (!slot.commands)
    {
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.gate))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&slot.commands))) || FAILED(slot.commands->Close()))
        {
            slot.commands.Reset(); slot.allocator.Reset(); slot.fence.Reset(); slot.gate.Reset();
            Say("Could not prepare the finished-picture option.");
            return nullptr;
        }
        slot.commands->SetName(L"DLSS-NR finished picture"); // named for the D3D12 device-removed report
    }
    return next;
}

void Arm(Slot& slot, ID3D12GraphicsCommandList* cmd)
{
    slot.producer = cmd;
    ID3D12GraphicsCommandList* real = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**) &real)) slot.producer = real;
    DlssNrWatchedLists::Add(slot.producer); // the submit/reset hooks must hear about this list
    slot.serial = ++serial;
    slot.ready = slot.done + 1;
    slot.done = slot.ready;
    slot.submitted = false;
    slot.expired = false;
    slot.pending = true;
    slot.gated = false;
}

void Capture(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params, bool rr)
{
    if (!params) return;
    auto* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    auto* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");
    auto* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    // Depth is optional: NR runs without it.
    if (!motion || !output)
    { Cancel(); Say("Waiting for the game's movement data."); return; }
    auto* next = Acquire(cmd);
    if (!next) return;
    auto& slot = *next;
    const auto depthDesc = depth ? depth->GetDesc() : D3D12_RESOURCE_DESC {};
    const auto motionDesc = motion->GetDesc();
    // Logs a guide the copy could not take, once per shape, so the next unsupported case names itself.
    const auto refuse = [](const char* name, const D3D12_RESOURCE_DESC& d)
    {
        static std::set<std::tuple<std::string, UINT64, UINT, UINT16, UINT16, UINT, int, unsigned>> logged;
        if (!logged.emplace(name, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, d.SampleDesc.Count,
                            (int) d.Format, (unsigned) d.Flags).second)
            return;
        LOG_WARN("DLSS-NR finished picture: cannot copy the game's {} (dimension {}, {}x{}, array {}, mips {}, samples {}, "
                 "format {}, flags 0x{:X})",
                 name, (int) d.Dimension, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, d.SampleDesc.Count,
                 (int) d.Format, (unsigned) d.Flags);
    };
    if (depth && !Clone(slot.depth, depth, depthDesc))
    {
        refuse("depth", depthDesc);
        depth = nullptr; // NR runs without depth rather than skipping the frame
    }
    if (!depth) slot.depth.Reset(); // no stale depth from an earlier frame
    if (!Clone(slot.motion, motion, motionDesc))
    {
        refuse("motion vectors", motionDesc);
        Say("The game's movement data is not supported.");
        return;
    }
    slot.residualOnly = false;
    slot.frame = {};
    auto& frame = slot.frame;
    unsigned flags = 0, gameReset = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
    params->Get(NVSDK_NGX_Parameter_Reset, &gameReset);
    frame.Reset = gameReset != 0;
    frame.DepthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.MotionVectorsLowResolution = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    frame.RayReconstruction = rr;
    const auto outputDesc = output->GetDesc();
    frame.OutputWidth = (unsigned) outputDesc.Width;
    frame.OutputHeight = outputDesc.Height;
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &frame.DepthSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &frame.DepthSubrectBaseY);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.MotionSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &frame.MotionSubrectBaseY);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY);
    frame.ColourIsLinearHdr = false;
    frame.IndependentCommands = true;
    frame.FinishedPicture = true;
    frame.OutputArrivalState = D3D12_RESOURCE_STATE_PRESENT;
    frame.SubmissionEpoch = State::Instance().frameCount;
    // Resolve exactly the regions Dispatch will read, independently for depth and motion.
    // Preserve the allocation, offsets, formats and units, including output-resolution MV.
    const auto regions = ResolveGuideRegions(
        { (unsigned) depthDesc.Width, depthDesc.Height }, { (unsigned) motionDesc.Width, motionDesc.Height },
        { frame.RenderSubrectWidth, frame.RenderSubrectHeight }, { frame.OutputWidth, frame.OutputHeight },
        frame.MotionVectorsLowResolution, frame.DepthSubrectBaseX, frame.DepthSubrectBaseY,
        frame.MotionSubrectBaseX, frame.MotionSubrectBaseY);
    const auto copy = [&](ID3D12Resource* source, ID3D12Resource* destination,
                          const D3D12_RESOURCE_DESC& desc, GuideRegion region)
    {
        if (!source) return;
        // Only the top level is read. With more subresources, only that one is moved: the game's lower mips (a depth
        // pyramid) may rest in another state, which a whole-resource barrier would get wrong.
        const bool whole = desc.MipLevels == 1 && desc.DepthOrArraySize == 1;
        const auto move = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
        {
            if (whole) { Barrier(cmd, source, from, to); return; }
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = source;
            b.Transition.Subresource = 0;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            cmd->ResourceBarrier(1, &b);
        };
        move(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CopyFinishedGuide(cmd, destination, source, desc, region);
        move(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // The copy returns to COPY_DEST after the late dispatch.
    };
    copy(depth, slot.depth.Get(), depthDesc, regions.depth);
    copy(motion, slot.motion.Get(), motionDesc, regions.motion);
    Arm(slot, cmd);
}

bool CaptureResidual(ID3D12GraphicsCommandList* cmd, ID3D12Resource* clean, ID3D12Resource* residual,
                     float scale, bool sceneLinear)
{
    auto* next = Acquire(cmd);
    if (!next) return false;
    auto& slot = *next;
    if (!Clone(slot.residual, residual, residual->GetDesc()))
    { Say("The upscaled changes could not be saved."); return false; }
    Barrier(cmd, residual, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyResource(slot.residual.Get(), residual);
    Barrier(cmd, residual, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slot.frame = {};
    slot.frame.OutputWidth = (unsigned) clean->GetDesc().Width;
    slot.frame.OutputHeight = clean->GetDesc().Height;
    slot.frame.PreExposure = scale;
    slot.frame.SubmissionEpoch = State::Instance().frameCount;
    slot.residualOnly = true;
    slot.sceneLinear = sceneLinear;
    Arm(slot, cmd);
    return true;
}

// Publish copied guides only after the producer list executes. Expired recordings still need
// their fence signal, otherwise an unsubmitted capture could never safely be reused.
void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    for (auto& slot : slots)
        if (slot.pending && !slot.submitted)
            for (UINT i = 0; i < count; ++i)
                if (lists[i] == slot.producer)
                {
                    // Signal after ExecuteCommandLists, never when merely recording the copy.
                    slot.submitted = true;
                    if (slot.expired) slot.pending = false; // still signal: protect an unsubmitted expired capture
                    slot.producerQueue = RealQueue(queue);
                    if (FAILED(queue->Signal(slot.fence.Get(), slot.ready)))
                    {
                        // The copy already executed. Keep its unsignalled fence protecting
                        // the slot instead of treating this as a discarded recording.
                        slot.pending = false;
                        Say("The graphics queue stopped. Restart the game to retry.");
                    }
                    else
                        slot.gated = slot.gate && SUCCEEDED(queue->Signal(slot.gate.Get(), slot.ready));
                    break;
                }
}

} // namespace Late

void FinishedPictureResetCommandList(ID3D12CommandList* cmd)
{
    // Nearly every reset is of a list NR never recorded on: no lock for those (DlssNr_WatchedLists.h)
    const void* list = cmd;
    if (!DlssNrWatchedLists::Any(1, &list)) return;

    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    DlssNrWatchedLists::Remove(1, &list);
    NrLifetime().ResetRecording(cmd);
    SuperiorRun::ResetRecording(cmd);
    if (g_gpuTime) g_gpuTime->ResetRecording(cmd);
    if (g_ngxTime) g_ngxTime->ResetRecording(cmd);
    if (g_scanTime) g_scanTime->ResetRecording(cmd);
    // Reuse's coverage readbacks: a recording thrown away frees its slot, since no fence will ever be signalled for it.
    DetailReuse::ResetRecording(cmd);
    SceneCut::ResetRecording(cmd); // the scene-cut flag's readbacks, the same way
    if (!Late::tracking.load()) return;
    for (auto& slot : Late::slots)
        if (slot.pending && !slot.submitted && slot.producer == cmd)
        {
            slot.pending = false;
            slot.done = slot.ready - 1; // discarded recording: no GPU signal was promised
            Late::reset = true;
        }
}

bool WaitForFinishedPicture()
{
    if (!Late::tracking.load()) return true;
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    Late::Cancel();
    for (auto& slot : Late::slots)
    {
        // A capture-only slot signals on the producer queue, which may itself wait on a present.
        if (!slot.submitted || !TouchedBackBuffer(slot.ready, slot.done) || Late::Finished(slot)) continue;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        const auto hr = slot.fence->SetEventOnCompletion(slot.done, event);
        const bool finished = SUCCEEDED(hr) && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
        CloseHandle(event);
        if (!finished) return false;
    }
    return true;
}

std::string FinishedPictureStatus()
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    return Late::status;
}

void FinishedPictureSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    // Every submit in the process comes here, on any thread: only one with a list NR recorded on takes the lock NR
    // holds while it records (DlssNr_WatchedLists.h)
    const auto watched = reinterpret_cast<const void* const*>(lists);
    if (!DlssNrWatchedLists::Any(count, watched)) return;

    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    DlssNrWatchedLists::Remove(count, watched);
    NrLifetime().Submitted(queue, count, lists);
    // A list stays open until it is reset (or replayed): its reset must still reach NrLifetime.
    for (UINT i = 0; i < count; ++i)
        if (NrLifetime().Open(lists[i]))
            DlssNrWatchedLists::Add(lists[i]);
    SuperiorRun::Submitted(queue, count, lists);
    if (g_gpuTime) g_gpuTime->Submitted(queue, count, lists);
    if (g_ngxTime) g_ngxTime->Submitted(queue, count, lists);
    if (g_scanTime) g_scanTime->Submitted(queue, count, lists);
    // Reuse's coverage readbacks: a copy is safe to read once the list it was recorded on has been executed.
    DetailReuse::Submitted(queue, count, lists);
    SceneCut::Submitted(queue, count, lists); // the scene-cut flag's readbacks, the same way
    if (!Late::tracking.load()) return;
    Late::Submitted(queue, count, lists);
}

void FinishedPictureColorSpace(IDXGISwapChain* swapchain, DXGI_COLOR_SPACE_TYPE colorSpace)
{
    swapchain->SetPrivateData(Late::colorSpaceKey, sizeof(colorSpace), &colorSpace);
}

// An HDR10 (PQ) picture goes to NR as linear scRGB and comes back as PQ. Both finished-picture paths (the game's swap chain
// above and native input below) record the two conversions here, so they cannot drift. `pictureState` is the state the
// picture is in on entry and is left in; `linear` rests in UNORDERED_ACCESS before the decode's result is read.

// PQ picture -> `linear` (R16G16B16A16_FLOAT). False when the pass could not run.
static bool RecordPqDecode(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, D3D12_RESOURCE_STATES pictureState,
                           ID3D12Resource* linear, const DlssNrConstants& conversion)
{
    Barrier(cmd, color, pictureState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool decoded = g_compose->DispatchResidualPass(cmd, conversion, color, nullptr, nullptr, nullptr, linear);
    Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, pictureState);
    return decoded;
}

// `linear` (NR's result) -> PQ over `color`, through `encoded` (the picture's own format). The original picture is the
// second input of the pass (what the encode keeps of it). `linear` is left in UNORDERED_ACCESS.
static void RecordPqEncode(ID3D12GraphicsCommandList* cmd, ID3D12Resource* linear, ID3D12Resource* color,
                           D3D12_RESOURCE_STATES pictureState, ID3D12Resource* encoded, const DlssNrConstants& decoded,
                           bool privateGraphicsList = false)
{
    // The native/virtual-input callers share the game's list: leave all graphics state alone there.
    const auto pictureDesc = color->GetDesc();
    if (privateGraphicsList && pictureDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
        (pictureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 &&
        (pictureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0 &&
        g_compose->RecordFinishedPqRaster(cmd, linear, color, pictureState)) return;
    DlssNrConstants conversion = decoded;
    conversion.Mode = 1;
    Barrier(cmd, linear, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if ((pictureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 &&
        g_compose->FinishedPqDirectReady(pictureDesc.Format))
    {
        // Same per-pixel PQ math and alpha preservation; retain the separate path
        // when typed UAV loads/stores or PSO creation are unavailable.
        conversion.Mode = 6;
        Barrier(cmd, color, pictureState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool direct = g_compose->DispatchResidualPass(cmd, conversion, linear, nullptr, nullptr, nullptr, color);
        Barrier(cmd, color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pictureState);
        if (direct && pictureState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER order {};
            order.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            order.UAV.pResource = color;
            cmd->ResourceBarrier(1, &order);
        }
        if (direct)
        {
            Barrier(cmd, linear, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return;
        }
        conversion.Mode = 1;
    }
    Barrier(cmd, color, pictureState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (g_compose->DispatchResidualPass(cmd, conversion, linear, nullptr, color, nullptr, encoded))
    {
        Barrier(cmd, encoded, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(color, encoded);
        Barrier(cmd, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, encoded, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, pictureState);
    Barrier(cmd, linear, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// Shared production slot selection; application captures expire at the app Present boundary,
// ordinary lower presents retain their short presentation-epoch window.
static FinishedPick SelectFinishedInput(ID3D12CommandQueue* queue, const D3D12_RESOURCE_DESC& desc,
                                       bool residualOnly, bool gameFrameHandoff, uint64_t snapshot, size_t& index,
                                       std::array<FinishedInput, std::tuple_size_v<decltype(Late::slots)>>& inputs)
{
    const auto epoch = State::Instance().frameCount;
    auto* presentQueue = Late::RealQueue(queue);
    for (size_t i = 0; i < Late::slots.size(); ++i)
    {
        auto& slot = Late::slots[i];
        if (!slot.pending || slot.expired || !slot.submitted || (gameFrameHandoff && slot.serial > snapshot))
            continue;
        // Counted in presents, and frame generation adds some per game frame; a missed game frame is FinishedHistoryGap's job
        if (!gameFrameHandoff && FinishedStale(epoch, slot.frame.SubmissionEpoch, Late::PresentsPerGameFrame()))
        {
            Late::reset |= slot.frame.Reset; // dropped unapplied: keep the game's reset flag
            slot.expired = true;
            if (slot.submitted) slot.pending = false;
            continue;
        }
        inputs[i] = { slot.residualOnly == residualOnly && slot.frame.OutputWidth == desc.Width &&
                          slot.frame.OutputHeight == desc.Height,
                      slot.serial, slot.submitted && slot.producerQueue.Get() == presentQueue,
                      slot.submitted ? slot.fence->GetCompletedValue() : 0, slot.ready };
    }
    // No wait is queued here: a present on another queue waits on the slot's gate, under the watchdog (ApplyFinishedColor).
    return PickFinishedInput(inputs.data(), inputs.size(), index);
}

// The caller holds g_nrMutex. Handoff callers read all swapchain metadata before entering
// this lock: Streamline can hold its own lock while submitting commands that take g_nrMutex.
static void ApplyFinishedColor(ID3D12Resource* picture, ID3D12CommandQueue* queue, DXGI_COLOR_SPACE_TYPE colorSpace,
                               bool gameFrameHandoff, const D3D12_RESOURCE_DESC& desc, uint64_t snapshot)
{
    if (!Config::Instance()->DlssNrFinishedPicture.value_or_default() ||
        !Config::Instance()->DlssNrEnabled.value_or_default())
    {
        Late::Cancel();
        return;
    }
    if (!picture || !queue || State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
        return;
    Late::QuarantineAbandoned();
    // Nothing captured from the game's own upscaler is waiting: leave the history alone. With native input the
    // picture is owned by ApplyNativeInput, and rejecting a swapchain format here (B8G8R8A8 in World of Warcraft)
    // would Cancel() and reset the shared history on every present.
    const bool captured =
        std::any_of(Late::slots.begin(), Late::slots.end(), [](const auto& slot) { return slot.pending && slot.submitted; });
    // Before the early returns, so the settle sees every present, also those with nothing captured
    const bool gpuWaitAllowed = Late::GpuWaitAllowed(captured);
    if (!captured)
        return;
    Late::ComPtr<ID3D12Resource> color = picture;
    Late::ComPtr<ID3D12Device> currentDevice;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&currentDevice))) || currentDevice != Late::device)
        return;
    const bool screenPq = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const bool screenScrgb = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    const bool screenSdr = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    // [DlssNr] ColourEncoding: Auto keeps the colour-space rule; a forced choice picks the path, which is what lets
    // an FP16 buffer that holds gamma values through as SDR (DlssNr_ColourEncoding.h).
    using DlssNrColourEncoding::Screen;
    const auto screenChoice = DlssNrColourEncoding::ResolveFinished(
        Config::Instance()->DlssNrColourEncoding.value_or_default(), screenPq || screenScrgb || screenSdr,
        screenPq ? Screen::Pq : screenScrgb ? Screen::Scrgb : Screen::Sdr,
        desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        // BGRA too (RDR2 on D3D12, most 8-bit swapchains): with RGBA8, RGB10A2 and RGBA16F these are every format a
        // flip-model swapchain can have. The pass reads the picture through views, which order the channels, and its
        // scratch copies take the picture's own format, as for native input below.
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
            desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    DlssNr::ReportColourEncoding(screenChoice.choice, DiagFormatName(desc.Format), "finished picture", true);
    if (!screenChoice.supported)
    {
        Late::Cancel();
        Late::Say((std::string("This screen colour format is not supported (") + DiagFormatName(desc.Format) + ").").c_str());
        return;
    }
    const bool pq = screenChoice.screen == Screen::Pq;
    const bool scrgb = screenChoice.screen == Screen::Scrgb;
    const auto epoch = State::Instance().frameCount;
    const bool residualOnly = Config::Instance()->DlssNrRunBeforeSr.value_or_default() ||
                              Config::Instance()->DlssNrDeferredDlss.value_or_default();
    std::array<FinishedInput, std::tuple_size_v<decltype(Late::slots)>> inputs {};
    size_t index = 0;
    const auto pick = SelectFinishedInput(queue, desc, residualOnly, gameFrameHandoff, snapshot, index, inputs);
    if (pick == FinishedPick::None)
        return; // loading screen, another swapchain, or this real frame was already consumed
    // Rendered on another queue and not finished yet: the present queue waits for it on the GPU (DlssNr_CrossQueueGate.h)
    // The wait is watched before it is queued (a wait the watchdog could not watch is never queued), and queued before
    // anything is recorded: a later failure below leaves only a wait the gate's own signal ends.
    auto& waiting = Late::slots[index];
    if (pick == FinishedPick::NotReady && !inputs[index].sameQueue && inputs[index].completed != UINT64_MAX &&
        waiting.gated && gpuWaitAllowed && Late::Gate().Enabled() && Late::Gate().Watch(waiting.gate.Get(), waiting.ready, waiting.serial) &&
        SUCCEEDED(queue->Wait(waiting.gate.Get(), waiting.ready)))
    {
        if (!Late::loggedCrossQueue)
        {
            Late::loggedCrossQueue = true;
            LOG_INFO("DLSS-NR finished picture: the game renders on a different queue than it presents on; "
                     "the present waits on the GPU for the frame's render work");
        }
    }
    else if (pick == FinishedPick::NotReady)
    {
        // Ordinary presents may retry within their epoch window. App handoffs expire at EndStreamlinePicture.
        // Sustained skips are the history gap rule's; a removed device resets as before
        const auto& input = inputs[index];
        if (gameFrameHandoff || input.completed == UINT64_MAX)
            Late::reset = true;
        if (!input.sameQueue && input.completed != UINT64_MAX && !Late::loggedCrossQueue)
        {
            Late::loggedCrossQueue = true;
            LOG_INFO("DLSS-NR finished picture: the game renders on a different queue than it presents on; "
                     "frames still rendering at present are skipped instead of waited for");
        }
        // Say changes are logged: only after a sustained run, not on every alternation.
        if (++Late::stillRendering == 120)
            Late::Say("Skipping frames: the game's picture is still rendering when it is presented.");
        return;
    }
    Late::stillRendering = 0;
    auto& slot = Late::slots[index];
    // Drop other submitted evaluates from this picture, not their in-flight resources.
    for (auto& other : Late::slots)
        if (other.submitted && other.serial <= slot.serial)
        {
            if (&other != &slot && other.pending)
                Late::reset |= other.frame.Reset; // the game's reset flag outlives the capture it came with
            other.pending = false;
        }
    // Same queue: orders after the producer's signal. Another queue: already complete, or the GPU wait below.
    if (FAILED(slot.allocator->Reset()) ||
        FAILED(slot.commands->Reset(slot.allocator.Get(), nullptr)))
    {
        Late::reset = true;
        Late::Say("Could not prepare the finished picture.");
        return;
    }
    auto* cmd = slot.commands.Get();
    if (!g_compose)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", Late::device.Get());
    const auto before = g_nr.successfulDispatches;
    bool appliedResidual = false;
    if (slot.residualOnly)
    {
        if (slot.encoded && (slot.encoded->GetDesc().Width != desc.Width ||
            slot.encoded->GetDesc().Height != desc.Height || slot.encoded->GetDesc().Format != desc.Format))
            slot.encoded.Reset();
        if (!slot.encoded)
            slot.encoded.Attach(CreateScratch(Late::device.Get(), desc.Format, (unsigned) desc.Width, desc.Height));
        if (slot.encoded && Config::Instance()->DlssNrApplyModel.value_or_default())
        {
            Barrier(cmd, slot.residual.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            DlssNrConstants apply {};
            apply.Mode = pq ? 4 : scrgb ? 3 : 2;
            apply.Width = (unsigned) desc.Width; apply.Height = desc.Height;
            apply.WhitePoint = slot.frame.PreExposure;
            apply.TransferStrength = slot.sceneLinear ? 1.0f : 0.0f;
            apply.MaxRatio = std::clamp(Config::Instance()->DlssNrMaxRatio.value_or_default(), 1.0f, 8.0f);
            appliedResidual = g_compose->DispatchResidualPass(cmd, apply, color.Get(), nullptr,
                slot.residual.Get(), nullptr, slot.encoded.Get());
            if (appliedResidual)
            {
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(color.Get(), slot.encoded.Get());
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
            Barrier(cmd, slot.residual.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
    else
    {
        auto frame = slot.frame;
        frame.ColourIsLinearHdr = pq || scrgb;
        frame.InputEncoding = screenChoice.shaderConversion; // gamma 2.2 re-encoded as sRGB by the shared pass
        frame.ColourEncoding = (uint32_t) screenChoice.choice.encoding;
        frame.ColourEncodingForced = !screenChoice.choice.automatic;
        // Absolute display encodings: white at the game's HDR paper white (203 nits by default), in 80-nit scRGB units
        // (the PQ conversion above writes scRGB too).
        frame.WhitePointOverride =
            (pq || scrgb) ? DlssNrExposureState::PinnedWhitePoint(
                                Config::Instance()->DlssNrDisplayPaperWhiteNits.value_or_default(),
                                DlssNrExposureState::kNitsPerScrgbUnit)
                          : 0.0f;
        // slot.frame.SubmissionEpoch is still the capture's; frame's is overwritten with this present's just below
        frame.Reset |= Late::reset || (!gameFrameHandoff && FinishedHistoryGap(Late::lastAppliedEpoch,
                                                                              slot.frame.SubmissionEpoch,
                                                                              Late::PresentsPerGameFrame()));
        frame.SubmissionEpoch = epoch;
        if (slot.depth) Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DlssNrNative::SetPrecision(Config::Instance()->DlssNrPrecision.value_or_default());
        ID3D12Resource* nrColor = color.Get();
        bool colorReady = true;
        DlssNrConstants conversion {};
        conversion.Width = (unsigned) desc.Width;
        conversion.Height = desc.Height;
        if (pq)
        {
            auto ensure = [&](Late::ComPtr<ID3D12Resource>& resource, DXGI_FORMAT format) {
                if (resource && (resource->GetDesc().Width != desc.Width || resource->GetDesc().Height != desc.Height ||
                                 resource->GetDesc().Format != format)) resource.Reset();
                if (!resource) resource.Attach(CreateScratch(Late::device.Get(), format, (unsigned) desc.Width, desc.Height));
                return resource != nullptr;
            };
            colorReady = ensure(slot.linear, DXGI_FORMAT_R16G16B16A16_FLOAT) && ensure(slot.encoded, desc.Format);
            if (colorReady)
            {
                colorReady = RecordPqDecode(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT, slot.linear.Get(), conversion);
                // Dispatch reads the converted colour; its transition out of UAV orders the conversion.
                nrColor = slot.linear.Get();
                frame.OutputArrivalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
        }
        if (colorReady)
            g_compose->Dispatch(cmd, nrColor, slot.depth.Get(), slot.motion.Get(), nrColor, frame, queue);
        if (pq && colorReady && g_nr.successfulDispatches > before && Config::Instance()->DlssNrApplyModel.value_or_default())
        {
            RecordPqEncode(cmd, slot.linear.Get(), color.Get(), D3D12_RESOURCE_STATE_PRESENT, slot.encoded.Get(),
                           conversion, true);
        }
        Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (slot.depth) Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (FAILED(cmd->Close()))
    {
        Late::Say("Could not finish the picture. Restart the game to retry.");
        slot.pending = true; // quarantine the slot; do not reuse possibly recorded NR resources
        slot.submitted = false;
        return;
    }
    // Each finished-picture slot resets its list before recording again. It may remain in the pool unused
    // after a mode change, so Superior can retire this recording after the GPU fence without waiting for that reset.
    SuperiorRun::MarkSingleSubmit(cmd);
    ID3D12CommandList* lists[] = { cmd };
    queue->ExecuteCommandLists(1, lists);
    slot.done = slot.ready + 1;
    if (FAILED(queue->Signal(slot.fence.Get(), slot.done)))
    {
        Late::Say("The graphics queue stopped. Restart the game to retry.");
        return;
    }
    const bool ran = slot.residualOnly ? appliedResidual : g_nr.successfulDispatches > before;
    Late::reset = !ran;
    if (ran)
        Late::lastAppliedEpoch = slot.frame.SubmissionEpoch;
    Late::Say(!Config::Instance()->DlssNrApplyModel.value_or_default() ? "NR changes are hidden."
              : ran ? (slot.residualOnly  ? "Applying the pre-SR changes to the finished picture."
                       : gameFrameHandoff ? "Applying NR before Streamline interpolation."
                                          : "Applying NR to the finished picture.")
                    : "Preparing NR for the finished picture.");
    if (ran && (++Late::successes == 1 || Late::successes % 300 == 0))
        LOG_INFO("DLSS-NR finished picture: {} frames, {}x{}, FG {}", Late::successes,
                 desc.Width, desc.Height, State::Instance().currentFG &&
                 State::Instance().currentFG->IsActive() && !State::Instance().currentFG->IsPaused());
}

DXGI_COLOR_SPACE_TYPE NativeInputColourSpace(IDXGISwapChain* swapchain, DXGI_FORMAT format)
{
    auto colorSpace = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT colorSpaceSize = sizeof(colorSpace);

    if (swapchain != nullptr)
        swapchain->GetPrivateData(Late::colorSpaceKey, &colorSpaceSize, &colorSpace);

    return colorSpace;
}

// Ordinary picture lookup, shared by the unregistered-chain path and Streamline's FG-off
// pass-through. Swapchain calls precede g_nrMutex: the runtime can hold its presentation lock
// while submitting commands that take the NR lock, so querying it under that lock can deadlock.
static void ApplyFinishedSwapchain(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue, bool passThrough,
                                  uint64_t snapshot = UINT64_MAX)
{
    if (State::Instance().isShuttingDown)
        return;
    const auto& cfg = *Config::Instance();
    Late::ComPtr<IDXGISwapChain3> sc;
    Late::ComPtr<ID3D12Resource> color;
    D3D12_RESOURCE_DESC desc {};
    auto space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (cfg.DlssNrFinishedPicture.value_or_default() && cfg.DlssNrEnabled.value_or_default())
    {
        if (!swapchain || !queue || State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
            return;
        if (!passThrough && StreamlinePicture::Owns(swapchain))
            return; // registered app-facing Present owns this chain; exclude all lower/generated presents
        if (StreamlinePicture::NativeFgRunning())
        {
            FinishedPictureUnavailable("Paused: native frame generation has no application-picture handoff.");
            return;
        }
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc))) ||
            FAILED(SdrToHdr::GameBuffer(sc.Get(), sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color))))
            return;
        desc = color->GetDesc();
        space = NativeInputColourSpace(swapchain, desc.Format);
    }
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    ApplyFinishedColor(color.Get(), queue, space, passThrough, desc, snapshot);
}

void FinishedPictureUnavailable(const char* message)
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    Late::Cancel();
    Late::Say(message);
}

void FinishedPictureNote(const char* message)
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    Late::Say(message);
}

uint64_t BeginStreamlinePicture()
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    // Only captures already submitted at this boundary belong to the presented picture.
    // A worker may have armed the next frame before Present; leave it for the next boundary.
    uint64_t snapshot = 0;
    for (const auto& slot : Late::slots)
        if (slot.pending && slot.submitted && !slot.expired)
            snapshot = std::max(snapshot, slot.serial);
    return snapshot;
}

void EndStreamlinePicture(uint64_t snapshot, const char* unavailable)
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    // Newer worker recordings/submissions are for a later app picture. Only expire the
    // interval selected at Begin, even if another capture arrived while DXGI was queried.
    for (auto& slot : Late::slots)
        if (slot.serial <= snapshot)
        {
            if (slot.pending && !slot.expired)
                Late::reset |= slot.frame.Reset; // dropped unapplied: keep the game's reset flag
            slot.expired = true;
            if (slot.submitted) slot.pending = false;
        }
    Late::reset |= Late::successes == Late::lastApplicationSuccesses;
    Late::lastApplicationSuccesses = Late::successes;
    if (unavailable)
    {
        Late::reset = true;
        Late::Say(unavailable);
    }
}

void ApplyToFinishedPicture(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue)
{
    ApplyFinishedSwapchain(swapchain, queue, false);
}

void ApplyToStreamlinePicture(IDXGISwapChain* swapchain, ID3D12Resource* picture, ID3D12CommandQueue* queue,
                              uint64_t snapshot)
{
    if (State::Instance().isShuttingDown || !swapchain || !picture || !queue)
        return;
    const auto desc = picture->GetDesc();
    const auto space = NativeInputColourSpace(swapchain, desc.Format);
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    ApplyFinishedColor(picture, queue, space, true, desc, snapshot);
}

void ApplyToStreamlinePassthrough(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue, uint64_t snapshot)
{
    // Only the real app-facing Present calls this with FG off. Keep registration intact so
    // asynchronous/generated presents remain excluded when FG restarts.
    ApplyFinishedSwapchain(swapchain, queue, true, snapshot);
}

// The body of ApplyNativeInput and ApplyVirtualUpscalerInput. `fromVirtualUpscaler` skips the Finished Picture setting,
// which is about NR Only mode, not the picture (NR + frame generation keeps it off, for the game's own upscaler
// calls). Neither refuses while frame generation has replaced a D3D11 or Vulkan game's swap chain: both then run from that
// swap chain's present on the paired D3D12 queue, whose picture is the one presented. (The first NR Only read the game's
// D3D12 swap chain at present, which under that interop is frame generation's, and so refused it.)
// `game`: the picture is a Vulkan game's finished picture and `depth` and `motion` are the guides its own upscaler call was
// given (ApplyFinishedGameGuides), at the game's allocation and resting in COMMON; the frame then carries the game's flags,
// sizes, subrects and MV scale as the D3D12 capture's slot does (Late::Capture), and everything else is this function's.
static bool ApplyFinishedPictureInput(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                                      ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* historyDistrust,
                                      bool depthReversed, bool reset, DXGI_COLOR_SPACE_TYPE colorSpace,
                                      D3D12_RESOURCE_STATES pictureState, unsigned long long epoch,
                                      bool fromVirtualUpscaler, const FinishedGameFrame* game = nullptr)
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    if ((!fromVirtualUpscaler && !Config::Instance()->DlssNrFinishedPicture.value_or_default()) ||
        !Config::Instance()->DlssNrEnabled.value_or_default())
    {
        Late::Say(fromVirtualUpscaler ? "Enable Neural Rendering to use it on the game's picture."
                                      : "Native input needs Finished picture and Enable Neural Rendering on.");
        return false;
    }
    // Depth is optional: NR runs without it.
    if (!queue || !cmd || !color || !motion)
        return false;
    if (StreamlinePicture::NativeFgRunning())
    {
        Late::Say("Paused: native frame generation requires an application-picture handoff.");
        return false;
    }
    Late::ComPtr<ID3D12Device> currentDevice;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&currentDevice))))
        return false;
    if (Late::device && Late::device != currentDevice)
    {
        Late::Say("The graphics device changed. Restart the game to use this option.");
        return false;
    }
    Late::device = currentDevice;
    const auto desc = color->GetDesc();
    const bool screenPq = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const bool screenScrgb = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    const bool screenSdr = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    using DlssNrColourEncoding::Screen;
    const auto screenChoice = DlssNrColourEncoding::ResolveFinished(
        Config::Instance()->DlssNrColourEncoding.value_or_default(), screenPq || screenScrgb || screenSdr,
        screenPq ? Screen::Pq : screenScrgb ? Screen::Scrgb : Screen::Sdr,
        desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        // BGRA too: a Vulkan swapchain is usually B8G8R8A8 (DOOM Eternal's), and Optical F5Low's shared picture keeps
        // it, since swapping the channels on the game's queue needs a blit and the game may present from a compute queue
        // (a blit there is a GPU fault). The pass reads it through views, which order the channels, and its scratch
        // copies take the picture's own format.
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
            desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    DlssNr::ReportColourEncoding(screenChoice.choice, DiagFormatName(desc.Format), "native input", true);
    if (!screenChoice.supported)
    {
        Late::Say((std::string("This screen colour format is not supported (") + DiagFormatName(desc.Format) + ").").c_str());
        return false;
    }
    const bool pq = screenChoice.screen == Screen::Pq;
    const bool scrgb = screenChoice.screen == Screen::Scrgb;
    {
        // The encoding report above only logs a mismatch: say once per change what the picture is taken for.
        static int lastScreen = -1;
        if (lastScreen != (int) screenChoice.screen)
        {
            lastScreen = (int) screenChoice.screen;
            LOG_INFO("DLSS-NR native input colour space: {}", pq ? "HDR10 (PQ)" : scrgb ? "scRGB" : "SDR");
        }
    }
    // HDR10 goes to NR as linear scRGB and back, through the scratch pair (the same two passes as the game's swap chain
    // path, recorded by the same helpers).
    if (pq && !Late::EnsureNativeHdr(desc, epoch))
    {
        Late::Say("Could not make the HDR10 buffers. Restart the game to retry.");
        return false;
    }
    if (!g_compose)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", Late::device.Get());

    DlssNrFrameInfo frame {};
    frame.DepthInverted = depthReversed;
    frame.MvScaleX = 1.0f;
    frame.MvScaleY = 1.0f;
    frame.ColourIsLinearHdr = scrgb || pq;
    frame.InputEncoding = screenChoice.shaderConversion;
    frame.ColourEncoding = (uint32_t) screenChoice.choice.encoding;
    frame.ColourEncodingForced = !screenChoice.choice.automatic;
    // Absolute display encodings: white at the game's HDR paper white, in 80-nit scRGB units (the PQ decode writes scRGB too)
    frame.WhitePointOverride =
        (scrgb || pq) ? DlssNrExposureState::PinnedWhitePoint(Config::Instance()->DlssNrDisplayPaperWhiteNits.value_or_default(),
                                                      DlssNrExposureState::kNitsPerScrgbUnit)
              : 0.0f;
    frame.Reset = reset || Late::nativeReset;
    frame.OutputWidth = (unsigned) desc.Width;
    frame.OutputHeight = desc.Height;
    frame.FinishedPicture = true;
    // Optical F5Low measured the motion on this picture, so detail reuse may move detail with it.
    frame.MotionMatchesPicture = true;
    frame.HistoryDistrust = historyDistrust;
    if (game)
    {
        // The game's own guides, described as the D3D12 capture describes them. The motion is the game's, rendered for its
        // scene, not measured on this picture. A gap of more than two game frames between two applied pictures, or none
        // applied yet, starts NR's history over, as FinishedHistoryGap does for the D3D12 slots.
        if (game->outputWidth != desc.Width || game->outputHeight != desc.Height)
        {
            Late::Say("The game's upscaler output and the picture differ in size.");
            return false;
        }
        // Which weights this pass builds its model from: the selector (DlssNr::Weights, [DlssNr] WeightsSetA / WeightsSetB) reaches the model at
        // its creation on this device, as for any D3D12 game
        static std::string saidWeights = "\x01";
        const std::string weightsSet =
            DlssNr::WeightsRules::BuiltKey(Config::Instance()->DlssNrWeightsSetA.value_or_default(),
                                           Config::Instance()->DlssNrWeightsSetB.value_or_default());
        if (saidWeights != weightsSet)
        {
            saidWeights = weightsSet;
            LOG_INFO("DLSS-NR finished picture (Vulkan game): model weights {}",
                     weightsSet == "|" ? std::string("stock") : "sets \"" + weightsSet + "\" (A|B)");
        }
        frame.DepthInverted = game->depthInverted;
        frame.MvScaleX = game->mvScaleX;
        frame.MvScaleY = game->mvScaleY;
        frame.MotionVectorsLowResolution = game->mvLowRes;
        frame.RayReconstruction = game->rayReconstruction;
        frame.RenderSubrectWidth = game->renderWidth;
        frame.RenderSubrectHeight = game->renderHeight;
        frame.DepthSubrectBaseX = game->depthBaseX;
        frame.DepthSubrectBaseY = game->depthBaseY;
        frame.MotionSubrectBaseX = game->motionBaseX;
        frame.MotionSubrectBaseY = game->motionBaseY;
        frame.MotionMatchesPicture = false;
        frame.HistoryDistrust = nullptr;
        frame.Reset = game->reset || Late::nativeReset || FinishedHistoryGap(Late::gameGuidesApplied, game->captureEpoch, 1);
    }
    frame.IndependentCommands = true;
    frame.OutputArrivalState = pictureState;
    // The first evaluate waits for the epoch to move past the frame the model was made on, and detail reuse numbers
    // frames by it (a step over 1 is a skipped frame). The caller's present count (native::PresentEpoch): each comes
    // after the frame source executed the previous frame's list.
    frame.SubmissionEpoch = epoch;

    DlssNrNative::SetPrecision(Config::Instance()->DlssNrPrecision.value_or_default());
    const auto before = g_nr.successfulDispatches;
    ID3D12Resource* nrColor = color;
    DlssNrConstants conversion {};
    conversion.Width = (unsigned) desc.Width;
    conversion.Height = desc.Height;

    if (pq)
    {
        if (!RecordPqDecode(cmd, color, pictureState, Late::nativeHdr.linear.Get(), conversion))
        {
            Late::Say("Could not convert the HDR10 picture.");
            return false;
        }

        // Dispatch reads the converted colour; its transition out of UAV orders the conversion.
        nrColor = Late::nativeHdr.linear.Get();
        frame.OutputArrivalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // A Vulkan game's guides come from images shared across APIs (resting in COMMON); the pass reads them as the D3D12
    // capture's copies, which rest in NON_PIXEL_SHADER_RESOURCE
    if (game)
    {
        if (depth)
            Barrier(cmd, depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    g_compose->Dispatch(cmd, nrColor, depth, motion, nrColor, frame, queue);
    const bool ran = g_nr.successfulDispatches > before;

    if (pq && ran && Config::Instance()->DlssNrApplyModel.value_or_default())
        RecordPqEncode(cmd, nrColor, color, pictureState, Late::nativeHdr.encoded.Get(), conversion);

    if (game)
    {
        if (depth)
            Barrier(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cmd, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (ran)
            Late::gameGuidesApplied = game->captureEpoch;
    }

    Late::nativeReset = !ran;
    Late::Say(!Config::Instance()->DlssNrApplyModel.value_or_default() ? "NR changes are hidden." :
        game ? (ran ? "Applying NR to the finished picture." : "Preparing NR for the finished picture.") :
        ran ? "Applying NR to the finished picture (native input)." : "Preparing NR for the finished picture (native input).");
    if (ran && (++Late::successes == 1 || Late::successes % 300 == 0))
        LOG_INFO("DLSS-NR {}: {} frames, {}x{}", fromVirtualUpscaler ? "on the virtual upscaler's picture" : "native input",
                 Late::successes, desc.Width, desc.Height);
    return ran;
}

void NativeInputSkipped()
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    Late::nativeReset = true;
    Late::gameGuidesApplied = 0;
}

bool ApplyFinishedGameGuides(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                             ID3D12Resource* depth, ID3D12Resource* motion, const FinishedGameFrame& game,
                             DXGI_COLOR_SPACE_TYPE colorSpace, D3D12_RESOURCE_STATES pictureState,
                             unsigned long long epoch)
{
    const bool ran = ApplyFinishedPictureInput(queue, cmd, color, depth, motion, nullptr, game.depthInverted, game.reset,
                                               colorSpace, pictureState, epoch, false, &game);
    if (!ran)
        NativeInputSkipped();
    return ran;
}

bool ApplyNativeInput(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                      ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* historyDistrust,
                      bool depthReversed, bool reset,
                      DXGI_COLOR_SPACE_TYPE colorSpace, D3D12_RESOURCE_STATES pictureState, unsigned long long epoch)
{
    // Any frame NR did not run on (switched off, refused, still preparing) is a gap its history must not reach across
    const bool ran = ApplyFinishedPictureInput(queue, cmd, color, depth, motion, historyDistrust, depthReversed, reset,
                                               colorSpace, pictureState, epoch, false);
    if (!ran)
        NativeInputSkipped();
    return ran;
}

bool ApplyVirtualUpscalerInput(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                               ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* historyDistrust,
                               bool depthReversed, bool reset, DXGI_COLOR_SPACE_TYPE colorSpace,
                               D3D12_RESOURCE_STATES pictureState, unsigned long long epoch)
{
    const bool ran = ApplyFinishedPictureInput(queue, cmd, color, depth, motion, historyDistrust, depthReversed, reset,
                                               colorSpace, pictureState, epoch, true);
    if (!ran)
        NativeInputSkipped();
    return ran;
}
