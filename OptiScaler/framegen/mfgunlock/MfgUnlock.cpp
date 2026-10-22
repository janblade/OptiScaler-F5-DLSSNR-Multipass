#include "pch.h"

#include "FamilyAmpere.h"
#include "FamilyRtx40.h"
#include "FamilyFatbin.h"
#include "FamilySlDlssg.h"
#include "FamilyXeFG.h"
#include "Image.h"
#include "MfgUnlock.h"
#include "Locator.h"
#include "Patch.h"
#include "Provider.h"
#include "Status.h"
#include "PtxTuring.h"

#include <detours/detours.h>
#include <Config.h>
#include <Util.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/XeLL_Proxy.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include <State.h>
#include <misc/IdentifyGpu.h>

#pragma comment(lib, "Bcrypt.lib")

static_assert(static_cast<size_t>(MfgUnlock::Family::Count) == MfgUnlock::kFamilyNames.size());

namespace
{
std::mutex rtx40Mutex;
std::map<HMODULE, std::unique_ptr<MfgUnlock::AppliedPatchSet>> rtx40PatchSets;
std::set<HMODULE> rtx40RecoveryFailed;
std::map<HMODULE, MfgUnlock::Family> rtx40Families;
// Modules already decided (patched, stood aside or not needed), by path: a repeated load of the same module is not
// scanned again. Dropped when the module really unloads.
std::map<HMODULE, std::wstring> rtx40Decided;

std::mutex xeFgMutex;
MfgUnlock::AppliedPatchSet xeFgPatchSet;
HMODULE activeXeFgModule = nullptr;
HMODULE activeXeLlModule = nullptr;
bool xeFgPatchRecoveryFailed = false;
std::atomic<bool> additionalXeFgProviderObserved { false };

std::wstring GetModulePath(HMODULE module)
{
    if (module == nullptr)
        return {};

    std::array<wchar_t, 32768> path {};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    return { path.data(), length };
}

bool HasModuleFilename(HMODULE module, std::wstring_view filename)
{
    const auto path = GetModulePath(module);
    return !path.empty() &&
           MfgUnlock::Provider::NormalisePath(std::filesystem::path(path).filename().wstring()) == filename;
}

size_t GetMappedImageSize(HMODULE module)
{
    if (module == nullptr)
        return 0;

    constexpr size_t kMaximumImageSize = 512 * 1024 * 1024;
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    uintptr_t cursor = base;
    size_t mappedSize = 0;
    while (cursor >= base && cursor - base < kMaximumImageSize)
    {
        MEMORY_BASIC_INFORMATION region {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) != sizeof(region) ||
            region.AllocationBase != module || region.Type != MEM_IMAGE)
            break;

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (regionEnd <= cursor || regionEnd < base)
            return 0;
        mappedSize = static_cast<size_t>(regionEnd - base);
        cursor = regionEnd;
    }

    return mappedSize;
}

// Locators read live provider memory. A page that is gone or not readable must make the family stand aside, not take
// the game down, so each top-level locate runs under SEH. __try allows no C++ object with a destructor in its frame,
// hence the plain callback; the caller holds no lock that the locate itself takes.
bool RunGuarded(void (*locate)(void*), void* context)
{
    __try
    {
        locate(context);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH)
    {
        return false;
    }
}

// False when the locate faulted; whatever it produced is then incomplete and must not be applied.
template <typename Locate> bool Guarded(Locate&& locate)
{
    using Callable = std::remove_reference_t<Locate>;
    return RunGuarded([](void* context) { (*static_cast<Callable*>(context))(); },
                      const_cast<void*>(static_cast<const void*>(&locate)));
}

std::string HashFileSha256(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        LOG_WARN("[MFGUnlock] Could not open provider for SHA-256: {} (Win32 {})", wstring_to_string(path),
                 GetLastError());
        return {};
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
    {
        CloseHandle(file);
        LOG_WARN("[MFGUnlock] Could not open the SHA-256 provider: {}", wstring_to_string(path));
        return {};
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        LOG_WARN("[MFGUnlock] Could not initialize provider SHA-256: {}", wstring_to_string(path));
        return {};
    }

    std::array<UCHAR, 1 << 16> buffer {};
    bool ok = true;
    DWORD read = 0;
    while (true)
    {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            ok = false;
            break;
        }
        if (read == 0)
            break;
        if (BCryptHashData(hash, buffer.data(), read, 0) != 0)
        {
            ok = false;
            break;
        }
    }

    std::array<UCHAR, 32> digest {};
    ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);

    if (!ok)
    {
        LOG_WARN("[MFGUnlock] Could not finish provider SHA-256: {}", wstring_to_string(path));
        return {};
    }

    std::string result;
    result.reserve(digest.size() * 2);
    for (const UCHAR byte : digest)
        result += std::format("{:02x}", byte);
    return result;
}

MfgUnlock::FamilyStatus MakeStatus(MfgUnlock::FamilyState state, HMODULE xeFgModule,
                                   const MfgUnlock::FamilyXeFG::Report* report, std::string failure = {})
{
    MfgUnlock::FamilyStatus status;
    status.state = state;
    status.modulePath = GetModulePath(xeFgModule);
    status.failure = std::move(failure);
    if (report != nullptr)
    {
        status.patches.reserve(report->patches.size());
        for (const auto& patch : report->patches)
        {
            status.patches.push_back(
                { patch.name, patch.state == MfgUnlock::LocateState::Found
                                  ? "found"
                                  : std::string(MfgUnlock::FamilyXeFG::LocateStateName(patch.state)) });
        }
    }
    return status;
}

void PublishStatus(MfgUnlock::FamilyStatus status, bool error = false,
                   MfgUnlock::Family family = MfgUnlock::Family::XeFG)
{
    MfgUnlock::StatusRegistry::Set(family, std::move(status));
    const auto text = MfgUnlock::FormatDiagnostic(family, MfgUnlock::StatusRegistry::Get(family));
    if (error)
        LOG_ERROR("{}", wstring_to_string(text));
    else
        LOG_INFO("{}", wstring_to_string(text));
}

