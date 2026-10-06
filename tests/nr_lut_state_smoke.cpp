// Host check of DlssNr_Lut.h: DlssNr_LutEnsureParsed's reparse-on-change and sticky-failure behaviour. No
// GPU and no game -- writes small temp .cube files beside the binary, same as nr_lut_cube_parse_smoke.cpp.
// cl /std:c++20 /EHsc /W4 tests/nr_lut_state_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_Lut.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

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

static std::string WriteTemp(const char* name, const std::string& contents)
{
    const std::string path = std::string("nr_lut_state_smoke_") + name + ".cube";
    std::ofstream out(path);
    out << contents;
    out.close();
    return path;
}

int main()
{
    const std::string body2x2 = "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";

    // Empty path clears everything and reports no LUT wanted.
    {
        DlssNr_LutState state;
        CHECK(!DlssNr_LutEnsureParsed(&state, ""));
        CHECK(!state.Loaded());
        CHECK(state.loadedPath.empty());
    }

    // A valid file loads: Loaded() true, lattice size matches.
    const std::string pathA = WriteTemp("a", body2x2);
    {
        DlssNr_LutState state;
        CHECK(DlssNr_LutEnsureParsed(&state, pathA));
        CHECK(state.Loaded());
        CHECK(state.loadedPath == pathA);
        CHECK(state.lut.size == 2);
    }

    // The same path again is a cheap no-op (attemptedPath early-out) and stays loaded.
    {
        DlssNr_LutState state;
        CHECK(DlssNr_LutEnsureParsed(&state, pathA));
        CHECK(DlssNr_LutEnsureParsed(&state, pathA));
        CHECK(state.Loaded());
    }

    // The bug this test exists for (Review Pass, 2026-10-04): loading a working LUT, then trying a
    // DIFFERENT, bad path, must not blank out the working one. Loaded() must stay true and loadedPath/lut
    // must still be A's -- only `failed`/`error` describe the bad attempt.
    {
        const std::string badPath = WriteTemp("bad", "not a cube file at all\n");

        DlssNr_LutState state;
        CHECK(DlssNr_LutEnsureParsed(&state, pathA));
        CHECK(!DlssNr_LutEnsureParsed(&state, badPath));
        CHECK(state.Loaded());            // <- was false before the fix
        CHECK(state.loadedPath == pathA); // <- the working LUT must still be the one in effect
        CHECK(state.lut.size == 2);
        CHECK(state.failed);                   // the bad attempt is still recorded...
        CHECK(!state.error.empty());           // ...with its own error...
        CHECK(state.attemptedPath == badPath); // ...and its own path, distinct from loadedPath

        std::filesystem::remove(badPath);
    }

    // Switching to a second, different, VALID path does take effect (not the same bug in reverse: a good
    // reparse must still update loadedPath/lut, not get stuck on the first file loaded).
    {
        std::string body3 = "LUT_3D_SIZE 3\n";
        for (int i = 0; i < 27; ++i)
            body3 += "0 0 0\n";
        const std::string pathB = WriteTemp("b", body3);

        DlssNr_LutState state;
        CHECK(DlssNr_LutEnsureParsed(&state, pathA));
        CHECK(state.lut.size == 2);
        CHECK(DlssNr_LutEnsureParsed(&state, pathB));
        CHECK(state.Loaded());
        CHECK(state.loadedPath == pathB);
        CHECK(state.lut.size == 3);

        std::filesystem::remove(pathB);
    }

    // Clearing after a working load reports no LUT wanted again, not "failed".
    {
        DlssNr_LutState state;
        CHECK(DlssNr_LutEnsureParsed(&state, pathA));
        CHECK(!DlssNr_LutEnsureParsed(&state, ""));
        CHECK(!state.Loaded());
        CHECK(!state.failed);
        CHECK(state.loadedPath.empty());
    }

    std::filesystem::remove(pathA);

    printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
