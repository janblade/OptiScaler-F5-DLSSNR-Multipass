#include "pch.h"

#include "DlssNr_ExposureScan.h"
#include "DlssNr_ExposureAuto.h"
#include "DlssNr_ExposureMatch.h"
#include "DlssNr_ExposureScanCore.h"
#include "DlssNr_ExposureScanRecord.h"
#include "DlssNr_ExposureScanShape.h"
#include "DlssNr_ExposureScanSource.h"
#include "DlssNr_GameDefaults.h"

#include <shaders/dlssnr/DlssNr_ExposureAdapt.h>

#include <SysUtils.h>

#include <Config.h>
#include <Util.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <string>

static_assert(DXGI_FORMAT_R32G32_TYPELESS == DlssNrExposureScanShape::kDxgiR32G32Typeless);
static_assert(DXGI_FORMAT_R32G32_FLOAT == DlssNrExposureScanShape::kDxgiR32G32Float);
static_assert(DXGI_FORMAT_R16G16_TYPELESS == DlssNrExposureScanShape::kDxgiR16G16Typeless);
static_assert(DXGI_FORMAT_R16G16_FLOAT == DlssNrExposureScanShape::kDxgiR16G16Float);
static_assert(DXGI_FORMAT_R32_TYPELESS == DlssNrExposureScanShape::kDxgiR32Typeless);
static_assert(DXGI_FORMAT_R32_FLOAT == DlssNrExposureScanShape::kDxgiR32Float);
static_assert(DXGI_FORMAT_R16_TYPELESS == DlssNrExposureScanShape::kDxgiR16Typeless);
static_assert(DXGI_FORMAT_R16_FLOAT == DlssNrExposureScanShape::kDxgiR16Float);

namespace DlssNr
{
namespace ExposureScan
{
namespace
{
using Core::Tracked;

// A cap on how many candidates are tracked (Core::kMaxCandidates). Kept low originally as a statement that a tight
// filter should find only a handful -- but buffer-heavy engines crowd the real exposure out of a low cap: Cyberpunk's
// REDengine creates dozens of tiny UAV buffers the same 4/12 bytes as an exposure, and its real one can land past slot
// 24 (slot 61 on 2026-10-18). Now that the scan is crash-safe (references dropped at feature teardown), the real
// discriminator is MOVEMENT, not scarcity, so a larger cap costs only a few tiny copies a frame and stops the answer
// being crowded out.
constexpr size_t kMaxCandidates = Core::kMaxCandidates;

// Ring depth for the readbacks (Core::kSlots). Four, so the slot being read is four frames behind the slot being
// written and the read never waits on the GPU. Same depth and the same reason as the meter's.
constexpr unsigned int kSlots = Core::kSlots;

// Every candidate's value lands in one buffer, at its own offset, so there is one copy per candidate
// but only one buffer per slot. 16 bytes each is enough for the widest format worth reading.
// D3D12 requires a placed-footprint offset to be a multiple of
// D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512). A texture candidate at index i copies to i*kStride,
// so the stride is that alignment; buffer copies have no such rule and are unaffected by the waste.
constexpr unsigned int kStride = 512;

// What an exposure could plausibly be. Outside these it is a flag, a counter, a sentinel or a
// zeroed buffer nobody has written yet -- Nioh 3 offers all four, including one holding 1000000.
constexpr float kFloor = 1e-6f;
constexpr float kCeiling = 1e4f;

struct ScanState
{
    unsigned int examined = 0;
    // By slot: a freed slot stays until a new candidate takes it (DlssNr_ExposureScanShape.h)
    std::vector<Tracked> tracked;
    unsigned long long frames = 0; // NR frames seen (Core::EndFrameLocked)
    const char* status = "not started";
    bool complained = false;
    unsigned int nearMissLogged = 0;   // bounded diagnostic; see NoteResource

    // The D3D12 backend's readback ring, and the generation each slot's copy was taken for.
    ID3D12Resource* readback[kSlots] = {};
    uint32_t readbackGeneration[kSlots][kMaxCandidates] = {};
    unsigned long long frames12 = 0;

    // Core::NoteHandedLocked: the game's own handed exposure for the coming EndFrameLocked, when the pass sees a copy
    const void* handedNote = nullptr;
    Api handedNoteApi = Api::D3D12;

    // The recording (DlssNr_ExposureScanRecord.h). `listVersion` changes with every adopt and drop; `namedVersion` /
    // `namedHanded` are what the last names line said, so a new line is written only when the list changes.
    bool recording = false;
    unsigned int listVersion = 0;
    unsigned int namedVersion = (unsigned int) -1;
    int namedHanded = -1;
    std::chrono::steady_clock::time_point recordStart;
};

ScanState g_scan;
std::mutex g_scanMutex;

// Formats an exposure could plausibly be in, the same table on every API (DlssNr_ExposureScanShape.h, where the reasons
// are). outBytes is the size of the FIRST channel only -- that is the one texel this reads back. The copy uses the real
// format (outFormat) so it matches the source texture; the read uses outBytes.
bool PlausibleFormat(DXGI_FORMAT f, unsigned int* outBytes, const char** outName, DXGI_FORMAT* outFormat)
{
    const DlssNrExposureScanShape::Format format = DlssNrExposureScanShape::DxgiFloatFormat((uint32_t) f);
    *outFormat = f;
    *outBytes = format.bytes;
    *outName = format.name;
    return format.bytes != 0;
}

float HalfToFloatImpl(uint16_t h)
{
    const uint32_t sign = (uint32_t) (h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1Fu;
    uint32_t mantissa = h & 0x3FFu;

    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            const uint32_t bits = sign;
            float out;
            std::memcpy(&out, &bits, sizeof(out));
            return out;
        }

        // Subnormal: normalise it by hand.
        exponent = 1;

        while ((mantissa & 0x400u) == 0)
        {
            mantissa <<= 1;
            --exponent;
        }

        mantissa &= 0x3FFu;
    }
    else if (exponent == 31)
    {
        const uint32_t bits = sign | 0x7F800000u | (mantissa << 13);
        float out;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    const uint32_t bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (res == nullptr || from == to)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = res;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &barrier);
}

bool EnsureReadback(ID3D12Device* device)
{
    for (unsigned int i = 0; i < kSlots; ++i)
    {
        if (g_scan.readback[i] != nullptr)
            continue;

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kStride * kMaxCandidates;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&g_scan.readback[i]))))
        {
            g_scan.status = "could not allocate the readback buffers";
            return false;
        }
    }

    return true;
}

// Whether the scan should be running at all.
//
// Choosing it as the white point's source is the whole of the answer for anybody using this.
// The separate settings survive as developer overrides, for the one case a user has no reason
// to want: watching the scan in a game that supplies a REAL exposure, so the two can be
// compared in the log (ScanExposure), or recording it to a file (RecordExposureScan). That is
// validation work, not a control, and it does not belong in a panel.
//
// Auto (the default white point source) wants the scan too, since story 5: it looks for the game's exposure by itself
// and lets go of what it does not need (AutoFrameLocked). The asked-for modes keep every candidate, as before.
bool AskedFor()
{
    return Config::Instance()->DlssNrWhitePointSource.value_or_default() == DlssNrExposureDefault::kSourceScan ||
           Config::Instance()->DlssNrScanExposure.value_or_default() ||
           Config::Instance()->DlssNrRecordExposureScan.value_or_default();
}

