// Host check of DlssNr_ReuseDepthDefault.h: the per-game detail-reuse depth-tolerance default a game gets from the
// known-relaxed-game list. No GPU and no game needed.
// cl /std:c++20 /EHsc tests/nr_reuse_depth_default_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_ReuseDepthDefault.h"

#include <cmath>
#include <cstdio>

using namespace DlssNrReuseDepth;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main()
{
    // Known relaxed-depth games, by exe name: any case, with or without a path; anything else is not on the list.
    // Same shape as tests/nr_auto_trim_smoke.cpp's IsKnownUnexposedGame coverage, since both now go through the
    // same shared matcher (DlssNr_ExeMatch.h).
    {
        CHECK(IsKnownRelaxedDepthGame("RDR2.exe"));
        CHECK(IsKnownRelaxedDepthGame("rdr2.exe"));
        CHECK(IsKnownRelaxedDepthGame("PlayRDR2.exe"));
        CHECK(IsKnownRelaxedDepthGame("J:\\Games\\Red Dead Redemption 2\\RDR2.exe"));
        CHECK(IsKnownRelaxedDepthGame("C:/Games/RDR2/rdr2.exe"));
        CHECK(!IsKnownRelaxedDepthGame("rdr.exe")); // Red Dead Redemption 1: not measured
        CHECK(!IsKnownRelaxedDepthGame("nba2k27.exe"));
        CHECK(!IsKnownRelaxedDepthGame("witcher3.exe")); // fixed by the 0.051 global default, needs no quirk
        CHECK(!IsKnownRelaxedDepthGame("xrdr2.exe"));
        CHECK(!IsKnownRelaxedDepthGame("rdr2.exe.bak"));
        CHECK(!IsKnownRelaxedDepthGame(""));
    }
    // The measured value itself, so a future edit cannot silently drift from what was actually tested.
    {
        CHECK(std::fabs(kRdr2DepthTolerance - 0.267f) < 1e-6f);
    }
    // The user's own value always wins, on any game.
    {
        CHECK(Effective(std::optional<float>(0.5f), true, 0.051f) == 0.5f);
        CHECK(Effective(std::optional<float>(0.5f), false, 0.051f) == 0.5f);
    }
    // Without a user value: the shipped default for an unlisted game, the measured value for a known relaxed one.
    {
        CHECK(Effective(std::nullopt, false, 0.051f) == 0.051f);
        CHECK(Effective(std::nullopt, true, 0.051f) == kRdr2DepthTolerance);
    }

    printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
