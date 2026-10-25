#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "interpass_fixture.h"

int main()
{
    try
    {
        Runner r(true);
        const std::string shaders = "OptiScaler/shaders/dlssnr/";
        auto legacy = r.pipeline("artifacts/validation/host-shaders", "upstream_recompiled");
        auto recompiled = r.pipeline("artifacts/validation/host-shaders", "legacy_recompiled");
        auto referenceHdr = r.pipeline("artifacts/validation/host-shaders", "reference_hdr");
        auto referenceSdr = r.pipeline("artifacts/validation/host-shaders", "reference_sdr");
        auto candidateHdr = r.pipeline(shaders + "superior/precompile", "host_resolve_hdr");
        auto candidateSdr = r.pipeline(shaders + "superior/precompile", "host_resolve_sdr");
        auto referenceRawHdr = r.pipeline("artifacts/validation/host-shaders", "reference_raw_hdr");
        auto referenceRawSdr = r.pipeline("artifacts/validation/host-shaders", "reference_raw_sdr");
        auto candidateRawHdr = r.pipeline(shaders + "superior/precompile", "host_resolve_raw");
        auto candidateRawSdr = r.pipeline(shaders + "superior/precompile", "host_resolve_raw");
        size_t words = 0;
        unsigned cases = 0;
        for (const auto dims : { std::pair<UINT, UINT> { 1, 1 }, { 17, 11 }, { 65, 37 } })
            for (bool half : { false, true })
                for (unsigned curve = 0; curve < 8; ++curve)
                    for (unsigned scenario = 0; scenario < 8; ++scenario)
                        for (unsigned path = 0; path < 3; ++path)
                        {
                            const bool rawAnswer = path == 1u;
                            const bool nativeGrid = path != 0u;
                            HostConstants h {};
                            h.Mode = 1;
                            DlssNr::SuperiorScaling::SetGuidedRange(h, .5f);
                            h.Width = dims.first;
                            h.Height = dims.second;
                            h.WhitePoint = scenario % 2 ? 0.2f : 5.0f;
                            h.TransferStrength = 1.2f;
                            h.ColourStrength = 0.8f;
                            h.MaxRatio = 2.0f;
                            h.ApplyModel = scenario == 0 ? 0u : 1u;
                            h.Passthrough = scenario == 1;
                            h.ReversibleMode = curve;
                            h.PreExposure = 1.7f;
                            h.ExposurePreMul = 2.0f;
                            h.ExposureTrim = 1.4f;
                            h.UseGameExposure = scenario == 2;
                            h.UseExposureWhitePoint = scenario == 3;
                            h.ExposureBaseScale = 1.3f;
                            h.ExposureTrimAnchorCount = scenario == 3 ? 2u : 0u;
                            h.ExposureTrimAnchorExposure0 = 0.1f;
                            h.ExposureTrimAnchorTrim0 = 0.8f;
                            h.ExposureTrimAnchorExposure1 = 10.0f;
                            h.ExposureTrimAnchorTrim1 = 1.6f;
                            h.InputEncoding = scenario == 4 ? 3u : scenario == 5 ? 4u : 0u;
                            h.OriginalIsGameColour = 1u;
                            h.CompareMode = scenario == 6 ? 1u : 0u;
                            h.CompareZoom = 1;
                            h.CompareSplit = 0.5f;
                            h.SkinProtection = scenario == 7;
                            h.SkinDetail = 0.3f;
                            h.SkinColour = 0.4f;
                            h.EnvironmentDetail = h.EnvironmentColour = 1;
                            h.ModelWorkScale = nativeGrid ? 1.f : .65f;
                            h.GuideHeight = rawAnswer ? 1u : 0u;
                            h.Transfer = 1;
                            h.GuideWidth = 1;
                            h.MvScaleX = 0.8f;
                            h.MvScaleY = 0.661f;
                            DlssNrConstants bytes {};
                            static_assert(sizeof(bytes) == sizeof(h));
                            std::memcpy(&bytes, &h, sizeof(h));
                            const UINT sw = nativeGrid ? dims.first : std::max(1u, dims.first * 65u / 100u),
                                       sh = nativeGrid ? dims.second : std::max(1u, dims.second * 65u / 100u);
                            auto compare = [&](ID3D12PipelineState* a, ID3D12PipelineState* b, const char* name)
                            {
                                const auto x = r.run(a, dims.first, dims.second, dims.first, dims.second, bytes, 0,
                                                     half, 8, sw, sh, nullptr, 2);
                                const auto y = r.run(b, dims.first, dims.second, dims.first, dims.second, bytes, 0,
                                                     half, 8, sw, sh, nullptr, 2);
                                if (x != y)
                                {
                                    for (size_t i = 0; i < x.size(); ++i)
                                        if (x[i] != y[i])
                                        {
                                            std::cout << "First differing word: " << i << " " << std::hex << x[i] << " "
                                                      << y[i] << std::dec << '\n';
                                            break;
                                        }
                                    throw std::runtime_error(
                                        std::string(name) + " mismatch: curve " + std::to_string(curve) + " scenario " +
                                        std::to_string(scenario) + " size " + std::to_string(dims.first));
                                }
                                words += x.size();
                                ++cases;
                            };
                            compare(legacy.Get(), recompiled.Get(), "Unmodified janblade resolve");
                            compare(rawAnswer ? (h.Passthrough ? referenceRawSdr.Get() : referenceRawHdr.Get())
                                              : (h.Passthrough ? referenceSdr.Get() : referenceHdr.Get()),
                                    rawAnswer ? (h.Passthrough ? candidateRawSdr.Get() : candidateRawHdr.Get())
                                              : (h.Passthrough ? candidateSdr.Get() : candidateHdr.Get()),
                                    "v59 estimator in janblade composition");
                    }
        std::cout << "Host composition GPU bit parity PASS: " << cases << " cases, " << words << " words\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