bool AutoOn()
{
    return Config::Instance()->DlssNrWhitePointSource.value_or_default() == DlssNrExposureDefault::kSourceAuto;
}

bool Wanted() { return AskedFor() || AutoOn(); }

// What a module is to the scan: the game's, someone else's (OptiScaler, middleware), or one that only forwards the
// game's call (Streamline's proxy device, DlssNrExposureScanSource::PassesThrough). Cached per module; a skipped module
// is logged once. Under g_scanMutex.
enum class Maker
{
    Game,
    Other,
    Forwarder
};

Maker MakerOfLocked(HMODULE module)
{
    static std::unordered_map<HMODULE, Maker> known;

    if (const auto it = known.find(module); it != known.end())
        return it->second;

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const std::string name = wstring_to_string(std::filesystem::path(path).filename().wstring());

    Maker maker = Maker::Game;

    if (module != dllModule && DlssNrExposureScanSource::PassesThrough(name))
        maker = Maker::Forwarder;
    else if (!DlssNrExposureScanSource::FromGame(name, module == dllModule))
        maker = Maker::Other;

    known.emplace(module, maker);

    if (maker == Maker::Other)
        LOG_INFO("DLSS-NR exposure scan: skipping small resources made by {} (not the game's)",
                 module == dllModule ? std::string("OptiScaler") : name);
    else if (maker == Maker::Forwarder)
        LOG_INFO("DLSS-NR exposure scan: {} forwards the game's calls; the module that called it decides", name);

    return maker;
}

// Whether the resource came from the game (DlssNr_ExposureScanSource.h). Asked only for resources that already look
// like an exposure, so the lookup is rare. `caller` is the creation call's return address; when that is a forwarder
// (the game calls Streamline's proxy device, whose call reaches the detoured device function), the stack is walked to
// the first module that is neither OptiScaler nor a forwarder, and that one decides. Under g_scanMutex.
bool FromGameLocked(void* caller)
{
    const HMODULE module = caller != nullptr ? Util::GetCallerModule(caller) : nullptr;

    if (module == nullptr)
        return true;

    const Maker maker = MakerOfLocked(module);

    if (maker != Maker::Forwarder)
        return maker == Maker::Game;

    void* frames[32] = {};
    const USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);

    for (USHORT i = 0; i < count; ++i)
    {
        const HMODULE frameModule = Util::GetCallerModule(frames[i]);

        if (frameModule == nullptr || frameModule == dllModule)
            continue;

        const Maker frameMaker = MakerOfLocked(frameModule);

        if (frameMaker != Maker::Forwarder)
            return frameMaker == Maker::Game;
    }

    return true; // nothing above the forwarder: not known to be someone else's
}

// The recording's file writer. The render thread only appends text; a thread of its own opens the file (the log's name
// with ".exposurescan.csv", in the log's folder, started over each session), writes and flushes once a second. Writing
// and flushing on the render thread made NBA 2K27 stutter (2026-10-14: gone with the scan off, NR's GPU cost
// unchanged). Made once and never freed, so the detached thread never outlives it.
class RecordWriter
{
  public:
    static RecordWriter& Instance()
    {
        static RecordWriter* writer = new RecordWriter();
        return *writer;
    }

    void Append(std::string text)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_failed)
            return;

        _pending += text;

        if (!_started)
        {
            _started = true;
            std::filesystem::path path(Config::Instance()->LogFileName.value_or_default());
            path.replace_extension(L".exposurescan.csv");
            std::thread([this, path] { Run(path); }).detach();
        }
    }

  private:
    void Run(const std::filesystem::path& path)
    {
        std::FILE* file = nullptr;

        if (_wfopen_s(&file, path.c_str(), L"w") != 0 || file == nullptr)
        {
            LOG_WARN("DLSS-NR exposure scan: could not open the recording {}", wstring_to_string(path.wstring()));
            std::lock_guard<std::mutex> lock(_mutex);
            _failed = true;
            _pending.clear();
            return;
        }

        LOG_INFO("DLSS-NR exposure scan: recording every candidate to {}", wstring_to_string(path.wstring()));

        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            std::string text;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                text.swap(_pending);
            }

            if (!text.empty())
            {
                std::fwrite(text.data(), 1, text.size(), file);
                std::fflush(file);
            }
        }
    }

    std::mutex _mutex;
    std::string _pending;
    bool _started = false;
    bool _failed = false;
};

// One row of the recording, every kEveryFrames frames, only when asked for (RecordExposureScan): a scan that runs for
// the picture writes no file. Under g_scanMutex.
void RecordLocked(const FrameExposure& frame)
{
    using namespace DlssNrExposureScanRecord;

    if (!Config::Instance()->DlssNrRecordExposureScan.value_or_default() || !Due(g_scan.frames))
        return;

    if (!g_scan.recording)
    {
        g_scan.recording = true;
        g_scan.recordStart = std::chrono::steady_clock::now();
    }

    std::string text;

    int handed = -1;

    for (size_t i = 0; i < g_scan.tracked.size(); ++i)
    {
        if (g_scan.tracked[i].alive && g_scan.tracked[i].handed)
            handed = (int) i;
    }

    if (g_scan.listVersion != g_scan.namedVersion || handed != g_scan.namedHanded)
    {
        std::vector<Named> names;
        names.reserve(g_scan.tracked.size());

        // A freed slot keeps its column (the columns are slots) and says so
        for (const Tracked& t : g_scan.tracked)
            names.push_back({ t.alive ? t.shape + Core::ApiSuffix(t.api) : std::string("(gone)"), t.handed });

        text = NamesLine(names) + "\n" + ColumnsLine(names.size()) + "\n";
        g_scan.namedVersion = g_scan.listVersion;
        g_scan.namedHanded = handed;
    }

    Sample s;
    s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_scan.recordStart).count();
    s.frame = g_scan.frames;
    s.meter = frame.meter;
    s.game = frame.game;
    s.pre = frame.pre;
    s.values.reserve(g_scan.tracked.size());

    for (const Tracked& t : g_scan.tracked)
        s.values.push_back(t.alive && std::isfinite(t.latest) ? t.latest : 0.0f);

    text += Row(s) + "\n";
    RecordWriter::Instance().Append(std::move(text));
}

// ---------------------------------------------------------------------------------------------------------------------
// Auto (story 5, DlssNr_ExposureAuto.h): the lifecycle, the history the matcher reads, and letting go of candidates.
// Everything under g_scanMutex, except the matcher, which runs on a thread of its own over a copy of the history.

