#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "packed_stage_fixture.h"

// Only host resolve changed. Resources, input residual, Low64 and coefficients are identical for A/B.
// NR and private DLSS inference, metadata, packing, guide/low preparation and CPU setup are outside timestamps.
int main()
{
    try
    {
        Runner r(true, 112);
        std::ofstream out("artifacts/validation/host-opt-timing.csv");
        out << std::setprecision(9) << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        for (bool packed : { false, true })
            for (int hdr : { -1, 0 })
                for (int scale : { 65, 100 })
                {
                    if (!packed && scale == 100)
                        continue; // ordinary native NR has no Superior temporal resolve
                    PackedStages f(r, scale, hdr);
                    auto& s = packed ? f.after : f.before;
                    const UINT offset = packed ? 72 : 40;
                    const std::string variant = std::string(packed ? "packed_" : "") + (hdr < 0 ? "sdr" : "hdr");
                    auto baseline = r.pipeline("artifacts/validation/pre-host-opt", "host_resolve_" + variant);
                    auto* candidate = packed ? f.packedResolve.Get() : f.resolve.Get();
                    if (!packed)
                    {
                        HostConstants h {};
                        h.Mode = 1;
                        h.Width = 3840;
                        h.Height = 2160;
                        h.WhitePoint = 1;
                        h.TransferStrength = h.ColourStrength = 1;
                        h.MaxRatio = 2;
                        h.ApplyModel = 1;
                        h.Passthrough = hdr < 0;
                        h.ReversibleMode = hdr < 0 ? 0 : unsigned(hdr);
                        h.Transfer = 1;
                        h.ModelWorkScale = scale / 100.f;
                        h.GuideWidth = 1;
                        h.SkinDetail = h.SkinColour = h.EnvironmentDetail = h.EnvironmentColour = 1;
                        const float gain = DlssNr::SuperiorScaling::ResolutionScale(3840, 2160, s.w, s.h);
                        h.MvScaleX = DlssNr::SuperiorScaling::EffectiveBand(.950f, gain, 1, 1);
                        h.MvScaleY = DlssNr::SuperiorScaling::EffectiveBand(.861f, gain, 1, 1);
                        s.resolveCb = f.cb(h);
                    }
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
                    const auto draw = [&](ID3D12PipelineState* pso)
                    {
                        f.dispatch(pso, s.resolveCb.Get(), offset + 24, 3840, 2160);
                        D3D12_RESOURCE_BARRIER b {};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        b.UAV.pResource = f.images.native.Get();
                        r.list->ResourceBarrier(1, &b);
                    };
                    const std::string name = std::string(packed ? "packed_" : "ordinary_") + (hdr < 0 ? "sdr" : "hdr0");
                    measure(
                        f.images, out, name.c_str(), scale, [&] { draw(baseline.Get()); }, [&] { draw(candidate); });
                }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
