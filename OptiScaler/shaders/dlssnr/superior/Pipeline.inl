// Included by DlssNr_Dx12.cpp after janblade's model/resource helpers.
// Model creation, pass profiles, native/bridge entry points and the UI remain janblade's.
// Only Superior's input, inter-pass and output path lives here. Shader ABI/bytecode: v59.
namespace
{
namespace SuperiorRun
{
using Constants = DlssNr::Superior::DlssNrConstants;
using Microsoft::WRL::ComPtr;
constexpr auto Uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto Srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

template<class F> struct OnExit
{
    F action;
    ~OnExit() { action(); }
};
template<class F> OnExit(F) -> OnExit<F>;

struct Texture
{
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = Uav;
    ID3D12Resource* Get() const { return resource.Get(); }
    void To(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES next)
    {
        if (resource && state != next) Barrier(cmd, resource.Get(), state, next);
        state = next;
    }
};

struct Generation
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    unsigned width = 0, height = 0, workWidth = 0, workHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool inverted = false, beforeSr = false, reset = true;
    bool temporal = false, submitted = false, failed = false;
    int preset = 1;
    ID3D12GraphicsCommandList* creation = nullptr; // normalized identity only
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    Texture proxy, original, workProxy, depth, motion, interLow, interWorking, finalLow;
    Texture carrier, temporalOutput, temporalMotion, unit, meter, exposure, active, held;
    Texture nativeAnswer, finalColour, colourStats[2], colourHistory[2];
    bool colourHistoryValid = false;
    unsigned colourCursor = 0;
    std::unique_ptr<OS_Dx12> superUp, superDown;
    Scaler scaler = Scaler::Count;
    bool holding = false;
    float heldWhite = 1.0f;
    unsigned heldExposureSource = 0;
    float heldPreExposure = 1.0f;
    uint32_t expectedMask = 0, successfulMask = 0;
    unsigned previousPasses = 0;
    bool haveSchedule = false;
    ~Generation()
    {
        if (feature && NVNGXProxy::D3D12_ReleaseFeature())
            NVNGXProxy::D3D12_ReleaseFeature()(feature);
        if (parameters && NVNGXProxy::D3D12_DestroyParameters())
            NVNGXProxy::D3D12_DestroyParameters()(parameters);
    }
};

struct State
{
    DlssNr::GpuLifetime lifetime;
    std::shared_ptr<Generation> current;
    bool wasEnabled = false;
    std::string status;
    std::array<ID3D12CommandList*, 64> watched {};
};
// Same process-local ownership as janblade's g_nr. Keep unresolved GPU ownership at teardown.
State& GetState() { static auto* value = new State; return *value; }

void Say(const char* text)
{
    auto& s = GetState();
    if (s.status == text) return;
    s.status = text;
    LOG_INFO("Superior Scaling: {}", text);
}

void Record(ID3D12GraphicsCommandList* cmd)
{
    GetState().lifetime.Record(cmd);
    ID3D12GraphicsCommandList* real = nullptr;
    auto* identity = Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**) &real) ? real : cmd;
    DlssNrWatchedLists::Add(identity);
    auto& watched = GetState().watched;
    if (std::find(watched.begin(), watched.end(), identity) == watched.end())
        if (const auto empty = std::find(watched.begin(), watched.end(), nullptr); empty != watched.end())
            *empty = identity;
        else DlssNrWatchedLists::g_full = true; // conservative overflow: never miss a reset/submit

}

void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    auto& s = GetState();
    s.lifetime.Submitted(queue, count, lists);
    // Keep these identities watched until Reset, including replayed command lists.
    // The generic janblade timers stop watching after the first submission.
    for (UINT i = 0; i < count; ++i)
        if (std::find(s.watched.begin(), s.watched.end(), lists[i]) != s.watched.end())
            DlssNrWatchedLists::Add(lists[i]);
    if (s.current && !s.current->submitted)
        for (UINT i = 0; i < count; ++i)
            if (lists[i] == s.current->creation) s.current->submitted = true;
}

void ResetRecording(ID3D12CommandList* cmd)
{
    auto& s = GetState();
    s.lifetime.ResetRecording(cmd);
    for (auto& list : s.watched) if (list == cmd) list = nullptr;
    if (s.current && !s.current->submitted && s.current->creation == cmd)
    {
        auto old = std::move(s.current);
        s.lifetime.Retire([old] {});
        s.lifetime.BeginGeneration();
    }
}

bool Ensure(Texture& t, ID3D12Device* device, DXGI_FORMAT format, unsigned w, unsigned h)
{
    if (t.resource)
    {
        const auto d = t.resource->GetDesc();
        if (d.Width == w && d.Height == h && d.Format == format) return true;
        auto old = std::move(t.resource);
        GetState().lifetime.Retire([old] {});
    }
    t.state = Uav;
    t.resource.Attach(CreateScratch(device, format, w, h));
    return t.resource != nullptr;
}