constexpr unsigned int kMatchEveryRows = 50; // ~5 s at the recording's ~10 rows a second
constexpr size_t kMaxHistoryRows = 6000;     // ~10 min: a search gives up after 5

struct AutoState
{
    DlssNrExposureAuto::Lifecycle life;
    DlssNrExposureAuto::Phase said = DlssNrExposureAuto::Phase::Watching;
    bool started = false;
    std::chrono::steady_clock::time_point start;
    bool handedSeen = false;
    unsigned long long frames = 0; // NR frames Auto has seen (the scan's own count stops while nothing is held)
    unsigned int adoptions = 0; // candidates taken, ever
    unsigned int adoptionsSeen = 0;
    unsigned int search = 0; // the search the history belongs to (Lifecycle::SearchNumber)

    // One row per recording row (DlssNrExposureScanRecord::Due): the meter, and each slot's value. A slot's column
    // belongs to the owner of generation owner[slot], from row first[slot] on.
    std::vector<float> meter;
    std::vector<std::vector<float>> values;
    std::vector<size_t> first;
    std::vector<uint32_t> owner;
    unsigned int rowsSinceMatch = 0;

    std::future<DlssNrExposureMatch::Verdict> matching;
    unsigned int matchingSearch = 0;
    std::vector<uint32_t> matchingOwner; // the owners the running match was given

    // What was found: the slot, its owner, its units and Automatic's level against it
    int found = -1;
    uint32_t foundOwner = 0;
    DlssNrExposureMatch::Fit fit;
    float level = 0.0f;

    // The found exposure as the picture uses it, worked out once per NR frame (FoundExposure reads it). A match that
    // moves with our meter's unadapted reading (lag 0-1 rows) is the game's per-frame target, before its own eye
    // adaptation: Cyberpunk 2077's jumps up to ~1 EV between frames when the camera pans and stops, a flash if used as
    // it is. That one is eased as Automatic's own reading is (DlssNr_ExposureAdapt.h, the same seconds); a match that
    // trails the meter is the game's eased value already and is used as it is.
    float now = 0.0f;
    std::chrono::steady_clock::time_point nowAt;
};

AutoState g_auto;

// Each API's own way to let go of a candidate (Core::AdoptLocked's `drop`; null: Core::DropLocked)
void (*g_drop[3])(size_t slot) = {};

