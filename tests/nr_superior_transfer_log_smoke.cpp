// The composition log's "transfer" words (DlssNr::SuperiorScaling::TransferLogName): it named every Superior frame
// "matched residual" and NVIDIA residual "classic".
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\nr_superior_transfer_log_smoke.cpp
#include <cstdio>
#include <string>
#include "../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"

using namespace DlssNr::SuperiorScaling;

static int g_failed = 0;
static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failed;
    }
}

int main()
{
    // The ordinary modes, whatever Superior did last
    Check(TransferLogName(0u, Resolve::None, 0) == "classic", "classic");
    Check(TransferLogName(1u, Resolve::None, 0) == "matched residual", "matched residual");
    Check(TransferLogName(2u, Resolve::None, 0) == "NVIDIA residual", "NVIDIA residual (was printed as classic)");
    Check(TransferLogName(1u, Resolve::PrivateDlss, 11) == "matched residual", "a stale Superior state is ignored");

    // Superior selected: what the frame got
    Check(TransferLogName(Transfer, Resolve::PrivateDlss, 11) == "Superior Scaling (private DLSS preset 11)",
          "private DLSS names the preset that runs");
    Check(TransferLogName(Transfer, Resolve::NoDepth, 11).find("no game depth") != std::string::npos,
          "no depth says why the DLSS does not run");
    Check(TransferLogName(Transfer, Resolve::Raw, 0) == "Superior Scaling without DLSS", "No DLSS");
    Check(TransferLogName(Transfer, Resolve::None, 0).rfind("matched residual", 0) == 0,
          "selected but not resolving is still matched residual, and says so");

    // The hold: a state alternating frame by frame never reaches the log; one that lasts kFrames frames does
    {
        ResolveHold hold;
        for (int i = 0; i < 200; ++i)
            Check(hold.Update(i % 2 ? Resolve::NoDepth : Resolve::PrivateDlss) == Resolve::None,
                  "alternating states stay at the shown one");
        Resolve shown = Resolve::None;
        for (unsigned i = 0; i < ResolveHold::kFrames; ++i)
            shown = hold.Update(Resolve::NoDepth);
        Check(shown == Resolve::NoDepth, "kFrames in a row switch");
        Check(hold.Update(Resolve::PrivateDlss) == Resolve::NoDepth, "one frame back does not switch");
        Check(hold.Update(Resolve::NoDepth) == Resolve::NoDepth, "and the count starts over");
    }

    if (g_failed == 0)
        std::printf("PASS\n");
    return g_failed == 0 ? 0 : 1;
}
