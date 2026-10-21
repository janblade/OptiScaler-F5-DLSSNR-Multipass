// Included by DlssNr_Dx12.cpp after janblade's model/resource helpers.
// Model creation, pass profiles, native/bridge entry points and the UI remain janblade's.
// Only Superior's scaling, guides, inter-pass and stabilization live here. Shader ABI/bytecode: v59.
namespace
{
namespace SuperiorRun
{
using Constants = DlssNr::Superior::DlssNrConstants;
using Microsoft::WRL::ComPtr;
constexpr auto Uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto Srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

struct Texture
{
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = Uav;
    ID3D12Resource* Get() const { return resource.Get(); }
    void To(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES next)
    {
        if (resource && state != next)
            Barrier(cmd, resource.Get(), state, next);
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
    Texture depth, motion, interLow, interWorking, finalLow;
    Texture carrier, temporalOutput, temporalMotion, unit;
    uint32_t expectedMask = 0, successfulMask = 0;
    unsigned previousPasses = 0, lastCompleted = 0, lastCovered = 0;
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
State& GetState()
{
    static auto* value = new State;
    return *value;
}

void Say(const char* text)
{
    auto& s = GetState();
    if (s.status == text)
        return;
    s.status = text;
    if (*text)
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
        else
            DlssNrWatchedLists::g_full = true; // conservative overflow: never miss a reset/submit
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
            if (lists[i] == s.current->creation)
                s.current->submitted = true;
}

void ResetRecording(ID3D12CommandList* cmd)
{
    auto& s = GetState();
    s.lifetime.ResetRecording(cmd);
    for (auto& list : s.watched)
        if (list == cmd)
            list = nullptr;
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
        if (d.Width == w && d.Height == h && d.Format == format)
            return true;
        auto old = std::move(t.resource);
        GetState().lifetime.Retire([old] {});
    }
    t.state = Uav;
    t.resource.Attach(CreateScratch(device, format, w, h));
    return t.resource != nullptr;
}

bool Dispatch(DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, const Constants& c, ID3D12Resource* source,
              ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* motion, ID3D12Resource* previous,
              ID3D12Resource* low, Texture& output, Texture* keep = nullptr, uint32_t* immutableSlot = nullptr)
{
    output.To(cmd, Uav);
    if (keep)
        keep->To(cmd, Uav);
    if (!shader.DispatchSuperiorPass(cmd, c, source, model, original, motion, previous, low, output.Get(),
                                     keep ? keep->Get() : nullptr, immutableSlot))
        return false;
    output.To(cmd, Srv);
    if (keep)
        keep->To(cmd, Srv);
    return true;
}

Constants GuideConstants(unsigned w, unsigned h, const DlssNr::GuideRegions& regions, const DlssNrFrameInfo& frame,
                         bool pixelUnits, bool hold)
{
    Constants c {};
    c.Mode = 10u;
    c.Width = w;
    c.Height = h;
    c.GuideWidth = regions.depth.width;
    c.GuideHeight = regions.depth.height;
    c.DebugView = regions.depth.x;
    c.CompareMode = regions.depth.y;
    c.TransferStrength = float(regions.motion.width);
    c.ColourStrength = float(regions.motion.height);
    c.CompareSwap = regions.motion.x;
    c.Transfer = regions.motion.y;
    c.MvScaleX = c.MvScaleY = 1.0f;
    if (pixelUnits)
    {
        const auto rw = frame.MotionVectorsLowResolution ? frame.RenderSubrectWidth : frame.OutputWidth;
        const auto rh = frame.MotionVectorsLowResolution ? frame.RenderSubrectHeight : frame.OutputHeight;
        c.MvScaleX = frame.MvScaleX * (float(w) / std::max(rw ? rw : regions.motion.width, 1u));
        c.MvScaleY = frame.MvScaleY * (float(h) / std::max(rh ? rh : regions.motion.height, 1u));
        if (hold)
            c.MvScaleX = c.MvScaleY = 0;
    }
    return c;
}

bool CreateTemporal(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd)
{
    Record(cmd); // includes unit-exposure initialization even if NGX creation fails
    const auto w = g.workWidth, h = g.workHeight;
    if (!Ensure(g.carrier, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, w, h) ||
        !Ensure(g.temporalOutput, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, w, h) ||
        !Ensure(g.temporalMotion, g.device.Get(), DXGI_FORMAT_R32G32_FLOAT, w, h) ||
        !Ensure(g.unit, g.device.Get(), DXGI_FORMAT_R32_FLOAT, 1, 1))
        return false;
    Constants unit {};
    unit.Mode = 7u;
    unit.Width = unit.Height = 1u;
    if (!Dispatch(shader, cmd, unit, g_nr.output, nullptr, nullptr, nullptr, nullptr, nullptr, g.unit))
        return false;
    if (!NVNGXProxy::InitDx12(g.device.Get()) || !NVNGXProxy::D3D12_AllocateParameters() ||
        !NVNGXProxy::D3D12_DestroyParameters() || !NVNGXProxy::D3D12_CreateFeature() ||
        !NVNGXProxy::D3D12_EvaluateFeature() || !NVNGXProxy::D3D12_ReleaseFeature())
        return false;
    if (NVNGXProxy::D3D12_AllocateParameters()(&g.parameters) != NVSDK_NGX_Result_Success || !g.parameters)
        return false;
    auto* p = g.parameters;
    p->Set(NVSDK_NGX_Parameter_Width, w);
    p->Set(NVSDK_NGX_Parameter_Height, h);
    p->Set(NVSDK_NGX_Parameter_OutWidth, w);
    p->Set(NVSDK_NGX_Parameter_OutHeight, h);
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int) NVSDK_NGX_PerfQuality_Value_DLAA);
    for (const char* key :
         { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality })
        p->Set(key, (unsigned) g.preset);
    const unsigned flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                           NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                           (g.inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0u);
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
    const auto gains = DlssNr::SuperiorScaling::BlockGains(cfg, first, completed, scale, g_nr.superiorMainOnlyGains);
    c.MvScaleX = gains.high;
    c.MvScaleY = gains.low;
    c.GuideWidth = 1u;
    c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;
    return c;
}

bool InterPass(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, const Config& cfg,
               ID3D12Resource* nativeProxy, ID3D12Resource* originalBase, ID3D12Resource* answer, unsigned covered,
               unsigned completed, float scale)
{
    const auto desc = originalBase->GetDesc();
    const unsigned workW = unsigned(desc.Width), workH = desc.Height;
    auto c = Shaping(cfg, covered, completed, scale);
    c.DirectResolveFlags |= 16u | 64u;
    if (c.MvScaleX == c.MvScaleY)
    {
        c.GuideWidth = 0u;
        c.DirectResolveFlags |= 32u;
    }
    ID3D12Resource* low = nullptr;
    if (c.GuideWidth)
    {
        if (!Ensure(g.interLow, g.device.Get(), desc.Format, (workW + 7u) / 8u, (workH + 7u) / 8u))
            return false;
        Constants l {};
        l.Mode = 34u;
        l.Width = (workW + 7u) / 8u;
        l.Height = (workH + 7u) / 8u;
        l.DirectResolveFlags = 32768u;
        if (!Dispatch(shader, cmd, l, originalBase, answer, nullptr, nullptr, nullptr, nullptr, g.interLow))
            return false;
        low = g.interLow.Get();
    }
    if (!Ensure(g.interWorking, g.device.Get(), desc.Format, workW, workH))
        return false;
    c.Mode = 36u;
    c.Width = workW;
    c.Height = workH;
    return Dispatch(shader, cmd, c, originalBase, answer, nativeProxy, nullptr, nullptr, low, g.interWorking);
}

ID3D12Resource* Stabilize(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* proxy,
                          ID3D12Resource* answer, ID3D12Resource* originalMotion, const DlssNr::GuideRegions& regions,
                          const DlssNrFrameInfo& frame, bool reset, bool hold)
{
    auto encode = GuideConstants(g.workWidth, g.workHeight, regions, frame, true, hold);
    encode.Mode = 39u; // v50: fixed carrier + motion; reuse this frame's exact NR depth.
    if (!Dispatch(shader, cmd, encode, proxy, answer, originalMotion, nullptr, nullptr, nullptr, g.carrier,
                  &g.temporalMotion))
        return nullptr;
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
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,
           std::isfinite(frame.FrameTimeMs) && frame.FrameTimeMs > 0 ? frame.FrameTimeMs : 16.67f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    if (NVNGXProxy::D3D12_EvaluateFeature()(cmd, g.feature, p, nullptr) != NVSDK_NGX_Result_Success)
    {
        g.reset = true;
        Say("Private DLSS evaluation failed; keeping the game frame.");
        return nullptr;
    }
    g.temporalOutput.To(cmd, Srv);
    Constants decode {};
    decode.Mode = 38u;
    decode.Width = g.workWidth;
    decode.Height = g.workHeight;
    if (!Dispatch(shader, cmd, decode, g.temporalOutput.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, g.carrier))
        return nullptr;
    g.reset = false;
    return g.carrier.Get();
}

// Called before janblade records its normal input/exposure path. Only private DLSS
// creation gets a submission-only frame, exactly like NR feature creation above it.
bool Begin(DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Device* device, DXGI_FORMAT format,
           unsigned width, unsigned height, unsigned workWidth, unsigned workHeight, const DlssNrFrameInfo& frame,
           bool haveDepth, ID3D12CommandQueue* queue)
{
    auto& state = GetState();
    state.lifetime.Collect();
    const bool temporal = workWidth < width || workHeight < height;
    if (temporal && !haveDepth)
    {
        Say("Temporal enlargement needs current depth and motion guides.");
        return false;
    }
    const int preset =
        DlssNr::SuperiorScaling::Preset(Config::Instance()->DlssNrSuperiorScalingPreset.value_or_default());
    auto* previous = state.current.get();
    if (previous &&
        (previous->device.Get() != device || previous->width != width || previous->height != height ||
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
        g.device = device;
        g.queue = queue;
        g.width = width;
        g.height = height;
        g.workWidth = workWidth;
        g.workHeight = workHeight;
        g.format = format;
        g.inverted = frame.DepthInverted;
        g.beforeSr = frame.BeforeUpscale;
        g.temporal = temporal;
        g.preset = preset;
    }
    auto& g = *state.current;
    if (g.failed)
        return false;
    if (temporal && !g.feature)
    {
        g.failed = !CreateTemporal(g, shader, cmd);
        Say(g.failed ? "Private DLSS initialization failed." : "Waiting for private DLSS creation submission.");
        return false;
    }
    if (temporal && !g.submitted)
        return false;
    Record(cmd);
    return true;
}

bool MatchGuides(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth,
                 ID3D12Resource* motion, const DlssNr::GuideRegions& regions, const DlssNrFrameInfo& frame)
{
    if (!Ensure(g.depth, g.device.Get(), DXGI_FORMAT_R32_FLOAT, g.workWidth, g.workHeight) ||
        !Ensure(g.motion, g.device.Get(), DXGI_FORMAT_R32G32_FLOAT, g.workWidth, g.workHeight))
        return false;
    const auto c = GuideConstants(g.workWidth, g.workHeight, regions, frame, false, false);
    return Dispatch(shader, cmd, c, depth, motion, nullptr, nullptr, nullptr, nullptr, g.depth, &g.motion);
}

ID3D12Resource* FinalLow(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* proxy,
                         ID3D12Resource* residual)
{
    if (!Ensure(g.finalLow, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, (g.workWidth + 7u) / 8u,
                (g.workHeight + 7u) / 8u))
        return nullptr;
    Constants c {};
    c.Mode = 18u;
    c.Transfer = 1u;
    c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;
    c.Width = (g.workWidth + 7u) / 8u;
    c.Height = (g.workHeight + 7u) / 8u;
    return Dispatch(shader, cmd, c, proxy, residual, nullptr, nullptr, nullptr, nullptr, g.finalLow) ? g.finalLow.Get()
                                                                                                     : nullptr;
}
} // namespace SuperiorRun
} // namespace