bool Dispatch(DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, const Constants& c,
              ID3D12Resource* source, ID3D12Resource* model, ID3D12Resource* original,
              ID3D12Resource* motion, ID3D12Resource* previous, ID3D12Resource* low,
              Texture& output, Texture* keep = nullptr, uint32_t* immutableSlot = nullptr)
{
    output.To(cmd, Uav);
    if (keep) keep->To(cmd, Uav);
    if (!shader.DispatchSuperiorPass(cmd, c, source, model, original, motion, previous, low,
                                    output.Get(), keep ? keep->Get() : nullptr, immutableSlot)) return false;
    output.To(cmd, Srv);
    if (keep) keep->To(cmd, Srv);
    return true;
}

Constants GuideConstants(unsigned w, unsigned h, const DlssNr::GuideRegions& regions,
                         const DlssNrFrameInfo& frame, bool pixelUnits, bool hold)
{
    Constants c {};
    c.Mode = 10u;
    c.Width = w; c.Height = h;
    c.GuideWidth = regions.depth.width; c.GuideHeight = regions.depth.height;
    c.DebugView = regions.depth.x; c.CompareMode = regions.depth.y;
    c.TransferStrength = float(regions.motion.width); c.ColourStrength = float(regions.motion.height);
    c.CompareSwap = regions.motion.x; c.Transfer = regions.motion.y;
    c.MvScaleX = c.MvScaleY = 1.0f;
    if (pixelUnits)
    {
        const auto rw = frame.MotionVectorsLowResolution ? frame.RenderSubrectWidth : frame.OutputWidth;
        const auto rh = frame.MotionVectorsLowResolution ? frame.RenderSubrectHeight : frame.OutputHeight;
        c.MvScaleX = frame.MvScaleX * float(w) / std::max(rw ? rw : regions.motion.width, 1u);
        c.MvScaleY = frame.MvScaleY * float(h) / std::max(rh ? rh : regions.motion.height, 1u);
        if (hold) c.MvScaleX = c.MvScaleY = 0;
    }
    return c;
}

void ExposureConstants(Constants& c, const Config& cfg, unsigned source, float preExposure)
{
    c.ExposureMode = source;
    c.PreExposure = std::isfinite(preExposure) && preExposure > 1e-6f ? preExposure : 1.0f;
    const float trim = source == 3 ? cfg.DlssNrAutoExposureTrim.value_or_default()
                                 : cfg.DlssNrWhitePointTrim.value_or_default();
    c.ExposureTrim = std::isfinite(trim) ? std::clamp(trim, .001f, 1000.0f) : 1.0f;
    const float protection = cfg.DlssNrAutoExposureHighlightProtection.value_or_default();
    c.ExposureProtection = std::isfinite(protection) ? std::clamp(protection, 0.0f, 100.0f) : 0.0f;
    auto text = source == 3 ? cfg.DlssNrAutoExposureTrimAnchors.value_or_default()
                            : cfg.DlssNrGameExposureTrimAnchors.value_or_default();
    std::replace(text.begin(), text.end(), ';', ' ');
    std::istringstream input(text);
    std::array<std::pair<float, float>, 8> anchors {};
    unsigned count = 0;
    float point, value; char colon;
    while (count < anchors.size() && input >> point >> colon >> value)
        if (colon == ':' && std::isfinite(point) && point > 0 && std::isfinite(value) && value > 0)
            anchors[count++] = { point, std::clamp(value, .001f, 1000.0f) };
    std::stable_sort(anchors.begin(), anchors.begin() + count);
    for (unsigned i = 0; i < count; ++i)
    {
        if (i + 1 < count && anchors[i].first == anchors[i + 1].first) continue;
        c.ExposureAnchors[2 * c.ExposureAnchorCount] = anchors[i].first;
        c.ExposureAnchors[2 * c.ExposureAnchorCount++ + 1] = anchors[i].second;
    }
}

bool CreateTemporal(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd)
{
    Record(cmd); // includes unit-exposure initialization even if NGX creation fails
    const auto w = g.workWidth, h = g.workHeight;
    if (!Ensure(g.carrier, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, w, h) ||
        !Ensure(g.temporalOutput, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, w, h) ||
        !Ensure(g.temporalMotion, g.device.Get(), DXGI_FORMAT_R32G32_FLOAT, w, h) ||
        !Ensure(g.unit, g.device.Get(), DXGI_FORMAT_R32_FLOAT, 1, 1)) return false;
    Constants unit {}; unit.Mode = 7u; unit.Width = unit.Height = 1u;
    if (!Dispatch(shader, cmd, unit, g.proxy.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, g.unit)) return false;
    if (!NVNGXProxy::InitDx12(g.device.Get()) || !NVNGXProxy::D3D12_AllocateParameters() ||
        !NVNGXProxy::D3D12_DestroyParameters() || !NVNGXProxy::D3D12_CreateFeature() ||
        !NVNGXProxy::D3D12_EvaluateFeature() || !NVNGXProxy::D3D12_ReleaseFeature()) return false;
    if (NVNGXProxy::D3D12_AllocateParameters()(&g.parameters) != NVSDK_NGX_Result_Success || !g.parameters) return false;
    auto* p = g.parameters;
    p->Set(NVSDK_NGX_Parameter_Width, w); p->Set(NVSDK_NGX_Parameter_Height, h);
    p->Set(NVSDK_NGX_Parameter_OutWidth, w); p->Set(NVSDK_NGX_Parameter_OutHeight, h);
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u); p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int) NVSDK_NGX_PerfQuality_Value_DLAA);
    for (const char* key : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality }) p->Set(key, (unsigned) g.preset);
    const unsigned flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
        NVSDK_NGX_DLSS_Feature_Flags_AutoExposure | (g.inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    Record(cmd);
    const auto result = NVNGXProxy::D3D12_CreateFeature()(cmd, NVSDK_NGX_Feature_SuperSampling, p, &g.feature);
    ID3D12GraphicsCommandList* real = nullptr;
    g.creation = Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**) &real) ? real : cmd;
    return result == NVSDK_NGX_Result_Success && g.feature;
}

