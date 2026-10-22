// Build: cl /std:c++20 /EHsc /W4 /wd4701 tests\mfgunlock_live_probe.cpp
// external\RTX40MFG-Unlock\source\native\third_party\minhook\src\hde\hde64.c
// Loads a real libxess_fg.dll + libxell.dll pair normally, locates the six sites and applies/restores the patch
// transaction, printing the protection state of every touched region. Patches live copies only; files are untouched.
#include "../OptiScaler/framegen/mfgunlock/FamilyXeFG.h"
#include "../OptiScaler/framegen/mfgunlock/Patch.h"

#include <psapi.h>

#include <cstdio>
#include <filesystem>

#pragma comment(lib, "Psapi.lib")

namespace
{
size_t MappedSize(HMODULE module)
{
    MODULEINFO info {};
    return GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) ? info.SizeOfImage : 0;
}

void PrintRegion(const char* label, const void* address)
{
    MEMORY_BASIC_INFORMATION info {};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info))
    {
        std::printf("  %s: VirtualQuery failed (%lu)\n", label, GetLastError());
        return;
    }
    std::printf("  %s: base=%p size=0x%zX state=0x%lX protect=0x%lX type=0x%lX\n", label, info.BaseAddress,
                info.RegionSize, info.State, info.Protect, info.Type);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::fwprintf(stderr, L"Usage: mfgunlock_live_probe.exe <path\\libxess_fg.dll>\n");
        return 2;
    }

    const std::filesystem::path xefgPath(argv[1]);
    const auto xellPath = xefgPath.parent_path() / L"libxell.dll";
    SetDllDirectoryW(xefgPath.parent_path().c_str());
    HMODULE xefg = LoadLibraryW(xefgPath.c_str());
    HMODULE xell = LoadLibraryW(xellPath.c_str());
    if (xefg == nullptr || xell == nullptr)
    {
        std::fwprintf(stderr, L"load failed: xefg=%p xell=%p (%lu)\n", static_cast<void*>(xefg),
                      static_cast<void*>(xell), GetLastError());
        return 1;
    }

    const MfgUnlock::ImageView xefgImage(xefg, MappedSize(xefg));
    const MfgUnlock::ImageView xellImage(xell, MappedSize(xell));
    const auto report = MfgUnlock::FamilyXeFG::Locate(xefgImage, xellImage, 5);
    std::printf("locate complete=%d failure='%s'\n", report.Complete() ? 1 : 0, report.failure.c_str());
    if (!report.Complete())
        return 1;

    const auto operations = MfgUnlock::FamilyXeFG::BuildOperations(report, xefgImage, xellImage);
    for (size_t i = 0; i < operations.size(); ++i)
        PrintRegion(report.patches[i].name.c_str(), operations[i].address);

    MfgUnlock::AppliedPatchSet set;
    const auto applied = set.Apply(operations);
    std::printf("apply: applied=%d failedIndexValid=%d failedIndex=%zu rollbackComplete=%d lastError=%lu\n",
                applied.applied ? 1 : 0, applied.failedIndexValid ? 1 : 0, applied.failedIndex,
                applied.rollbackComplete ? 1 : 0, GetLastError());
    for (size_t i = 0; i < operations.size(); ++i)
        PrintRegion(report.patches[i].name.c_str(), operations[i].address);

    const auto restored = set.Restore();
    std::printf("restore: applied=%d active=%d\n", restored.applied ? 1 : 0, set.IsActive() ? 1 : 0);
    return applied.applied && restored.applied ? 0 : 1;
}