void LetGoLocked(size_t slot)
{
    const Tracked& t = g_scan.tracked[slot];
    void (*drop)(size_t) = g_drop[(int) t.api];
    (drop != nullptr ? drop : Core::DropLocked)(slot);
}

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
    }

    return "";
}

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
    if (g_auto.meter.size() >= kMaxHistoryRows)
        return;

    const size_t row = g_auto.meter.size();
    g_auto.meter.push_back(meter);

    const size_t slots = g_scan.tracked.size();
    g_auto.values.resize(slots);
    g_auto.first.resize(slots, row);
    g_auto.owner.resize(slots, 0);

    for (size_t i = 0; i < slots; ++i)
    {
        const Tracked& t = g_scan.tracked[i];
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

    if (!g_auto.matching.valid() ||
        g_auto.matching.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return Verdict::NotSure;

    const DlssNrExposureMatch::Verdict v = g_auto.matching.get();

    if (g_auto.matchingSearch != g_auto.search)
        return Verdict::NotSure;

    switch (v.what)
    {
    case DlssNrExposureMatch::What::Found:
    {
        const size_t slot = (size_t) v.index;

        if (v.index < 0 || slot >= g_scan.tracked.size() || slot >= g_auto.matchingOwner.size() ||
            !g_scan.tracked[slot].alive || g_scan.tracked[slot].generation != g_auto.matchingOwner[slot])
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
        g_auto.foundOwner = g_scan.tracked[slot].generation;
        g_auto.fit = v.fit;
        g_auto.level = level;

        static const char* kKinds[] = { "unsure",    "steady",     "not a float", "a timer",  "noisy",
                                        "unrelated", "multiplier", "divisor",     "stop value" };
        LOG_INFO("DLSS-NR exposure scan (Auto): candidate {} ({}{}) is the game's exposure: a {}, corr {:.3f}, slope "
                 "{:.3f}, lag {} rows, over {} rows and {:.1f} EV; Automatic's level against it {:.4g}",
                 slot + 1, g_scan.tracked[slot].shape, Core::ApiSuffix(g_scan.tracked[slot].api),
                 kKinds[(int) v.fit.kind], v.fit.corr, v.fit.slope, v.fit.lag, v.rows, v.meterTravelEv, level);
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
    if (g_auto.matching.valid() || g_auto.rowsSinceMatch < kMatchEveryRows || g_auto.meter.empty())
        return;

    g_auto.rowsSinceMatch = 0;
    g_auto.matchingSearch = g_auto.search;
    g_auto.matchingOwner = g_auto.owner;

    std::vector<std::vector<float>> values = g_auto.values;
    for (std::vector<float>& column : values)
        column.resize(g_auto.meter.size(), 0.0f);

    g_auto.matching = std::async(std::launch::async,
                                 [meter = g_auto.meter, values = std::move(values), first = g_auto.first]
                                 { return DlssNrExposureMatch::MatchEach(meter, values, first); });
}

// Once per NR frame, before the recording's row. Only on Auto: the asked-for modes keep every candidate.
void AutoFrameLocked(const FrameExposure& frame)
{
    using namespace DlssNrExposureAuto;

    if (!AutoOn())
        return;

    const auto now = std::chrono::steady_clock::now();

    if (!g_auto.started)
    {
        g_auto.started = true;
        g_auto.start = now;
    }

    // What the game hands over, latched; a texture Game exposure gave up on reading does not count
    g_auto.handedSeen = g_auto.handedSeen || frame.handed != nullptr;

    if (g_auto.life.CopiesAll() && DlssNrExposureScanRecord::Due(g_auto.frames++))
        AppendRowLocked(frame.meter);

    Frame f;
    f.seconds = std::chrono::duration<float>(now - g_auto.start).count();
    f.handed = g_auto.handedSeen && !DlssNr::GameExposureOffer().Unusable();
    f.verdict = CollectMatchLocked();
    f.matchAlive = g_auto.found >= 0 && (size_t) g_auto.found < g_scan.tracked.size() &&
                   g_scan.tracked[g_auto.found].alive && g_scan.tracked[g_auto.found].generation == g_auto.foundOwner;
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

    // Let go: everything, or everything but the match
    if (g_auto.life.LetsGo() || g_auto.life.CopiesMatchOnly())
    {
        for (size_t i = 0; i < g_scan.tracked.size(); ++i)
        {
            if (g_scan.tracked[i].alive && !(g_auto.life.CopiesMatchOnly() && (int) i == g_auto.found))
                LetGoLocked(i);
        }
    }

    // The found exposure for this frame (AutoState::now)
    {
        float target = 0.0f;

        if (g_auto.life.Now() == Phase::Found && f.matchAlive)
            target = g_auto.level * DlssNrExposureMatch::AsExposure(g_auto.fit, g_scan.tracked[g_auto.found].latest);

        if (!DlssNrExposureAdapt::Valid(target))
        {
            g_auto.now = 0.0f;
        }
        else if (std::abs(g_auto.fit.lag) > 1)
        {
            g_auto.now = target;
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
        }

        g_auto.nowAt = now;
    }

    if (g_auto.life.Now() != g_auto.said)
    {
        g_auto.said = g_auto.life.Now();
        LOG_INFO("DLSS-NR exposure scan (Auto): {}", PhaseName(g_auto.said));
    }
}

// Auto has finished with candidates for the session (Done, Baked): none is taken any more
bool AutoClosedLocked()
{
    using DlssNrExposureAuto::Phase;
    return AutoOn() && (g_auto.life.Now() == Phase::Done || g_auto.life.Now() == Phase::Baked);
}

} // namespace

// Everything the two entry points share: does this description look like a number rather than a
// picture, and if so what is it.
//
// Widened from the first attempt, which asked for at most 4x4 and one or two channels and found
// nothing anywhere. That was tuned on what an exposure buffer ought to look like rather than on what
// engines actually allocate: some keep a small histogram beside the value, some keep a few frames of
// history, and some put the whole thing in a four-channel texture and use one channel. The filter
// only has to be tight enough that the list stays readable.
bool LooksLikeANumber(const D3D12_RESOURCE_DESC& rd, std::string* outShape, unsigned int* outBytes,
                      bool* outIsBuffer, DXGI_FORMAT* outFormat)
{
    // An exposure is computed, so it is written by a shader. This is the one condition worth being
    // strict about: it removes almost everything without removing anything that could be the answer.
    if ((rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
        return false;

    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        // 256 texels rather than 16. A 16x16 texture is still a number by any reasonable measure and
        // a 256-bin histogram is exactly how a lot of engines compute one (DlssNrExposureScanShape::SmallTexture).
        if (!DlssNrExposureScanShape::SmallTexture(rd.Width, rd.Height))
            return false;

        const char* name = nullptr;

        if (!PlausibleFormat(rd.Format, outBytes, &name, outFormat))
            return false;

        *outIsBuffer = false;
        *outShape = std::to_string((unsigned int) rd.Width) + "x" + std::to_string(rd.Height) + " " + name;
        return true;
    }

    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        *outFormat = DXGI_FORMAT_UNKNOWN;

        // Unreal moved eye adaptation off a texture and onto a buffer, so buffers have to be in
        // scope or a whole engine's worth of games is invisible.
        //
        // 128 bytes, down from 4kB. The wider bound filled all twelve slots in Nioh 3 with 256, 512
        // and 768 byte buffers that never held anything but zero, and the real answer -- eight bytes
        // -- only made the list because it happened to be created early. A cap that can be filled by
        // junk is a cap that can hide the answer. At least the 4 bytes read (DlssNrExposureScanShape::SmallBuffer).
        if (!DlssNrExposureScanShape::SmallBuffer(rd.Width))
            return false;

        *outIsBuffer = true;
        *outBytes = 4;
        *outShape = "buffer, " + std::to_string((unsigned int) rd.Width) + " bytes";
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------------------------
// The list every backend shares (DlssNr_ExposureScanCore.h)
// ---------------------------------------------------------------------------------------------

namespace Core
{
std::mutex& Mutex() { return g_scanMutex; }

namespace
{
thread_local int t_held = 0;
}

HeldScope::HeldScope() { ++t_held; }
HeldScope::~HeldScope() { --t_held; }
bool Held() { return t_held > 0; }

void NoteHandedLocked(Api api, const void* handle)
{
    g_scan.handedNote = handle;
    g_scan.handedNoteApi = api;
}

bool Wanted() { return ExposureScan::Wanted(); }

bool Active() { return Config::Instance()->DlssNrEnabled.value_or_default() && ExposureScan::Wanted(); }

bool FromGameLocked(void* caller) { return ExposureScan::FromGameLocked(caller); }

void ExaminedLocked() { g_scan.examined++; }

std::vector<Tracked>& ListLocked() { return g_scan.tracked; }

const char* ApiSuffix(Api api)
{
    switch (api)
    {
    case Api::D3D11:
        return " [D3D11]";
    case Api::Vulkan:
        return " [Vulkan]";
    default:
        return "";
    }
}

float HalfToFloat(uint16_t h) { return HalfToFloatImpl(h); }

int AdoptLocked(Api api, void* handle, void* device, const std::string& shape, unsigned int bytes, bool isBuffer,
                uint32_t format, uint64_t size, void (*drop)(size_t slot))
{
    if (handle == nullptr || AutoClosedLocked())
        return -1;

    for (const Tracked& t : g_scan.tracked)
    {
        if (t.alive && t.api == api && t.handle == handle)
            return -1;
    }

    if (drop != nullptr)
        g_drop[(int) api] = drop;

    const int rank = DlssNrExposureScanShape::Rank(isBuffer, size);
    int slot = DlssNrExposureScanShape::FreeSlot(g_scan.tracked, kMaxCandidates);

    // Full: a candidate less likely to be the exposure makes room
    if (slot < 0)
    {
        const int victim =
            DlssNrExposureScanShape::EvictFor(g_scan.tracked, rank, [api](const Tracked& t) { return t.api == api; });

        if (victim >= 0)
        {
            LOG_INFO("DLSS-NR exposure scan: the list is full; candidate {} ({}{}) makes room for a likelier one ({}{})",
                     victim + 1, g_scan.tracked[victim].shape, ApiSuffix(api), shape, ApiSuffix(api));
            (drop != nullptr ? drop : DropLocked)((size_t) victim);
            slot = DlssNrExposureScanShape::FreeSlot(g_scan.tracked, kMaxCandidates);
        }
    }

    if (slot < 0)
    {
        if (!g_scan.complained)
        {
            g_scan.complained = true;
            LOG_WARN("DLSS-NR exposure scan: more than {} candidates, so the filter is too loose here "
                     "rather than the game having {} exposures",
                     kMaxCandidates, kMaxCandidates);
        }

        return -1;
    }

    if ((size_t) slot == g_scan.tracked.size())
        g_scan.tracked.emplace_back();

    Tracked& t = g_scan.tracked[slot];
    const uint32_t generation = t.generation + 1;
    t = Tracked {};
    t.api = api;
    t.handle = handle;
    t.device = device;
    t.alive = true;
    t.generation = generation;
    t.shape = shape;
    t.isBuffer = isBuffer;
    t.bytes = bytes;
    t.format = format;
    t.rank = rank;

    if (api != Api::Vulkan)
        ((IUnknown*) handle)->AddRef();

    g_scan.listVersion++;
    g_auto.adoptions++;

    LOG_INFO("DLSS-NR exposure scan: candidate {} -- {}{}", slot + 1, shape, ApiSuffix(api));
    return slot;
}

void DropLocked(size_t slot)
{
    if (slot >= g_scan.tracked.size() || !g_scan.tracked[slot].alive)
        return;

    Tracked& t = g_scan.tracked[slot];

    if (t.api != Api::Vulkan && t.handle != nullptr)
        ((IUnknown*) t.handle)->Release();

    t.alive = false;
    t.handle = nullptr;
    t.device = nullptr;
    t.handed = false;
    g_scan.listVersion++;
}

void ConsumeLocked(size_t slot, uint32_t generation, const void* bytes)
{
    if (!DlssNrExposureScanShape::SameOwner(g_scan.tracked, slot, generation))
        return;

    Tracked& t = g_scan.tracked[slot];
    float value = 0.0f;

    if (t.bytes == 2)
    {
        uint16_t half = 0;
        std::memcpy(&half, bytes, sizeof(half));
        value = HalfToFloatImpl(half);
    }
    else
    {
        std::memcpy(&value, bytes, sizeof(value));
    }

    if (!std::isfinite(value))
        return;

    // Only values that could BE an exposure are allowed into the range, and this is
    // the whole of "8 watching, none moving" never changing.
    //
    // A buffer is usually zero the first time it is read -- created but not yet
    // written, or read a frame before the game fills it. That zero became `lowest`,
    // and since movement is a ratio guarded by `lowest > kFloor`, one early zero
    // disqualified that candidate for the rest of the session however the light
    // changed. The range has to be built from plausible samples, not from whichever
    // sample happened to be first.
    if (value <= kFloor || value >= kCeiling)
    {
        t.latest = value;
        t.reads++;
        return;
    }

    if (t.inRange == 0)
    {
        t.lowest = value;
        t.highest = value;
    }
    else
    {
        t.lowest = std::min(t.lowest, value);
        t.highest = std::max(t.highest, value);
    }

    t.inRange++;

    // "Moves" is the whole point of the readout, and the first version of this test
    // was wrong in a way that mattered: a spread of ten percent of the highest value
    // seen is a threshold of zero when the highest value seen is zero, so three buffers
    // sitting at 0.00000 with float noise around them all reported MOVES.
    //
    // Ratios, not differences, and only over values that could be an exposure at all.
    // An exposure is positive, is not a thousandth of a thousandth, and does not sit at
    // a million. Nioh 3's real one runs 0.0019 to 0.616 -- a factor of three hundred --
    // so a quarter is a low bar that noise cannot reach.
    if (t.inRange > 1 && t.highest > t.lowest * 1.25f)
        t.moves = true;

    t.latest = value;
    t.reads++;
}

bool AnyAliveLocked(Api api)
{
    for (const Tracked& t : g_scan.tracked)
    {
        if (t.alive && t.api == api)
            return true;
    }

    return false;
}

void EndFrameLocked(const FrameExposure& passFrame)
{
    // The game's own resource when the pass was handed OptiScaler's copy of it (NoteHandedLocked)
    FrameExposure frame = passFrame;

    if (g_scan.handedNote != nullptr)
    {
        frame.handed = g_scan.handedNote;
        frame.handedApi = g_scan.handedNoteApi;
        g_scan.handedNote = nullptr;
    }

    // Auto's lifecycle runs every NR frame, candidates or not: a handed texture, or new candidates after giving up,
    // change its course
    AutoFrameLocked(frame);

    bool anyAlive = false;

    for (const Tracked& t : g_scan.tracked)
        anyAlive = anyAlive || t.alive;

    if (!anyAlive)
    {
        // Candidates are picked up as the game creates them, so an empty list also means the scan was
        // switched on after they were made, or a feature was just released and cleared them.
        g_scan.status = "no exposure-shaped buffer seen yet -- one turns up when the game creates its buffers "
                        "(a loading screen or a resolution change)";
        return;
    }

    // Which candidate, if any, is the exposure texture the game hands to the upscaler. In a game that hands one over,
    // that is the known answer the scan is checked against; not finding it among the candidates means the filter
    // misses what an exposure looks like in this game.
    {
        bool found = false;

        for (size_t i = 0; i < g_scan.tracked.size(); ++i)
        {
            Tracked& t = g_scan.tracked[i];
            const bool handed =
                t.alive && frame.handed != nullptr && t.api == frame.handedApi && t.handle == frame.handed;

            if (handed && !t.handed)
                LOG_INFO("DLSS-NR exposure scan: candidate {} ({}{}) is the exposure the game hands to the upscaler",
                         (unsigned int) (i + 1), t.shape, ApiSuffix(t.api));

            // Kept while the candidate lives: a game can hand over two images by turns (Doom Eternal: this frame's and
            // the last), and both are its exposure
            t.handed = t.handed || handed;
            found = found || handed;
        }

        static bool saidMissing = false;

        if (frame.handed != nullptr && !found && !saidMissing && g_scan.frames >= 600)
        {
            saidMissing = true;

            if (frame.handedApi == Api::D3D12)
            {
                const D3D12_RESOURCE_DESC rd = ((ID3D12Resource*) frame.handed)->GetDesc();
                LOG_WARN("DLSS-NR exposure scan: the game hands over an exposure texture ({}x{} format {}, dim {}, "
                         "flags {:#x}) that is not among the {} candidates: the filter misses it",
                         (unsigned int) rd.Width, rd.Height, (int) rd.Format, (int) rd.Dimension,
                         (unsigned int) rd.Flags, (unsigned int) g_scan.tracked.size());
            }
            else
            {
                LOG_WARN("DLSS-NR exposure scan: the game hands over an exposure image that is not among the {} "
                         "candidates: the filter misses it",
                         (unsigned int) g_scan.tracked.size());
            }
        }
    }

    RecordLocked(frame);

    // Periodic movement readout. The menu's Advanced panel shows which candidate tracks the light, but
    // the log did not -- so a game the scan is being taught (Cyberpunk) could not be cracked from a log
    // alone. Every ~300 frames, name the candidates that MOVE and their travel: the exposure is the one
    // that swings widely between bright and dark. Throttled, and only while the scan is wanted.
    if (g_scan.frames > 0 && g_scan.frames % 300 == 0)
    {
        unsigned int movers = 0;
        unsigned int alive = 0;

        for (size_t i = 0; i < g_scan.tracked.size(); ++i)
        {
            const Tracked& t = g_scan.tracked[i];
            alive += t.alive ? 1u : 0u;

            if (!t.alive || !t.moves)
                continue;

            movers++;
            LOG_INFO("DLSS-NR scan mover: candidate {} ({}{}) range {:.5f}..{:.5f} (x{:.1f}), latest {:.5f}",
                     (unsigned int) (i + 1), t.shape, ApiSuffix(t.api), t.lowest, t.highest,
                     t.lowest > kFloor ? t.highest / t.lowest : 0.0f, t.latest);
        }

        if (movers == 0)
            LOG_INFO("DLSS-NR scan: {} candidates tracked, none moving yet -- go between bright and dark", alive);
    }

    g_scan.frames++;
    g_scan.status = "";
}
} // namespace Core

void NoteResource(const D3D12_RESOURCE_DESC* desc, ID3D12Resource* resource, void* caller)
{
    if (Core::Held())
        return;

    // Gated on the scan being wanted, not just NR being on -- see NoteUav for why.
    if (!Config::Instance()->DlssNrEnabled.value_or_default() || !Wanted())
        return;

    if (desc == nullptr || resource == nullptr)
        return;

    std::string shape;
    unsigned int bytes = 4;
    bool isBuffer = false;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_scan.examined++;

    if (!LooksLikeANumber(*desc, &shape, &bytes, &isBuffer, &fmt))
    {
        // Near-miss diagnostic. A resource a shader writes (UAV) that the filter rejected: logging its
        // shape -- bounded to the first 40 so it cannot flood -- reveals whether a game the scan finds
        // nothing in (Cyberpunk 2077) has an exposure the filter narrowly misses (a small UAV texture
        // in an unlisted format, or a UAV buffer just over 128 bytes -> widen precisely to match) or
        // nothing scannable at all (only large buffers/textures -> the exposure is baked in a bigger
        // buffer and no filter change can help). Read these against Examined() in the log.
        if ((desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 && g_scan.nearMissLogged < 40)
        {
            g_scan.nearMissLogged++;
            LOG_INFO("DLSS-NR scan near-miss #{}: UAV dim {} {}x{}x{} fmt {} (filter rejected)",
                     g_scan.nearMissLogged, (int) desc->Dimension, (unsigned int) desc->Width,
                     desc->Height, desc->DepthOrArraySize, (int) desc->Format);
        }

        return;
    }

    if (!FromGameLocked(caller))
        return;

    Core::AdoptLocked(Api::D3D12, resource, nullptr, shape, bytes, isBuffer, (uint32_t) fmt,
                      isBuffer ? desc->Width : desc->Width * desc->Height);
}

unsigned int Examined()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);
    return g_scan.examined;
}

void NoteUav(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, void* caller)
{
    if (Core::Held())
        return;

    // Gated on the scan being wanted (WhitePointSource 2, ScanExposure or RecordExposureScan), not just on NR being on.
    //
    // This was once deliberately ungated, so a scan switched on mid-game would still have the eye
    // adaptation view the engine created at load. That reasoning assumed recording was only a
    // description and a pointer; it is not -- Adopt AddRef's every candidate, so with the scan off
    // (the default) NR still pinned up to 64 buffers the game or DLSS owns. Pinning foreign resources
    // past their owner's teardown is what removed the device in Cyberpunk, and ReleaseTrackedResources
    // only drops them at an NGX feature release, not when the game frees its own. Starfield re-adopted
    // 64 such buffers after each of 241 DLSS-G recreations in one session, for a scan nobody asked for.
    //
    // The early capture also bought less than it looked: every feature release clears the list, and
    // candidates only come back as the game creates new resources. So a scan set in the ini works from
    // launch as before; one switched on mid-game finds candidates at the next loading screen or
    // resolution change (Tick's status says so).
    if (!Config::Instance()->DlssNrEnabled.value_or_default() || !Wanted())
        return;

    if (resource == nullptr)
        return;

    const D3D12_RESOURCE_DESC rd = resource->GetDesc();

    std::string shape;
    unsigned int bytes = 4;
    bool isBuffer = false;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_scan.examined++;

    if (!LooksLikeANumber(rd, &shape, &bytes, &isBuffer, &fmt))
        return;

    if (!FromGameLocked(caller))
        return;

    Core::AdoptLocked(Api::D3D12, resource, nullptr, shape, bytes, isBuffer, (uint32_t) fmt,
                      isBuffer ? rd.Width : rd.Width * rd.Height);
}

void Tick(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const FrameExposure& frame)
{
    if (!Wanted())
        return;

    if (device == nullptr || cmdList == nullptr)
        return;

    // Before the lock, deliberately. EnsureReadback creates a committed resource, and that call is
    // detoured to hkCreateCommittedResource -> NoteResource, which takes g_scanMutex. Holding it
    // here would be a self-deadlock on a non-recursive mutex the first frame the scan runs. The
    // readback ring is only ever touched on this (render) thread, so it needs no lock of its own.
    if (!EnsureReadback(device))
        return;

    std::lock_guard<std::mutex> lock(g_scanMutex);
    Core::HeldScope held;

    // Read the slot written four frames ago before overwriting it. Retired by now, so this reads
    // mapped memory rather than waiting on the GPU.
    if (g_scan.frames12 >= kSlots)
    {
        const unsigned int slot = (unsigned int) (g_scan.frames12 % kSlots);
        ID3D12Resource* old = g_scan.readback[slot];
        void* mapped = nullptr;
        D3D12_RANGE range { 0, kStride * kMaxCandidates };

        if (old != nullptr && SUCCEEDED(old->Map(0, &range, &mapped)) && mapped != nullptr)
        {
            const unsigned char* base = (const unsigned char*) mapped;

            for (size_t i = 0; i < g_scan.tracked.size(); ++i)
            {
                if (g_scan.tracked[i].api == Api::D3D12)
                    Core::ConsumeLocked(i, g_scan.readbackGeneration[slot][i], base + i * kStride);
            }

            D3D12_RANGE nothingWritten { 0, 0 };
            old->Unmap(0, &nothingWritten);
        }
    }

    // The frame's bookkeeping, once per NR frame, for the candidates of every API (a D3D11 or Vulkan game whose NR
    // runs here has its values read on its own API: DlssNr_ExposureScan_Dx11.cpp, DlssNr_ExposureScan_Vk.cpp)
    Core::EndFrameLocked(frame);

    const unsigned int slot = (unsigned int) (g_scan.frames12 % kSlots);
    ID3D12Resource* dst = g_scan.readback[slot];

    if (dst == nullptr)
        return;

    // The state a candidate is in is the game's business and nothing here has a contract about it.
    //
    // UNORDERED_ACCESS is the assumption, and it is the reasonable one: every candidate got here by
    // having an unordered access view created on it, which is what a compute shader writes through,
    // and an eye adaptation buffer is written every frame and read by the next pass. It is still an
    // assumption, which is why the whole scan is behind a setting that is off by default -- getting
    // this wrong on someone's machine costs them a frame or a device, and nobody who has not asked
    // for the scan should be exposed to that.
    for (size_t i = 0; i < g_scan.tracked.size(); ++i)
    {
        Tracked& t = g_scan.tracked[i];

        if (!t.alive || t.api != Api::D3D12 || t.handle == nullptr)
            continue;

        ID3D12Resource* resource = (ID3D12Resource*) t.handle;
        g_scan.readbackGeneration[slot][i] = t.generation;

        Barrier(cmdList, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

        if (t.isBuffer)
        {
            cmdList->CopyBufferRegion(dst, i * kStride, resource, 0, t.bytes);
        }
        else
        {
            D3D12_TEXTURE_COPY_LOCATION src {};
            src.pResource = resource;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION to {};
            to.pResource = dst;
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Offset = i * kStride;
            to.PlacedFootprint.Footprint.Format = (DXGI_FORMAT) t.format;
            to.PlacedFootprint.Footprint.Width = 1;
            to.PlacedFootprint.Footprint.Height = 1;
            to.PlacedFootprint.Footprint.Depth = 1;
            to.PlacedFootprint.Footprint.RowPitch = 256;

            D3D12_BOX one { 0, 0, 0, 1, 1, 1 };
            cmdList->CopyTextureRegion(&to, 0, 0, 0, &src, &one);
        }

        Barrier(cmdList, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    g_scan.frames12++;
}

// How many frames of watching without movement before saying so. At sixty frames a second this is
// about half a minute, which is long enough to have walked somewhere with different light in it and
// short enough that nobody waits on it wondering.
constexpr unsigned int kPatience = 1800;

Verdict Where()
{
    if (!Wanted())
        return Verdict::Off;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (!Core::AnyAliveLocked(Api::D3D12) && !Core::AnyAliveLocked(Api::D3D11) && !Core::AnyAliveLocked(Api::Vulkan))
        return Verdict::Waiting;

    unsigned int mostReads = 0;

    for (const Tracked& t : g_scan.tracked)
    {
        if (!t.alive)
            continue;

        if (t.moves)
            return Verdict::Found;

        mostReads = std::max(mostReads, t.reads);
    }

    return mostReads >= kPatience ? Verdict::Barren : Verdict::Watching;
}

const char* Headline()
{
    static std::string line;

    switch (Where())
    {
    case Verdict::Off:
        line = "";
        break;

    case Verdict::Waiting:
    {
        // The examined count is the whole diagnosis. Zero means the hook is not running and no
        // amount of playing will change that; a large number means the game genuinely has nothing
        // shaped like an exposure, which is an answer rather than a failure.
        const unsigned int seen = Examined();
        line = seen == 0 ? "DLSS-NR exposure scan: NOT RUNNING -- no resources seen at all"
                         : "DLSS-NR exposure scan: examined " + std::to_string(seen) +
                               " resources, none shaped like an exposure";
        break;
    }

    case Verdict::Watching:
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        unsigned int mostReads = 0;

        size_t alive = 0;

        for (const Tracked& t : g_scan.tracked)
        {
            if (!t.alive)
                continue;

            alive++;
            mostReads = std::max(mostReads, t.reads);
        }

        line = "DLSS-NR exposure scan: watching " + std::to_string(alive) +
               ", none moving yet -- walk between light and shade  (" +
               std::to_string(mostReads * 100 / kPatience) + "%)";
        break;
    }

    case Verdict::Found:
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);

        // The widest travel wins where several move. An exposure swings by orders of magnitude
        // between a dark interior and open daylight; anything that merely wobbles is something else.
        size_t best = 0;
        float bestRatio = 0.0f;

        for (size_t i = 0; i < g_scan.tracked.size(); ++i)
        {
            const Tracked& t = g_scan.tracked[i];

            if (!t.alive || !t.moves || t.lowest <= kFloor)
                continue;

            const float ratio = t.highest / t.lowest;

            if (ratio > bestRatio)
            {
                bestRatio = ratio;
                best = i;
            }
        }

        char buf[192];
        // The live value is in here so the line visibly ticks. Without it the indicator looks stuck
        // the moment the range settles, which is exactly when it has succeeded.
        snprintf(buf, sizeof(buf),
                 "DLSS-NR exposure scan: FOUND -- candidate %zu = %.5f  (%.5f..%.5f, x%.0f)  done",
                 best + 1, g_scan.tracked[best].latest, g_scan.tracked[best].lowest,
                 g_scan.tracked[best].highest, bestRatio);
        line = buf;
        break;
    }

    case Verdict::Barren:
        line = "DLSS-NR exposure scan: nothing moved. No exposure to find here.";
        break;
    }

    return line.c_str();
}

float BestValue(int* outIndex, float* outLowest, float* outHighest)
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    int best = -1;
    float bestRatio = 0.0f;

    for (size_t i = 0; i < g_scan.tracked.size(); ++i)
    {
        const Tracked& t = g_scan.tracked[i];

        if (!t.alive || !t.moves || t.lowest <= kFloor)
            continue;

        const float ratio = t.highest / t.lowest;

        if (ratio > bestRatio)
        {
            bestRatio = ratio;
            best = (int) i;
        }
    }

    if (best < 0)
        return 0.0f;

    if (outIndex != nullptr)
        *outIndex = best + 1;

    if (outLowest != nullptr)
        *outLowest = g_scan.tracked[best].lowest;

    if (outHighest != nullptr)
        *outHighest = g_scan.tracked[best].highest;

    return g_scan.tracked[best].latest;
}

std::vector<Candidate> Report()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    std::vector<Candidate> out;
    out.reserve(g_scan.tracked.size());

    for (const Tracked& t : g_scan.tracked)
    {
        if (!t.alive)
            continue;

        Candidate c;
        c.shape = t.shape + Core::ApiSuffix(t.api);
        c.latest = t.latest;
        c.lowest = t.lowest;
        c.highest = t.highest;
        c.reads = t.reads;
        c.moves = t.moves;
        out.push_back(c);
    }

    return out;
}

const char* Status()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);
    return g_scan.status;
}

