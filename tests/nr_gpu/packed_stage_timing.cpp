#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "packed_stage_fixture.h"

int main()
{
    try
    {
        Runner r(true, 112);
        std::ofstream out("artifacts/validation/packed-stage-timing.csv");
        out << std::setprecision(9);
        out << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        for (int hdr : { -1, 0 })
            for (int scale : { 65, 100 })
            {
                PackedStages f(r, scale, hdr);
                measure(
                    f.images, out, hdr < 0 ? "packed_route_sdr" : "packed_route_hdr0", scale, [&] { f.draw(false); },
                    [&] { f.draw(true); });
            }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
