// "Tune for this scene", D3D12 side: the GPU resources, the copies and the stats pass. The run itself -- the menu's
// requests, the sweep's EV per evaluation, the readback bookkeeping, the log -- is shared with Vulkan
// (DlssNr_ExposureCalibrate_Run.h, which also describes the per-evaluation flow); DlssNr_ExposureCalibrate.h has the
// sweep and the why. Included by DlssNr_Dx12.cpp inside its anonymous namespace, right after g_frames.
//
// The input has to be copied at the encode: when the game's colour is not UAV-capable the resolve writes into the
// encode's untouched copy.

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to);
ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, unsigned int height);
void ParkNrResource(ID3D12Resource*& res);

namespace Cal = DlssNrExposureCalibrate;

// The stats grid: 64x64 tiles, four RGBA32F texels each, side by side (Cal::ReduceGrid has the layout). 256 * 16 = 4096
// bytes a row, a multiple of the 256 a texture-to-buffer copy needs, so the readback is a flat array.
constexpr unsigned int kCalGridWidth = Cal::kGridTiles * Cal::kGridColumns;
constexpr unsigned int kCalGridHeight = Cal::kGridTiles;
constexpr unsigned int kCalRowBytes = Cal::kGridRowFloats * sizeof(float);
constexpr unsigned int kCalGridBytes = kCalRowBytes * kCalGridHeight;
static_assert(kCalRowBytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);

// The shared run (DlssNr_ExposureCalibrate.cpp holds its game side and the menu's API).
Cal::RunState& g_cal = Cal::TheRun();

class CalibrationDx12 final : public Cal::Backend
{
  public:
    ID3D12Device* device = nullptr; // this evaluation's, for Create
    ID3D12Resource* input[2] = {};
    ID3D12Resource* output[2] = {};
    bool inputReadable[2] = {}; // NON_PIXEL_SHADER_RESOURCE, else COPY_DEST
    bool outputReadable[2] = {};
    ID3D12Resource* grid = nullptr;
    ID3D12Resource* readback[Cal::kRing] = {};

    bool Held() const override { return grid != nullptr; }

    // Parked, not released: the last evaluation's copies may still be in flight.
    void Release() override
    {
        for (unsigned int i = 0; i < 2; ++i)
        {
            ParkNrResource(input[i]);
            ParkNrResource(output[i]);
            inputReadable[i] = outputReadable[i] = false;
        }

        ParkNrResource(grid);

        for (unsigned int i = 0; i < Cal::kRing; ++i)
            ParkNrResource(readback[i]);
    }

    bool Create() override
    {
        if (device == nullptr)
            return false;

        grid = CreateScratch(device, DXGI_FORMAT_R32G32B32A32_FLOAT, kCalGridWidth, kCalGridHeight);

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

        bool ok = grid != nullptr;

        for (unsigned int i = 0; ok && i < Cal::kRing; ++i)
            ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&readback[i])));

        if (!ok)
            Release();

        return ok;
    }

    // An unmappable slot is an empty sample, which the sweep drops.
    Cal::Stats Reduce(unsigned int slot) override
    {
        void* mapped = nullptr;
        D3D12_RANGE range { 0, kCalGridBytes };

        if (readback[slot] == nullptr || FAILED(readback[slot]->Map(0, &range, &mapped)) || mapped == nullptr)
        {
            Cal::Stats empty {};
            empty.detailBand = NAN;
            return empty;
        }

        const Cal::Stats s = Cal::ReduceGrid(static_cast<const float*>(mapped));
        D3D12_RANGE none { 0, 0 };
        readback[slot]->Unmap(0, &none);
        return s;
    }
};

CalibrationDx12 g_calDx;

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

// The pinned white point of this evaluation, or 0 when no run is on.
float CalibrationWhitePoint() { return g_cal.frameWhitePoint; }

bool CalibrationWanted() { return Cal::WantedNow(); }

void CalibrationIdleFrame() { Cal::IdleNow(); }