// The asked-for modes (the manual anchors, ScanExposure, RecordExposureScan): their menu and the game's exposure read
// beside the meter for the recording. Auto scans too (AutoStatus), but shows only its own line.
bool Scanning() { return AskedFor(); }

std::string AutoStatus()
{
    if (!AutoOn())
        return {};

    std::lock_guard<std::mutex> lock(g_scanMutex);
    std::string line = PhaseName(g_auto.life.Now());

    if (g_auto.life.Now() == DlssNrExposureAuto::Phase::Found && g_auto.found >= 0 &&
        (size_t) g_auto.found < g_scan.tracked.size())
        line += " (" + g_scan.tracked[g_auto.found].shape + ")";

    return line;
}

bool FoundExposure(float* multiplier)
{
    if (multiplier == nullptr || !AutoOn())
        return false;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (g_auto.life.Now() != DlssNrExposureAuto::Phase::Found || g_auto.found < 0 ||
        (size_t) g_auto.found >= g_scan.tracked.size())
        return false;

    const Tracked& t = g_scan.tracked[g_auto.found];

    if (!t.alive || t.generation != g_auto.foundOwner || !DlssNrExposureAdapt::Valid(g_auto.now))
        return false;

    *multiplier = g_auto.now;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Multi-point anchoring
// ---------------------------------------------------------------------------------------------

namespace
{
std::vector<AnchorPoint> g_anchors;   // guarded by g_scanMutex, kept sorted by scan ascending

void SortAnchorsLocked()
{
    std::sort(g_anchors.begin(), g_anchors.end(),
              [](const AnchorPoint& a, const AnchorPoint& b) { return a.scan < b.scan; });
}
}  // namespace

// Load the persisted table (or migrate a pre-existing single anchor) exactly once, before any lock
// is taken -- LoadAnchors/AnchorAdd take g_scanMutex themselves, so this must not hold it.
void EnsureAnchorsLoaded()
{
    static std::once_flag once;
    std::call_once(once, [] {
        auto& cfg = *Config::Instance();
        const std::string ser = cfg.DlssNrScanAnchors.value_or_default();

        if (!ser.empty())
        {
            LoadAnchors(ser);
            return;
        }

        // Migration: fold a single-anchor ini from before this feature into a one-row table so the
        // user does not lose the calibration they already set.
        const float v = cfg.DlssNrScanAnchorValue.value_or_default();
        const float w = cfg.DlssNrScanAnchorWhitePoint.value_or_default();

        if (v > kFloor && w > 1e-6f)
            AnchorAdd(v, w);
    });
}

std::vector<AnchorPoint> Anchors()
{
    EnsureAnchorsLoaded();
    std::lock_guard<std::mutex> lock(g_scanMutex);
    return g_anchors;
}

bool AnchorAdd(float scan, float white)
{
    if (!(scan > kFloor && scan < kCeiling) || !(white > 1e-6f))
        return false;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    // A near-duplicate scan value would make log(v_{k+1}) - log(v_k) ~ 0 and divide the interpolation
    // by zero. Replace the existing point's white instead of adding a second at the same place.
    for (auto& p : g_anchors)
    {
        if (scan > p.scan * 0.98f && scan < p.scan * 1.02f)
        {
            p.white = white;
            return true;
        }
    }

    if (g_anchors.size() >= 8)
        return false;

    g_anchors.push_back({ scan, white });
    SortAnchorsLocked();
    return true;
}

void AnchorSetWhite(int index, float white)
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (index >= 0 && index < (int) g_anchors.size() && white > 1e-6f)
        g_anchors[index].white = white;
}

