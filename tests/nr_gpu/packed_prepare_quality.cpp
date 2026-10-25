#include "pch.h"
#include "superior_prepare_fixture.h"
int main()
{
    try
    {
        Runner r(true, 32);
        const std::string shaders = "OptiScaler/shaders/dlssnr/superior/precompile";
        auto standard = r.pipeline(shaders, "DlssNr"), carrier = r.pipeline(shaders, "dlssnr_superior_carrier");
        unsigned cases = 0;
        size_t words = 0;
        for (auto dims : { std::pair<UINT, UINT> { 32, 16 }, { 224, 128 } })
            for (float mv : { 1.f, 0.f, -0.f })
                for (int fixture = 0; fixture < 4; ++fixture)
                    for (bool half : { false, true })
                    {
                        PrepareFixture f(r, dims.first, dims.second, dims.first, dims.second, dims.first, dims.second,
                                         { 0, 0, dims.first, dims.second }, { 0, 0, dims.first, dims.second }, mv, mv,
                                         fixture, half);
                        words += f.quality(standard.Get(), carrier.Get(), carrier.Get());
                        ++cases;
                    }
        std::cout << "Packed carrier/MV preparation bit parity PASS: " << cases << " cases, " << words
                  << " words, finite/nonfinite, hold, FP16/FP32\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
