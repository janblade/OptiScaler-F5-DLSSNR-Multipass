// Host check of DlssNr_ExposureScanRecord.h (the exposure scan's CSV lines) and DlssNr_ExposureScanSource.h (whose
// resources it adopts). No GPU and no game needed.
// cl /std:c++20 /EHsc tests/nr_exposure_scan_record_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_ExposureScanRecord.h"
#include "../OptiScaler/dlssnr/DlssNr_ExposureScanSource.h"

#include <cstdio>

using namespace DlssNrExposureScanRecord;

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
    // Cadence: frame 0 and every kEveryFrames after it
    CHECK(Due(0));
    CHECK(!Due(1));
    CHECK(Due(kEveryFrames));
    CHECK(!Due(kEveryFrames + 1));

    // Names: numbered from 1 as the log's "candidate N" lines are, the handed one marked
    CHECK(NamesLine({}) == "# candidates:");
    CHECK(NamesLine({ { "buffer, 4 bytes", false }, { "1x1 R32_FLOAT", true } }) ==
          "# candidates: 1=buffer, 4 bytes; 2=1x1 R32_FLOAT (handed)");

    // Columns follow the candidate count
    CHECK(ColumnsLine(0) == "seconds,frame,meter,game,pre");
    CHECK(ColumnsLine(2) == "seconds,frame,meter,game,pre,c1,c2");

    // Rows: fixed fields, then one value per candidate
    Sample s;
    s.seconds = 12.3456;
    s.frame = 600;
    s.meter = 0.125f;
    s.game = 1.3195f;
    s.pre = 1.0f;
    s.values = { 0.5f, 0.0f, 1e-5f };
    CHECK(Row(s) == "12.346,600,0.125,1.3195,1,0.5,0,1e-05");

    s.values.clear();
    CHECK(Row(s) == "12.346,600,0.125,1.3195,1");

    // Whose resource: only the game's own
    using DlssNrExposureScanSource::FromGame;
    CHECK(FromGame("NBA2K27.exe", false));
    CHECK(FromGame("Cyberpunk2077.exe", false));
    CHECK(FromGame("", false));         // no module found: not known to be someone else's
    CHECK(!FromGame("dxgi.dll", true)); // OptiScaler itself, whatever it is named
    CHECK(!FromGame("nvngx_dlss.dll", false));
    CHECK(!FromGame("NVNGX_DLSSG.DLL", false));
    CHECK(!FromGame("sl.dlss_g.dll", false));
    CHECK(!FromGame("libxess_fg.dll", false));
    CHECK(!FromGame("amd_fidelityfx_upscaler_dx12.dll", false));
    CHECK(!FromGame("ffx_fsr3upscaler_x64.dll", false));

    printf(fails == 0 ? "nr_exposure_scan_record_smoke: PASS\n" : "nr_exposure_scan_record_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
