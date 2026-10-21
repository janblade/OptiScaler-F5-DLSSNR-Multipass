#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "packed_fixture.h"
#include "stage_timing.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"

// Compares 389e8d96's ordinary-grid route with packed stabilization + direct guided reconstruction.
// Private DLSS inference is replaced by identity carrier decode in BOTH variants. No full-game result is claimed.
struct PackedStages
{
    Runner& r;
    LowFixture images;
    DlssNr::Spatial::Layout layout;
    ComPtr<ID3D12Resource> warp, packedProxy, packedAnswer, packedDepth, packedMotion;
    struct Stage
    {
        ComPtr<ID3D12Resource> proxy, answer, depth, motion, carrier, temporalMotion, residual, low;
        ComPtr<ID3D12Resource> guidesCb, encodeCb, decodeCb, lowCb, resolveCb;
        UINT w = 0, h = 0;
    };
    Stage before, after;
    ComPtr<ID3D12Resource> packCb, unpackCb, packedGuidesCb;
    ComPtr<ID3D12PipelineState> spatial, guides, standard, carrier, low, resolve, packedResolve;
    PackedStages(Runner& runner, int scale, int hdr) : r(runner), images(r, 100, true)
    {
        namespace Sp = DlssNr::Spatial;
        namespace Ss = DlssNr::SuperiorScaling;
        Sp::Settings settings;
        settings.enabled = true;
        layout = Sp::Build(settings, 3840, 2160, scale / 100.f, false);
        warp = UploadWarp(r, layout);
        const std::string dir = "OptiScaler/shaders/dlssnr/";
        spatial = r.pipeline(dir + "precompile", "dlssnr_spatial");
        guides = r.pipeline(dir + "precompile", "dlssnr_spatial_guides");
        standard = r.pipeline(dir + "superior/precompile", "DlssNr");
        carrier = r.pipeline(dir + "superior/precompile", "dlssnr_superior_carrier");
        low = r.pipeline(dir + "superior/precompile", "dlssnr_superior_low");
        resolve = r.pipeline(dir + "superior/precompile", hdr < 0 ? "host_resolve_sdr" : "host_resolve_hdr");
        packedResolve =
            r.pipeline(dir + "superior/precompile", hdr < 0 ? "host_resolve_packed_sdr" : "host_resolve_packed_hdr");
        packedProxy = texture(layout.modelW, layout.modelH);
        packedAnswer = texture(layout.modelW, layout.modelH);
        packedDepth = texture(layout.modelW, layout.modelH, DXGI_FORMAT_R32_FLOAT);
        packedMotion = texture(layout.modelW, layout.modelH, DXGI_FORMAT_R32G32_FLOAT);
        before.w = layout.ordinaryW;
        before.h = layout.ordinaryH;
        after.w = layout.modelW;
        after.h = layout.modelH;
        for (auto* s : { &before, &after })
        {
            s->carrier = texture(s->w, s->h);
            s->residual = texture(s->w, s->h);
            s->temporalMotion = texture(s->w, s->h, DXGI_FORMAT_R32G32_FLOAT);
            s->low = texture((s->w + 7) / 8, (s->h + 7) / 8);
            DlssNrConstants c {};
            c.Mode = 39;
            c.Width = s->w;
            c.Height = s->h;
            c.TransferStrength = float(s == &before ? 3840 : s->w);
            c.ColourStrength = float(s == &before ? 2160 : s->h);
            c.MvScaleX = s == &before ? s->w / 3840.f : 1.f;
            c.MvScaleY = s == &before ? s->h / 2160.f : 1.f;
            s->encodeCb = cb(c);
            c = {};
            c.Mode = 38;
            c.Width = s->w;
            c.Height = s->h;
            s->decodeCb = cb(c);
            c = {};
            c.Mode = 18;
            c.Transfer = 1;
            c.DirectResolveFlags = 65536;
            c.Width = (s->w + 7) / 8;
            c.Height = (s->h + 7) / 8;
            s->lowCb = cb(c);
            HostConstants h {};
            h.Mode = 1;
            h.Width = 3840;
            h.Height = 2160;
            h.WhitePoint = 1;
            h.TransferStrength = h.ColourStrength = 1;
            h.MaxRatio = 2;
            h.ApplyModel = 1;
            h.Passthrough = hdr < 0;
            h.ReversibleMode = hdr < 0 ? 0 : UINT(hdr);
            h.Transfer = 1;
            h.ModelWorkScale = scale / 100.f;
            h.GuideWidth = 1;
            h.SkinDetail = h.SkinColour = h.EnvironmentDetail = h.EnvironmentColour = 1;
            const float gain = Ss::ResolutionScale(3840, 2160, layout.modelW, layout.modelH);
            h.MvScaleX = Ss::EffectiveBand(.950f, gain, 1, 1);
            h.MvScaleY = Ss::EffectiveBand(.861f, gain, 1, 1);
            s->resolveCb = cb(h);
        }
        before.proxy = texture(before.w, before.h);
        before.answer = texture(before.w, before.h);
        before.depth = texture(before.w, before.h, DXGI_FORMAT_R32_FLOAT);
        before.motion = texture(before.w, before.h, DXGI_FORMAT_R32G32_FLOAT);
        const DlssNr::GuideRegions regions { { 0, 0, 3840, 2160 }, { 0, 0, 3840, 2160 } };
        packCb = cb(Sp::MakeConstants(layout, 100, regions, 1, 1));
        unpackCb = cb(Sp::MakeConstants(layout, 102, regions, 1, 1));
        packedGuidesCb = cb(Sp::MakeGuideConstants(layout, regions, { 3840, 2160 }, 1, 1));
        DlssNrConstants c {};
        c.Mode = 10;
        c.Width = before.w;
        c.Height = before.h;
        c.GuideWidth = 3840;
        c.GuideHeight = 2160;
        c.TransferStrength = 3840;
        c.ColourStrength = 2160;
        c.MvScaleX = c.MvScaleY = 1;
        before.guidesCb = cb(c);
        table(0, images.inputs[0].Get(), nullptr, nullptr, nullptr, nullptr, packedProxy.Get());
        table(8, images.inputs[1].Get(), nullptr, nullptr, nullptr, nullptr, packedAnswer.Get());
        table(16, images.inputs[2].Get(), images.inputs[2].Get(), images.inputs[2].Get(), nullptr, nullptr,
              packedDepth.Get(), packedMotion.Get());
        table(24, packedProxy.Get(), packedAnswer.Get(), nullptr, nullptr, nullptr, before.proxy.Get(),
              before.answer.Get());
        table(32, images.inputs[2].Get(), images.inputs[2].Get(), nullptr, nullptr, nullptr, before.depth.Get(),
              before.motion.Get());
        table(40, before.proxy.Get(), before.answer.Get(), images.inputs[2].Get(), nullptr, nullptr,
              before.carrier.Get(), before.temporalMotion.Get());
        table(48, before.carrier.Get(), nullptr, nullptr, nullptr, nullptr, before.residual.Get());
        table(56, before.proxy.Get(), before.residual.Get(), nullptr, nullptr, nullptr, before.low.Get());
        table(64, before.proxy.Get(), before.residual.Get(), images.inputs[2].Get(), nullptr, before.low.Get(),
              images.native.Get());
        table(72, packedProxy.Get(), packedAnswer.Get(), packedMotion.Get(), nullptr, nullptr, after.carrier.Get(),
              after.temporalMotion.Get());
        table(80, after.carrier.Get(), nullptr, nullptr, nullptr, nullptr, after.residual.Get());
        table(88, packedProxy.Get(), after.residual.Get(), nullptr, nullptr, nullptr, after.low.Get());
        table(96, packedProxy.Get(), after.residual.Get(), images.inputs[2].Get(), warp.Get(), after.low.Get(),
              images.native.Get());
        r.begin();
        roots();
        dispatch(spatial.Get(), packCb.Get(), 0, layout.modelW, layout.modelH);
        dispatch(spatial.Get(), packCb.Get(), 8, layout.modelW, layout.modelH);
        dispatch(guides.Get(), packedGuidesCb.Get(), 16, layout.modelW, layout.modelH);
        for (auto* t : { packedProxy.Get(), packedAnswer.Get(), packedDepth.Get(), packedMotion.Get() })
            r.barrier(t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        r.finish();
    }
    ComPtr<ID3D12Resource> texture(UINT w, UINT h, DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        auto d = images.work->GetDesc();
        d.Width = w;
        d.Height = h;
        d.Format = format;
        return r.resource(d, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    template <class T> ComPtr<ID3D12Resource> cb(const T& c)
    {
        static_assert(sizeof(c) == 256);
        auto b = r.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p;
        check(b->Map(0, nullptr, &p));
        std::memcpy(p, &c, 256);
        b->Unmap(0, nullptr);
        return b;
    }
    void table(UINT offset, ID3D12Resource* source, ID3D12Resource* model, ID3D12Resource* original,
               ID3D12Resource* motion, ID3D12Resource* lowField, ID3D12Resource* target, ID3D12Resource* keep = nullptr)
    {
        auto cpu = r.heap->GetCPUDescriptorHandleForHeapStart();
        ID3D12Resource* srvs[] = {
            source, model ? model : source,      original ? original : source, motion ? motion : source,
            source, lowField ? lowField : source
        };
        for (UINT i = 0; i < 6; ++i)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d {};
            d.Format = srvs[i]->GetDesc().Format;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Texture2D.MipLevels = 1;
            r.device->CreateShaderResourceView(srvs[i], &d, { cpu.ptr + SIZE_T(offset + i) * r.stride });
        }
        for (UINT i = 6; i < 8; ++i)
        {
            auto* t = i == 7 && keep ? keep : target;
            D3D12_UNORDERED_ACCESS_VIEW_DESC d {};
            d.Format = t->GetDesc().Format;
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            r.device->CreateUnorderedAccessView(t, nullptr, &d, { cpu.ptr + SIZE_T(offset + i) * r.stride });
        }
    }
    void roots()
    {
        r.list->SetComputeRootSignature(r.root.Get());
        ID3D12DescriptorHeap* heaps[] = { r.heap.Get() };
        r.list->SetDescriptorHeaps(1, heaps);
    }
    void dispatch(ID3D12PipelineState* p, ID3D12Resource* constants, UINT offset, UINT w, UINT h, UINT groupY = 8)
    {
        auto gpu = r.heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(offset) * r.stride;
        r.list->SetPipelineState(p);
        r.list->SetComputeRootDescriptorTable(0, gpu);
        r.list->SetComputeRootConstantBufferView(1, constants->GetGPUVirtualAddress());
        r.list->Dispatch((w + 7) / 8, (h + groupY - 1) / groupY, 1);
    }
    void draw(bool candidate)
    {
        auto& s = candidate ? after : before;
        const UINT offset = candidate ? 72 : 40;
        std::vector<ID3D12Resource*> written;
        auto readable = [&](ID3D12Resource* t)
        {
            r.barrier(t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            written.push_back(t);
        };
        if (!candidate)
        {
            dispatch(spatial.Get(), unpackCb.Get(), 24, s.w, s.h);
            readable(s.proxy.Get());
            readable(s.answer.Get());
            dispatch(standard.Get(), s.guidesCb.Get(), 32, s.w, s.h);
            readable(s.depth.Get());
            readable(s.motion.Get());
        }
        dispatch(carrier.Get(), s.encodeCb.Get(), offset, s.w, s.h);
        readable(s.carrier.Get());
        readable(s.temporalMotion.Get());
        // The only substitute for proprietary inference: identity output, then the actual carrier decoder.
        dispatch(carrier.Get(), s.decodeCb.Get(), offset + 8, s.w, s.h);
        readable(s.residual.Get());
        dispatch(low.Get(), s.lowCb.Get(), offset + 16, (s.w + 7) / 8, (s.h + 7) / 8, 16);
        readable(s.low.Get());
        dispatch(candidate ? packedResolve.Get() : resolve.Get(), s.resolveCb.Get(), offset + 24, 3840, 2160);
        for (auto* t : written)
            r.barrier(t, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = images.native.Get();
        r.list->ResourceBarrier(1, &b);
    }
};
int main()
{
    try
    {
        Runner r(true, 112);
        std::ofstream out("artifacts/validation/packed-stage-timing.csv");
        out << std::setprecision(9);
        out << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        for (int hdr : { -1, 0 })
            for (int scale : { 65, 100 })
            {
                PackedStages f(r, scale, hdr);
                measure(
                    f.images, out, hdr < 0 ? "packed_route_sdr" : "packed_route_hdr0", scale, [&] { f.draw(false); },
                    [&] { f.draw(true); });
            }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
