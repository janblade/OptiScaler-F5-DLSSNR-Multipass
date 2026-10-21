#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "packed_fixture.h"
#include "superior_input_fixture.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"

int main()
{
    try
    {
        Runner r(true);
        const std::string candidate = "OptiScaler/shaders/dlssnr/superior/precompile";
        const std::string reference = "artifacts/validation/host-shaders";
        auto hdr = r.pipeline(candidate, "host_resolve_packed_hdr"),
             sdr = r.pipeline(candidate, "host_resolve_packed_sdr");
        auto rhdr = r.pipeline(reference, "reference_packed_hdr"), rsdr = r.pipeline(reference, "reference_packed_sdr");
        unsigned cases = 0;
        size_t components = 0;
        double maxError = 0;
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
                        settings.centerX = 65;
                        settings.workX = 85;
                        settings.centerY = 70;
                        settings.workY = 95;
                        settings.offsetX = 7;
                        settings.offsetY = -5;
                        settings.shiftX = -2;
                        settings.shiftY = 1;
                    }
                    const auto layout = DlssNr::Spatial::Build(settings, dims.first, dims.second, scale, false);
                    if (!layout.active)
                        throw std::runtime_error("Inactive fixture layout: " + std::to_string(dims.first) + " scale " +
                                                 std::to_string(scale) + " preset " + std::to_string(preset) +
                                                 " status " + std::to_string(int(layout.status)));
                    auto warp = UploadWarp(r, layout);
                    for (bool half : { false, true })
                        for (unsigned curve = 0; curve < 8; ++curve)
                            for (unsigned scenario = 0; scenario < 8; ++scenario)
                            {
                                HostConstants h {};
                                h.Mode = 1;
                                h.Width = dims.first;
                                h.Height = dims.second;
                                h.WhitePoint = .4f;
                                h.ApplyModel = 1;
                                h.TransferStrength = 1;
                                h.ColourStrength = 1;
                                h.MaxRatio = 2;
                                h.Transfer = 1;
                                h.ReversibleMode = curve;
                                h.PreExposure = 1.7f;
                                h.ExposureTrim = 1.2f;
                                h.DebugView = scenario >= 4 ? scenario - 4 : 0;
                                h.InputEncoding = scenario == 4 ? 3 : scenario == 5 ? 4 : 0;
                                h.Passthrough = scenario == 0;
                                h.OriginalIsGameColour = 1;
                                h.UseGameExposure = scenario == 1;
                                h.UseExposureWhitePoint = scenario == 2;
                                h.ExposureTrimAnchorCount = scenario == 2 ? 2 : 0;
                                h.ExposureTrimAnchorExposure0 = .1f;
                                h.ExposureTrimAnchorTrim0 = .8f;
                                h.ExposureTrimAnchorExposure1 = 10;
                                h.ExposureTrimAnchorTrim1 = 1.6f;
                                h.CompareMode = scenario == 3 ? 1 : 0;
                                h.CompareZoom = 2;
                                h.CompareSplit = .37f;
                                h.SkinProtection = scenario == 3;
                                h.SkinDetail = .3f;
                                h.SkinColour = .4f;
                                h.EnvironmentDetail = h.EnvironmentColour = 1;
                                h.ModelWorkScale = scale;
                                h.GuideWidth = 1;
                                const float gainScale = DlssNr::SuperiorScaling::ResolutionScale(
                                    dims.first, dims.second, layout.modelW, layout.modelH);
                                h.MvScaleX = DlssNr::SuperiorScaling::EffectiveBand(.8f, gainScale, 1, 1);
                                h.MvScaleY = DlssNr::SuperiorScaling::EffectiveBand(.661f, gainScale, 1, 1);
                                DlssNrConstants bytes {};
                                std::memcpy(&bytes, &h, 256);
                                auto run = [&](ID3D12PipelineState* pso)
                                {
                                    return r.run(pso, dims.first, dims.second, dims.first, dims.second, bytes, scenario,
                                                 half, 8, layout.modelW, layout.modelH, nullptr, 3, nullptr, 0, 0, 8,
                                                 warp.Get(), half ? InputFixture::halfBits : nullptr);
                                };
                                const auto a = run(h.Passthrough ? rsdr.Get() : rhdr.Get());
                                const auto b = run(h.Passthrough ? sdr.Get() : hdr.Get());
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
                                                throw std::runtime_error("Nonfinite mismatch");
                                            continue;
                                        }
                                        const double error = std::abs(double(x) - y);
                                        maxError = std::max(maxError, error);
                                        const double tolerance = 0; // Independent reference must match bit for bit.
                                        if (error > tolerance || a[i] != b[i])
                                            throw std::runtime_error(
                                                "Packed guided mismatch: " + std::to_string(error) + " curve " +
                                                std::to_string(curve) + " size " + std::to_string(dims.first) +
                                                " scale " + std::to_string(scale) + " layout " +
                                                std::to_string(preset) + " half " + std::to_string(half) +
                                                " scenario " + std::to_string(scenario) + " word " + std::to_string(i) +
                                                " values " + std::to_string(x) + " / " + std::to_string(y));
                                        ++components;
                                    }
                                ++cases;
                            }
                }
        std::cout << "Packed guided host reference PASS: " << cases << " cases, " << components
                  << " components, max absolute error " << maxError << "\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
