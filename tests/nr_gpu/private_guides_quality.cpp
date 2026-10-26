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
        Runner r(true, 64, 3);
        const std::string dir = "OptiScaler/shaders/dlssnr/superior/precompile";
        auto joint = r.pipeline(dir, "host_uniform_joint_guides"), fused = r.pipeline(dir, "host_private_guides"),
             carrier = r.pipeline(dir, "dlssnr_superior_carrier");
        auto texture = [&](DXGI_FORMAT fmt, UINT w, UINT h)
        {
            D3D12_RESOURCE_DESC d {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = w;
            d.Height = h;
            d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
            d.Format = fmt;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return r.resource(d, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        auto upload = [&](DXGI_FORMAT fmt, UINT w, UINT h, UINT pixelBytes, const void* source)
        {
            auto t = texture(fmt, w, h);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
            UINT64 bytes;
            const auto desc = t->GetDesc();
            r.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
            auto b = r.buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            char* p;
            check(b->Map(0, nullptr, reinterpret_cast<void**>(&p)));
            for (UINT y = 0; y < h; ++y)
                std::memcpy(p + fp.Footprint.RowPitch * y,
                            static_cast<const char*>(source) + size_t(y) * w * pixelBytes, size_t(w) * pixelBytes);
            b->Unmap(0, nullptr);
            r.begin();
            r.barrier(t.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION a {}, z {};
            a.pResource = b.Get();
            a.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            a.PlacedFootprint = fp;
            z.pResource = t.Get();
            z.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            r.list->CopyTextureRegion(&z, 0, 0, 0, &a, nullptr);
            r.barrier(t.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            r.finish();
            return t;
        };
        auto cb = [&](const auto& c)
        {
            auto b = r.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* p;
            check(b->Map(0, nullptr, &p));
            std::memcpy(p, &c, 256);
            b->Unmap(0, nullptr);
            return b;
        };
        auto table = [&](UINT offset, std::array<ID3D12Resource*, 6> inputs, std::array<ID3D12Resource*, 3> outputs)
        {
            auto p = r.heap->GetCPUDescriptorHandleForHeapStart();
            p.ptr += SIZE_T(offset) * r.stride;
            for (auto* t : inputs)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC d {};
                d.Format = t->GetDesc().Format;
                d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                d.Texture2D.MipLevels = 1;
                r.device->CreateShaderResourceView(t, &d, p);
                p.ptr += r.stride;
            }
            for (auto* t : outputs)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC d {};
                d.Format = t->GetDesc().Format;
                d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                r.device->CreateUnorderedAccessView(t, nullptr, &d, p);
                p.ptr += r.stride;
            }
        };
        unsigned cases = 0;
        size_t words = 0;
        auto quantJoint = r.pipeline(dir, "host_uniform_joint_guides_quantized");
        auto quantFused = r.pipeline(dir, "host_private_guides_quantized");
        for (bool quantized : { false, true })
            for (auto dim : { std::pair<UINT, UINT> { 64, 48 }, { 129, 97 }, { 3840, 32 }, { 16384, 8 } })
                for (float scale : { .5f, .75f })
                    for (bool inverted : { false, true })
                        for (bool hold : { false, true })
                            for (unsigned pattern = 0; pattern < 6; ++pattern)
                            {
                                const UINT W = dim.first, H = dim.second, sw = UINT(W * scale), sh = UINT(H * scale),
                                           aw = sw + 13, ah = sh + 11;
                                constexpr UINT dx = 3, dy = 5, mx = 7, my = 2;
                                std::vector<float> depth(size_t(aw) * ah, 12345), motion(size_t(aw) * ah * 2, 12345);
                                for (UINT y = 0; y < sh; ++y)
                                    for (UINT x = 0; x < sw; ++x)
                                    {
                                        float z =
                                            pattern == 2 ? (x > sw / 2 ? .2f : .7f) : .2f + x * .0001f + y * .0002f;
                                        if (inverted)
                                            z = 1 - z;
                                        depth[size_t(y + dy) * aw + x + dx] =
                                            pattern == 3 && (x + y) % 17 == 0 ? NAN : z;
                                        const size_t i = (size_t(y + my) * aw + x + mx) * 2;
                                        motion[i] = pattern == 2 ? (x > sw / 2 ? 12.f : -2.f) : 4.f + x * .01f;
                                        motion[i + 1] = pattern == 3 && (x + y) % 13 == 0 ? INFINITY : -2.f + y * .01f;
                                        if (pattern >= 4)
                                        {
                                            const uint32_t special[] { 1u, 0x80000000u, 0x7f7fffffu, 0xff7fffffu };
                                            std::memcpy(&motion[i], &special[(x + y) % 4], 4);
                                            std::memcpy(&motion[i + 1], &special[(x + y + 1) % 4], 4);
                                            if (pattern == 5)
                                                std::memcpy(&depth[size_t(y + dy) * aw + x + dx], &special[(x + y) % 4],
                                                            4);
                                        }
                                    }
                                std::vector<uint16_t> proxy(size_t(W) * H * 4), answer(proxy.size());
                                for (size_t i = 0; i < proxy.size(); ++i)
                                {
                                    const float a = float(int(i * 17 % 319) - 80) / 128.f;
                                    proxy[i] = InputFixture::halfBits(pattern == 3 && i % 97 == 0 ? NAN : a);
                                    answer[i] =
                                        InputFixture::halfBits(pattern == 3 && i % 89 == 0 ? INFINITY
                                                               : pattern == 5              ? -a * 8.f
                                                                              : a + float(int(i % 7) - 3) * .02f);
                                }
                                auto d = upload(DXGI_FORMAT_R32_FLOAT, aw, ah, 4, depth.data()),
                                     m = upload(DXGI_FORMAT_R32G32_FLOAT, aw, ah, 8, motion.data());
                                auto p = upload(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 8, proxy.data()),
                                     a = upload(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 8, answer.data());
                                auto od = texture(DXGI_FORMAT_R32_FLOAT, W, H),
                                     raw = texture(DXGI_FORMAT_R32G32_FLOAT, W, H),
                                     oc = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H),
                                     om = texture(DXGI_FORMAT_R32G32_FLOAT, W, H),
                                     nd = texture(DXGI_FORMAT_R32_FLOAT, W, H),
                                     nc = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H),
                                     nm = texture(DXGI_FORMAT_R32G32_FLOAT, W, H);
                                DlssNr::Spatial::Layout l {};
                                l.nativeW = W + 17;
                                l.nativeH = H + 11;
                                l.modelW = W;
                                l.modelH = H;
                                l.warp.x = pw::BuildAxis(l.nativeW, l.nativeW, W, 1, 0, 0);
                                l.warp.y = pw::BuildAxis(l.nativeH, l.nativeH, H, 1, 0, 0);
                                const DlssNr::GuideRegions regions { { dx, dy, sw, sh }, { mx, my, sw, sh } };
                                auto g = DlssNr::Spatial::MakeGuideConstants(l, regions, { sw, sh }, 1, 1);
                                g.mode = 104;
                                g.padding[0] = 1;
                                DlssNr::Spatial::SetDepthPrecision(g, quantized ? 2.f / 65535.f : 0.f);
                                g.unused = inverted ? 1.f : 0.f;
                                auto gc = cb(g);
                                g.padding[1] = hold ? 0.f : float(W) / sw;
                                g.padding[2] = hold ? 0.f : float(H) / sh;
                                auto fc = cb(g);
                                DlssNrConstants c {};
                                c.Mode = 39;
                                c.Width = W;
                                c.Height = H;
                                c.TransferStrength = float(W);
                                c.ColourStrength = float(H);
                                c.MvScaleX = g.padding[1];
                                c.MvScaleY = g.padding[2];
                                auto cc = cb(c);
                                table(0, { d.Get(), d.Get(), m.Get(), d.Get(), d.Get(), d.Get() },
                                      { od.Get(), raw.Get(), od.Get() });
                                table(16, { p.Get(), a.Get(), raw.Get(), p.Get(), p.Get(), p.Get() },
                                      { oc.Get(), om.Get(), oc.Get() });
                                table(32, { p.Get(), d.Get(), m.Get(), a.Get(), p.Get(), p.Get() },
                                      { nd.Get(), nm.Get(), nc.Get() });
                                r.begin();
                                ID3D12DescriptorHeap* heaps[] = { r.heap.Get() };
                                r.list->SetDescriptorHeaps(1, heaps);
                                r.list->SetComputeRootSignature(r.root.Get());
                                const auto draw = [&](ID3D12PipelineState* ps, ID3D12Resource* constants, UINT offset)
                                {
                                    auto gpu = r.heap->GetGPUDescriptorHandleForHeapStart();
                                    gpu.ptr += UINT64(offset) * r.stride;
                                    r.list->SetPipelineState(ps);
                                    r.list->SetComputeRootDescriptorTable(0, gpu);
                                    r.list->SetComputeRootConstantBufferView(1, constants->GetGPUVirtualAddress());
                                    r.list->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                                };
                                draw(quantized ? quantJoint.Get() : joint.Get(), gc.Get(), 0);
                                for (auto* t : { od.Get(), raw.Get() })
                                    r.barrier(t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                draw(carrier.Get(), cc.Get(), 16);
                                draw(quantized ? quantFused.Get() : fused.Get(), fc.Get(), 32);
                                r.finish();
                                for (auto pair : { std::pair<ID3D12Resource*, ID3D12Resource*> { od.Get(), nd.Get() },
                                                   { om.Get(), nm.Get() },
                                                   { oc.Get(), nc.Get() } })
                                {
                                    const auto x = ReadTexture(r, pair.first,
                                                               pair.first == od.Get()
                                                                   ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                                   : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                                    const auto y = ReadTexture(r, pair.second, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                                    if (x != y)
                                        throw std::runtime_error("Fused private guides not bit-identical: case " +
                                                                 std::to_string(cases));
                                    words += x.size();
                                }
                                ++cases;
                            }
        std::cout << "Fused private guide/carrier bit parity PASS: " << cases << " cases, " << words
                  << " words; odd/partial groups, independent origins, layers, reversed Z, nonfinite guides/colour and "
                     "Hold\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