void AddModuleMetadata(MfgUnlock::FamilyStatus& status)
{
    const std::filesystem::path path(status.modulePath);
    if (path.empty())
        return;

    version_t version {};
    if (Util::GetFileVersion(path.wstring(), &version))
    {
        status.version = std::format("{}.{}.{}.{}", version.major, version.minor, version.patch, version.reserved);
    }
    else
    {
        status.version = "unavailable";
        LOG_WARN("[MFGUnlock] Could not read XeFG file version: {}", wstring_to_string(path.wstring()));
    }
    status.sha256 = HashFileSha256(path.wstring());
}

void SetWaitingStatus(std::string reason)
{
    MfgUnlock::FamilyStatus status;
    status.state = MfgUnlock::FamilyState::Waiting;
    status.failure = std::move(reason);
    PublishStatus(std::move(status));
}

// The XeFG/XeLL pair that stood aside, by module and path: the same pair is not scanned again on the next context.
// Forgotten when either module really unloads. Guarded by xeFgMutex.
struct XeFgDecision
{
    HMODULE xeFg = nullptr;
    HMODULE xeLl = nullptr;
    std::wstring xeFgPath;
    std::wstring xeLlPath;
};
std::optional<XeFgDecision> xeFgStoodAside;

bool TryApplyXeFG(HMODULE xeFgModule, HMODULE xeLlModule)
{
    const auto mode = Config::Instance()->MfgUnlockXeFG.value_or_default();
    if (mode == MfgUnlockMode::Off)
        return false;

    const XeFgDecision pair { xeFgModule, xeLlModule, GetModulePath(xeFgModule), GetModulePath(xeLlModule) };
    std::lock_guard lock(xeFgMutex);
    if (xeFgStoodAside.has_value() && xeFgStoodAside->xeFg == pair.xeFg && xeFgStoodAside->xeLl == pair.xeLl &&
        xeFgStoodAside->xeFgPath == pair.xeFgPath && xeFgStoodAside->xeLlPath == pair.xeLlPath)
        return false;
    // Publishes a stand-aside for this pair and remembers it (xeFgMutex is held).
    const auto standAside = [&pair](MfgUnlock::FamilyStatus status)
    {
        xeFgStoodAside = pair;
        PublishStatus(std::move(status), true);
    };

    // A game that owns frame generation may ship its own libxess_fg.dll; it is never patched.
    if (State::Instance().externalFrameGeneration)
    {
        standAside(MakeStatus(MfgUnlock::FamilyState::StoodAside, xeFgModule, nullptr,
                              "external frame generation is active"));
        return false;
    }

    if (!HasModuleFilename(xeFgModule, L"libxess_fg.dll"))
    {
        standAside(MakeStatus(MfgUnlock::FamilyState::StoodAside, xeFgModule, nullptr,
                              "loaded XeFG module is not libxess_fg.dll"));
        return false;
    }
    if (!HasModuleFilename(xeLlModule, L"libxell.dll"))
    {
        SetWaitingStatus("waiting for the XeLL provider used by XeFG");
        return false;
    }

    if (xeFgPatchSet.IsActive())
    {
        if (xeFgModule == activeXeFgModule && xeLlModule == activeXeLlModule)
            return true;
        // Not remembered: this pair may be patched once the other one unloads.
        PublishStatus(MakeStatus(MfgUnlock::FamilyState::StoodAside, xeFgModule, nullptr,
                                 "another XeFG/XeLL provider pair is already patched"),
                      true);
        return false;
    }

    const size_t xeFgSize = GetMappedImageSize(xeFgModule);
    const size_t xeLlSize = GetMappedImageSize(xeLlModule);
    const MfgUnlock::ImageView xeFgImage(xeFgModule, xeFgSize);
    const MfgUnlock::ImageView xeLlImage(xeLlModule, xeLlSize);
    // The patched sites hold XeFG's own ceiling (3); a value below it would lower XeFG under its stock behaviour.
    const auto count = static_cast<uint8_t>(std::clamp(Config::Instance()->FGXeFGMaxInterpolations.value_or_default(),
                                                       MfgUnlock::kXeFGOriginalDefaultMaxInterpolations, 5));
    MfgUnlock::FamilyXeFG::Report report;
    std::vector<MfgUnlock::PatchOperation> operations;
    if (!Guarded(
            [&]
            {
                report = MfgUnlock::FamilyXeFG::Locate(xeFgImage, xeLlImage, count);
                if (report.Complete())
                    operations = MfgUnlock::FamilyXeFG::BuildOperations(report, xeFgImage, xeLlImage);
            }))
    {
        standAside(MakeStatus(MfgUnlock::FamilyState::StoodAside, xeFgModule, nullptr,
                              "provider memory could not be read while locating"));
        return false;
    }
    if (!report.Complete())
    {
        standAside(MakeStatus(MfgUnlock::FamilyState::StoodAside, xeFgModule, &report, report.failure));
        return false;
    }

    if (operations.size() != report.patches.size())
    {
        standAside(MakeStatus(MfgUnlock::FamilyState::StoodAside, xeFgModule, &report,
                              "could not build the complete patch transaction"));
        return false;
    }

    const auto applied = xeFgPatchSet.Apply(operations);
    if (!applied.applied)
    {
        if (xeFgPatchSet.IsActive())
        {
            activeXeFgModule = xeFgModule;
            activeXeLlModule = xeLlModule;
            const auto recovery = xeFgPatchSet.Restore();
            if (!recovery.applied)
            {
                xeFgPatchRecoveryFailed = true;
                // Not remembered: PrepareXeFG keeps retrying the restore.
                PublishStatus(MakeStatus(MfgUnlock::FamilyState::StoodAside, xeFgModule, &report,
                                         "patch transaction failed and partial changes could not be restored"),
                              true);
                return false;
            }
            xeFgPatchRecoveryFailed = false;
            activeXeFgModule = nullptr;
            activeXeLlModule = nullptr;
        }

        const std::string failedPatch =
            applied.failedIndexValid && applied.failedIndex < report.patches.size()
                ? report.patches[applied.failedIndex].name
                : "page protection";
        standAside(MakeStatus(MfgUnlock::FamilyState::StoodAside, xeFgModule, &report,
                              "transaction failed at " + failedPatch));
        return false;
    }

    activeXeFgModule = xeFgModule;
    activeXeLlModule = xeLlModule;
    xeFgPatchRecoveryFailed = false;
    PublishStatus(MakeStatus(MfgUnlock::FamilyState::Active, xeFgModule, &report));
    return true;
}

