#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "four_candidates_fixture.h"
#include "superior_input_fixture.h"

int main()
{
    try
    {
        Runner r(true);
        unsigned cases = 0;
        size_t words = 0;
        for (bool packed : { false, true })
            for (int curve : { -1, 0, 2, 4, 6 })
            {
                const auto suffix =
                    std::string(packed ? "packed_" : "") + (curve < 0 ? "sdr" : "hdr" + std::to_string(curve));
                auto base = r.pipeline(CandidateDirectory, "base_" + suffix);
                for (const std::string family : { "fixed", "lds", "cached", "production_fixed", "production_cached" })
                {
                    const bool production = family.find("production_") == 0;
                    const bool cached = family == "cached" || family == "production_cached";
                    if (!production && family != "fixed" && curve != 0 && curve != -1)
                        continue;
                    if (cached && !packed)
                        continue;
                    auto candidate = r.pipeline(
                        production ? "OptiScaler/shaders/dlssnr/superior/precompile" : CandidateDirectory,
                        production ? (cached ? "host_cached_" : "host_normal_") + suffix : family + "_" + suffix);
                    for (auto dims : { std::pair<UINT, UINT> { 193, 129 }, { 385, 217 } })
                        for (float scale : { .5f, .65f, 1.f })
                            for (int preset = 0; preset < 4; ++preset)
                            {
                                DlssNr::Spatial::Settings settings;
                                settings.enabled = true;
                                if (preset < 3)
                                    DlssNr::Spatial::ApplyNamedLayout(settings, preset);
                                else
                                {
                                    settings.offsetX = 7;
                                    settings.offsetY = -5;
                                    settings.shiftX = -2;
                                    settings.shiftY = 1;
                                }
                                auto layout = DlssNr::Spatial::Build(settings, dims.first, dims.second, scale, false);
                                auto warp = packed ? UploadWarp(r, layout) : ComPtr<ID3D12Resource> {};
                                auto cache = cached ? CachedWarp(r, layout) : ComPtr<ID3D12Resource> {};
                                for (bool half : { false, true })
                                    for (unsigned scenario = 0; scenario < 4; ++scenario)
                                    {
                                        HostConstants h {};
                                        h.Mode = 1;
                                        h.Width = dims.first;
                                        h.Height = dims.second;
                                        h.WhitePoint = .4f;
                                        h.ApplyModel = 1;
                                        h.TransferStrength = .9f;
                                        h.ColourStrength = .8f;
                                        h.MaxRatio = 2;
                                        h.Passthrough = curve < 0;
                                        h.ReversibleMode = std::max(0, curve);
                                        h.PreExposure = 1.7f;
                                        h.ExposureTrim = 1.2f;
                                        h.InputEncoding = scenario == 1 ? 3 : scenario == 2 ? 4 : 0;
                                        h.OriginalIsGameColour = 1;
                                        h.UseGameExposure = scenario == 1;
                                        h.UseExposureWhitePoint = scenario == 2;
                                        h.ExposureTrimAnchorCount = scenario == 2 ? 2 : 0;
                                        h.ExposureTrimAnchorExposure0 = .1f;
                                        h.ExposureTrimAnchorTrim0 = .8f;
                                        h.ExposureTrimAnchorExposure1 = 10;
                                        h.ExposureTrimAnchorTrim1 = 1.6f;
                                        h.SkinProtection = scenario == 3;
                                        h.SkinDetail = .3f;
                                        h.SkinColour = .4f;
                                        h.EnvironmentDetail = h.EnvironmentColour = 1;
                                        h.ModelWorkScale = scale;
                                        h.GuideWidth = 1;
                                        h.MvScaleX = .8f;
                                        h.MvScaleY = .661f;
                                        DlssNrConstants bytes {};
                                        std::memcpy(&bytes, &h, 256);
                                        const UINT sw = packed ? layout.modelW : std::max(1u, UINT(dims.first * scale));
                                        const UINT sh =
                                            packed ? layout.modelH : std::max(1u, UINT(dims.second * scale));
                                        auto run = [&](ID3D12PipelineState* p, ID3D12Resource* m)
                                        {
                                            return r.run(p, dims.first, dims.second, dims.first, dims.second, bytes,
                                                         scenario, half, 8, sw, sh, nullptr, 3, nullptr, 0, 0, 8, m,
                                                         half ? InputFixture::halfBits : nullptr);
                                        };
                                        auto a = run(base.Get(), warp.Get());
                                        auto b = run(candidate.Get(), cache ? cache.Get() : warp.Get());
                                        if (a != b)
                                        {
                                            size_t n = 0;
                                            while (n < a.size() && a[n] == b[n])
                                                ++n;
                                            throw std::runtime_error(
                                                family + " " + suffix + " mismatch size " + std::to_string(dims.first) +
                                                " scale " + std::to_string(scale) + " layout " +
                                                std::to_string(preset) + " half " + std::to_string(half) + " scene " +
                                                std::to_string(scenario) + " word " + std::to_string(n));
                                        }
                                        words += a.size();
                                        ++cases;
                                    }
                            }
                    std::cout << family << " " << suffix << " PASS\n" << std::flush;
                }
            }
        std::cout << "Host candidates bit-exact PASS " << cases << " cases " << words << " words\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
