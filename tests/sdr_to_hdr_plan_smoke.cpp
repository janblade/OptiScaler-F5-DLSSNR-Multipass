// Host test for the swap chain desc edit of HDR output (output/SdrToHdr_Create.h, the inline part): the plan a creation site
// gets, the edit it applies to its desc, and the desc it puts back when the create with the HDR format fails (the retry). The
// part that needs Config and the log (PrepareCreate itself, Unproxy) is not in this test.
//
//   cl /std:c++20 /EHsc /W4 tests\sdr_to_hdr_plan_smoke.cpp
#include "../OptiScaler/output/SdrToHdr_Create.h"

#include <cstdio>

using namespace SdrToHdr;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

// A site's desc
struct Desc
{
    DXGI_SWAP_EFFECT effect;
    UINT count;
    UINT flags;
    DXGI_FORMAT format;
};

CreateRequest RequestOf(const Desc& d)
{
    CreateRequest r;
    r.format = d.format;
    r.swapEffect = d.effect;
    r.bufferCount = d.count;
    r.flags = d.flags;
    r.flipModel = d.effect == DXGI_SWAP_EFFECT_FLIP_DISCARD || d.effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    return r;
}

bool Same(const Desc& a, const Desc& b)
{
    return a.effect == b.effect && a.count == b.count && a.flags == b.flags && a.format == b.format;
}
} // namespace

