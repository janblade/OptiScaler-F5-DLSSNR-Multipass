// Build: cl /std:c++20 /EHsc /W4 tests\mfgunlock_check.cpp /link Psapi.lib Bcrypt.lib Version.lib
#include "../OptiScaler/framegen/mfgunlock/Image.h"
#include "../OptiScaler/framegen/mfgunlock/FamilyXeFG.h"
#include "../OptiScaler/framegen/mfgunlock/FamilyRtx40.h"
#include "../OptiScaler/framegen/mfgunlock/FamilyFatbin.h"
#include "../OptiScaler/framegen/mfgunlock/FamilySlDlssg.h"
#include "../OptiScaler/framegen/mfgunlock/Provider.h"

#include <Windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <winver.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <vector>

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Version.lib")

namespace
{
std::string Sha256(const std::filesystem::path& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) != 0)
    {
        CloseHandle(file);
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
    CloseHandle(file);

    if (!ok)
        return {};

    std::string result;
    result.reserve(digest.size() * 2);
    for (UCHAR byte : digest)
        result += std::format("{:02x}", byte);
    return result;
}

std::string FileVersion(const std::filesystem::path& path)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0)
        return "unavailable";

    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return "unavailable";

    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) || info == nullptr ||
        infoSize < sizeof(*info))
        return "unavailable";

    return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

std::string PatchBytes(std::span<const std::byte> bytes);

bool CheckModule(const std::filesystem::path& path)
{
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (module == nullptr)
    {
        std::fwprintf(stderr, L"ERROR: could not map %ls (Win32 error %lu)\n", path.c_str(), GetLastError());
        return false;
    }

    MODULEINFO info {};
    const bool gotInfo = GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) != FALSE;
    const MfgUnlock::ImageView image(module, gotInfo ? info.SizeOfImage : 0);
    const bool valid = gotInfo && image.IsValid();
    if (!valid)
    {
        std::fwprintf(stderr, L"ERROR: %ls is not a valid mapped AMD64 PE image\n", path.c_str());
        FreeLibrary(module);
        return false;
    }

    const std::string hash = Sha256(path);
    if (hash.empty())
    {
        std::fwprintf(stderr, L"ERROR: could not hash %ls\n", path.c_str());
        FreeLibrary(module);
        return false;
    }

    std::wprintf(L"%ls | version %hs | sha256 %hs\n", path.c_str(), FileVersion(path).c_str(), hash.c_str());

    bool isDlssg = false;
    if (MfgUnlock::Provider::IsDlssgPath(path.wstring()))
    {
        std::puts("  provider: nvngx_dlssg");
        isDlssg = true;
    }
    else if (MfgUnlock::Provider::IsStreamlineDlssgPath(path.wstring()))
    {
        std::puts("  provider: sl.dlss_g");
        const auto report = MfgUnlock::FamilyRtx40::Streamline::Locate(image);
        std::printf("  sl.dlss_g compiled ceiling=%u flip field=0x%X value=%u\n", report.compiledCeiling,
                    report.flipField, report.flipValue);
        for (const auto& patch : report.patches)
        {
            const auto state = MfgUnlock::FamilyXeFG::LocateStateName(patch.state);
            std::printf("  patch %s: %.*s", patch.name.c_str(), static_cast<int>(state.size()), state.data());
            if (patch.state == MfgUnlock::LocateState::Found)
                std::printf(" +0x%08X expected=%s replacement=%s", patch.rva, PatchBytes(patch.expected).c_str(),
                            PatchBytes(patch.replacement).c_str());
            std::putchar('\n');
        }
        if (!report.failure.empty())
            std::printf("  RTX 40 sl.dlss_g stood aside: %s\n", report.failure.c_str());
        else
            std::puts("  RTX 40 sl.dlss_g: clamp and flip metering found uniquely (read-only)");
        FreeLibrary(module);
        return report.Complete();
    }
    else if (MfgUnlock::Provider::HasNgxMarker(image))
    {
        std::puts("  provider: nvngx_dlssg (renamed or cached)");
        isDlssg = true;
    }
    else
        std::puts("  provider: unclassified");

    bool result = true;
    if (isDlssg)
    {
        const auto report = MfgUnlock::FamilyRtx40::Locate(image);
        std::printf("  RTX 40 layouts: advertise=%s validate=%s\n", report.advertiseLayout.c_str(),
                    report.validateLayout.c_str());
        for (const auto& patch : report.patches)
        {
            const auto state = MfgUnlock::FamilyXeFG::LocateStateName(patch.state);
            std::printf("  patch %s: %.*s", patch.name.c_str(), static_cast<int>(state.size()), state.data());
            if (patch.state == MfgUnlock::LocateState::Found)
                std::printf(" +0x%08X expected=%s replacement=%s", patch.rva, PatchBytes(patch.expected).c_str(),
                            PatchBytes(patch.replacement).c_str());
            std::putchar('\n');
        }
        if (!report.failure.empty())
            std::printf("  RTX 40 family stood aside: %s\n", report.failure.c_str());
        else
            std::puts("  RTX 40 family: both gates found uniquely (read-only)");
        result = report.Complete();

        const auto fatbin = MfgUnlock::FamilyRtx40::Fatbin::Locate(image);
        if (fatbin.Complete())
            std::printf("  midpoint fix: %u fatbin containers, %zu byte-range patches (read-only)\n",
                        fatbin.containers, fatbin.patches.size());
        else
            std::printf("  midpoint fix stood aside: %s\n", fatbin.failure.c_str());
    }
    else
    {
        std::puts("  locators: none for this module");
    }
    FreeLibrary(module);
    return result;
}

