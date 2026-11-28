// [DlssNr] SuperiorAtNative: when Superior's private DLSS also runs at 100% and above, and when that counts as
// supersampling (its generation then works at the frame's size on the averaged-down pair).
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\nr_superior_at_native_smoke.cpp
#include <cmath>
#include <cstdio>
#include <limits>
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
    // On at and above 100% with a private DLSS that can run
    Check(StabilizeAtNative(true, 1.0f, false, false, CnnPreference, false), "100% CNN");
    Check(StabilizeAtNative(true, 1.11f, false, false, TransformerK, false), "111% Transformer");
    Check(StabilizeAtNative(true, 2.0f, false, false, CnnD, false), "200% saved CNN D counts as CNN");
    // Below 100% the existing rule (NeedsTemporal) decides
    Check(!StabilizeAtNative(true, 0.99f, false, false, CnnPreference, false), "99% left to NeedsTemporal");
    // Off, compressed edges, No DLSS, no NGX DLSS SR: the ordinary path, as before
    Check(!StabilizeAtNative(false, 1.11f, false, false, CnnPreference, false), "setting off");
    // Compress screen edges: above 100% the unpacked pair is averaged to native by compression's own down-leg and the
    // private DLSS steadies it; at exactly 100% and below compression's packed rule (NeedsTemporal) decides.
    Check(StabilizeAtNative(true, 1.11f, true, true, CnnPreference, false), "compressed, supersampled: runs");
    Check(StabilizeAtNative(true, 1.5f, true, true, TransformerK, false), "compressed 150% Transformer: runs");
    Check(!StabilizeAtNative(true, 1.0f, true, false, CnnPreference, false), "compressed at 100%: packed rule");
    Check(!StabilizeAtNative(true, 1.0001f, true, false, CnnPreference, false),
          "compressed, scale rounds back to native: packed rule");
    Check(!StabilizeAtNative(true, 0.9f, true, false, CnnPreference, false), "compressed below 100%: packed rule");
    // the flag worked out the way DlssNr_Dx12.cpp does: a scale that rounds back to native is not supersampled
    Check(!StabilizeAtNative(true, 1.0001f, true,
                             StabilizeSupersampled(true, 2560, 1440, WorkingExtent(2560, 1.0001f),
                                                   WorkingExtent(1440, 1.0001f)),
                             CnnPreference, false),
          "compressed, 1.0001 as Dx12 works it out: packed rule");
    Check(StabilizeAtNative(true, 1.2f, true,
                            StabilizeSupersampled(true, 2560, 1440, WorkingExtent(2560, 1.2f),
                                                  WorkingExtent(1440, 1.2f)),
                            CnnPreference, false),
          "compressed, 1.2 as Dx12 works it out: runs");
    Check(!StabilizeAtNative(false, 1.5f, true, true, CnnPreference, false), "compressed, setting off");
    Check(!StabilizeAtNative(true, 1.5f, true, true, NoPrivateDlss, false), "compressed, No DLSS");
    Check(!StabilizeAtNative(true, 1.5f, true, true, CnnPreference, true), "compressed, SR unavailable");
    Check(!StabilizeAtNative(true, 1.11f, false, false, NoPrivateDlss, false), "No DLSS");
    Check(!StabilizeAtNative(true, 1.11f, false, false, CnnPreference, true), "SR unavailable");
    Check(!StabilizeAtNative(true, std::numeric_limits<float>::quiet_NaN(), false, false, CnnPreference, false), "NaN scale");

    // Supersampling = the real raster is larger, not the scale value
    Check(!StabilizeSupersampled(true, 2560, 1440, 2560, 1440), "100%: native raster");
    Check(StabilizeSupersampled(true, 2560, 1440, WorkingExtent(2560, 1.11f), WorkingExtent(1440, 1.11f)), "111%");
    Check(!StabilizeSupersampled(true, 2560, 1440, WorkingExtent(2560, 1.0001f), WorkingExtent(1440, 1.0001f)),
          "a scale that rounds back to native is not supersampling");
    Check(!StabilizeSupersampled(false, 2560, 1440, 2842, 1598), "not at native: never");

    std::printf(g_failed ? "%d check(s) FAILED\n" : "all checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
