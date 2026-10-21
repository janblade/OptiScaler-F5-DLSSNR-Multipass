#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "superior_resolve_fixture.h"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Spatial.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"
#include <functional>

// Resident synthetic stages only; excludes NR/DLSS inference and CPU setup.
void measure(LowFixture& f, std::ofstream& out, const char* stage, int scale, const std::function<void()>& baseline,
             const std::function<void()>& candidate)
{
    auto& r = f.r;
    for (unsigned block = 0; block < 8; ++block)
        for (unsigned quartet = 0; quartet < 48; ++quartet)
        {
            r.begin();
            r.list->SetComputeRootSignature(r.root.Get());
            ID3D12DescriptorHeap* heaps[] = { r.heap.Get() };
            r.list->SetDescriptorHeaps(1, heaps);
            for (unsigned position = 0; position < 4; ++position)
            {
                const bool changed = (position == 1 || position == 2) != (block % 2 != 0);
                r.list->EndQuery(f.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, position * 2);
                for (unsigned repeat = 0; repeat < 4; ++repeat)
                    (changed ? candidate : baseline)();
                r.list->EndQuery(f.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, position * 2 + 1);
            }
            r.list->ResolveQueryData(f.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 8, f.times.Get(), 0);
            r.finish();
            UINT64* stamps;
            check(f.times->Map(0, nullptr, reinterpret_cast<void**>(&stamps)));
            if (quartet >= 8)
                for (unsigned position = 0; position < 4; ++position)
                {
                    const bool changed = (position == 1 || position == 2) != (block % 2 != 0);
                    const double ms =
                        double(stamps[position * 2 + 1] - stamps[position * 2]) * 1000.0 / f.frequency / 4;
                    out << stage << ',' << scale << ',' << (changed ? "candidate" : "base") << ',' << block << ','
                        << quartet - 8 << ',' << position << ',' << ms << '\n';
                }
            f.times->Unmap(0, nullptr);
        }
    out.flush();
    std::cout << stage << " P" << scale << " complete\n" << std::flush;
}