bool IsNvidiaGpu()
{
    return IdentifyGpu::getPrimaryGpu().vendorId == VendorId::Nvidia;
}

MfgUnlock::FamilyStatus MakeRtx40Status(MfgUnlock::FamilyState state, HMODULE module,
                                        const MfgUnlock::FamilyRtx40::Report* report, std::string failure = {})
{
    MfgUnlock::FamilyStatus status;
    status.state = state;
    status.modulePath = GetModulePath(module);
    status.failure = std::move(failure);
    if (report != nullptr)
    {
        for (const auto& patch : report->patches)
        {
            status.patches.push_back({ patch.name, patch.state == MfgUnlock::LocateState::Found
                                                       ? "found"
                                                       : std::string(MfgUnlock::FamilyXeFG::LocateStateName(
                                                             patch.state)) });
        }
    }
    return status;
}

void PublishRtx40(MfgUnlock::FamilyStatus status, bool error = false,
                  MfgUnlock::Family family = MfgUnlock::Family::RTX40)
{
    PublishStatus(std::move(status), error, family);
}

enum class Rtx40Tier
{
    None,
    Ada,
    Ampere,
    Turing
};

// RTX 50 (Blackwell) and newer generate multiple frames natively; nothing is patched there in any mode.
bool IsBlackwellOrNewer()
{
    const auto gpu = IdentifyGpu::getPrimaryGpu();
    return gpu.vendorId == VendorId::Nvidia && gpu.nvidiaArchInfo.architecture_id >= NV_GPU_ARCHITECTURE_GB200;
}

// RTX 20/30 wins on Ampere and Turing cards; RTX 40 covers Ada (or any older NVIDIA GPU when forced). Forcing RTX
// 20/30 on another card uses the Ampere kernels.
Rtx40Tier ChooseRtx40Tier(MfgUnlockMode mode40, MfgUnlockMode mode2030)
{
    const auto gpu = IdentifyGpu::getPrimaryGpu();
    const bool nvidia = gpu.vendorId == VendorId::Nvidia;
    const auto arch = gpu.nvidiaArchInfo.architecture_id;
    if (IsBlackwellOrNewer())
        return Rtx40Tier::None;
    if (mode2030 != MfgUnlockMode::Off && nvidia)
    {
        if (arch == NV_GPU_ARCHITECTURE_GA100)
            return Rtx40Tier::Ampere;
        if (arch == NV_GPU_ARCHITECTURE_TU100)
            return Rtx40Tier::Turing;
    }
    if (mode40 != MfgUnlockMode::Off && (mode40 == MfgUnlockMode::On ? nvidia : arch == NV_GPU_ARCHITECTURE_AD100))
        return Rtx40Tier::Ada;
    if (mode2030 == MfgUnlockMode::On && nvidia)
        return Rtx40Tier::Ampere;
    return Rtx40Tier::None;
}

// RTX 20: nvngx_dlssg's kernels are PTX for newer cards. The driver is handed rewritten sm_75 PTX text instead of the
// shipped fatbin whenever a container with that shape is loaded (see PtxTuring.h). Hooks stay installed for the life of
// the process and pass everything through while no Turing provider is patched.
std::atomic<bool> turingRewriteWanted { false };
std::atomic<bool> cudaHooksInstalled { false };
HMODULE hookedNvcuda = nullptr; // guarded by cudaRewriteMutex
std::mutex cudaRewriteMutex;

struct CudaRewriteEntry
{
    uint64_t contentHash = 0;
    std::shared_ptr<const std::string> text; // null: not ours or not rewritable
};
// Keyed by image address, but an entry only counts when the container bytes still hash the same.
std::map<const void*, CudaRewriteEntry> cudaRewriteCache;

using PFN_cuModuleLoadData = int(WINAPI*)(void**, const void*);
using PFN_cuModuleLoadDataEx = int(WINAPI*)(void**, const void*, unsigned, int*, void**);
using PFN_cuLibraryLoadData = int(WINAPI*)(void**, const void*, void*, void**, unsigned, void*, void**, unsigned);
PFN_cuModuleLoadData o_cuModuleLoadData = nullptr;
PFN_cuModuleLoadData o_cuModuleLoadFatBinary = nullptr;
PFN_cuModuleLoadDataEx o_cuModuleLoadDataEx = nullptr;
PFN_cuLibraryLoadData o_cuLibraryLoadData = nullptr;

