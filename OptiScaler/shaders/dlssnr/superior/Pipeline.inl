// Included by DlssNr_Dx12.cpp after janblade's model/resource helpers.
// Model creation, pass profiles, native/bridge entry points and the UI remain janblade's.
// Only Superior's scaling, guides, inter-pass and stabilization live here. Shader ABI/bytecode: v59.
namespace
{
namespace SuperiorRun
{
using Constants = DlssNr::Superior::DlssNrConstants;
namespace Fields = DlssNr::Superior::Fields;
namespace Private = DlssNr::SuperiorPrivate;
using namespace DlssNr::Superior;
using Microsoft::WRL::ComPtr;
constexpr auto Uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto Srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Frames to wait for the private DLSS creation's submission before giving up on it for this session.
constexpr unsigned kCreationWaitFrames = 240;

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

// Whether a handle made under NGX runtime `epoch` still exists. NGX shutdown destroys every feature and parameter
// block itself, so after it (or after a restart) the handle is dropped rather than released or evaluated.
bool NgxAlive(uint64_t epoch) { return NVNGXProxy::IsDx12Inited() && NVNGXProxy::Dx12Epoch() == epoch; }

struct ResidentCnn
{
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    uint64_t ngxEpoch = 0, createEpoch = 0;
    const void* creation = nullptr;
    unsigned waitedFrames = 0;
    bool submitted = false, ready = false;
    ~ResidentCnn()
    {
        if (!NgxAlive(ngxEpoch))
            return;
        if (feature)
            NVNGXProxy::D3D12_ReleaseFeature()(feature);
        if (parameters)
            NVNGXProxy::D3D12_DestroyParameters()(parameters);
    }
};

struct GhostHistory
{
    Texture reconstructed, state[2], raw[2], reference[2], field, expanded;
    Texture rings, featherOutput;
    bool initialized = false, submitted = false;
    bool checkedTypedLoad = false, typedLoad = false;
    unsigned index = 0;
    uint64_t epoch = 0;
    const void* commands = nullptr;
};

struct MotionSample
{
    Texture result;
    ComPtr<ID3D12Resource> readback, input;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    bool busy = false, recorded = false, ready = false;
    double measuredAt = 0;
    uint64_t context = 0;
};

struct MotionProbe
{
    std::array<std::shared_ptr<MotionSample>, 8> samples;
    Private::CnnMotion selection;
    DlssNr::GuideRegion region {};
    float scaleX = 0, scaleY = 0;
    double nextSample = 0;
    uint64_t context = 0;
    bool suspended = false;
};

struct Generation
{
    ComPtr<ID3D12Device> device;
    unsigned width = 0, height = 0, workWidth = 0, workHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool inverted = false, beforeSr = false, reset = true;
    bool temporal = false, shaping = false, packed = false;
    bool privateFailed = false, residentAttempted = false;
    unsigned actualPreset = 0;
    Private::Fade presetFade;
    std::shared_ptr<ResidentCnn> resident;
    std::unique_ptr<MotionProbe> motionProbe;
    std::unique_ptr<GhostHistory> ghosts;
    bool PrivateReady() const { return temporal && !privateFailed && feature && waitedFrames == UINT_MAX; }
    bool guidesMatched = false;
    DlssNr::Spatial::Layout layout {};
    Texture warp, warpCache;
    ComPtr<ID3D12Resource> warpUpload;
    unsigned preset = 1;
    // Creation is gated like the NR feature's: the first evaluate waits for a later submission epoch.
    const void* creation = nullptr; // identity only, to notice a creation recording thrown away by Reset
    bool creationSubmitted = false;
    unsigned long long createEpoch = 0;
    unsigned waitedFrames = 0;
    uint64_t ngxEpoch = 0;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    Texture depth, motion, interLow, interWorking, finalLow;
    Texture carrier, temporalOutput, temporalMotion, unit;
    uint32_t expectedMask = 0, successfulMask = 0;
    unsigned previousPasses = 0, lastCompleted = 0, lastCovered = 0;
    bool haveSchedule = false;
    // Forget the NGX handles without releasing them: NGX shut down (or restarted) and destroyed them already.
    void DropNgx()
    {
        feature = nullptr;
        parameters = nullptr;
    }
    ~Generation()
    {
        if (!NgxAlive(ngxEpoch))
            return;
        if (feature && NVNGXProxy::D3D12_ReleaseFeature())
            NVNGXProxy::D3D12_ReleaseFeature()(feature);
        if (parameters && NVNGXProxy::D3D12_DestroyParameters())
            NVNGXProxy::D3D12_DestroyParameters()(parameters);
    }
};

// Superior's identities that the shared 64-entry watched table could not take (DlssNr_WatchedLists.h). Counted in
// g_overflow, so the hooks still take the lock for them, and dropped when the list is reset or destroyed.
struct Overflow
{
    std::mutex mutex;
    std::vector<const void*> lists;
};
Overflow& GetOverflow()
{
    static auto* value = new Overflow;
    return *value;
}
void Watch(const void* identity)
{
    if (identity == nullptr)
        return;
    auto& overflow = GetOverflow();
    std::lock_guard lock(overflow.mutex);
    if (std::find(overflow.lists.begin(), overflow.lists.end(), identity) != overflow.lists.end())
        return;
    if (DlssNrWatchedLists::TryAdd(identity))
        return;
    overflow.lists.push_back(identity);
    ++DlssNrWatchedLists::g_overflow;
}
void Forget(const void* identity)
{
    auto& overflow = GetOverflow();
    std::lock_guard lock(overflow.mutex);
    if (const auto it = std::find(overflow.lists.begin(), overflow.lists.end(), identity); it != overflow.lists.end())
    {
        overflow.lists.erase(it);
        --DlssNrWatchedLists::g_overflow;
    }
}
// A recorded list was destroyed (GpuLifetime's watch, on the destroying thread): it can never be submitted again.
void Destroyed(const void* identity)
{
    DlssNrWatchedLists::Remove(1, &identity);
    Forget(identity);
}

struct State
{
    DlssNr::GpuLifetime lifetime { &Destroyed };
    std::shared_ptr<Generation> current;
    bool wasSelected = false, wasActive = false;
    std::string status;
    std::string unavailable;   // why Superior is not running this session; empty while it runs
    bool srUnavailable = false;
    std::weak_ptr<Generation> retiringPrivate;
};
// Same process-local ownership as janblade's g_nr. Keep unresolved GPU ownership at teardown.
State& GetState()
{
    static auto* value = new State;
    return *value;
}
// Set by the first Superior frame. Until then the NGX shutdown path does not touch the NR lock at all.
std::atomic_bool g_used = false;

void Say(const char* text)
{
    auto& s = GetState();
    if (s.status == text)
        return;
    s.status = text;
    if (*text)
        LOG_INFO("Superior Scaling: {}", text);
}

// Retire the current generation: its resources and NGX handles go once every list that used them has completed.
void Drop()
{
    auto& s = GetState();
    if (!s.current)
        return;
    auto old = std::move(s.current);
    if (old->feature)
        s.retiringPrivate = old;
    s.lifetime.Retire([old] {});
    s.lifetime.BeginGeneration();
}

// For the rest of the session (until Superior is selected again) the ordinary path runs, with Matched residual.
void GiveUp(const std::string& reason, bool srUnavailable)
{
    auto& s = GetState();
    s.unavailable = reason;
    s.srUnavailable = s.srUnavailable || srUnavailable;
    LOG_WARN("Superior Scaling unavailable: {}; using Matched residual instead", reason);
    Say("");
    Drop();
}

void Record(ID3D12GraphicsCommandList* cmd)
{
    auto& s = GetState();
    s.lifetime.Record(cmd, true);
    Watch(Util::RealObjectIdentity(cmd));
}

void MarkSingleSubmit(ID3D12GraphicsCommandList* cmd)
{
    if (g_used)
        GetState().lifetime.MarkSingleSubmit(cmd);
}

void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    auto& s = GetState();
    s.lifetime.Submitted(queue, count, lists);
    // The hook stopped watching these lists after this submission. A list whose recording is still open can be
    // submitted again (replayed) and is still to be Reset, so it stays watched until then; others are forgotten.
    if (s.current && s.current->creation)
        for (UINT i = 0; i < count; ++i)
            if (Util::RealObjectIdentity(lists[i]) == s.current->creation)
                s.current->creationSubmitted = true;
    if (s.current)
        for (UINT i = 0; i < count; ++i)
        {
            auto& g = *s.current;
            if (g.resident && g.resident->creation == Util::RealObjectIdentity(lists[i]))
                g.resident->submitted = true;
            if (g.ghosts && g.ghosts->commands == Util::RealObjectIdentity(lists[i]))
                g.ghosts->submitted = true;
        }
    for (UINT i = 0; i < count; ++i)
        if (s.lifetime.Open(lists[i]))
            Watch(lists[i]);
        else
            Forget(lists[i]);
}

void ResetRecording(ID3D12CommandList* cmd)
{
    auto& s = GetState();
    s.lifetime.ResetRecording(cmd);
    Forget(cmd);
    // The private DLSS creation was recorded on a list that is being reset without ever being submitted:
    // those creation commands never run, so the feature must be built again.
    if (!s.current)
        return;
    const auto identity = Util::RealObjectIdentity(cmd);
    auto& g = *s.current;
    if (g.ghosts && g.ghosts->commands == identity && !g.ghosts->submitted)
    {
        auto abandoned = std::shared_ptr<GhostHistory>(std::move(g.ghosts));
        s.lifetime.Retire([abandoned] {});
    }
    if (g.resident && g.resident->creation == identity && !g.resident->submitted)
    {
        auto abandoned = std::move(g.resident);
        s.lifetime.Retire([abandoned] {});
        g.residentAttempted = false;
    }
    if (g.creation == identity && !g.creationSubmitted)
        Drop();
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
              ID3D12Resource* low, Texture& output, Texture* keep = nullptr, uint32_t* immutableSlot = nullptr,
              bool outputStaysUav = false, bool keepStaysUav = false)
{
    output.To(cmd, Uav);
    if (keep)
        keep->To(cmd, Uav);
    if (!shader.DispatchSuperiorPass(cmd, c, source, model, original, motion, previous, low, output.Get(),
                                     keep ? keep->Get() : nullptr, immutableSlot))
        return false;
    const auto finish = [&](Texture& texture, bool staysUav)
    {
        if (!staysUav)
            texture.To(cmd, Srv);
        else
        {
            auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(texture.Get());
            cmd->ResourceBarrier(1, &barrier);
        }
    };
    finish(output, outputStaysUav);
    if (keep && keep != &output)
        finish(*keep, keepStaysUav);
    return true;
}

Constants GuideConstants(unsigned w, unsigned h, const DlssNr::GuideRegions& regions, const DlssNrFrameInfo& frame,
                         bool pixelUnits, bool hold)
{
    Constants c {};
    c.Mode = 10u;
    c.Width = w;
    c.Height = h;
    Fields::SetDepthRegion(c, regions.depth.x, regions.depth.y, regions.depth.width, regions.depth.height);
    Fields::SetMotionRegion(c, regions.motion.x, regions.motion.y, regions.motion.width, regions.motion.height);
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

// Immutable 11-float4 warp metadata uses the resolve's otherwise unused D3D12 motion SRV (t3).
// The generation owns its upload until all recorded command lists have retired. No per-frame copy/dispatch.
bool CreateWarp(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd)
{
    if (!g.packed)
        return true;
    if (!Ensure(g.warp, g.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 11, 1))
        return false;
    const auto data = pw::MakeShaderConstants(g.layout.warp);
    static_assert(sizeof(data) == 11 * 16);
    const auto desc = g.warp.Get()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bytes = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&g.warpUpload))))
        return false;
    void* mapped = nullptr;
    const D3D12_RANGE noRead { 0, 0 };
    if (FAILED(g.warpUpload->Map(0, &noRead, &mapped)))
        return false;
    std::memcpy(static_cast<char*>(mapped) + footprint.Offset, &data, sizeof(data));
    g.warpUpload->Unmap(0, nullptr);
    g.warp.To(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
    src.pResource = g.warpUpload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    dst.pResource = g.warp.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    g.warp.To(cmd, Srv);
    // Optional acceleration. Failure leaves the immutable metadata and arithmetic PSOs usable.
    if (Ensure(g.warpCache, g.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, std::max(11u, std::max(g.width, g.height)),
               3) &&
        shader.BuildSuperiorWarpCache(cmd, g.width, g.height, g.warp.Get(), g.warpCache.Get()))
        g.warpCache.To(cmd, Srv);
    else
        g.warpCache.resource.Reset();
    return true;
}