void AnchorRemove(int index)
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (index >= 0 && index < (int) g_anchors.size())
        g_anchors.erase(g_anchors.begin() + index);
}

void AnchorClear()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_anchors.clear();
}

float AnchoredWhitePoint(float scanNow, bool inverted, float trim)
{
    EnsureAnchorsLoaded();
    trim = std::clamp(trim, 0.25f, 4.0f);

    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (g_anchors.empty() || !(scanNow > kFloor))
        return 0.0f;

    // One point is the original ratio law, and it needs the direction the two-point case reads off
    // the data instead.
    if (g_anchors.size() == 1)
    {
        const AnchorPoint& p = g_anchors[0];

        if (!(p.scan > kFloor) || !(p.white > 1e-6f))
            return 0.0f;

        const float ratio = inverted ? scanNow / p.scan : p.scan / scanNow;
        return std::clamp(p.white * ratio * trim, 0.01f, 4096.0f);
    }

    // Clamp at the ends: holding the outermost calibrated white point beats extrapolating a ratio
    // into a scene darker or brighter than anything the user calibrated.
    if (scanNow <= g_anchors.front().scan)
        return std::clamp(g_anchors.front().white * trim, 0.01f, 4096.0f);

    if (scanNow >= g_anchors.back().scan)
        return std::clamp(g_anchors.back().white * trim, 0.01f, 4096.0f);

    // Between two points: interpolate log(white) against log(scan). A white point is a divisor, so
    // log is the space in which it is linear -- and it makes the two-point case reproduce the exact
    // ratio law of the single-point case, so adding a second point never causes a jump.
    for (size_t k = 0; k + 1 < g_anchors.size(); ++k)
    {
        const AnchorPoint& a = g_anchors[k];
        const AnchorPoint& b = g_anchors[k + 1];

        if (scanNow >= a.scan && scanNow <= b.scan && b.scan > a.scan * 1.0001f)
        {
            const float t = (std::log(scanNow) - std::log(a.scan)) / (std::log(b.scan) - std::log(a.scan));
            const float w = std::exp(std::log(a.white) + t * (std::log(b.white) - std::log(a.white)));
            return std::clamp(w * trim, 0.01f, 4096.0f);
        }
    }

    return std::clamp(g_anchors.back().white * trim, 0.01f, 4096.0f);
}