Constants Shaping(const Config& cfg, unsigned first, unsigned completed, float scale)
{
    Constants c {};
    c.ResidualHistoryValid = 1u;
    c.ResidualBlend = .015f;
    c.ResidualScale = 3.0f;
    c.ResidualConfidenceSensitivity = 1.0f;
    const auto gains = DlssNr::SuperiorScaling::BlockGains(cfg, first, completed, scale);
    c.MvScaleX = gains.high; c.MvScaleY = gains.low;
    c.GuideWidth = 1u;
    c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;
    return c;
}

bool InterPass(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, const Config& cfg,
               ID3D12Resource* originalBase, ID3D12Resource* answer, unsigned covered, unsigned completed, float scale)
{
    auto c = Shaping(cfg, covered, completed, scale);
    c.DirectResolveFlags |= 16u | 64u;
    if (c.MvScaleX == c.MvScaleY) { c.GuideWidth = 0u; c.DirectResolveFlags |= 32u; }
    ID3D12Resource* low = nullptr;
    if (c.GuideWidth)
    {
        if (!Ensure(g.interLow, g.device.Get(), g.format, (g.workWidth + 7u) / 8u, (g.workHeight + 7u) / 8u)) return false;
        Constants l {}; l.Mode = 34u; l.Width = (g.workWidth + 7u) / 8u; l.Height = (g.workHeight + 7u) / 8u;
        l.DirectResolveFlags = 32768u;
        if (!Dispatch(shader, cmd, l, originalBase, answer, nullptr, nullptr, nullptr, nullptr, g.interLow)) return false;
        low = g.interLow.Get();
    }
    if (!Ensure(g.interWorking, g.device.Get(), g.format, g.workWidth, g.workHeight)) return false;
    c.Mode = 36u; c.Width = g.workWidth; c.Height = g.workHeight;
    return Dispatch(shader, cmd, c, originalBase, answer, g.proxy.Get(), nullptr, nullptr, low, g.interWorking);
}

ID3D12Resource* Stabilize(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd,
                        ID3D12Resource* proxy, ID3D12Resource* answer, ID3D12Resource* originalMotion,
                        const DlssNr::GuideRegions& regions, const DlssNrFrameInfo& frame, bool reset, bool hold)
{
    auto encode = GuideConstants(g.workWidth, g.workHeight, regions, frame, true, hold);
    encode.Mode = 39u; // v50: fixed carrier + motion; reuse this frame's exact NR depth.
    if (!Dispatch(shader, cmd, encode, proxy, answer, originalMotion, nullptr, nullptr, nullptr,
                  g.carrier, &g.temporalMotion)) return nullptr;
    g.temporalOutput.To(cmd, Uav);
    auto* p = g.parameters;
    p->Set(NVSDK_NGX_Parameter_Color, g.carrier.Get());
    p->Set(NVSDK_NGX_Parameter_Output, g.temporalOutput.Get());
    p->Set(NVSDK_NGX_Parameter_Depth, g.depth.Get());
    p->Set(NVSDK_NGX_Parameter_MotionVectors, g.temporalMotion.Get());
    p->Set(NVSDK_NGX_Parameter_ExposureTexture, g.unit.Get());
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, g.workWidth);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, g.workHeight);
    p->Set(NVSDK_NGX_Parameter_Reset, (unsigned) (reset || g.reset));
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f); p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f); p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,
           std::isfinite(frame.FrameTimeMs) && frame.FrameTimeMs > 0 ? frame.FrameTimeMs : 16.67f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    if (NVNGXProxy::D3D12_EvaluateFeature()(cmd, g.feature, p, nullptr) != NVSDK_NGX_Result_Success)
    { g.reset = true; Say("Private DLSS evaluation failed; keeping the game frame."); return nullptr; }
    g.temporalOutput.To(cmd, Srv);
    Constants decode {}; decode.Mode = 38u; decode.Width = g.workWidth; decode.Height = g.workHeight;
    if (!Dispatch(shader, cmd, decode, g.temporalOutput.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, g.carrier))
        return nullptr;
    g.reset = false;
    return g.carrier.Get();
}

