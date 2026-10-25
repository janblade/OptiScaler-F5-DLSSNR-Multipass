#include "pch.h"
#include "interpass_fixture.h"

int main()
{
    try
    {
        Runner r(true);
        const std::string dir = "OptiScaler/shaders/dlssnr/superior/precompile";
        auto reference = r.pipeline(dir, "DlssNr"), candidate = r.pipeline(dir, "host_low_raw");
        unsigned cases = 0;
        size_t words = 0;
        for (const auto size : { std::pair<UINT, UINT> { 1, 1 },
                                 { 1, 19 },
                                 { 23, 1 },
                                 { 7, 9 },
                                 { 16, 16 },
                                 { 17, 11 },
                                 { 65, 37 },
                                 { 129, 73 },
                                 { 257, 143 } })
            for (int fixture = 0; fixture < 4; ++fixture)
                for (int pattern : { 0, 1, 2 })
                    for (bool half : { false, true })
                    {
                        const auto w = (size.first + 7) / 8, h = (size.second + 7) / 8;
                        DlssNrConstants c {};
                        c.Mode = 18;
                        c.Transfer = 0;
                        c.Width = w;
                        c.Height = h;
                        c.DirectResolveFlags = 65536;
                        auto a = r.run(reference.Get(), size.first, size.second, w, h, c, fixture, half, 8, size.first,
                                       size.second, nullptr, pattern);
                        auto b = r.run(candidate.Get(), size.first, size.second, w, h, c, fixture, half, 8, size.first,
                                       size.second, nullptr, pattern, nullptr, 0, 0, 16);
                        if (a != b)
                            throw std::runtime_error("Raw cached low differs from v59 reference at " +
                                                     std::to_string(size.first) + "x" + std::to_string(size.second));
                        ++cases;
                        words += a.size();
                    }
        std::cout << "Spatial raw low GPU bit parity PASS: " << cases << " cases, " << words
                  << " words, FP16/FP32 targets, HDR/nonfinite inputs and partial tiles\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