void LoadAnchors(const std::string& serialized)
{
    std::vector<AnchorPoint> parsed;
    size_t i = 0;

    while (i < serialized.size() && parsed.size() < 8)
    {
        size_t semi = serialized.find(';', i);
        std::string tok = serialized.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
        i = semi == std::string::npos ? serialized.size() : semi + 1;

        const size_t colon = tok.find(':');
        if (colon == std::string::npos)
            continue;

        try
        {
            const float v = std::stof(tok.substr(0, colon));
            const float w = std::stof(tok.substr(colon + 1));

            if (v > kFloor && v < kCeiling && w > 1e-6f)
                parsed.push_back({ v, w });
        }
        catch (...)
        {
            // A malformed token is skipped rather than aborting the whole table.
        }
    }

    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_anchors = std::move(parsed);
    SortAnchorsLocked();
}

std::string SerializeAnchors()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    std::string out;
    char buf[64];

    for (const AnchorPoint& p : g_anchors)
    {
        snprintf(buf, sizeof(buf), "%.6g:%.6g;", p.scan, p.white);
        out += buf;
    }

    return out;
}

// Drop the scan's references to the resources it captured, WITHOUT touching our own readback buffers.
//
// The scan AddRef's every candidate it adopts (Adopt) but nothing ever released them -- Shutdown() has
// no callers -- so a Streamline/DLSS-D-owned resource that passes the filter is pinned by our stray
// AddRef, and when the driver frees its (placed) heap at feature teardown the surviving wrapper points
// at freed memory: the use-after-free that removed the device in Cyberpunk (a lock on a freed object in
// nvwgf2umx). Calling this at feature release drops our references first, so nothing we hold outlives
// the heap. Only the candidates (foreign resources) are released here -- NOT the readback ring, which
// is ours and may have GPU copies in flight; freeing that here would be a new hazard. Capture is gated
// on NR being enabled AND the scan being wanted (NoteUav), so this is a no-op whenever the scan is off
// (nothing captured). The scan re-adopts candidates as the game creates new resources.
void ReleaseTrackedResources()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    // D3D12's only: D3D11 has no placed heaps that a held reference could outlive, and Vulkan's candidates are dropped
    // as the game destroys them (DlssNr_ExposureScan_Vk.cpp)
    for (size_t i = 0; i < g_scan.tracked.size(); ++i)
    {
        if (g_scan.tracked[i].api == Api::D3D12)
            Core::DropLocked(i);
    }

    g_scan.complained = false;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    for (size_t i = 0; i < g_scan.tracked.size(); ++i)
        Core::DropLocked(i);

    g_scan.tracked.clear();

    for (unsigned int i = 0; i < kSlots; ++i)
    {
        if (g_scan.readback[i] != nullptr)
        {
            g_scan.readback[i]->Release();
            g_scan.readback[i] = nullptr;
        }
    }

    g_scan.frames = 0;
    g_scan.frames12 = 0;
    g_scan.status = "not started";
}

} // namespace ExposureScan
} // namespace DlssNr
