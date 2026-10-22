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
        const std::string family = argc > 1 ? argv[1] : "fixed";
        const bool screen = argc > 2 && std::string(argv[2]) == "screen";
        const bool sdr65 = argc > 2 && std::string(argv[2]) == "sdr65";
        Runner r(true, 128);
        std::ofstream out("artifacts/validation/four-" + family + "-timing.csv");
        out << std::setprecision(9) << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        for (bool packed : { false, true })
            for (int curve : { -1, 0, 2, 4, 6 })
                for (int scale : { 50, 65, 100 })
                {
                    if (sdr65 && (packed || curve != -1 || scale != 65))
                        continue;
                    if (!packed && scale == 100)
                        continue;
                    if (family != "fixed" && family != "cached_fixed" && family != "final" && curve != 0 && curve != -1)
                        continue;
                    if (family.find("cached") == 0 && !packed)
                        continue;
                    PackedStages f(r, scale, curve);
                    auto& s = packed ? f.after : f.before;
                    ComPtr<ID3D12Resource> cache;
                    if (family.find("cached") == 0 || (family == "final" && packed))
                        cache = CachedWarp(r, f.layout);
                    const auto suffix =
                        std::string(packed ? "packed_" : "") + (curve < 0 ? "sdr" : "hdr" + std::to_string(curve));
                    auto base =
                        r.pipeline(CandidateDirectory, (family == "cached_fixed" ? "fixed_" : "base_") + suffix);
                    auto candidate = r.pipeline(family == "final" ? "OptiScaler/shaders/dlssnr/superior/precompile"
                                                                  : CandidateDirectory,
                                                family == "final" ? (packed ? "host_cached_" : "host_normal_") + suffix
                                                                  : family + "_" + suffix);
                    const UINT offset = packed ? 72 : 40;
                    f.table(104, s.proxy ? s.proxy.Get() : f.packedProxy.Get(), s.residual.Get(),
                            f.images.inputs[2].Get(), cache ? cache.Get() : f.warp.Get(), s.low.Get(),
                            f.images.native.Get());
                    r.begin();
                    f.roots();
                    if (!packed)
                    {
                        f.dispatch(f.spatial.Get(), f.unpackCb.Get(), 24, s.w, s.h);
                        r.barrier(s.proxy.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        r.barrier(s.answer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    }
                    f.dispatch(f.carrier.Get(), s.encodeCb.Get(), offset, s.w, s.h);
                    r.barrier(s.carrier.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    f.dispatch(f.carrier.Get(), s.decodeCb.Get(), offset + 8, s.w, s.h);
                    r.barrier(s.residual.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    f.dispatch(f.low.Get(), s.lowCb.Get(), offset + 16, (s.w + 7) / 8, (s.h + 7) / 8, 16);
                    r.barrier(s.low.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    r.finish();
                    auto draw = [&](ID3D12PipelineState* p, UINT table)
                    {
                        f.dispatch(p, s.resolveCb.Get(), table, 3840, 2160);
                        D3D12_RESOURCE_BARRIER b {};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        b.UAV.pResource = f.images.native.Get();
                        r.list->ResourceBarrier(1, &b);
                    };
                    if (!screen)
                    {
                        r.begin();
                        f.roots();
                        draw(base.Get(), offset + 24);
                        r.finish();
                        auto expected = ReadTexture(r, f.images.native.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        r.begin();
                        f.roots();
                        draw(candidate.Get(), 104);
                        r.finish();
                        if (expected != ReadTexture(r, f.images.native.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                            throw std::runtime_error("4K quality mismatch " + family + " " + suffix + " P" +
                                                     std::to_string(scale));
                    }
                    MeasureCandidate(
                        f.images, out, suffix.c_str(), scale, [&] { draw(base.Get(), offset + 24); },
                        [&] { draw(candidate.Get(), 104); }, screen);
                }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