// Everything availability depends on, from this evaluation. Only called while wanted (it parses the Trim anchors).
Cal::Situation CalibrationSituation(const Config& cfg, bool usingAutoExposure, bool isHdrBuffer, bool finishedPicture,
                                    bool gameExposureNow, bool colourConverted)
{
    Cal::Situation s;
    s.source = cfg.DlssNrWhitePointSource.value_or_default();
    s.anchorKey = DlssNrGameScale::AnchorKey(g_nr.gamePreExposure, g_nr.gameExposure);
    s.proxyMode = cfg.DlssNrUseProxy.value_or_default();
    s.holdFrame = cfg.DlssNrHoldFrame.value_or_default();
    s.finishedPicture = finishedPicture;
    s.hdr = isHdrBuffer;
    s.colourConverted = colourConverted;
    s.autoRunning = usingAutoExposure;
    s.followLocked = DlssNr::FollowGameOn(cfg) && DlssNrFollowGame::Instance().Locked();
    s.followDisagreementEv = FollowDisagreementEv();
    s.gameExposureNow = gameExposureNow;
    s.gameExposureReading = g_nr.gameExposure > 1e-6f;
    // Where NR runs without a Tune: not Before SR under Ray Reconstruction or an unsupported colour layout, so no wait.
    s.beforeSrSet = cfg.DlssNrRunBeforeSr.value_or_default() && g_nr.beforeSrPlacement;

    return s;
}

// The base white point the tuned source's Trim multiplies, 0 without a reading: Game exposure's is the same as the
// encode's, so a sweep measures the white the picture is made with (DlssNrGameScale::WhiteBase; with the game's
// exposure it needs a reading, as is it does not), Automatic's is its own metering, following the game's exposure
// or not (Cal::RelearnFollowOnStart has why).
float CalibrationBase(const Config& cfg)
{
    if (cfg.DlssNrWhitePointSource.value_or_default() == 1)
        return g_nr.gameExposure > 1e-6f ? DlssNrGameScale::WhiteBase(cfg.DlssNrGameExposureScale.value_or_default(),
                                                                      g_nr.gamePreExposure, g_nr.gameExposure)
                                         : 0.0f;

    return AutoOwnBaseWhitePoint();
}

// Before the white point is resolved, while CalibrationWanted().
void CalibrationBeginFrame(const Config& cfg, ID3D12Device* device, unsigned int width, unsigned int height,
                           const Cal::Situation& situation, float baseWhitePoint)
{
    g_calDx.device = device;
    Cal::BeginFrameNow(g_calDx, cfg, width, height, situation, baseWhitePoint, g_frames);
}

bool CalibrationActive() { return Cal::Active(g_cal, g_calDx); }

void CalibrationShutdown() { Cal::ShutdownNow(g_calDx); }

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
    CalibrationCopy(cmdList, device, g_calDx.input[i], g_calDx.inputReadable[i], untouched,
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
    CalibrationCopy(cmdList, device, g_calDx.output[i], g_calDx.outputReadable[i], edited,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);

    if (!g_cal.measurePending)
        return;

    const bool haveAll = g_calDx.outputReadable[i] && g_calDx.outputReadable[prev] && g_calDx.inputReadable[i] &&
                         g_calDx.inputReadable[prev] && modelInput != nullptr;
    const unsigned int slot = Cal::FreeSlot(g_cal);

    if (!haveAll || slot == Cal::kRing)
        return; // left pending: the next CalibrationBeginFrame returns the ticket empty

    // The stats shader reads its own layout over the first fields (dlssnr_detail_stats.hlsl's cbuffer): Mode unused,
    // WhitePoint = the measuring white point, Width/Height = the frame, TransferStrength/ColourStrength = the damage
    // thresholds.
    DlssNrConstants params {};
    params.Mode = 0;
    params.WhitePoint = g_cal.measureWhitePoint;
    params.Width = width;
    params.Height = height;
    const DlssNrProxyCurve::TuneThresholds damage =
        DlssNrProxyCurve::Thresholds(Config::Instance()->DlssNrReversibleMode.value_or_default());
    params.TransferStrength = damage.shoulder;
    params.ColourStrength = damage.floor;

    if (!pass->DispatchDetailStats(cmdList, params, g_calDx.output[i], g_calDx.output[prev], g_calDx.input[i],
                                   g_calDx.input[prev], modelInput, g_calDx.grid))
        return;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = g_calDx.grid;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_calDx.readback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kCalGridWidth;
    dst.PlacedFootprint.Footprint.Height = kCalGridHeight;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kCalRowBytes;

    Barrier(cmdList, g_calDx.grid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, g_calDx.grid, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    Cal::Submitted(g_cal, slot, g_frames);
}