enum class Created
{
    Ok,
    ResourcesFailed, // our own surfaces or passes: worth another try once Superior is selected again
    SrUnavailable    // the NGX DLSS SR runtime: not here on this machine/runtime
};

NVSDK_NGX_Result CreateFeature(NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle*& feature, ID3D12GraphicsCommandList* cmd,
                               unsigned preset)
{
    for (const char* key :
         { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality })
        p->Set(key, preset);
    feature = nullptr;
    return NVNGXProxy::D3D12_CreateFeature()(cmd, NVSDK_NGX_Feature_SuperSampling, p, &feature);
}

void SetCreationParameters(const Generation& g, NVSDK_NGX_Parameter* p)
{
    const auto w = g.workWidth, h = g.workHeight;
    p->Set(NVSDK_NGX_Parameter_Width, w);
    p->Set(NVSDK_NGX_Parameter_Height, h);
    p->Set(NVSDK_NGX_Parameter_OutWidth, w);
    p->Set(NVSDK_NGX_Parameter_OutHeight, h);
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int) NVSDK_NGX_PerfQuality_Value_DLAA);
    const unsigned flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                           NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                           (g.inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
}

Created CreateTemporal(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd)
{
    Record(cmd); // includes unit-exposure initialization even if NGX creation fails
    const auto w = g.workWidth, h = g.workHeight;
    if (!Ensure(g.carrier, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, w, h) ||
        !Ensure(g.temporalOutput, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, w, h) ||
        !Ensure(g.temporalMotion, g.device.Get(), DXGI_FORMAT_R32G32_FLOAT, w, h) ||
        !Ensure(g.unit, g.device.Get(), DXGI_FORMAT_R32_FLOAT, 1, 1))
        return Created::ResourcesFailed;
    Constants unit {};
    unit.Mode = 7u;
    unit.Width = unit.Height = 1u;
    // Mode 7 only writes 1.0 (precompile/dlssnr.hlsl, "gMode == 7") and reads no SRV. g_nr.output, at rest in UAV
    // state, is just the stand-in every SRV slot must hold; it is never accessed through that descriptor.
    if (!Dispatch(shader, cmd, unit, g_nr.output, nullptr, nullptr, nullptr, nullptr, nullptr, g.unit))
        return Created::ResourcesFailed;
    if (!NVNGXProxy::InitDx12(g.device.Get()) || !NVNGXProxy::D3D12_AllocateParameters() ||
        !NVNGXProxy::D3D12_DestroyParameters() || !NVNGXProxy::D3D12_CreateFeature() ||
        !NVNGXProxy::D3D12_EvaluateFeature() || !NVNGXProxy::D3D12_ReleaseFeature())
        return Created::SrUnavailable;
    g.ngxEpoch = NVNGXProxy::Dx12Epoch();
    if (NVNGXProxy::D3D12_AllocateParameters()(&g.parameters) != NVSDK_NGX_Result_Success || !g.parameters)
    {
        g.parameters = nullptr;
        return Created::SrUnavailable;
    }
    SetCreationParameters(g, g.parameters);
    LOG_INFO("Superior Scaling: creating private DLSS preset {}", g.preset);
    Record(cmd);
    auto result = CreateFeature(g.parameters, g.feature, cmd, g.preset);
    g.actualPreset = g.preset;
    if (result != NVSDK_NGX_Result_Success || !g.feature)
    {
        // A runtime without the requested preset (newer 310.x DLLs dropped the CNN presets A-E): its default.
        static std::once_flag once;
        std::call_once(once, [&]
                       { LOG_WARN("Superior Scaling: private DLSS preset {} unavailable ({:X}); using the runtime's "
                                  "default preset", g.preset, (unsigned) result); });
        g.actualPreset = 0;
        result = CreateFeature(g.parameters, g.feature, cmd, NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    }
    g.creation = Util::RealObjectIdentity(cmd);
    return result == NVSDK_NGX_Result_Success && g.feature ? Created::Ok : Created::SrUnavailable;
}

static_assert(unsigned(NVSDK_NGX_DLSS_Hint_Render_Preset_D) == 4u &&
              unsigned(NVSDK_NGX_DLSS_Hint_Render_Preset_E) == 5u &&
              unsigned(NVSDK_NGX_DLSS_Hint_Render_Preset_K) == 11u);

#include "PrivateEffects.inl"

Constants Shaping(const Config& cfg, unsigned first, unsigned completed,
                  DlssNr::SuperiorScaling::FrequencyScales scales)
{
    Constants c {};
    c.ResidualHistoryValid = 1u;
    c.ResidualBlend = .015f;
    c.ResidualScale = 3.0f;
    c.ResidualConfidenceSensitivity = 1.0f;
    const auto gains = DlssNr::SuperiorScaling::BlockGains(cfg, first, completed, scales, g_nr.superiorMainOnlyGains);
    Fields::SetBandGains(c, gains.high, gains.low);
    Fields::SetInterPassLowField(c, true);
    c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;
    return c;
}

bool InterPass(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, const Config& cfg,
               ID3D12Resource* nativeProxy, ID3D12Resource* originalBase, ID3D12Resource* answer, unsigned covered,
               unsigned completed, DlssNr::SuperiorScaling::FrequencyScales scales)
{
    const auto desc = originalBase->GetDesc();
    const unsigned workW = unsigned(desc.Width), workH = desc.Height;
    auto c = Shaping(cfg, covered, completed, scales);
    c.DirectResolveFlags |= 16u | 64u;
    if (Fields::HighGain(c) == Fields::LowGain(c))
    {
        Fields::SetInterPassLowField(c, false);
        c.DirectResolveFlags |= 32u;
    }
    ID3D12Resource* low = nullptr;
    if (Fields::InterPassLowField(c))
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

DlssNr::Spatial::Constants UniformGuideConstants(const Generation& g, const DlssNr::GuideRegions& regions,
                                                 const DlssNrFrameInfo& frame, bool haveDepth)
{
    DlssNr::Spatial::Layout l {};
    l.nativeW = g.width;
    l.nativeH = g.height;
    l.modelW = g.workWidth;
    l.modelH = g.workHeight;
    l.warp.x = pw::BuildAxis(g.width, g.width, g.workWidth, 1, 0, 0);
    l.warp.y = pw::BuildAxis(g.height, g.height, g.workHeight, 1, 0, 0);
    const DlssNr::GuideExtent reference =
        frame.MotionVectorsLowResolution ? DlssNr::GuideExtent { frame.RenderSubrectWidth, frame.RenderSubrectHeight }
                                         : DlssNr::GuideExtent { frame.OutputWidth, frame.OutputHeight };
    auto c = DlssNr::Spatial::MakeGuideConstants(l, regions, reference, frame.MvScaleX, frame.MvScaleY);
    c.mode = 104u;
    c.padding[0] = haveDepth && regions.depth.valid() ? 1.f : 0.f;
    if (!haveDepth)
    {
        c.depthRect[0] = c.depthRect[1] = 0;
        c.depthRect[2] = c.depthRect[3] = 1;
    }
    return c;
}

bool MatchGuides(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth,
                 ID3D12Resource* motion, const DlssNr::GuideRegions& regions, const DlssNrFrameInfo& frame,
                 float depthPrecision = 0.f);

ID3D12Resource* Stabilize(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* proxy,
                          ID3D12Resource* answer, ID3D12Resource* originalMotion, const DlssNr::GuideRegions& regions,
                          const DlssNrFrameInfo& frame, bool reset, bool hold, ID3D12Resource* packedDepth,
                          ID3D12Resource* packedMotion, DlssNr::GuideExtent packedGuideExtent,
                          float packedDepthPrecision)
{
    if (!g.feature || !g.parameters || !NgxAlive(g.ngxEpoch))
        return nullptr;
    auto encode = GuideConstants(g.workWidth, g.workHeight, regions, frame, true, hold);
    encode.Mode = 39u; // v50: fixed carrier + motion; reuse this frame's exact NR depth.
    auto* motion = originalMotion;
    auto* temporalDepth = g.packed ? packedDepth : g.depth.Get();
    bool prepared = false;
    if (g.packed)
    {
        if (!packedDepth || !packedMotion)
            return nullptr;
        // Packed vectors already contain PwPack(p+v)-PwPack(p), in packed pixels. Never scale/warp twice.
        motion = packedMotion;
        Fields::SetMotionRegion(encode, 0, 0, g.workWidth, g.workHeight);
        encode.MvScaleX = encode.MvScaleY = hold ? 0.f : 1.f;
        // The allocation may exceed the active subrect under dynamic resolution.
        if (packedGuideExtent.width != g.workWidth || packedGuideExtent.height != g.workHeight)
        {
            // NR accepts an independent guide subrect. Private DLAA has no independent
            // depth-size parameter: reconstruct its guides on the carrier's raster.
            // These source vectors already contain the non-linear endpoint warp, in
            // the smaller guide lattice's pixels; never apply PwPack a second time.
            const unsigned gw = packedGuideExtent.width, gh = packedGuideExtent.height;
            const DlssNr::GuideRegions packedRegions { { 0, 0, gw, gh }, { 0, 0, gw, gh } };
            auto packedFrame = frame;
            packedFrame.MotionVectorsLowResolution = true;
            packedFrame.RenderSubrectWidth = packedFrame.OutputWidth = gw;
            packedFrame.RenderSubrectHeight = packedFrame.OutputHeight = gh;
            packedFrame.MvScaleX = packedFrame.MvScaleY = 1.f;
            encode.MvScaleX = hold ? 0.f : float(g.workWidth) / gw;
            encode.MvScaleY = hold ? 0.f : float(g.workHeight) / gh;
            if (!Ensure(g.depth, g.device.Get(), DXGI_FORMAT_R32_FLOAT, g.workWidth, g.workHeight))
                return nullptr;
            auto fused = UniformGuideConstants(g, packedRegions, packedFrame, true);
            DlssNr::Spatial::SetDepthPrecision(fused, packedDepthPrecision);
            fused.padding[1] = encode.MvScaleX;
            fused.padding[2] = encode.MvScaleY;
            Record(cmd);
            g.depth.To(cmd, Uav);
            g.carrier.To(cmd, Uav);
            g.temporalMotion.To(cmd, Uav);
            prepared = shader.DispatchSuperiorPrivateGuides(cmd, fused, proxy, answer, packedDepth, packedMotion,
                                                            g.depth.Get(), g.temporalMotion.Get(), g.carrier.Get());
            if (prepared)
            {
                g.depth.To(cmd, Srv);
                g.carrier.To(cmd, Srv);
                g.temporalMotion.To(cmd, Srv);
            }
            else
            {
                if (!MatchGuides(g, shader, cmd, packedDepth, packedMotion, packedRegions, packedFrame,
                                 packedDepthPrecision))
                    return nullptr;
                motion = g.motion.Get();
            }
            temporalDepth = g.depth.Get();
        }
        // Carrier preparation converts vector units once and writes zero motion while held.
        // The default, colour-sized guide path needs no extra guide dispatch.
    }
    else if (g.guidesMatched)
    {
        // Reuse the raw resampled NR field. Convert to working pixels once in this fused carrier pass.
        motion = g.motion.Get();
        Fields::SetMotionRegion(encode, 0, 0, g.workWidth, g.workHeight);
    }
    if (!prepared &&
        !Dispatch(shader, cmd, encode, proxy, answer, motion, nullptr, nullptr, nullptr, g.carrier, &g.temporalMotion))
        return nullptr;
    bool idle = false;
    float mix = g.presetFade.weight;
    if (g.resident && g.resident->ready)
    {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto& probe = *g.motionProbe;
        UpdateMotion(g, shader, cmd, originalMotion, regions.motion, frame, now, hold || reset);
        if (probe.selection.Advance(now))
            g.reset = true;
        idle = probe.selection.idle;
        mix *= probe.selection.fade.weight;
    }
    g.temporalOutput.To(cmd, Uav);
    auto* p = idle ? g.resident->parameters : g.parameters;
    auto* feature = idle ? g.resident->feature : g.feature;
    p->Set(NVSDK_NGX_Parameter_Color, g.carrier.Get());
    p->Set(NVSDK_NGX_Parameter_Output, g.temporalOutput.Get());
    p->Set(NVSDK_NGX_Parameter_Depth, temporalDepth);
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
    if (NVNGXProxy::D3D12_EvaluateFeature()(cmd, feature, p, nullptr) != NVSDK_NGX_Result_Success)
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
    if (g.preset == DlssNr::SuperiorScaling::TransformerK)
    {
        if (!RejectTransformerGhosts(g, shader, cmd, decode, temporalDepth, proxy, answer, frame, reset || g.reset,
                                     packedDepthPrecision))
            return nullptr;
    }
    else
    {
        // Stable CNN frames use the existing read-only decode. Only a fade needs the current edit.
        if (mix < 1.f)
            Fields::SetPrivateFade(decode, 1.f - mix);
        if (!Dispatch(shader, cmd, decode, g.temporalOutput.Get(), nullptr, mix < 1.f ? proxy : nullptr, nullptr,
                      mix < 1.f ? answer : nullptr, nullptr, g.carrier))
            return nullptr;
    }
    g.reset = false;
    return g.carrier.Get();
}

// The private preset Begin picks (PrivatePresetChoice): kept for the session so a moving Auto scale does not recreate
// the private DLSS and reset history; NR and NGX shutdown clear it, so a new device or runtime picks again.
DlssNr::SuperiorScaling::PrivatePresetChoice& PresetChoice()
{
    static DlssNr::SuperiorScaling::PrivatePresetChoice choice;
    return choice;
}

enum class Began
{
    Run,        // record Superior this frame
    Wait,       // packed metadata belongs to an unsubmitted recording on another list
    Unavailable // Superior cannot run: GiveUp() was called, skip this frame, the next one runs the ordinary path
};

// Called before recording the normal input and exposure path.
Began Begin(DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Device* device, DXGI_FORMAT format,
            unsigned width, unsigned height, unsigned workWidth, unsigned workHeight, float modelScale,
            const DlssNrFrameInfo& frame, bool temporal, bool haveDepth, const DlssNr::Spatial::Layout* layout)
{
    auto& state = GetState();
    g_used = true;
    state.lifetime.Collect();
    // WarpMap.hlsli maps uv * (gWidth, gHeight) -- the frame size the resolve and warp cache are dispatched at -- through
    // PwPack, which expects pixels of the layout's native size (gNativeWorkSize.xy). The packed layout is built from this
    // same frame, so they are equal; refuse a layout that is not rather than warp with the wrong scale.
    if (layout && (layout->nativeW != width || layout->nativeH != height))
    {
        GiveUp("its packed layout does not match the frame size", false);
        return Began::Unavailable;
    }
    const auto& cfg = *Config::Instance();
    using namespace DlssNr::SuperiorScaling;
    const unsigned preset = PresetChoice().Select(cfg.DlssNrSuperiorScalingPreset.value_or_default(),
                                                  cfg.DlssNrModelResolutionAuto.value_or_default(),
                                                  cfg.DlssNrWorkingScale.value_or_default(), modelScale);
    const bool shaping = temporal;
    temporal = shaping && preset != NoPrivateDlss;
    if (auto* previous = state.current.get())
    {
        const bool ngxGone = (previous->feature || previous->parameters) && !NgxAlive(previous->ngxEpoch);
        if (ngxGone)
            previous->DropNgx();
        const bool geometryChanged =
            previous->device.Get() != device || previous->width != width || previous->height != height ||
            previous->workWidth != workWidth || previous->workHeight != workHeight || previous->format != format ||
            previous->inverted != frame.DepthInverted || previous->beforeSr != frame.BeforeUpscale ||
            previous->packed != (layout != nullptr) || (layout && previous->layout != *layout);
        const bool selectionChanged = previous->temporal != temporal || previous->preset != preset;
        if (!ngxGone && !geometryChanged && haveDepth && previous->PrivateReady() &&
            (previous->actualPreset == CnnD || previous->actualPreset == CnnE))
        {
            const auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (previous->presetFade.Advance(!selectionChanged, now) > 0.f && selectionChanged)
            {
                Record(cmd);
                return Began::Run;
            }
        }
        if (ngxGone || geometryChanged || selectionChanged)
        {
            Drop();
            state.lifetime.Collect(); // if the old recording is already retired, switch in this frame
            Say("");
            if (geometryChanged)
                g_nr.reset = true;
        }
    }
    if (!state.current)
    {
        state.current = std::make_shared<Generation>();
        auto& g = *state.current;
        g.device = device;
        g.width = width;
        g.height = height;
        g.workWidth = workWidth;
        g.workHeight = workHeight;
        g.format = format;
        g.inverted = frame.DepthInverted;
        g.beforeSr = frame.BeforeUpscale;
        g.temporal = temporal;
        g.shaping = shaping;
        g.packed = layout != nullptr;
        if (layout)
            g.layout = *layout;
        g.preset = preset;
    }
    auto& g = *state.current;
    Record(cmd);
    const auto identity = Util::RealObjectIdentity(cmd);
    if (g.packed && !g.warp.Get())
    {
        if (!CreateWarp(g, shader, cmd))
        {
            GiveUp("its packed warp could not be created", false);
            return Began::Unavailable;
        }
        g.creation = identity;
        g.creationSubmitted = false;
    }
    if (g.packed && !g.creationSubmitted && g.creation != identity)
        return Began::Wait;

    // Compose the current edit while a private feature is unavailable or its creation is pending.
    if (!haveDepth)
        g.reset = true;
    if (!temporal || !haveDepth || g.privateFailed)
        return Began::Run;
    if (!g.feature)
    {
        if (!state.retiringPrivate.expired())
        {
            Say("Waiting for the previous private DLSS to retire. Using the current NR edit.");
            return Began::Run;
        }
        if (CreateTemporal(g, shader, cmd) != Created::Ok)
        {
            g.privateFailed = true;
            Say("Private DLSS unavailable. Using the current NR edit.");
            return Began::Run;
        }
        g.creationSubmitted = false;
        g.createEpoch = frame.SubmissionEpoch;
        g.waitedFrames = 0;
        return Began::Run;
    }
    if (g.waitedFrames != UINT_MAX)
    {
        if (!g.creationSubmitted || frame.SubmissionEpoch == g.createEpoch)
        {
            if (++g.waitedFrames > kCreationWaitFrames)
            {
                g.privateFailed = true;
                Say("Private DLSS creation was not submitted. Using the current NR edit.");
            }
            return Began::Run;
        }
        g.waitedFrames = UINT_MAX;
        LOG_INFO("Superior Scaling: private DLSS preset {} ready after submission", g.actualPreset);
        Say("");
    }
    UpdateResidentCnn(g, cmd, frame);
    return Began::Run;
}

// The NGX D3D12 runtime is going away; it destroys every handle itself, so ours are dropped, not released.
void NgxShutdown()
{
    auto& s = GetState();
    if (s.current)
        s.current->DropNgx();
    Drop();
    PresetChoice() = {};
}

// NR shutdown: retire the current generation and run what has already completed; anything still in flight keeps its
// ownership for process teardown (like DeferredSr::Shutdown), never released under the GPU.
void Shutdown()
{
    auto& s = GetState();
    Drop();
    s.lifetime.Collect();
    PresetChoice() = {};
}

bool MatchGuides(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth,
                 ID3D12Resource* motion, const DlssNr::GuideRegions& regions, const DlssNrFrameInfo& frame,
                 float depthPrecision)
{
    if (!Ensure(g.depth, g.device.Get(), DXGI_FORMAT_R32_FLOAT, g.workWidth, g.workHeight) ||
        !Ensure(g.motion, g.device.Get(), DXGI_FORMAT_R32G32_FLOAT, g.workWidth, g.workHeight))
        return false;
    constexpr unsigned method = 2u;
    auto c = UniformGuideConstants(g, regions, frame, depth != nullptr);
    DlssNr::Spatial::SetDepthPrecision(c, depthPrecision);
    Record(cmd);
    g.depth.To(cmd, Uav);
    g.motion.To(cmd, Uav);
    g.guidesMatched =
        shader.DispatchSpatial(cmd, c, depth ? depth : motion, depth, motion, g.depth.Get(), g.motion.Get(), method);
    g.depth.To(cmd, Srv);
    g.motion.To(cmd, Srv);
    return g.guidesMatched;
}

ID3D12Resource* FinalLow(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* proxy,
                         ID3D12Resource* residual, bool signedResidual)
{
    const auto desc = proxy->GetDesc();
    const unsigned lowW = (unsigned(desc.Width) + 7u) / 8u, lowH = (desc.Height + 7u) / 8u;
    if (!Ensure(g.finalLow, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, lowW, lowH))
        return nullptr;
    Constants c {};
    c.Mode = 18u;
    c.Transfer = signedResidual ? 1u : 0u;
    c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;
    c.Width = lowW;
    c.Height = lowH;
    return Dispatch(shader, cmd, c, proxy, residual, nullptr, nullptr, nullptr, nullptr, g.finalLow) ? g.finalLow.Get()
                                                                                                     : nullptr;
}
} // namespace SuperiorRun
} // namespace
