// Compress screen edges, D3D12 (menu: NR Input, "Compress screen edges"; ini [DlssNr] Spatial*).
//
// NR works on a smaller picture: the middle of the frame stays 1:1 and the edges are squeezed into the rest of the space
// (DlssNr_Spatial.h has the layout, precompile/dlssnr_spatial.hlsl the three passes). A frame goes
//   encode -> pack the colour (100) and the depth + motion (101) -> the model at the packed size ->
//   unpack the packed model input and the model's answer to the ordinary grid (102) -> the resolve.
// The ordinary grid is the one NR works on without compression (the model resolution, 16-pixel grid), so the resolve,
// the enlargement and the supersample down-leg see the same kind of pair they always did, only softer at the edges:
// both halves go through the same resampling, so what the resolve carries over to the frame is the model's change.
//
// Motion is packed by its end points (Pack(p + v) - Pack(p)), so the packed vectors are exact in packed pixels and the
// model gets a scale of 1. The packed textures are RGBA16F, also the model's own input and output surfaces: the answer
// is a resample of a resample, not something the swap chain's format should quantise.
//
// Anything that goes wrong turns compression off (Status::TurnedOff*), not NR: the next frame runs the ordinary path,
// and a change of layout (or Retry) tries again. Reuse detail between frames does not run while this does.
//
// Included inside DlssNr_Dx12.cpp's anonymous namespace, after DlssNr_SceneCut.inl. Dispatch calls Begin where it
// decides the frame, Prepare once the layout is known to be wanted, and the rest around the model, all under g_nrMutex.
// The menu reads Published(), a copy made under a lock of its own.
namespace EdgeCompression
{
namespace Sp = DlssNr::Spatial;

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Everything that makes last frame's packed picture a different picture from this frame's: the layout, and the formats
// and sizes of what is packed.
struct Signature
{
    DXGI_FORMAT colour = DXGI_FORMAT_UNKNOWN, depth = DXGI_FORMAT_UNKNOWN, motion = DXGI_FORMAT_UNKNOWN;
    unsigned int depthW = 0, depthH = 0, motionW = 0, motionH = 0;
    bool operator==(const Signature& o) const
    {
        return colour == o.colour && depth == o.depth && motion == o.motion && depthW == o.depthW &&
               depthH == o.depthH && motionW == o.motionW && motionH == o.motionH;
    }
};

Sp::Layout layout;                      // this frame's
Sp::Status fallback = Sp::Status::Off;  // why compression turned itself off; held until the layout changes or Retry
Signature signature;
bool signatureValid = false;

ID3D12Resource* color = nullptr;  // the packed colour (model input)
ID3D12Resource* depth = nullptr;  // R32F
ID3D12Resource* motion = nullptr; // RG32F
ID3D12Resource* proxy = nullptr;  // the packed model input, unpacked to the ordinary grid
ID3D12Resource* answer = nullptr; // the model's answer, unpacked the same way
ID3D12Resource* proxyNative = nullptr;  // supersampling only: both averaged down to native
ID3D12Resource* answerNative = nullptr;
OS_Dx12* proxyDown = nullptr; // the down-leg for the proxy; the answer goes through g_nr.superDown, the same filter
unsigned int madeFor[6] = {}; // packed w/h, ordinary w/h, native w/h the textures above were made for
bool madeSuper = false;

std::mutex publishedLock;
Sp::Published published;

void Publish(const Sp::Layout& l, Sp::Status status)
{
    Sp::Published p = Sp::Publish(l, status, false);
    std::lock_guard<std::mutex> lock(publishedLock);
    published = p;
}

void Release()
{
    for (ID3D12Resource** r : { &color, &depth, &motion, &proxy, &answer, &proxyNative, &answerNative })
        ParkNrResource(*r);

    for (unsigned int& v : madeFor)
        v = 0;
}

void ReleaseDown()
{
    delete proxyDown;
    proxyDown = nullptr;
}

// Why compression is not applied or has turned itself off, for the log (the menu has its own words).
const char* Describe(Sp::Status status)
{
    switch (status)
    {
    case Sp::Status::Off: return "off";
    case Sp::Status::Active: return "on";
    case Sp::Status::Proxy: return "not used with the driver-proxy backend";
    case Sp::Status::BadSettings: return "a setting is outside its range";
    case Sp::Status::TooSmall: return "the packed picture would be under a quarter of the frame";
    case Sp::Status::NothingToCompress: return "the working size is 100% on both axes";
    case Sp::Status::ThinEdge: return "the layout leaves less than one pixel at an edge";
    case Sp::Status::TurnedOffResources: return "its shader or textures could not be created";
    case Sp::Status::TurnedOffDispatch: return "a compute pass failed";
    case Sp::Status::TurnedOffModel: return "NR rejected the packed picture";
    }
    return "?";
}

// Decides this frame. Returns whether the picture is packed. resetHistory is set when the packed picture is a different
// one from last frame's, which also lifts a held fallback so the new layout gets its try.
bool Begin(const Config& cfg, unsigned int width, unsigned int height, float scale, bool proxyBackend,
           const Signature& now, bool& resetHistory)
{
    const Sp::Settings settings = Sp::ReadSettings(cfg);
    const Sp::Layout next = Sp::Build(settings, width, height, scale, true);

    if (signatureValid && (layout != next || !(signature == now)))
    {
        if (layout.requested || next.requested)
            resetHistory = true;
        fallback = Sp::Status::Off;
    }

    layout = next;
    signature = now;
    signatureValid = true;

    Sp::Status status = next.status;

    if (settings.enabled && proxyBackend)
        status = Sp::Status::Proxy;
    else if (next.active && fallback != Sp::Status::Off)
        status = fallback;

    const bool active = status == Sp::Status::Active;
    Publish(next, status);

    static Sp::Status loggedStatus = Sp::Status::Off;
    static unsigned int loggedW = 0, loggedH = 0;

    if (status != loggedStatus || (active && (loggedW != next.modelW || loggedH != next.modelH)))
    {
        loggedStatus = status;
        loggedW = active ? next.modelW : 0;
        loggedH = active ? next.modelH : 0;

        if (active)
            LOG_INFO("DLSS-NR compress screen edges: on, the model works on {}x{} instead of {}x{} ({:.0f}% of the "
                     "pixels), middle {:.0f}%x{:.0f}% of the frame",
                     next.modelW, next.modelH, next.ordinaryW, next.ordinaryH,
                     100.0 * next.modelW * next.modelH / (double) (next.ordinaryW * (double) next.ordinaryH),
                     100.0 * (next.centerBounds.right - next.centerBounds.left),
                     100.0 * (next.centerBounds.bottom - next.centerBounds.top));
        else if (settings.enabled)
            LOG_INFO("DLSS-NR compress screen edges: not applied ({})", Describe(status));
        else
            LOG_INFO("DLSS-NR compress screen edges: off");
    }

    return active;
}

// Compression turned itself off; NR carries on from the next frame without it.
void TurnOff(Sp::Status why)
{
    fallback = why;
    Publish(layout, why);
    LOG_WARN("DLSS-NR compress screen edges turned itself off: {}; NR continues without it", Describe(why));
}

bool Make(ID3D12Resource*& slot, ID3D12Device* device, DXGI_FORMAT format, unsigned int w, unsigned int h)
{
    slot = CreateScratch(device, format, w, h);
    return slot != nullptr;
}

// The textures for this layout, made again when the sizes change. False if one could not be made.
bool Prepare(ID3D12Device* device, const Sp::Layout& l, bool supersample)
{
    const unsigned int want[6] = { l.modelW, l.modelH, l.ordinaryW, l.ordinaryH, l.nativeW, l.nativeH };

    if (color != nullptr && std::equal(std::begin(want), std::end(want), std::begin(madeFor)) &&
        madeSuper == supersample)
        return true;

    Release();

    bool ok = Make(color, device, kFormat, l.modelW, l.modelH) &&
              Make(depth, device, DXGI_FORMAT_R32_FLOAT, l.modelW, l.modelH) &&
              Make(motion, device, DXGI_FORMAT_R32G32_FLOAT, l.modelW, l.modelH) &&
              Make(proxy, device, kFormat, l.ordinaryW, l.ordinaryH) &&
              Make(answer, device, kFormat, l.ordinaryW, l.ordinaryH);

    if (ok && supersample)
        ok = Make(proxyNative, device, kFormat, l.nativeW, l.nativeH) &&
             Make(answerNative, device, kFormat, l.nativeW, l.nativeH);

    if (!ok)
    {
        Release();
        return false;
    }

    std::copy(std::begin(want), std::end(want), std::begin(madeFor));
    madeSuper = supersample;
    return true;
}

// Dispatch went away or NR was shut down.
void Shutdown()
{
    for (ID3D12Resource** r : { &color, &depth, &motion, &proxy, &answer, &proxyNative, &answerNative })
    {
        if (*r != nullptr)
            (*r)->Release();
        *r = nullptr;
    }

    for (unsigned int& v : madeFor)
        v = 0;

    ReleaseDown();
    layout = {};
    fallback = Sp::Status::Off;
    signatureValid = false;
    std::lock_guard<std::mutex> lock(publishedLock);
    published = {};
}
} // namespace EdgeCompression