bool Run(DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Device* device,
         ID3D12Resource* target, ID3D12Resource* depth, ID3D12Resource* motion,
         const DlssNrFrameInfo& frame, const DlssNr::GuideRegions& regions,
         unsigned width, unsigned height, unsigned workWidth, unsigned workHeight, float scale,
         unsigned requestedPasses, ID3D12CommandQueue* queue, D3D12_RESOURCE_STATES arrival)
{
    const auto& cfg = *Config::Instance();
    auto& state = GetState();
    state.lifetime.Collect();
    const auto format = target->GetDesc().Format;
    const bool reduced = workWidth < width || workHeight < height;
    const bool temporal = reduced && !frame.BeforeUpscale;
    if (cfg.DlssNrReversibleMode.value_or_default() > 4u || frame.InputEncoding == 3u || frame.InputEncoding == 4u)
    {
        Say("Select an sRGB/linear input and a soft-knee, Neutwo or hybrid curve for Superior Scaling.");
        return false;
    }
    if (reduced && (frame.BeforeUpscale || !depth || !regions.depth.valid()))
    {
        Say("Below 100%, Superior Scaling requires post-SR NR and current depth/motion guides.");
        return false;
    }
    const int preset = DlssNr::SuperiorScaling::Preset(cfg.DlssNrSuperiorScalingPreset.value_or_default());
    auto* previous = state.current.get();
    if (previous && (previous->device.Get() != device || previous->width != width || previous->height != height ||
        previous->workWidth != workWidth || previous->workHeight != workHeight || previous->format != format ||
        previous->inverted != frame.DepthInverted || previous->beforeSr != frame.BeforeUpscale ||
        previous->temporal != temporal || previous->preset != preset || (queue && previous->queue.Get() != queue)))
    {
        auto old = std::move(state.current);
        state.lifetime.Retire([old] {});
        state.lifetime.BeginGeneration();
        g_nr.reset = true;
    }
    if (!state.current)
    {
        state.current = std::make_shared<Generation>();
        auto& g = *state.current;
        g.device = device; g.queue = queue; g.width = width; g.height = height;
        g.workWidth = workWidth; g.workHeight = workHeight; g.format = format;
        g.inverted = frame.DepthInverted; g.beforeSr = frame.BeforeUpscale; g.temporal = temporal; g.preset = preset;
    }
    auto& g = *state.current;
    if (g.failed) { Say("Private DLSS initialization failed; switch Superior Scaling off and on to retry."); return false; }
    if (!Ensure(g.proxy, device, format, width, height) || !Ensure(g.original, device, format, width, height) ||
        ((workWidth != width || workHeight != height) && !Ensure(g.workProxy, device, format, workWidth, workHeight)))
    { Say("Input resource allocation failed."); return false; }
    if (temporal && !g.feature)
    {
        g.failed = !CreateTemporal(g, shader, cmd);
        Say(g.failed ? "Private DLSS initialization failed." : "Waiting for private DLSS creation submission.");
        return false;
    }
    if (temporal && !g.submitted) return false;

    Record(cmd);
    unsigned passes = 1;
    if (g_nr.passScratch && g_nr.passClampScratch)
        for (unsigned p = 1; p < requestedPasses && g_nr.passFeature[p] && !g_nr.passPendingSubmission[p]; ++p) ++passes;
    const auto schedule = DlssNr::SuperiorScaling::Schedule(cfg, passes, scale);
    if (!g.haveSchedule || g.expectedMask != schedule.expectedMask || g.previousPasses != passes)
    {
        g_nr.reset = true; g.reset = true; g.haveSchedule = true;
        g.expectedMask = schedule.expectedMask; g.previousPasses = passes;
    }
    bool reset = g_nr.reset || frame.Reset;
    if (!g_gpuTime) g_gpuTime = std::make_unique<DlssNrGpuTime>(device, "total");
    if (!g_ngxTime) g_ngxTime = std::make_unique<DlssNrGpuTime>(device, "models");
    g_gpuTime->Start(cmd);
    D3D12_RESOURCE_STATES targetState = arrival;
    const auto transitionTarget = [&](D3D12_RESOURCE_STATES next) {
        if (targetState != next) Barrier(cmd, target, targetState, next);
        targetState = next;
    };
    ID3D12Resource* gameTarget = target;
    const bool crop = target->GetDesc().Width != width || target->GetDesc().Height != height;
    if (crop)
    {
        if (!Ensure(g.active, device, format, width, height)) { g_gpuTime->Abandon(); return false; }
        transitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.active.To(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
        DlssNr::CopyActiveColor(cmd, g.active.Get(), target, DlssNr::ColorExtent { width, height });
        transitionTarget(arrival);
        g.active.To(cmd, Srv); target = g.active.Get(); targetState = Srv;
    }
    ID3D12Resource* depthIn = nullptr;
    ID3D12Resource* motionIn = nullptr;
    std::array<ID3D12Resource*, 3> readableModels {};
    // All exits restore caller and reusable NR resources, including failed encode/evaluate/PSO paths.
    OnExit finish { [&] {
        for (auto* model : readableModels) if (model) Barrier(cmd, model, Srv, Uav);
        if (depthIn && depthIn == g_nr.depthClone) Barrier(cmd, depthIn, Srv, D3D12_RESOURCE_STATE_COPY_DEST);
        if (motionIn && motionIn == g_nr.motionClone) Barrier(cmd, motionIn, Srv, D3D12_RESOURCE_STATE_COPY_DEST);
        transitionTarget(crop ? Uav : arrival);
        if (crop) g.active.state = Uav;
        g_gpuTime->End(cmd);
        if (auto ms = g_gpuTime->ReadGpuTime(queue)) g_lastGpuTime = ms;
        if (auto ms = g_ngxTime->ReadGpuTime(queue)) g_lastNgxTime = ms;
        g_gpuTime->DiscardFresh(); g_ngxTime->DiscardFresh();
    } };

    float white = frame.WhitePointOverride > 0 ? frame.WhitePointOverride : cfg.DlssNrWhitePointScale.value_or_default();
    const bool wasHeld = g.holding;
    const bool hold = cfg.DlssNrHoldFrame.value_or_default();
    if (hold)
    {
        if (!g.holding)
        {
            if (!Ensure(g.held, device, format, width, height)) return false;
            transitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE); g.held.To(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(g.held.Get(), target); g.held.To(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.holding = true; g.heldWhite = white;
        }
        else
        {
            transitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(target, g.held.Get());
        }
        white = g.heldWhite;
    }
    else g.holding = false;
    DlssNr::NoteExposureFrame(cfg, frame.ExposureTexture != nullptr && !frame.GameAutoExposure, false);
    const unsigned source = DlssNr::WhitePointSourceEffective(cfg);
    Constants exposure {};
    ID3D12Resource* exposureTexture = nullptr;
    if (wasHeld && hold && g.exposure.Get() && g.heldExposureSource == source)
    {
        exposureTexture = g.exposure.Get();
        ExposureConstants(exposure, cfg, source, g.heldPreExposure);
    }
    else if (frame.ColourIsLinearHdr && frame.WhitePointOverride <= 0 && (source == 3 || source == 1))
    {
        auto* gameExposure = static_cast<ID3D12Resource*>(frame.ExposureTexture);
        const auto ed = gameExposure ? gameExposure->GetDesc() : D3D12_RESOURCE_DESC {};
        const bool valid = source == 3 || (gameExposure && ed.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            ed.Width == 1 && ed.Height == 1 && ed.DepthOrArraySize == 1 && ed.SampleDesc.Count == 1);
        if (valid && Ensure(g.exposure, device, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1))
        {
            ExposureConstants(exposure, cfg, source, frame.PreExposure);
            exposure.ExposureSourceWidth = width; exposure.ExposureSourceHeight = height;
            bool ready = false;
            if (source == 3 && Ensure(g.meter, device, DXGI_FORMAT_R32G32B32A32_FLOAT, 64, 64))
            {
                auto c = exposure; c.Mode = 3u; c.Width = c.Height = 64u;
                if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) c.DirectResolveFlags = 65536u;
                transitionTarget(Srv);
                ready = Dispatch(shader, cmd, c, target, nullptr, nullptr, nullptr, nullptr, nullptr, g.meter);
                c.Mode = 11u; c.Width = c.Height = 1u;
                ready = ready && Dispatch(shader, cmd, c, g.meter.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, g.exposure);
            }
            else if (source == 1)
            {
                const auto prior = static_cast<D3D12_RESOURCE_STATES>(frame.ExposureState);
                if (prior != Srv) Barrier(cmd, gameExposure, prior, Srv);
                auto c = exposure; c.Mode = 2u; c.Width = c.Height = 1u;
                ready = Dispatch(shader, cmd, c, gameExposure, nullptr, nullptr, nullptr, nullptr, nullptr, g.exposure);
                if (prior != Srv) Barrier(cmd, gameExposure, Srv, prior);
            }
            if (ready)
            {
                exposureTexture = g.exposure.Get(); g.heldExposureSource = source; g.heldPreExposure = exposure.PreExposure;
            }
        }
    }
    Constants encode = exposureTexture ? exposure : Constants {};
    encode.Mode = 0u; encode.Width = width; encode.Height = height; encode.WhitePoint = white;
    encode.Passthrough = frame.ColourIsLinearHdr ? 0u : 1u;
    encode.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default(); encode.DirectResolveFlags = 65536u;
    transitionTarget(Srv);
    if (!Dispatch(shader, cmd, encode, target, nullptr, nullptr, exposureTexture, nullptr, nullptr, g.proxy, &g.original)) return false;
    ID3D12Resource* modelInput = g.proxy.Get();
    if (workWidth != width || workHeight != height)
    {
        bool built = false;
        if (scale > 1.0f)
        {
            const auto scaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (g.scaler != scaler)
            {
                auto* up = g.superUp.release();
                auto* down = g.superDown.release();
                state.lifetime.Retire([up, down] { delete up; delete down; });
                g.scaler = scaler;
            }
            if (!g.superUp) g.superUp = std::make_unique<OS_Dx12>("Superior supersample up", device, true, scaler);
            if (!g.superDown) g.superDown = std::make_unique<OS_Dx12>("Superior supersample down", device, false, scaler);
            g.workProxy.To(cmd, Uav);
            built = g.superUp->Dispatch(cmd, modelInput, g.workProxy.Get());
            if (built) g.workProxy.To(cmd, Srv);
        }
        Constants down {}; down.Mode = 2u; down.Width = workWidth; down.Height = workHeight;
        down.DirectResolveFlags = 65536u; // custom Area, independent of the legacy down/upscaler settings
        if (!built && !Dispatch(shader, cmd, down, modelInput, nullptr, nullptr, nullptr, nullptr, nullptr, g.workProxy)) return false;
        modelInput = g.workProxy.Get();
    }
    depthIn = ReadableGuide(device, cmd, depth, &g_nr.depthClone);
    motionIn = ReadableGuide(device, cmd, motion, &g_nr.motionClone);
    if (!motionIn || (depth && !depthIn)) return false;
    auto modelGuides = regions;
    ID3D12Resource* modelDepth = depthIn;
    ID3D12Resource* modelMotion = motionIn;
    if (reduced)
    {
        if (!Ensure(g.depth, device, DXGI_FORMAT_R32_FLOAT, workWidth, workHeight) ||
            !Ensure(g.motion, device, DXGI_FORMAT_R32G32_FLOAT, workWidth, workHeight)) return false;
        const auto resize = GuideConstants(workWidth, workHeight, regions, frame, false, hold);
        if (!Dispatch(shader, cmd, resize, depthIn, motionIn, nullptr, nullptr, nullptr, nullptr, g.depth, &g.motion)) return false;
        modelDepth = g.depth.Get(); modelMotion = g.motion.Get();
        modelGuides = { { 0, 0, workWidth, workHeight }, { 0, 0, workWidth, workHeight } };
    }
    const unsigned referenceW = frame.MotionVectorsLowResolution ? frame.RenderSubrectWidth : frame.OutputWidth;
    const unsigned referenceH = frame.MotionVectorsLowResolution ? frame.RenderSubrectHeight : frame.OutputHeight;
    const float mvX = DlssNr::SuperiorScaling::MotionScale(frame.MvScaleX, modelGuides.motion.width, referenceW);
    const float mvY = DlssNr::SuperiorScaling::MotionScale(frame.MvScaleY, modelGuides.motion.height, referenceH);
    const auto readable = [&](ID3D12Resource* r) {
        if (std::find(readableModels.begin(), readableModels.end(), r) == readableModels.end())
        {
            const auto empty = std::find(readableModels.begin(), readableModels.end(), nullptr);
            assert(empty != readableModels.end());
            Barrier(cmd, r, Uav, Srv); *empty = r;
        }
    };
    const auto writable = [&](ID3D12Resource* r) {
        const auto it = std::find(readableModels.begin(), readableModels.end(), r);
        if (it != readableModels.end()) { Barrier(cmd, r, Srv, Uav); *it = nullptr; }
    };
    SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
    ID3D12Resource* answer = nullptr;
    ID3D12Resource* passInput = modelInput;
    ID3D12Resource* passOutput = g_nr.output;
    unsigned covered = 0, completed = 0;
    uint32_t successfulMask = 0, clampSlots[2] { UINT32_MAX, UINT32_MAX };
    bool correctionFailed = false;
    g_ngxTime->Start(cmd);
    for (unsigned pass = 0; pass < passes; ++pass)
    {
        writable(passOutput);
        const auto tuning = PassTuning(cfg, pass);
        const bool passReset = reset || (pass > 0 && g_nr.passNeedsReset[pass]);
        void* feature = pass == 0 ? g_nr.feature : g_nr.passFeature[pass];
        int result;
        if (cfg.DlssNrUseProxy.value_or_default())
            result = (int) DlssNr::Proxy::Run(cmd, device, passInput, modelDepth, modelMotion, passOutput,
                workWidth, workHeight, modelGuides.depth.width, modelGuides.depth.height,
                modelGuides.motion.width, modelGuides.motion.height, modelGuides.depth.x, modelGuides.depth.y,
                modelGuides.motion.x, modelGuides.motion.y, frame.DepthInverted, passReset, mvX, mvY);
        else
        {
            // Full model evaluation every frame, as in v59; janblade's temporal reuse is a separate mode.
            DlssNrNative::BeginEvaluate(feature, passReset, 1u, 1u,
                (long long) (g_nr.successfulDispatches & 0x3FFFFFFFFFFFFFFFull), cmd, false, DlssNrVitReuse::kCopyPlain);
            result = g_nr.evaluate(cmd, feature, g_nr.capabilityParams, passInput, modelDepth, modelMotion, passOutput,
                workWidth, workHeight, modelGuides.depth.width, modelGuides.depth.height,
                modelGuides.motion.width, modelGuides.motion.height, modelGuides.depth.x, modelGuides.depth.y,
                modelGuides.motion.x, modelGuides.motion.y, frame.DepthInverted ? 1 : 0, passReset ? 1 : 0,
                tuning.intensity, (int) PassStyle(cfg, pass), tuning.structure, tuning.tone, tuning.skin,
                tuning.autoMask ? 1 : 0, mvX, mvY);
            DlssNrNative::EndEvaluate(cmd);
        }
        if (result != NVSDK_NGX_Result_Success) break;
        if (pass > 0) g_nr.passNeedsReset[pass] = false;
        answer = passOutput; readable(answer); completed = pass + 1u;
        if (completed == passes) break;
        const bool due = schedule.Due(pass, covered);
        if (due && InterPass(g, shader, cmd, cfg, modelInput, answer, covered, completed, scale))
        { covered = completed; successfulMask |= 1u << pass; passInput = g.interWorking.Get(); }
        else
        {
            if (due) { correctionFailed = true; reset = true; }
            writable(g_nr.passClampScratch);
            Constants clamp {}; clamp.Mode = 8u; clamp.Width = workWidth; clamp.Height = workHeight;
            if (!shader.DispatchSuperiorPass(cmd, clamp, answer, nullptr, nullptr, nullptr, nullptr, nullptr,
                                            g_nr.passClampScratch, nullptr, &clampSlots[pass % 2u])) break;
            readable(g_nr.passClampScratch); passInput = g_nr.passClampScratch;
        }
        passOutput = passOutput == g_nr.output ? g_nr.passScratch : g_nr.output;
    }
    g_ngxTime->End(cmd);
    for (unsigned skipped = completed; skipped < passes; ++skipped) g_nr.passNeedsReset[skipped] = true;
    g_nr.reset = correctionFailed || completed == 0;
    if (!answer || !completed) { g.reset = true; Say("NR evaluation failed; keeping the game frame."); return false; }
    if (g.successfulMask != successfulMask) g.reset = true;
    g.successfulMask = successfulMask;
    ID3D12Resource* resolveAnswer = answer;
    ID3D12Resource* resolveProxy = modelInput;
    if (scale > 1.0f && g.superDown && Ensure(g.nativeAnswer, device, format, width, height))
    {
        g.nativeAnswer.To(cmd, Uav);
        if (g.superDown->Dispatch(cmd, answer, g.nativeAnswer.Get()))
        {
            g.nativeAnswer.To(cmd, Srv);
            resolveAnswer = g.nativeAnswer.Get(); resolveProxy = g.proxy.Get();
        }
    }
    if (temporal)
    {
        resolveAnswer = Stabilize(g, shader, cmd, modelInput, answer, motionIn, regions, frame, reset, hold);
        if (!resolveAnswer) return false;
    }
    Constants resolve = exposureTexture ? exposure : Constants {};
    resolve.Mode = 1u; resolve.WhitePoint = white; resolve.Width = width; resolve.Height = height;
    resolve.Passthrough = encode.Passthrough; resolve.ReversibleMode = encode.ReversibleMode;
    resolve.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
    resolve.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
    resolve.MaxRatio = cfg.DlssNrMaxRatio.value_or_default(); resolve.MaxDarkening = std::clamp(cfg.DlssNrMaxDarkening.value_or_default(), 0.0f, 100.0f);
    resolve.DebugScale = cfg.DlssNrWhitePointScale.value_or_default();
    resolve.DebugView = cfg.DlssNrDebugView.value_or_default();
    if (resolve.DebugView == 5u) resolve.DebugView = 0u;
    resolve.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
    resolve.CompareMode = cfg.DlssNrCompare.value_or_default(); resolve.CompareSplit = cfg.DlssNrCompareSplit.value_or_default();
    resolve.CompareZoom = std::max(1.0f, cfg.DlssNrCompareZoom.value_or_default());
    resolve.CompareSwap = cfg.DlssNrCompareSwap.value_or_default() ? 1u : 0u;
    const auto strength = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f; };
    resolve.SkinProtection = cfg.DlssNrSkinProtection.value_or_default(); resolve.ShowSkinMask = cfg.DlssNrShowSkinMask.value_or_default();
    resolve.SkinDetail = strength(cfg.DlssNrSkinDetail.value_or_default());
    resolve.SkinColour = cfg.DlssNrSkinToneEnabled.value_or_default() ? strength(cfg.DlssNrSkinColour.value_or_default()) : 0.0f;
    resolve.EnvironmentDetail = strength(cfg.DlssNrEnvironmentDetail.value_or_default());
    resolve.EnvironmentColour = strength(cfg.DlssNrEnvironmentColour.value_or_default());
    resolve.ReplaceDetailStrength = cfg.DlssNrReplaceDetailStrength.value_or_default(); resolve.ModelWorkScale = scale;
    ID3D12Resource* low = nullptr;
    if (temporal)
    {
        const auto shaping = Shaping(cfg, covered, completed, scale);
        resolve.Transfer = 9u; resolve.ResidualHistoryValid = 1u; resolve.ResidualBlend = .015f;
        resolve.ResidualScale = 3.0f; resolve.ResidualConfidenceSensitivity = 1.0f;
        resolve.MvScaleX = shaping.MvScaleX; resolve.MvScaleY = shaping.MvScaleY;
        resolve.GuideWidth = !(resolve.MvScaleX == 1.0f && resolve.MvScaleY == 1.0f);
        resolve.DirectResolveFlags = 65536u;
        if (resolve.GuideWidth)
        {
            if (Ensure(g.finalLow, device, DXGI_FORMAT_R16G16B16A16_FLOAT, (workWidth + 7u) / 8u, (workHeight + 7u) / 8u))
            {
                Constants l {}; l.Mode = 18u; l.Width = (workWidth + 7u) / 8u; l.Height = (workHeight + 7u) / 8u;
                l.Transfer = 1u; l.Passthrough = resolve.Passthrough; l.DirectResolveFlags = 65536u;
                if (Dispatch(shader, cmd, l, modelInput, resolveAnswer, nullptr, nullptr, nullptr, nullptr, g.finalLow)) low = g.finalLow.Get();
            }
            if (!low) resolve.GuideWidth = 0u; // same guided-only fallback as v59
        }
    }
    if (resolve.DebugView == 4u) { resolveProxy = modelInput; resolve.DebugView = 1u; }
    // v59's optional final colour recovery; no dispatch/allocation when disabled.
    const float chroma = std::clamp(cfg.DlssNrFinalChromaticityRecovery.value_or_default(), 0.0f, 100.0f);
    const float saturation = std::clamp(cfg.DlssNrFinalSaturationRecovery.value_or_default(), 0.0f, 100.0f);
    const bool colourEnabled = (chroma > 0 || saturation > 0) && resolve.CompareMode == 0 &&
                              resolve.DebugView == 0 && resolve.ShowSkinMask == 0 && resolve.ApplyModel;
    if (!colourEnabled || reset) g.colourHistoryValid = false;
    constexpr unsigned statsW = 32;
    const unsigned statsH = std::max(1u, unsigned((uint64_t(statsW) * height + width / 2u) / width));
    const bool colourReady = colourEnabled && Ensure(g.finalColour, device, format, width, height) &&
        Ensure(g.colourStats[0], device, DXGI_FORMAT_R32G32B32A32_FLOAT, statsW, statsH) &&
        Ensure(g.colourStats[1], device, DXGI_FORMAT_R32G32B32A32_FLOAT, statsW, statsH) &&
        Ensure(g.colourHistory[0], device, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1) &&
        Ensure(g.colourHistory[1], device, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1);
    const bool targetUav = (target->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    ID3D12Resource* original = targetUav ? g.original.Get() : target;
    ID3D12Resource* destination = targetUav ? target : g.original.Get();
    if (colourReady) g.finalColour.To(cmd, Uav);
    else if (targetUav) transitionTarget(Uav); else g.original.To(cmd, Uav);
    if (!shader.DispatchSuperiorPass(cmd, resolve, resolveProxy, resolveAnswer, original, exposureTexture, nullptr, low,
                                    colourReady ? g.finalColour.Get() : destination, nullptr)) return false;
    bool colourApplied = false;
    if (colourReady)
    {
        g.finalColour.To(cmd, Srv);
        Constants stats {}; stats.Mode = 29u; stats.Width = statsW; stats.Height = statsH;
        stats.Passthrough = resolve.Passthrough;
        if (Dispatch(shader, cmd, stats, original, g.finalColour.Get(), nullptr, nullptr, nullptr, nullptr,
                     g.colourStats[0], &g.colourStats[1]))
        {
            const auto write = g.colourCursor & 1u, read = write ^ 1u;
            Constants reduce {}; reduce.Mode = 30u; reduce.Width = reduce.Height = 1;
            reduce.GuideWidth = statsW; reduce.GuideHeight = statsH;
            reduce.MaxRatio = std::clamp(cfg.DlssNrFinalColourSmoothingMs.value_or_default(), 0.0f, 5000.0f);
            reduce.DebugScale = std::clamp(frame.FrameTimeMs, .01f, 1000.0f);
            reduce.ApplyModel = g.colourHistoryValid;
            auto* previousHistory = g.colourHistoryValid ? g.colourHistory[read].Get() : g.colourStats[0].Get();
            if (Dispatch(shader, cmd, reduce, g.colourStats[0].Get(), g.colourStats[1].Get(), previousHistory,
                         nullptr, nullptr, nullptr, g.colourHistory[write]))
            {
                g.colourHistoryValid = true; g.colourCursor = read;
                Constants apply {}; apply.Mode = 31u; apply.Width = width; apply.Height = height;
                apply.Passthrough = resolve.Passthrough; apply.TransferStrength = chroma * .01f;
                apply.ColourStrength = saturation * .01f;
                apply.ApplyModel = cfg.DlssNrFinalChromaticityPreserveSaturation.value_or_default();
                apply.Transfer = std::min(cfg.DlssNrFinalSaturationMode.value_or_default(), 1u);
                apply.MaxDarkening = std::clamp(cfg.DlssNrFinalHighSaturationProtection.value_or_default() * .01f, 0.0f, 1.0f);
                if (targetUav) transitionTarget(Uav); else g.original.To(cmd, Uav);
                colourApplied = shader.DispatchSuperiorPass(cmd, apply, g.finalColour.Get(), g.colourHistory[write].Get(),
                                                           nullptr, nullptr, nullptr, nullptr, destination, nullptr);
            }
        }
        if (!colourApplied)
        {
            g.finalColour.To(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(target, g.finalColour.Get());
        }
    }
    if (!targetUav && (!colourReady || colourApplied))
    {
        g.original.To(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE); transitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(target, g.original.Get());
    }
    if (crop)
    {
        transitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, gameTarget, arrival, D3D12_RESOURCE_STATE_COPY_DEST);
        DlssNr::CopyActiveColor(cmd, gameTarget, target, DlssNr::ColorExtent { width, height });
        Barrier(cmd, gameTarget, D3D12_RESOURCE_STATE_COPY_DEST, arrival);
    }
    ++g_nr.successfulDispatches;
    Say(temporal ? "Active: Area, cached Low36/Cross5, private DLSS DLAA, v59 guided resolve."
                 : "Active at native size: ordinary NR, without enlargement or private DLSS.");
    return true;
}
} // namespace SuperiorRun
} // namespace