int main()
{
    const UINT flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
                       DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    for (DXGI_SWAP_EFFECT effect : { DXGI_SWAP_EFFECT_DISCARD, DXGI_SWAP_EFFECT_SEQUENTIAL })
    {
        const char* name = effect == DXGI_SWAP_EFFECT_DISCARD ? "DISCARD" : "SEQUENTIAL";
        const Desc game { effect, 1, flags, DXGI_FORMAT_R8G8B8A8_UNORM };
        char what[160];

        // upgrade: the site applies the edit, the create with the HDR desc fails, the site puts its desc back
        Plan plan;
        MakeUpgradePlan(RequestOf(game), true, plan);
        Desc desc = game;
        ApplySwapDesc(plan, desc.effect, desc.count, desc.flags);
        desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT; // PrepareCreate's change of the format
        std::snprintf(what, sizeof(what), "%s: the plan upgrades and edits the desc", name);
        Check(plan.upgraded && plan.editDesc && desc.effect != effect && desc.count == 2 &&
                  (desc.flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE) == 0,
              what);
        std::snprintf(what, sizeof(what), "%s: the plan keeps what the game asked for", name);
        Check(plan.gameSwapEffect == effect && plan.gameBufferCount == 1 && plan.gameFlags == flags, what);

        RestoreSwapDesc(plan, desc.effect, desc.count, desc.flags);
        desc.format = plan.gameFormat == DXGI_FORMAT_UNKNOWN ? game.format : plan.gameFormat; // Unproxy's
        std::snprintf(what, sizeof(what), "%s: after the failed create the desc is exactly the game's (effect, count, flags, format)", name);
        Check(Same(desc, game), what);
    }

    // The site had already edited its desc (borderless, V-Sync override changed the flags and count, still bitblt): the restore
    // gives back THAT desc, not the game's
    {
        const Desc siteEdited { DXGI_SWAP_EFFECT_DISCARD, 3, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, DXGI_FORMAT_B8G8R8A8_UNORM };
        auto request = RequestOf(siteEdited);
        request.hasOriginal = true;
        request.originalSwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        request.originalBufferCount = 1;
        request.originalFlags = 0;
        Plan plan;
        MakeUpgradePlan(request, true, plan);
        Desc desc = siteEdited;
        ApplySwapDesc(plan, desc.effect, desc.count, desc.flags);
        Check(desc.effect == DXGI_SWAP_EFFECT_FLIP_DISCARD && desc.count == 3 && desc.flags == DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING,
              "a site-edited desc: the upgrade keeps its count and tearing flag");
        RestoreSwapDesc(plan, desc.effect, desc.count, desc.flags);
        Check(Same(desc, siteEdited), "a site-edited desc: the restore gives the site's desc back exactly");
        Check(plan.gameBufferCount == 1 && plan.gameFlags == 0, "... while the plan reports what the game asked for (count 1, no flags)");
    }

    // The V-Sync override already made a bitblt request flip (Review Pass B-P2): no desc edit, but an upgraded chain
    {
        const Desc forced { DXGI_SWAP_EFFECT_FLIP_DISCARD, 2, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, DXGI_FORMAT_R8G8B8A8_UNORM };
        auto request = RequestOf(forced);
        request.hasOriginal = true;
        request.originalSwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        request.originalBufferCount = 1;
        request.originalFlags = 0;
        Plan plan;
        MakeUpgradePlan(request, false, plan);
        Desc desc = forced;
        ApplySwapDesc(plan, desc.effect, desc.count, desc.flags);
        Check(plan.upgraded && !plan.editDesc && Same(desc, forced), "V-Sync override already flipped it: upgraded, the desc is not touched");
        RestoreSwapDesc(plan, desc.effect, desc.count, desc.flags);
        Check(Same(desc, forced), "... and the restore does not touch it either");
        Check(plan.gameSwapEffect == DXGI_SWAP_EFFECT_DISCARD && plan.gameBufferCount == 1, "... the wrapper is told the game asked for DISCARD, one buffer");
    }

    // A flip request, and a bitblt request that does not upgrade: not upgraded, nothing edited
    {
        Plan plan;
        const Desc flip { DXGI_SWAP_EFFECT_FLIP_DISCARD, 3, 0, DXGI_FORMAT_R8G8B8A8_UNORM };
        MakeUpgradePlan(RequestOf(flip), false, plan);
        Desc desc = flip;
        ApplySwapDesc(plan, desc.effect, desc.count, desc.flags);
        Check(!plan.upgraded && !plan.editDesc && Same(desc, flip), "a flip request: not upgraded, desc untouched");

        Plan plain;
        const Desc bitblt { DXGI_SWAP_EFFECT_DISCARD, 1, 0, DXGI_FORMAT_R8G8B8A8_UNORM };
        MakeUpgradePlan(RequestOf(bitblt), false, plain); // Decide said no (display, frame generation ...)
        desc = bitblt;
        ApplySwapDesc(plain, desc.effect, desc.count, desc.flags);
        RestoreSwapDesc(plain, desc.effect, desc.count, desc.flags);
        Check(!plain.upgraded && Same(desc, bitblt), "a bitblt request Decide did not upgrade: not upgraded, desc untouched (off path too)");

        // exclusive full screen / multisampled flip made by an earlier edit: no mimicry for those
        auto fullscreen = RequestOf({ DXGI_SWAP_EFFECT_FLIP_DISCARD, 2, 0, DXGI_FORMAT_R8G8B8A8_UNORM });
        fullscreen.hasOriginal = true;
        fullscreen.originalSwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        fullscreen.windowed = false;
        Plan fs;
        MakeUpgradePlan(fullscreen, false, fs);
        Check(!fs.upgraded, "exclusive full screen: never an upgraded chain");
    }

    // Classification: OptiScaler's frame generation output as HDR output sees it
    {
        Check(FgBackendOf(FGOutput::XeFG) == FgBackend::XeFG && FgBackendOf(FGOutput::FSRFG) == FgBackend::FsrFG &&
                  FgBackendOf(FGOutput::DLSSG) == FgBackend::Dlssg && FgBackendOf(FGOutput::NoFG) == FgBackend::None,
              "FgBackendOf: every output");
        Check(FgBackendOf((FGOutput) 99) == FgBackend::None, "FgBackendOf: an unknown output is none");
        Check(FgAnswersFullscreenForGame(FGOutput::XeFG) && FgAnswersFullscreenForGame(FGOutput::FSRFG) &&
                  !FgAnswersFullscreenForGame(FGOutput::DLSSG) && !FgAnswersFullscreenForGame(FGOutput::NoFG),
              "the game is told full screen for XeFG and FSR FG chains kept borderless");
    }

    // The frame generation library's exclusive full screen request: an HDR chain is made windowed, the fullscreen desc is not passed
    {
        Plan plan;
        BOOL windowed = FALSE;
        ApplyWindowed(plan, windowed);
        Check(windowed == FALSE && KeepFullscreenDesc(plan, true) && !KeepFullscreenDesc(plan, false),
              "no exclusiveToWindowed: the desc is left as it was");
        plan.exclusiveToWindowed = true;
        ApplyWindowed(plan, windowed);
        Check(windowed == TRUE && !KeepFullscreenDesc(plan, true),
              "exclusiveToWindowed: Windowed becomes TRUE and the fullscreen desc is not passed on");

        Check(ExclusiveToWindowed(true, false) && !ExclusiveToWindowed(true, true) && !ExclusiveToWindowed(false, false),
              "ExclusiveToWindowed: only the library's chain asked for exclusive full screen");

        // the create with the HDR format failed: the retry passes the game's fullscreen desc again
        plan.proxied = true;
        plan.upgraded = true;
        plan.editDesc = true;
        ClearForRetry(plan);
        Check(!plan.exclusiveToWindowed && !plan.proxied && !plan.upgraded && !plan.editDesc && KeepFullscreenDesc(plan, true),
              "ClearForRetry: the plan stands down and the retry passes the fullscreen desc");
    }

    std::printf(g_failures == 0 ? "sdr_to_hdr_plan_smoke: PASS\n" : "sdr_to_hdr_plan_smoke: %d FAIL\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