int main(int argc, char** argv)
{
    try
    {
        const bool nativeTemporal = argc == 2 && std::string(argv[1]) == "--native-temporal";
        namespace Sp = DlssNr::Spatial;
        namespace Ss = DlssNr::SuperiorScaling;
        Runner r(true, 64);
        const std::string current = "OptiScaler/shaders/dlssnr/superior/precompile";
        auto carrier = r.pipeline(current, "dlssnr_superior_carrier");
        auto low = r.pipeline(current, "dlssnr_superior_low");
        auto rawLow = r.pipeline(current, "host_low_raw");
        auto copyShader = r.pipeline(current, "DlssNr");
        auto guides = r.pipeline("OptiScaler/shaders/dlssnr/precompile", "dlssnr_spatial_guides");
        auto legacy = r.pipeline("OptiScaler/shaders/dlssnr/precompile", "DlssNr");
        std::ofstream out(nativeTemporal ? "artifacts/validation/native-temporal-timing.csv"
                                         : "artifacts/validation/spatial-timing.csv");
        if (!out)
            throw std::runtime_error("CSV open failed");
        out << "stage,scale,variant,block,quartet,position,gpu_ms\n" << std::setprecision(9);
        Sp::Settings settings;
        settings.enabled = true;
        if (!nativeTemporal)
            for (int scale : { 65, 100 })
            {
                LowFixture f(r, 100);
                const auto layout = Sp::Build(settings, 3840, 2160, scale / 100.f, false);
                auto desc = f.work->GetDesc();
                desc.Width = layout.modelW;
                desc.Height = layout.modelH;
                desc.Format = DXGI_FORMAT_R32_FLOAT;
                auto depth = r.resource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                desc.Format = DXGI_FORMAT_R32G32_FLOAT;
                auto motion = r.resource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                // Smooth motion in render-pixel units; only the reference-to-frame scale changes.
                auto cpu = r.heap->GetCPUDescriptorHandleForHeapStart();
                D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
                u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                // Reuse the synthetic source for read-only depth/MV; a smooth float field exercises both curve
                // branches.
                for (unsigned i = 0; i < 6; ++i)
                {
                    auto* input = f.inputs[i == 1 ? 0 : 2].Get();
                    r.device->CreateShaderResourceView(input, nullptr, { cpu.ptr + SIZE_T(i) * r.stride });
                }
                u.Format = DXGI_FORMAT_R32_FLOAT;
                r.device->CreateUnorderedAccessView(depth.Get(), nullptr, &u, { cpu.ptr + 6 * r.stride });
                u.Format = DXGI_FORMAT_R32G32_FLOAT;
                r.device->CreateUnorderedAccessView(motion.Get(), nullptr, &u, { cpu.ptr + 7 * r.stride });
                const DlssNr::GuideRegions regions { { 0, 0, 1920, 1080 }, { 0, 0, 1920, 1080 } };
                const auto old = Sp::MakeConstants(layout, 101, regions, 1.f, -1.f);
                const auto fixed = Sp::MakeGuideConstants(layout, regions, { 1920, 1080 }, 1.f, -1.f);
                const auto constants = [&](const auto& c)
                {
                    auto cb = r.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                    void* p;
                    check(cb->Map(0, nullptr, &p));
                    std::memcpy(p, &c, 256);
                    cb->Unmap(0, nullptr);
                    return cb;
                };
                auto cbA = constants(old), cbB = constants(fixed);
                const auto draw = [&](ID3D12Resource* cb)
                {
                    r.list->SetComputeRootDescriptorTable(0, r.heap->GetGPUDescriptorHandleForHeapStart());
                    r.list->SetComputeRootConstantBufferView(1, cb->GetGPUVirtualAddress());
                    r.list->SetPipelineState(guides.Get());
                    r.list->Dispatch((layout.modelW + 7) / 8, (layout.modelH + 7) / 8, 1);
                    for (auto* resource : { depth.Get(), motion.Get() })
                    {
                        D3D12_RESOURCE_BARRIER b {};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        b.UAV.pResource = resource;
                        r.list->ResourceBarrier(1, &b);
                    }
                };
                measure(
                    f, out, "guide_pack", scale, [&] { draw(cbA.Get()); }, [&] { draw(cbB.Get()); });
            }
        for (int hdr : { -1, 0 })
            for (int scale : { 65, 100 })
            {
                if (nativeTemporal && scale != 100)
                    continue;
                ResolveFixture f(r, scale, carrier.Get(), hdr);
                // Packed/unpacked NR inputs are RGBA16F in production. Prepare
                // matching resident textures outside all measured windows.
                const auto halfDesc = f.images.work->GetDesc();
                auto halfProxy = r.resource(halfDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                auto halfAnswer = r.resource(halfDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                DlssNrConstants copy {}; copy.Mode=14;copy.Width=f.images.ww;copy.Height=f.images.wh;
                auto copyCb=f.cb(copy);
                f.table(0,f.images.inputs[0].Get(),f.images.inputs[0].Get(),halfProxy.Get());
                f.table(8,f.images.inputs[1].Get(),f.images.inputs[1].Get(),halfAnswer.Get());
                r.begin();f.bindRoots();
                f.dispatch(copyShader.Get(),copyCb.Get(),0,f.images.ww,f.images.wh);
                f.dispatch(copyShader.Get(),copyCb.Get(),8,f.images.ww,f.images.wh);
                r.barrier(halfProxy.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                r.barrier(halfAnswer.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                r.finish();
                const auto layout = Sp::Build(settings, 3840, 2160, scale / 100.f, false);
                auto before = nativeTemporal
                                  ? r.pipeline(current, hdr < 0 ? "host_resolve_raw_sdr" : "host_resolve_raw_hdr")
                              : scale == 100 ? legacy
                                             : r.pipeline("artifacts/validation/pre-spatial",
                                                          hdr < 0 ? "host_resolve_sdr" : "host_resolve_hdr");
                auto after = r.pipeline(current, scale == 100 && !nativeTemporal
                                                     ? (hdr < 0 ? "host_resolve_raw_sdr" : "host_resolve_raw_hdr")
                                                     : (hdr < 0 ? "host_resolve_sdr" : "host_resolve_hdr"));
                HostConstants h {};
                h.Mode = 1;
                h.Width = 3840;
                h.Height = 2160;
                h.WhitePoint = 1;
                h.ApplyModel = 1;
                h.Transfer = 11;
                h.Passthrough = hdr < 0;
                h.TransferStrength = h.ColourStrength = 1;
                h.MaxRatio = 2;
                h.SkinDetail = h.SkinColour = h.EnvironmentDetail = h.EnvironmentColour = 1;
                h.ModelWorkScale = scale / 100.f;
                h.GuideWidth = 1;
                h.MvScaleX = Ss::EffectiveBand(.950f, scale / 100.f, 1, 1);
                h.MvScaleY = Ss::EffectiveBand(.861f, scale / 100.f, 1, 1);
                const auto cb = [&](const HostConstants& host)
                {
                    DlssNrConstants bytes;
                    std::memcpy(&bytes, &host, 256);
                    return f.cb(bytes);
                };
                if (nativeTemporal)
                {
                    const float baselineScale = Ss::ResolutionScale(3840, 2160, layout.modelW, layout.modelH);
                    h.MvScaleX = Ss::EffectiveBand(.950f, baselineScale, 1, 1);
                    h.MvScaleY = Ss::EffectiveBand(.861f, baselineScale, 1, 1);
                    h.GuideHeight = 1u;
                }
                auto cbA = cb(h);
                const float finalScale = Ss::ResolutionScale(3840, 2160, layout.modelW, layout.modelH);
                h.MvScaleX = Ss::EffectiveBand(.950f, finalScale, 1, 1);
                h.MvScaleY = Ss::EffectiveBand(.861f, finalScale, 1, 1);
                h.GuideHeight = scale == 100 && !nativeTemporal ? 1u : 0u;
                auto cbB = cb(h);
                DlssNrConstants l {};
                l.Mode = 18;
                l.Transfer = scale == 100 && !nativeTemporal ? 0u : 1u;
                l.Width = (f.images.ww + 7) / 8;
                l.Height = (f.images.wh + 7) / 8;
                l.DirectResolveFlags = 65536;
                f.lowCb = f.cb(l);
                auto* answer=scale==100?halfAnswer.Get():f.edit.Get();
                f.table(48,halfProxy.Get(),answer,f.images.low.Get());
                f.table(56,halfProxy.Get(),answer,f.images.native.Get());
                if (nativeTemporal)
                {
                    f.table(0, halfProxy.Get(), f.edit.Get(), f.images.low.Get());
                    f.table(8, halfProxy.Get(), f.edit.Get(), f.images.native.Get());
                }
                const auto draw = [&](bool changed)
                {
                    if (nativeTemporal)
                    {
                        f.dispatch(changed ? low.Get() : rawLow.Get(), f.lowCb.Get(), changed ? 0u : 48u, l.Width,
                                   l.Height, 16);
                        r.barrier(f.images.low.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        f.dispatch(changed ? after.Get() : before.Get(), changed ? cbB.Get() : cbA.Get(),
                                   changed ? 8u : 56u, 3840, 2160);
                        r.barrier(f.images.low.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        D3D12_RESOURCE_BARRIER b {};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        b.UAV.pResource = f.images.native.Get();
                        r.list->ResourceBarrier(1, &b);
                        return;
                    }
                    if (scale != 100 || changed)
                    {
                        f.dispatch(scale == 100 ? rawLow.Get() : low.Get(), f.lowCb.Get(), 48, l.Width, l.Height, 16);
                        r.barrier(f.images.low.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    }
                    f.dispatch(changed ? after.Get() : before.Get(), changed ? cbB.Get() : cbA.Get(), 56, 3840, 2160);
                    if (scale != 100 || changed)
                        r.barrier(f.images.low.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    D3D12_RESOURCE_BARRIER b {};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    b.UAV.pResource = f.images.native.Get();
                    r.list->ResourceBarrier(1, &b);
                };
                measure(
                    f.images, out,
                    nativeTemporal ? (hdr < 0 ? "native_temporal_sdr" : "native_temporal_hdr0")
                                   : (hdr < 0 ? "final_sdr" : "final_hdr0"),
                    scale, [&] { draw(false); }, [&] { draw(true); });
            }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
