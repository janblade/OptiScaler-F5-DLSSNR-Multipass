// Build: cl /std:c++20 /EHsc /W4 /wd4701 tests\mfgunlock_rtx40_probe.cpp
// OptiScaler\framegen\mfgunlock\hde\hde64.c
// Usage: mfgunlock_rtx40_probe.exe <nvngx_dlssg.dll | driver .bin>
// Maps the provider without running it, applies the RTX 40 transaction, verifies the bytes and restores them.
#include "../OptiScaler/framegen/mfgunlock/FamilyRtx40.h"
#include "../OptiScaler/framegen/mfgunlock/FamilyAmpere.h"
#include "../OptiScaler/framegen/mfgunlock/FamilySlDlssg.h"
#include "../OptiScaler/framegen/mfgunlock/FamilyFatbin.h"

#include <psapi.h>

#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return 2;

    HMODULE module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    MODULEINFO info {};
    if (module == nullptr || !GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
    {
        std::fwprintf(stderr, L"could not map %ls\n", argv[1]);
        return 1;
    }

    const MfgUnlock::ImageView image(module, info.SizeOfImage);
    const std::wstring_view argument(argv[1]);
    if (argument.find(L"sl.dlss_g") != std::wstring_view::npos)
    {
        const auto sl = MfgUnlock::FamilyRtx40::Streamline::Locate(image);
        std::printf("sl.dlss_g complete=%d ceiling=%u field=0x%X value=%u %s\n", sl.Complete() ? 1 : 0,
                    sl.compiledCeiling, sl.flipField, sl.flipValue, sl.failure.c_str());
        if (!sl.Complete())
            return 1;
        const auto slOperations = MfgUnlock::FamilyRtx40::Streamline::BuildOperations(sl, image);
        MfgUnlock::AppliedPatchSet slSet;
        const auto slApplied = slSet.Apply(slOperations);
        bool slMatch = slApplied.applied;
        for (const auto& patch : sl.patches)
        {
            const auto* now = image.DataAtRva(patch.rva, patch.replacement.size());
            slMatch = slMatch && now != nullptr &&
                      std::memcmp(now, patch.replacement.data(), patch.replacement.size()) == 0;
        }
        const auto slRestored = slSet.Restore();
        bool slBack = slRestored.applied;
        for (const auto& patch : sl.patches)
        {
            const auto* now = image.DataAtRva(patch.rva, patch.expected.size());
            slBack = slBack && now != nullptr && std::memcmp(now, patch.expected.data(), patch.expected.size()) == 0;
        }
        std::printf("operations=%zu applied=%d bytesMatch=%d restored=%d\n", slOperations.size(),
                    slApplied.applied ? 1 : 0, slMatch ? 1 : 0, slBack ? 1 : 0);
        return slMatch && slBack ? 0 : 1;
    }

    const auto report = MfgUnlock::FamilyRtx40::Locate(image);
    std::printf("layouts advertise=%s validate=%s complete=%d %s\n", report.advertiseLayout.c_str(),
                report.validateLayout.c_str(), report.Complete() ? 1 : 0, report.failure.c_str());
    if (!report.Complete())
        return 1;

    const auto operations = MfgUnlock::FamilyRtx40::BuildOperations(report, image);
    MfgUnlock::AppliedPatchSet patchSet;
    const auto applied = patchSet.Apply(operations);
    bool bytesMatch = applied.applied;
    for (const auto& patch : report.patches)
    {
        const auto* now = image.DataAtRva(patch.rva, patch.replacement.size());
        bytesMatch = bytesMatch && now != nullptr && std::memcmp(now, patch.replacement.data(), patch.replacement.size()) == 0;
    }
    const auto restored = patchSet.Restore();
    bool bytesRestored = restored.applied;
    for (const auto& patch : report.patches)
    {
        const auto* now = image.DataAtRva(patch.rva, patch.expected.size());
        bytesRestored = bytesRestored && now != nullptr && std::memcmp(now, patch.expected.data(), patch.expected.size()) == 0;
    }
    std::printf("operations=%zu applied=%d bytesMatch=%d restored=%d\n", operations.size(), applied.applied ? 1 : 0,
                bytesMatch ? 1 : 0, bytesRestored ? 1 : 0);

    // Midpoint fix, applied together with the gates as the runtime does.
    const auto fatbin = MfgUnlock::FamilyRtx40::Fatbin::Locate(image);
    auto all = operations;
    auto midpoint = MfgUnlock::FamilyRtx40::Fatbin::BuildOperations(fatbin, image);
    all.insert(all.end(), midpoint.begin(), midpoint.end());
    MfgUnlock::AppliedPatchSet combined;
    const auto combinedApplied = combined.Apply(all);
    bool midMatch = fatbin.Complete() && combinedApplied.applied;
    for (const auto& patch : fatbin.patches)
    {
        const auto* now = image.DataAtRva(patch.rva, patch.replacement.size());
        midMatch = midMatch && now != nullptr && std::memcmp(now, patch.replacement.data(), patch.replacement.size()) == 0;
    }
    const auto combinedRestored = combined.Restore();
    bool midBack = combinedRestored.applied;
    for (const auto& patch : fatbin.patches)
    {
        const auto* now = image.DataAtRva(patch.rva, patch.expected.size());
        midBack = midBack && now != nullptr && std::memcmp(now, patch.expected.data(), patch.expected.size()) == 0;
    }
    std::printf("midpoint containers=%u operations=%zu applied=%d failedIndex=%zu valid=%d bytesMatch=%d restored=%d %s\n",
                fatbin.containers, all.size(), combinedApplied.applied ? 1 : 0, combinedApplied.failedIndex,
                combinedApplied.failedIndexValid ? 1 : 0, midMatch ? 1 : 0, midBack ? 1 : 0,
                fatbin.failure.c_str());
    if (combinedApplied.failedIndexValid && combinedApplied.failedIndex < all.size())
    {
        MEMORY_BASIC_INFORMATION mbi {};
        VirtualQuery(all[combinedApplied.failedIndex].address, &mbi, sizeof(mbi));
        std::printf("  failing address protect=0x%lX state=0x%lX\n", mbi.Protect, mbi.State);
    }
    const bool adaOk = bytesMatch && bytesRestored && midMatch && midBack;

    // RTX 30 set: the same gates, the architecture immediates lowered to Ampere and the kernels retargeted to sm_86.
    const auto ampereArch = MfgUnlock::FamilyAmpere::Locate(image, MfgUnlock::FamilyAmpere::kArchAmpereLow);
    const auto ampereFatbin = MfgUnlock::FamilyRtx40::Fatbin::Locate(image, MfgUnlock::FamilyRtx40::Fatbin::Target::Ampere);
    std::printf("ampere arch complete=%d patches=%zu %s | fatbin complete=%d containers=%u rejected=%u %s\n",
                ampereArch.Complete() ? 1 : 0, ampereArch.patches.size(), ampereArch.failure.c_str(),
                ampereFatbin.Complete() ? 1 : 0, ampereFatbin.containers, ampereFatbin.rejected,
                ampereFatbin.failure.c_str());
    if (!ampereArch.Complete() || !ampereFatbin.Complete())
        return 1;

    // RTX 20 locate-time check: every container must come out of the sm_75 rewrite.
    const auto containerRvas = MfgUnlock::FamilyRtx40::Fatbin::ContainerRvas(image);
    unsigned turingOk = 0;
    for (const uint32_t rva : containerRvas)
        turingOk += MfgUnlock::PtxTuring::BuildFromContainer(
                        reinterpret_cast<const uint8_t*>(image.DataAtRva(rva, 16)))
                        .ok;
    std::printf("turing containers=%zu rewritable=%u\n", containerRvas.size(), turingOk);
    if (containerRvas.empty() || turingOk != containerRvas.size())
        return 1;

    auto ampereOperations = operations;
    auto archOperations = MfgUnlock::FamilyRtx40::BuildOperationsFromPatches(ampereArch.patches, image);
    auto kernelOperations = MfgUnlock::FamilyRtx40::Fatbin::BuildOperations(ampereFatbin, image);
    ampereOperations.insert(ampereOperations.end(), archOperations.begin(), archOperations.end());
    ampereOperations.insert(ampereOperations.end(), kernelOperations.begin(), kernelOperations.end());
    MfgUnlock::AppliedPatchSet ampereSet;
    const auto ampereApplied = ampereSet.Apply(ampereOperations);
    bool ampereMatch = ampereApplied.applied;
    const auto verify = [&](const auto& patches, bool wantReplacement) {
        bool good = true;
        for (const auto& patch : patches)
        {
            const auto& wanted = wantReplacement ? patch.replacement : patch.expected;
            const auto* now = image.DataAtRva(patch.rva, wanted.size());
            good = good && now != nullptr && std::memcmp(now, wanted.data(), wanted.size()) == 0;
        }
        return good;
    };
    ampereMatch = ampereMatch && verify(report.patches, true) && verify(ampereArch.patches, true) &&
                  verify(ampereFatbin.patches, true);
    const auto ampereRestored = ampereSet.Restore();
    const bool ampereBack = ampereRestored.applied && verify(report.patches, false) &&
                            verify(ampereArch.patches, false) && verify(ampereFatbin.patches, false);
    std::printf("ampere operations=%zu applied=%d bytesMatch=%d restored=%d\n", ampereOperations.size(),
                ampereApplied.applied ? 1 : 0, ampereMatch ? 1 : 0, ampereBack ? 1 : 0);
    return adaOk && ampereMatch && ampereBack ? 0 : 1;
}
