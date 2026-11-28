// Host check of DrsCallerRule.h: f5low's private Streamline gets "no NVIDIA app override", the game's keeps it.
// cl /std:c++20 /EHsc /W4 tests/drs_caller_rule_smoke.cpp
#include "../OptiScaler/nvapi/DrsCallerRule.h"

#include <cstdio>

using namespace drscaller;

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
    // f5low's renamed copy (f5low-fg generators/dlssg_files.h)
    CHECK(IsF5LowStreamline(L"xefg_slinterposer.dll"));
    CHECK(IsF5LowStreamline(L"XEFG_SLINTERPOSER.DLL"));
    CHECK(IsF5LowStreamline(L"sl.xefg_common.dll"));
    CHECK(IsF5LowStreamline(L"sl.xefg_dlss_g.dll"));
    CHECK(IsF5LowStreamline(L"SL.XEFG_Reflex.dll"));

    // the game's own Streamline and NVIDIA's override plugins are not f5low's
    CHECK(!IsF5LowStreamline(L"sl.interposer.dll"));
    CHECK(!IsF5LowStreamline(L"sl.common.dll"));
    CHECK(!IsF5LowStreamline(L"sl.dlss_g.dll"));
    CHECK(!IsF5LowStreamline(L"160_E658700.dll")); // sl_common_override_0 from ProgramData
    CHECK(!IsF5LowStreamline(L"xefg_slinterposer.dll.bak"));
    CHECK(!IsF5LowStreamline(L"libxess_fg.dll"));
    CHECK(!IsF5LowStreamline(L""));

    // only the Streamline override key is answered
    CHECK(AnswerOtaOverrideOff(0x10E41E06, L"xefg_slinterposer.dll"));
    CHECK(!AnswerOtaOverrideOff(0x10E41E06, L"sl.interposer.dll"));
    CHECK(!AnswerOtaOverrideOff(0x10E41E03, L"xefg_slinterposer.dll")); // FG DLL override: not ours to change
    CHECK(!AnswerOtaOverrideOff(0x10E41DF3, L"sl.xefg_common.dll"));

    if (fails == 0)
        printf("drs_caller_rule_smoke: PASS\n");
    return fails == 0 ? 0 : 1;
}
