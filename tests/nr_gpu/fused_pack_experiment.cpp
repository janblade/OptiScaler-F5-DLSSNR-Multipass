#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "four_candidates_fixture.h"
#include "packed_stage_fixture.h"

int main(int argc, char** argv)
{
    try
    {
        Runner r(true, 128, 3);
        const bool timing = argc > 1 && std::string(argv[1]) == "timing";
        bool exact = true;
        auto fused = r.pipeline(CandidateDirectory, argc > 2 ? argv[2] : "fused_pack");
        std::ofstream out("artifacts/validation/four-fused-timing.csv");
        out << std::setprecision(9) << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        size_t words = 0;
        for (int preset = 0; preset < 3; ++preset)
            for (int scale : { 50, 65, 100 })
            {
                DlssNr::Spatial::Settings settings;
                settings.enabled = true;
                DlssNr::Spatial::ApplyNamedLayout(settings, preset);
                PackedStages f(r, scale, 0, &settings);
                f.table(104, f.images.inputs[0].Get(), f.images.inputs[2].Get(), f.images.inputs[2].Get(), nullptr,
                        nullptr, f.packedDepth.Get(), f.packedMotion.Get());
                D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
                u.Format = f.packedProxy->GetDesc().Format;
                u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                auto cpu = r.heap->GetCPUDescriptorHandleForHeapStart();
                r.device->CreateUnorderedAccessView(f.packedProxy.Get(), nullptr, &u, { cpu.ptr + 112 * r.stride });
                auto baseColour = ReadTexture(r, f.packedProxy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                auto baseDepth = ReadTexture(r, f.packedDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                auto baseMotion = ReadTexture(r, f.packedMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                auto states = [&](bool write)
                {
                    for (auto* t : { f.packedProxy.Get(), f.packedDepth.Get(), f.packedMotion.Get() })
                        r.barrier(t,
                                  write ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                        : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  write ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                };
                auto draw = [&](bool combined)
                {
                    states(true);
                    if (combined)
                        f.dispatch(fused.Get(), f.packedGuidesCb.Get(), 104, f.layout.modelW, f.layout.modelH);
                    else
                    {
                        f.dispatch(f.spatial.Get(), f.packCb.Get(), 0, f.layout.modelW, f.layout.modelH);
                        f.dispatch(f.guides.Get(), f.packedGuidesCb.Get(), 16, f.layout.modelW, f.layout.modelH);
                    }
                    states(false);
                };
                r.begin();
                f.roots();
                draw(true);
                r.finish();
                auto verify = [&](const std::vector<uint32_t>& expected, ID3D12Resource* target, const char* name)
                {
                    auto actual = ReadTexture(r, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    size_t wrong = 0, first = 0;
                    for (size_t i = 0; i < expected.size(); ++i)
                        if (expected[i] != actual[i])
                        {
                            if (!wrong)
                                first = i;
                            ++wrong;
                        }
                    if (wrong)
                    {
                        exact = false;
                        const std::string message = std::string("Fused ") + name + " differs " + std::to_string(wrong) +
                                                    " words first " + std::to_string(first) + " bits " +
                                                    std::to_string(expected[first]) + "/" +
                                                    std::to_string(actual[first]) + " preset " +
                                                    std::to_string(preset) + " scale " + std::to_string(scale);
                        std::cout << "REJECT quality: " << message << '\n' << std::flush;
                    }
                };
                verify(baseColour, f.packedProxy.Get(), "colour");
                verify(baseDepth, f.packedDepth.Get(), "depth");
                verify(baseMotion, f.packedMotion.Get(), "motion");
                words += baseColour.size() + baseDepth.size() + baseMotion.size();
                std::cout << "Fused comparison P" << scale << " layout " << preset << '\n' << std::flush;
                if (timing)
                    MeasureCandidate(
                        f.images, out, ("pack_layout" + std::to_string(preset)).c_str(), scale, [&] { draw(false); },
                        [&] { draw(true); });
            }
        std::cout << "Fused bit-exact " << (exact ? "PASS" : "FAIL") << ' ' << words << " words\n";
        if (!exact)
            return 2;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