bool ReadMagic(const void* image, uint32_t& magic)
{
    __try
    {
        magic = *static_cast<const volatile uint32_t*>(image);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

struct CudaSubstitute
{
    const void* image;
    std::shared_ptr<const std::string> keepAlive; // owns the rewritten text for the duration of the load call
};

// FNV-1a over the container bytes (resolving a fatbinary wrapper), 0 when it cannot be read.
uint64_t ContainerHash(const void* image)
{
    __try
    {
        auto container = static_cast<const uint8_t*>(image);
        if (*reinterpret_cast<const uint32_t*>(container) == MfgUnlock::PtxTuring::kFatbinWrapperMagic)
            container = *reinterpret_cast<const uint8_t* const*>(container + 8);
        if (container == nullptr || *reinterpret_cast<const uint32_t*>(container) != MfgUnlock::PtxTuring::kFatbinMagic)
            return 0;
        const uint64_t fatSize = *reinterpret_cast<const uint64_t*>(container + 8);
        if (fatSize == 0 || fatSize > (256ull << 20))
            return 0;
        uint64_t hash = 1469598103934665603ull;
        for (uint64_t i = 0; i < fatSize + 0x10; ++i)
            hash = (hash ^ container[i]) * 1099511628211ull;
        return hash;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

CudaSubstitute SubstituteCudaImage(const void* image)
{
    if (image == nullptr || !turingRewriteWanted.load(std::memory_order_acquire))
        return { image, nullptr };

    // Proves in the tester's log that nvngx_dlssg's kernel loads reach this hook at all.
    static std::atomic<unsigned> seen{ 0 };
    if (seen.fetch_add(1, std::memory_order_relaxed) < 3)
        LOG_INFO("[MFGUnlock] RTX 20: CUDA module load seen (image {:X})", reinterpret_cast<uintptr_t>(image));

    uint32_t magic = 0;
    if (!ReadMagic(image, magic) ||
        (magic != MfgUnlock::PtxTuring::kFatbinMagic && magic != MfgUnlock::PtxTuring::kFatbinWrapperMagic))
        return { image, nullptr };

    // 0: the container could not be read in full. BuildFromContainer is not guarded, so leave such an image alone.
    const uint64_t hash = ContainerHash(image);
    if (hash == 0)
        return { image, nullptr };
    std::lock_guard lock(cudaRewriteMutex);
    auto found = cudaRewriteCache.find(image);
    if (found == cudaRewriteCache.end() || found->second.contentHash != hash)
    {
        CudaRewriteEntry entry;
        entry.contentHash = hash;
        const auto built = MfgUnlock::PtxTuring::BuildFromContainer(static_cast<const uint8_t*>(image));
        if (built.ok)
        {
            LOG_DEBUG("[MFGUnlock] RTX 20: rewrote a kernel container for sm_75 (source sm_{}, mma {}, min/max {}+{}, "
                      "cvt {})",
                      built.sourceArch, built.mma, built.minMaxVec, built.minMaxScalar, built.cvt);
            entry.text = std::make_shared<const std::string>(built.ptx);
        }
        else
        {
            // Containers that are not nvngx_dlssg's shape belong to someone else and are expected here.
            LOG_DEBUG("[MFGUnlock] RTX 20: kernel image left as is: {}", built.failure);
        }
        found = cudaRewriteCache.insert_or_assign(image, std::move(entry)).first;
    }
    if (!found->second.text)
        return { image, nullptr };
    return { found->second.text->c_str(), found->second.text };
}

int WINAPI hkCuModuleLoadData(void** module, const void* image)
{
    const auto substitute = SubstituteCudaImage(image);
    return o_cuModuleLoadData(module, substitute.image);
}

int WINAPI hkCuModuleLoadFatBinary(void** module, const void* image)
{
    const auto substitute = SubstituteCudaImage(image);
    return o_cuModuleLoadFatBinary(module, substitute.image);
}

int WINAPI hkCuModuleLoadDataEx(void** module, const void* image, unsigned numOptions, int* options, void** values)
{
    const auto substitute = SubstituteCudaImage(image);
    return o_cuModuleLoadDataEx(module, substitute.image, numOptions, options, values);
}

int WINAPI hkCuLibraryLoadData(void** library, const void* image, void* jitOptions, void** jitValues,
                               unsigned numJitOptions, void* libraryOptions, void** libraryValues,
                               unsigned numLibraryOptions)
{
    const auto substitute = SubstituteCudaImage(image);
    return o_cuLibraryLoadData(library, substitute.image, jitOptions, jitValues, numJitOptions, libraryOptions,
                               libraryValues, numLibraryOptions);
}

// Returns false only when hooking was needed and failed. Safe to call on every nvcuda load: it never hooks twice
// (a second attach would detour the first hook and recurse). A reloaded nvcuda is noticed by ForgetCudaHooks, called
// when the hooked module is unloaded.
bool InstallCudaHooksIfWanted()
{
    if (!turingRewriteWanted.load(std::memory_order_acquire))
        return true;
    const HMODULE nvcuda = GetModuleHandleW(L"nvcuda.dll");
    if (nvcuda == nullptr)
        return true; // installed when the module loads

    std::lock_guard lock(cudaRewriteMutex);
    if (cudaHooksInstalled.load(std::memory_order_relaxed) && nvcuda == hookedNvcuda)
        return true;
    cudaRewriteCache.clear();

    o_cuModuleLoadData = reinterpret_cast<PFN_cuModuleLoadData>(GetProcAddress(nvcuda, "cuModuleLoadData"));
    o_cuModuleLoadFatBinary = reinterpret_cast<PFN_cuModuleLoadData>(GetProcAddress(nvcuda, "cuModuleLoadFatBinary"));
    o_cuModuleLoadDataEx = reinterpret_cast<PFN_cuModuleLoadDataEx>(GetProcAddress(nvcuda, "cuModuleLoadDataEx"));
    o_cuLibraryLoadData = reinterpret_cast<PFN_cuLibraryLoadData>(GetProcAddress(nvcuda, "cuLibraryLoadData"));
    if (o_cuModuleLoadData == nullptr && o_cuModuleLoadDataEx == nullptr && o_cuLibraryLoadData == nullptr)
    {
        LOG_WARN("[MFGUnlock] RTX 20: nvcuda.dll has no module-load exports to hook");
        return false;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG attachError = NO_ERROR;
    const auto attach = [&attachError](PVOID* target, PVOID hook)
    {
        if (*target == nullptr)
            return;
        const LONG error = DetourAttach(target, hook);
        if (error != NO_ERROR)
            attachError = error;
    };
    attach(&(PVOID&) o_cuModuleLoadData, hkCuModuleLoadData);
    attach(&(PVOID&) o_cuModuleLoadFatBinary, hkCuModuleLoadFatBinary);
    attach(&(PVOID&) o_cuModuleLoadDataEx, hkCuModuleLoadDataEx);
    attach(&(PVOID&) o_cuLibraryLoadData, hkCuLibraryLoadData);
    if (attachError != NO_ERROR)
    {
        DetourTransactionAbort();
        LOG_ERROR("[MFGUnlock] RTX 20: could not hook the CUDA module loaders: {:X}", attachError);
        return false;
    }
    const auto result = DetourTransactionCommit();
    if (result != NO_ERROR)
    {
        LOG_ERROR("[MFGUnlock] RTX 20: could not hook the CUDA module loaders: {:X}", result);
        return false;
    }
    hookedNvcuda = nvcuda;
    cudaHooksInstalled.store(true, std::memory_order_release);
    LOG_INFO("[MFGUnlock] RTX 20: CUDA module loaders hooked (kernels are rewritten for sm_75)");
    return true;
}

// The hooked nvcuda went away, and its detours with it.
void ForgetCudaHooks()
{
    std::lock_guard lock(cudaRewriteMutex);
    cudaHooksInstalled.store(false, std::memory_order_relaxed);
    hookedNvcuda = nullptr;
    cudaRewriteCache.clear();
}

bool IsHookedNvcuda(HMODULE module)
{
    std::lock_guard lock(cudaRewriteMutex);
    return hookedNvcuda != nullptr && module == hookedNvcuda;
}

std::set<HMODULE> turingModules; // guarded by rtx40Mutex

// RTX 20: every kernel container has to come out of the sm_75 rewrite now, at locate time. A container the rewrite
// cannot take would reach the driver as shipped at load time and fail there, after the module already advertised MFG.
bool CheckTuringKernels(const MfgUnlock::ImageView& image, std::string& failure)
{
    const auto containers = MfgUnlock::FamilyRtx40::Fatbin::ContainerRvas(image);
    if (containers.empty())
    {
        failure = "no kernel containers";
        return false;
    }
    for (const uint32_t rva : containers)
    {
        const auto* container = image.DataAtRva(rva, MfgUnlock::FamilyRtx40::Fatbin::kContainerHeader);
        const auto built = MfgUnlock::PtxTuring::BuildFromContainer(reinterpret_cast<const uint8_t*>(container));
        if (!built.ok)
        {
            failure = "container@" + std::to_string(rva) + " cannot be rewritten for sm_75: " + built.failure;
            return false;
        }
    }
    return true;
}

bool EnableTuringCudaRewrite()
{
    turingRewriteWanted.store(true, std::memory_order_release);
    return InstallCudaHooksIfWanted();
}

// Locates every site of one provider: the sl.dlss_g pair, or the nvngx_dlssg gates plus, by tier, the midpoint fix
// (Ada) or the architecture sites and kernels (RTX 20/30). Reads live module memory; run it through Guarded.
void LocateRtx40(const MfgUnlock::ImageView& image, bool streamlinePlugin, Rtx40Tier tier,
                 MfgUnlock::FamilyRtx40::Report& report, std::vector<MfgUnlock::PatchOperation>& operations,
                 std::string& layouts)
{
    if (streamlinePlugin)
    {
        const auto slReport = MfgUnlock::FamilyRtx40::Streamline::Locate(image);
        report.patches = slReport.patches;
        report.failure = slReport.failure;
        if (slReport.Complete())
            operations = MfgUnlock::FamilyRtx40::Streamline::BuildOperations(slReport, image);
        layouts = "sl.dlss_g compiled ceiling=" + std::to_string(slReport.compiledCeiling) +
                  " flip field=" + std::to_string(slReport.flipField);
        return;
    }

    report = MfgUnlock::FamilyRtx40::Locate(image);
    layouts = "advertise=" + report.advertiseLayout + " validate=" + report.validateLayout;
    if (!report.Complete())
        return;
    operations = MfgUnlock::FamilyRtx40::BuildOperations(report, image);

    if (tier == Rtx40Tier::Ampere || tier == Rtx40Tier::Turing)
    {
        // RTX 30: every kernel retargeted to sm_86 in place. RTX 20: the kernels are rewritten into PTX text at load
        // time (CUDA hook), so the fatbin is only checked here (shape and rewrite). Either way the architecture is
        // reported as the card's own and there is no partial set.
        const bool turing = tier == Rtx40Tier::Turing;
        const char* label = turing ? "RTX 20" : "RTX 30";
        const auto arch = MfgUnlock::FamilyAmpere::Locate(
            image, turing ? MfgUnlock::FamilyAmpere::kArchTuringLow : MfgUnlock::FamilyAmpere::kArchAmpereLow);
        const auto kernels =
            MfgUnlock::FamilyRtx40::Fatbin::Locate(image, MfgUnlock::FamilyRtx40::Fatbin::Target::Ampere);
        report.patches.insert(report.patches.end(), arch.patches.begin(), arch.patches.end());
        std::string kernelFailure;
        if (!arch.Complete())
            report.failure =
                arch.failure.empty() ? std::string(label) + " architecture sites incomplete" : arch.failure;
        else if (!kernels.Complete())
            report.failure = std::string(label) + " kernel check: " + kernels.failure;
        else if (turing && !CheckTuringKernels(image, kernelFailure))
            report.failure = std::string(label) + " kernel check: " + kernelFailure;
        else
        {
            auto archOps = MfgUnlock::FamilyRtx40::BuildOperationsFromPatches(arch.patches, image);
            operations.insert(operations.end(), archOps.begin(), archOps.end());
            if (turing)
            {
                layouts += " sm_75 kernels=" + std::to_string(kernels.containers) + " containers (PTX rewrite)";
            }
            else
            {
                auto kernelOps = MfgUnlock::FamilyRtx40::Fatbin::BuildOperations(kernels, image);
                operations.insert(operations.end(), kernelOps.begin(), kernelOps.end());
                layouts += " sm_86 kernels=" + std::to_string(kernels.containers) + " containers";
            }
        }
        return;
    }

    // The midpoint fix is added when this build has its kernel containers; the gates work without it.
    const auto fatbin = MfgUnlock::FamilyRtx40::Fatbin::Locate(image);
    auto midpoint = MfgUnlock::FamilyRtx40::Fatbin::BuildOperations(fatbin, image);
    if (!operations.empty() && !midpoint.empty())
    {
        operations.insert(operations.end(), std::make_move_iterator(midpoint.begin()),
                          std::make_move_iterator(midpoint.end()));
        layouts += " midpoint=" + std::to_string(fatbin.containers) + " containers";
    }
    else
    {
        layouts += " midpoint=not applied (" + fatbin.failure + ")";
    }
}

// Drops everything known about a module (guarded by rtx40Mutex); returns whether its patches were in place. Nothing
// is written back: the module is gone, or a different one now sits at its address.
bool DropRtx40Locked(HMODULE module)
{
    const auto found = rtx40PatchSets.find(module);
    const bool wasActive = found != rtx40PatchSets.end() && found->second && found->second->IsActive();
    rtx40PatchSets.erase(module);
    rtx40Families.erase(module);
    rtx40RecoveryFailed.erase(module);
    rtx40Decided.erase(module);
    turingModules.erase(module);
    // The rewrite only runs while a Turing provider is patched; a reapply turns it back on.
    if (turingModules.empty())
        turingRewriteWanted.store(false, std::memory_order_release);
    return wasActive;
}

// Patches one nvngx_dlssg copy (the game's and the driver's can both load). Detect acts on Ada only and writes
// nothing elsewhere; On forces it on any older NVIDIA GPU. Stands aside when another frame generation owns the
// output. decided turns false only when the module could not be judged (a data-file load).
bool DecideRtx40(HMODULE module, MfgUnlockMode mode, MfgUnlockMode mode2030, bool& decided)
{
    decided = true;
    const auto tier = ChooseRtx40Tier(mode, mode2030);
    const auto family = tier == Rtx40Tier::Ada || (tier == Rtx40Tier::None && mode2030 == MfgUnlockMode::Off)
                            ? MfgUnlock::Family::RTX40
                            : MfgUnlock::Family::RTX2030;
    if (State::Instance().externalFrameGeneration)
    {
        PublishRtx40(MakeRtx40Status(MfgUnlock::FamilyState::StoodAside, module, nullptr,
                                     "external frame generation is active"),
                     false, family);
        return false;
    }
    if (tier == Rtx40Tier::None)
    {
        const bool only2030 = mode == MfgUnlockMode::Off;
        const bool nvidia = IsNvidiaGpu();
        PublishRtx40(MakeRtx40Status(MfgUnlock::FamilyState::NotNeeded, module, nullptr,
                                     !nvidia                ? "primary GPU is not NVIDIA"
                                     : IsBlackwellOrNewer() ? "primary GPU is an RTX 50 or newer (MFG is native)"
                                     : only2030             ? "primary GPU is not an RTX 20/30 (Turing/Ampere)"
                                                : "primary GPU is not an RTX 40 (Ada)"),
                     false, family);
        return false;
    }
    const size_t mappedSize = GetMappedImageSize(module);
    if (mappedSize == 0) // data-file or resource load, not a running image
    {
        decided = false;
        return false;
    }

    const MfgUnlock::ImageView image(module, mappedSize);
    const bool streamlinePlugin = MfgUnlock::Provider::IsStreamlineDlssgPath(GetModulePath(module));
    MfgUnlock::FamilyRtx40::Report report;
    std::vector<MfgUnlock::PatchOperation> operations;
    std::string layouts;
    const bool readable =
        Guarded([&] { LocateRtx40(image, streamlinePlugin, tier, report, operations, layouts); });
    if (!readable)
    {
        report = {};
        operations.clear();
        report.failure = "provider memory could not be read while locating";
    }
    if (!report.Complete())
    {
        PublishRtx40(MakeRtx40Status(MfgUnlock::FamilyState::StoodAside, module, &report, report.failure), true,
                     family);
        return false;
    }

    if (operations.empty())
    {
        PublishRtx40(MakeRtx40Status(MfgUnlock::FamilyState::StoodAside, module, &report,
                                     "could not build the patch transaction"),
                     true, family);
        return false;
    }

    std::lock_guard lock(rtx40Mutex);
    auto& slot = rtx40PatchSets[module];
    if (!slot)
        slot = std::make_unique<MfgUnlock::AppliedPatchSet>();
    // Another thread may have patched this module while this one scanned it; never undo that.
    if (slot->IsActive())
        return true;
    const auto applied = slot->Apply(operations);
    if (!applied.applied)
    {
        bool restored = true;
        if (slot->IsActive())
            restored = slot->Restore().applied;
        if (!restored)
            rtx40RecoveryFailed.insert(module);
        PublishRtx40(MakeRtx40Status(MfgUnlock::FamilyState::StoodAside, module, &report,
                                     restored ? "patch transaction failed"
                                              : "patch transaction failed and partial changes could not be restored"),
                     true, family);
        return false;
    }

    if (tier == Rtx40Tier::Turing)
    {
        // Without the CUDA hooks the patched module would advertise a feature whose kernels can never load.
        if (!EnableTuringCudaRewrite())
        {
            if (turingModules.empty())
                turingRewriteWanted.store(false, std::memory_order_release);
            const bool restored = slot->Restore().applied;
            if (!restored)
                rtx40RecoveryFailed.insert(module);
            PublishRtx40(MakeRtx40Status(MfgUnlock::FamilyState::StoodAside, module, &report,
                                         restored ? "could not hook the CUDA module loaders"
                                                  : "could not hook the CUDA module loaders and the patches could "
                                                    "not be restored"),
                         true, family);
            return false;
        }
        turingModules.insert(module);
    }
    rtx40RecoveryFailed.erase(module);
    rtx40Families[module] = family;
    auto status = MakeRtx40Status(MfgUnlock::FamilyState::Active, module, &report);
    status.failure = layouts;
    AddModuleMetadata(status);
    PublishRtx40(std::move(status), false, family);
    return true;
}

// Each module is judged once per load: the kernel32 and ntdll hooks both report a load, and games load the provider
// repeatedly. The decision is forgotten when the module really unloads (ForgetRtx40Module).
bool TryApplyRtx40(HMODULE module)
{
    const auto mode = Config::Instance()->MfgUnlockRTX40.value_or_default();
    const auto mode2030 = Config::Instance()->MfgUnlockRTX2030.value_or_default();
    if (mode == MfgUnlockMode::Off && mode2030 == MfgUnlockMode::Off)
        return false;

    const auto path = GetModulePath(module);
    {
        std::lock_guard lock(rtx40Mutex);
        const auto found = rtx40PatchSets.find(module);
        const auto known = rtx40Decided.find(module);
        if (known != rtx40Decided.end() && known->second != path)
            DropRtx40Locked(module); // the module we judged went without an unload notice
        else if (found != rtx40PatchSets.end() && found->second->IsActive())
            return true;
        else if (known != rtx40Decided.end())
            return false;
    }

    bool decided = false;
    const bool active = DecideRtx40(module, mode, mode2030, decided);
    if (decided)
    {
        std::lock_guard lock(rtx40Mutex);
        rtx40Decided[module] = path;
    }
    return active;
}

// nvcuda.dll loaded after a Turing provider was patched, and its module loaders could not be hooked. The patches are
// not undone on live code (see BeforeModuleUnload), so the rewrite stops being wanted and every patched Turing
// provider is reported as stood aside: DLSS-G is advertised but its kernels cannot load on this card.
void StandAsideTuringWithoutCuda()
{
    std::lock_guard lock(rtx40Mutex);
    turingRewriteWanted.store(false, std::memory_order_release);
    for (const HMODULE module : turingModules)
    {
        PublishRtx40(MakeRtx40Status(MfgUnlock::FamilyState::StoodAside, module, nullptr,
                                     "could not hook the CUDA module loaders of a later nvcuda.dll; RTX 20 frame "
                                     "generation will not work until the game restarts"),
                     true, MfgUnlock::Family::RTX2030);
    }
}

bool IsRtx40Module(HMODULE module)
{
    if (module == nullptr)
        return false;
    std::lock_guard lock(rtx40Mutex);
    return rtx40PatchSets.contains(module) || rtx40Decided.contains(module);
}

// The module really unloaded: its pages, and the patches in them, are gone. Only the bookkeeping is dropped.
void ForgetRtx40Module(HMODULE module)
{
    bool wasActive = false;
    auto family = MfgUnlock::Family::RTX40;
    {
        std::lock_guard lock(rtx40Mutex);
        if (const auto found = rtx40Families.find(module); found != rtx40Families.end())
            family = found->second;
        wasActive = DropRtx40Locked(module);
    }
    {
        std::lock_guard cacheLock(cudaRewriteMutex);
        cudaRewriteCache.clear();
    }

    // A stood-aside or not-needed diagnostic stays as it was.
    if (wasActive)
    {
        MfgUnlock::FamilyStatus status;
        status.state = MfgUnlock::FamilyState::Waiting;
        status.failure = "provider unloaded";
        PublishRtx40(std::move(status), false, family);
    }
}

HMODULE FindXeFGModule()
{
    {
        std::lock_guard lock(xeFgMutex);
        if (xeFgPatchSet.IsActive())
            return activeXeFgModule;
    }

    // Only the copy OptiScaler's own XeFG output calls is patched. A libxess_fg.dll the game loaded for its own frame
    // generation (XeFGProxy::MemModule, or any other load) is never touched.
    const HMODULE module = XeFGProxy::Module();
    return module != nullptr && HasModuleFilename(module, L"libxess_fg.dll") ? module : nullptr;
}
} // namespace

bool MfgUnlock::AnyFamilyEnabled()
{
    const auto* config = Config::Instance();
    return config->MfgUnlockXeFG.value_or_default() != MfgUnlockMode::Off ||
           config->MfgUnlockRTX40.value_or_default() != MfgUnlockMode::Off ||
           config->MfgUnlockRTX2030.value_or_default() != MfgUnlockMode::Off;
}

bool MfgUnlock::IsProviderModuleName(std::wstring_view name)
{
    if (!AnyFamilyEnabled())
        return false;

    const size_t slash = name.find_last_of(L"\\/");
    const std::wstring_view filename = slash == std::wstring_view::npos ? name : name.substr(slash + 1);
    constexpr std::wstring_view xeFgName = L"libxess_fg.dll";
    constexpr std::wstring_view cudaName = L"nvcuda.dll";
    return (filename.size() == xeFgName.size() &&
            CompareStringOrdinal(filename.data(), static_cast<int>(filename.size()), xeFgName.data(),
                                 static_cast<int>(xeFgName.size()), TRUE) == CSTR_EQUAL) ||
           (filename.size() == cudaName.size() &&
            CompareStringOrdinal(filename.data(), static_cast<int>(filename.size()), cudaName.data(),
                                 static_cast<int>(cudaName.size()), TRUE) == CSTR_EQUAL) ||
           Provider::IsDlssgPath(name) || Provider::IsStreamlineDlssgPath(name);
}

void MfgUnlock::ObserveProviderModule(HMODULE module)
{
    if (module == nullptr)
        return;

    if (HasModuleFilename(module, L"nvcuda.dll"))
    {
        if (!InstallCudaHooksIfWanted())
            StandAsideTuringWithoutCuda();
        return;
    }

    if (!HasModuleFilename(module, L"libxess_fg.dll"))
    {
        if (Provider::IsDlssgPath(GetModulePath(module)) || Provider::IsStreamlineDlssgPath(GetModulePath(module)))
            TryApplyRtx40(module);
        return;
    }

    if (Config::Instance()->MfgUnlockXeFG.value_or_default() == MfgUnlockMode::Off)
        return;

    // XeFG is patched from PrepareXeFG, before OptiScaler's XeFG output creates its context, and only in the copy that
    // output uses; a load is only noted here.
    std::lock_guard lock(xeFgMutex);
    if (xeFgPatchSet.IsActive() && module != activeXeFgModule)
        additionalXeFgProviderObserved.store(true, std::memory_order_release);
}

bool MfgUnlock::PrepareXeFG()
{
    MfgUnlock::FamilyStatus failedRecoveryStatus;
    bool recoveryFailed = false;
    {
        std::lock_guard lock(xeFgMutex);
        recoveryFailed = xeFgPatchRecoveryFailed;
        if (recoveryFailed)
        {
            const auto recovery = xeFgPatchSet.Restore();
            if (recovery.applied)
            {
                xeFgPatchRecoveryFailed = false;
                activeXeFgModule = nullptr;
                activeXeLlModule = nullptr;
                recoveryFailed = false;
            }
            else
            {
                failedRecoveryStatus = StatusRegistry::Get(Family::XeFG);
            }
        }
    }
    if (recoveryFailed)
    {
        if (failedRecoveryStatus.failure.empty())
            failedRecoveryStatus.failure = "provider patch restoration failed";
        PublishStatus(std::move(failedRecoveryStatus), true);
        return false;
    }

    const auto mode = Config::Instance()->MfgUnlockXeFG.value_or_default();
    if (mode == MfgUnlockMode::Off)
    {
        std::lock_guard lock(xeFgMutex);
        // Nothing patched: no status, no log, so an unused unlock changes nothing.
        if (!xeFgPatchSet.IsActive())
            return true;

        const auto restored = xeFgPatchSet.Restore();
        if (!restored.applied)
        {
            PublishStatus(MakeStatus(MfgUnlock::FamilyState::StoodAside, activeXeFgModule, nullptr,
                                     "unlock disabled but the provider patches could not be restored"),
                          true);
            xeFgPatchRecoveryFailed = true;
            return false;
        }
        activeXeFgModule = nullptr;
        activeXeLlModule = nullptr;
        xeFgPatchRecoveryFailed = false;
        MfgUnlock::FamilyStatus status;
        status.state = MfgUnlock::FamilyState::Off;
        PublishStatus(std::move(status));
        return true;
    }

    if (XeLLProxy::Module() == nullptr && !XeLLProxy::InitXeLL())
    {
        SetWaitingStatus("XeLL provider could not be initialized");
        return true;
    }

    const auto xeFgModule = FindXeFGModule();
    const auto xeLlModule = XeLLProxy::Module();
    if (xeFgModule == nullptr)
    {
        SetWaitingStatus("waiting for the real libxess_fg.dll loaded by the XeFG provider");
        return true;
    }
    if (xeLlModule == nullptr)
    {
        SetWaitingStatus("waiting for the XeLL provider used by XeFG");
        return true;
    }

    TryApplyXeFG(xeFgModule, xeLlModule);
    std::lock_guard lock(xeFgMutex);
    return !xeFgPatchRecoveryFailed;
}

void MfgUnlock::RefreshXeFGStatus()
{
    if (additionalXeFgProviderObserved.exchange(false, std::memory_order_acq_rel))
        LOG_WARN("[MFGUnlock] Additional XeFG provider loaded while another provider is patched; leaving it "
                 "untouched");

    auto status = StatusRegistry::Get(Family::XeFG);
    // Hashed once per status: every later context creation would only repeat the same line.
    if (status.modulePath.empty() || !status.sha256.empty() ||
        (status.state != FamilyState::Active && status.state != FamilyState::StoodAside))
        return;

    AddModuleMetadata(status);
    PublishStatus(std::move(status));
}

bool MfgUnlock::IsXeFGActive()
{
    std::lock_guard lock(xeFgMutex);
    return xeFgPatchSet.IsActive() && !xeFgPatchRecoveryFailed;
}

bool MfgUnlock::IsTrackedProviderModule(HMODULE module)
{
    if (module == nullptr)
        return false;

    if (!AnyFamilyEnabled())
        return false;

    if (IsRtx40Module(module) || IsHookedNvcuda(module))
        return true;

    std::lock_guard lock(xeFgMutex);
    return module == activeXeFgModule || module == activeXeLlModule ||
           (xeFgStoodAside.has_value() && (module == xeFgStoodAside->xeFg || module == xeFgStoodAside->xeLl));
}

bool MfgUnlock::BeforeModuleUnload(HMODULE module)
{
    // nvngx_dlssg / sl.dlss_g patches are never undone before an unload. A FreeLibrary that only drops a reference
    // leaves the module running, and its multi-byte instruction rewrites are written one byte at a time: undoing and
    // redoing them under a running thread could execute a half-written instruction. When the module really goes its
    // pages go with it, and AfterModuleUnload drops the bookkeeping. (Every XeFG write is a single atomic store, so
    // XeFG keeps its restore.)
    if (IsRtx40Module(module))
        return true;

    std::lock_guard lock(xeFgMutex);
    if (!xeFgPatchSet.IsActive() || (module != activeXeFgModule && module != activeXeLlModule))
        return true;

    const auto restored = xeFgPatchSet.Restore();
    if (!restored.applied)
    {
        xeFgPatchRecoveryFailed = true;
        auto status = StatusRegistry::Get(Family::XeFG);
        status.state = FamilyState::StoodAside;
        status.failure = "could not restore provider patches before unload; unload is being held";
        PublishStatus(std::move(status), true);
        return false;
    }

    xeFgPatchRecoveryFailed = false;
    activeXeFgModule = nullptr;
    activeXeLlModule = nullptr;
    MfgUnlock::FamilyStatus status;
    status.state = MfgUnlock::FamilyState::Waiting;
    status.failure = "provider unloaded; patches restored";
    PublishStatus(std::move(status));
    return true;
}

void MfgUnlock::AfterModuleUnload(HMODULE module)
{
    if (module == nullptr)
        return;

    const bool rtx40 = IsRtx40Module(module);
    const bool nvcuda = IsHookedNvcuda(module);
    HMODULE stillLoaded = nullptr;
    const bool remainsLoaded =
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(module), &stillLoaded) &&
        stillLoaded == module;
    if (nvcuda)
    {
        if (!remainsLoaded)
            ForgetCudaHooks(); // its detours went with it; a reload gets hooked again on its load notification
        return;
    }
    if (rtx40)
    {
        // Still loaded: only a reference went, the patches and their state stay as they are.
        if (!remainsLoaded)
            ForgetRtx40Module(module);
        return;
    }
    if (remainsLoaded)
    {
        if (!PrepareXeFG())
            LOG_ERROR("[MFGUnlock] XeFG provider remains loaded but its patches could not be safely prepared");
    }
    else
    {
        OnModuleUnloaded(module);
    }
}

void MfgUnlock::OnModuleUnloaded(HMODULE module)
{
    // The pair that stood aside is judged again if one of its modules comes back.
    std::lock_guard lock(xeFgMutex);
    if (xeFgStoodAside.has_value() && (module == xeFgStoodAside->xeFg || module == xeFgStoodAside->xeLl))
        xeFgStoodAside.reset();
}