std::string PatchBytes(std::span<const std::byte> bytes)
{
    std::string result;
    for (const auto byte : bytes)
    {
        if (!result.empty())
            result += ' ';
        result += std::format("{:02x}", std::to_integer<uint8_t>(byte));
    }
    return result;
}

bool CheckXeFGPair(const std::filesystem::path& path)
{
    const auto xellPath = path.parent_path() / L"libxell.dll";
    HMODULE xeFgModule = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (xeFgModule == nullptr)
    {
        std::fwprintf(stderr, L"ERROR: could not map %ls (Win32 error %lu)\n", path.c_str(), GetLastError());
        return false;
    }

    HMODULE xeLlModule = LoadLibraryExW(xellPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (xeLlModule == nullptr)
    {
        std::fwprintf(stderr, L"ERROR: could not map paired %ls (Win32 error %lu)\n", xellPath.c_str(), GetLastError());
        FreeLibrary(xeFgModule);
        return false;
    }

    MODULEINFO xeFgInfo {};
    MODULEINFO xeLlInfo {};
    const bool gotXeFgInfo = GetModuleInformation(GetCurrentProcess(), xeFgModule, &xeFgInfo, sizeof(xeFgInfo)) != FALSE;
    const bool gotXeLlInfo = GetModuleInformation(GetCurrentProcess(), xeLlModule, &xeLlInfo, sizeof(xeLlInfo)) != FALSE;
    const MfgUnlock::ImageView xeFgImage(xeFgModule, gotXeFgInfo ? xeFgInfo.SizeOfImage : 0);
    const MfgUnlock::ImageView xeLlImage(xeLlModule, gotXeLlInfo ? xeLlInfo.SizeOfImage : 0);
    if (!gotXeFgInfo || !gotXeLlInfo || !xeFgImage.IsValid() || !xeLlImage.IsValid())
    {
        std::fwprintf(stderr, L"ERROR: XeFG pair must contain valid mapped AMD64 PE images\n");
        FreeLibrary(xeLlModule);
        FreeLibrary(xeFgModule);
        return false;
    }

    const std::string xeFgHash = Sha256(path);
    const std::string xeLlHash = Sha256(xellPath);
    if (xeFgHash.empty() || xeLlHash.empty())
    {
        std::fwprintf(stderr, L"ERROR: could not hash the XeFG provider pair\n");
        FreeLibrary(xeLlModule);
        FreeLibrary(xeFgModule);
        return false;
    }

    std::wprintf(L"%ls | version %hs | sha256 %hs\n", path.c_str(), FileVersion(path).c_str(), xeFgHash.c_str());
    std::wprintf(L"%ls | version %hs | sha256 %hs\n", xellPath.c_str(), FileVersion(xellPath).c_str(),
                 xeLlHash.c_str());

    const auto report = MfgUnlock::FamilyXeFG::Locate(xeFgImage, xeLlImage, 5);
    for (const auto& patch : report.patches)
    {
        const auto moduleName = patch.module == MfgUnlock::FamilyXeFG::Module::XeFG ? "libxess_fg.dll" : "libxell.dll";
        std::printf("  patch %s: %.*s", patch.name.c_str(),
                    static_cast<int>(MfgUnlock::FamilyXeFG::LocateStateName(patch.state).size()),
                    MfgUnlock::FamilyXeFG::LocateStateName(patch.state).data());
        if (patch.state == MfgUnlock::LocateState::Found)
        {
            std::printf(" %s+0x%08X expected=%s replacement=%s", moduleName, patch.rva,
                        PatchBytes(patch.expected).c_str(), PatchBytes(patch.replacement).c_str());
        }
        std::putchar('\n');
    }
    if (!report.failure.empty())
        std::printf("  XeFG family stood aside: %s\n", report.failure.c_str());
    else
        std::puts("  XeFG family: all six structural locators found uniquely (read-only)");

    FreeLibrary(xeLlModule);
    FreeLibrary(xeFgModule);
    return report.Complete();
}

bool CheckPath(const std::filesystem::path& path)
{
    const auto filename = MfgUnlock::Provider::NormalisePath(path.filename().wstring());
    if (filename == L"libxess_fg.dll")
        return CheckXeFGPair(path);
    if (filename == L"libxell.dll")
    {
        std::fwprintf(stderr, L"ERROR: pass the paired libxess_fg.dll path; the checker loads sibling libxell.dll\n");
        return false;
    }
    return CheckModule(path);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::fwprintf(stderr, L"Usage: mfgunlock_check.exe <provider.dll | libxess_fg.dll> "
                             L"[provider.dll | libxess_fg.dll ...]\n");
        return 2;
    }

    bool ok = true;
    for (int index = 1; index < argc; ++index)
        ok = CheckPath(argv[index]) && ok;
    return ok ? 0 : 1;
}
