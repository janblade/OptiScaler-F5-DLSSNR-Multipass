#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "four_candidates_fixture.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"

int main()
{
    try
    {
        Runner r(true);
        const std::string dir = "OptiScaler/shaders/dlssnr/superior/precompile";
        const std::string ref = "artifacts/validation/refinement-reference";
        unsigned cases = 0;
        size_t components = 0;
        double maximum = 0;
        for (unsigned percent : { 50u, 65u, 70u, 88u, 100u })
            for (bool packed : { false, true })
                for (bool cache : { false, true })
                    for (bool half : { false, true })
                        for (unsigned curve : { 0u, 2u, 4u, 6u })
                            for (unsigned scenario = 0; scenario < 3; ++scenario)
                            {
                                if (cache && !packed)
                                    continue;
                                constexpr UINT W = 129, H = 97;
                                DlssNr::Spatial::Settings settings;
                                settings.enabled = true;
                                // At small native fixtures the production 100% layout can round to an invalid thin
                                // edge.
                                settings.centerX = settings.centerY = 65;
                                const auto layout = DlssNr::Spatial::Build(settings, W, H, percent / 100.f, false);
                                if (!layout.active)
                                    throw std::runtime_error("Inactive reference layout");
                                const bool sdr = scenario == 0;
                                auto warp = cache ? CachedWarp(r, layout) : UploadWarp(r, layout);
                                const std::string suffix = std::string(packed ? "packed_" : "") + (sdr ? "sdr" : "hdr");
                                auto reference = r.pipeline(ref, "literal_" + std::to_string(percent) + "_" + suffix);
                                const std::string candidateName =
                                    cache && (!sdr || curve == 0) ? std::string("host_cached_packed_") +
                                                                        (sdr ? "sdr" : "hdr" + std::to_string(curve))
                                                                  : "host_resolve_" + suffix;
                                auto candidate = r.pipeline(dir, candidateName);
                                HostConstants h {};
                                h.Mode = 1;
                                h.Width = W;
                                h.Height = H;
                                h.WhitePoint = .4f;
                                h.ApplyModel = 1;
                                h.TransferStrength = h.ColourStrength = 1;
                                h.MaxRatio = 2;
                                h.Transfer = 11;
                                h.Passthrough = sdr;
                                h.ReversibleMode = curve;
                                h.PreExposure = 1.7f;
                                h.ExposureTrim = 1.2f;
                                h.ExposureBaseScale = 1.1f;
                                h.SkinDetail = h.SkinColour = h.EnvironmentDetail = h.EnvironmentColour = 1;
                                h.ModelWorkScale = percent / 100.f;
                                h.MvScaleX = h.MvScaleY = 1;
                                h.UseGameExposure = scenario == 1;
                                h.UseExposureWhitePoint = scenario == 2;
                                h.OriginalIsGameColour = 1;
                                DlssNr::SuperiorScaling::SetGuidedRange(h, percent / 100.f);
                                const auto run = [&](ID3D12PipelineState* pso, ID3D12Resource* metadata)
                                {
                                    DlssNrConstants bytes {};
                                    std::memcpy(&bytes, &h, 256);
                                    return r.run(pso, W, H, W, H, bytes, scenario, half, 8, layout.modelW,
                                                 layout.modelH, nullptr, 3, nullptr, 0, 0, 8, metadata);
                                };
                                // Generic literal reference reads arithmetic metadata, never the cached LUT rows.
                                auto arithmetic = UploadWarp(r, layout);
                                const auto a = run(reference.Get(), packed ? arithmetic.Get() : nullptr);
                                const auto b = run(candidate.Get(), packed ? warp.Get() : nullptr);
                                for (size_t i = 0; i < a.size(); ++i)
                                    for (unsigned part = 0; part < (half ? 2u : 1u); ++part)
                                    {
                                        const float x =
                                            half ? TestHalf((a[i] >> (part * 16)) & 65535) : TestFloat(a[i]);
                                        const float y =
                                            half ? TestHalf((b[i] >> (part * 16)) & 65535) : TestFloat(b[i]);
                                        if (!std::isfinite(x) || !std::isfinite(y))
                                        {
                                            if (a[i] != b[i])
                                                throw std::runtime_error("Nonfinite reference mismatch");
                                            continue;
                                        }
                                        const double error = std::abs(double(x) - y);
                                        maximum = std::max(maximum, error);
                                        if (error > (half ? std::max(.0000611, std::abs(double(x)) * .001)
                                                          : 2e-5 * std::max(1.0, std::abs(double(x)))))
                                            throw std::runtime_error(
                                                "Literal cubic-sigma reference mismatch: " + candidateName + " P" +
                                                std::to_string(percent) + " curve " + std::to_string(curve));
                                        ++components;
                                    }
                                ++cases;
                            }
        std::cout << "Cubic guided PASS: " << cases << " cases / " << components << " components; maximum error "
                  << maximum
                  << "; literal 50/65/70/88/100%, SDR/HDR, game/automatic exposure, packed/uniform/cached, FP16/FP32\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
