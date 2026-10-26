#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "four_candidates_fixture.h"
#include "packed_stage_fixture.h"

int main()
{
    try
    {
        Runner r(true, 256, 3);
        std::ofstream out("artifacts/validation/review-timing.csv");
        out << std::setprecision(9) << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        const std::string dir = "OptiScaler/shaders/dlssnr/superior/precompile";
        const std::string baseline = "artifacts/validation/review-reference";
        for (unsigned quantization : { 0u, 16u })
            for (UINT renderW : { 1920u, 2560u })
                for (unsigned modelPercent : { 65u, 100u })
                    for (int mapping : { -1, 0 })
                    {
                        const UINT renderH = renderW == 1920 ? 1080 : 1440;
                        PackedStages f(r, modelPercent, mapping);
                        const bool privateDlss = true;
                        auto point = r.pipeline(baseline, "host_joint_guides");
                        const auto renderLayout =
                            modelPercent == 100 ? DlssNr::Spatial::RenderGuideLayout(f.layout, { renderW, renderH })
                                                : f.layout;
                        const bool reconstructPrivate =
                            renderLayout.modelW != f.layout.modelW || renderLayout.modelH != f.layout.modelH;
                        // Upload deterministic sloped depth and affine render-resolution motion outside timing.
                        auto upload = [&](DXGI_FORMAT format, bool isDepth)
                        {
                            auto image = f.texture(renderW, renderH, format);
                            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
                            UINT64 bytes;
                            const auto desc = image->GetDesc();
                            r.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
                            auto buffer = r.buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                            uint8_t* data;
                            check(buffer->Map(0, nullptr, reinterpret_cast<void**>(&data)));
                            for (UINT y = 0; y < renderH; ++y)
                                for (UINT x = 0; x < renderW; ++x)
                                {
                                    float value[2] { .2f + x * .00002f + y * .00001f, 0 };
                                    if (!isDepth)
                                    {
                                        value[0] = 4.f + x * .001f;
                                        value[1] = -2.f + y * .001f;
                                    }
                                    if (isDepth && quantization)
                                        value[0] = std::round(value[0] * 65535.f) / 65535.f;
                                    std::memcpy(data + y * fp.Footprint.RowPitch + x * (isDepth ? 4 : 8), value,
                                                isDepth ? 4 : 8);
                                }
                            buffer->Unmap(0, nullptr);
                            r.begin();
                            r.barrier(image.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
                            D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
                            src.pResource = buffer.Get();
                            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                            src.PlacedFootprint = fp;
                            dst.pResource = image.Get();
                            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                            r.barrier(image.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            r.finish();
                            return image;
                        };
                        auto sourceDepth = upload(DXGI_FORMAT_R32_FLOAT, true);
                        auto sourceMotion = upload(DXGI_FORMAT_R32G32_FLOAT, false);
                        auto packed =
                            r.pipeline(dir, quantization ? "host_joint_guides_quantized" : "host_joint_guides");
                        auto correction = r.pipeline(dir, quantization ? "host_joint_minified_guides_quantized"
                                                                       : "host_joint_minified_guides");
                        auto oldCorrection = r.pipeline(baseline, "host_joint_minified_guides");
                        auto uniform = r.pipeline(dir, "host_uniform_joint_guides");
                        auto originalUniform = r.pipeline(dir, "host_uniform_joint_guides");
                        ComPtr<ID3D12Resource> depth[2], motion[2], constants[2];
                        DlssNr::Spatial::MinifiedMotionDispatch plan[2];
                        for (unsigned variant = 0; variant < 2; ++variant)
                        {
                            const auto& layout = renderLayout;
                            depth[variant] = f.texture(layout.modelW, layout.modelH, DXGI_FORMAT_R32_FLOAT);
                            motion[variant] = f.texture(layout.modelW, layout.modelH, DXGI_FORMAT_R32G32_FLOAT);
                            const DlssNr::GuideRegions regions { { 0, 0, renderW, renderH },
                                                                 { 0, 0, renderW, renderH } };
                            auto c = DlssNr::Spatial::MakeGuideConstants(layout, regions, { renderW, renderH }, 1, 1);
                            c.padding[0] = 1;
                            DlssNr::Spatial::SetDepthPrecision(c, quantization ? 2.f / 65535.f : 0.f);
                            plan[variant] = DlssNr::Spatial::PrepareJointMinification(c);
                            constants[variant] = f.cb(c);
                            f.table(112 + 8 * variant, sourceDepth.Get(), sourceDepth.Get(), sourceMotion.Get(),
                                    nullptr, nullptr, depth[variant].Get(), motion[variant].Get());
                        }
                        auto privateDepth = f.texture(f.after.w, f.after.h, DXGI_FORMAT_R32_FLOAT);
                        auto privateMotion = f.texture(f.after.w, f.after.h, DXGI_FORMAT_R32G32_FLOAT);
                        auto l = f.layout;
                        l.warp.x = pw::BuildAxis(3840, 3840, l.modelW, 1, 0, 0);
                        l.warp.y = pw::BuildAxis(2160, 2160, l.modelH, 1, 0, 0);
                        const DlssNr::GuideRegions packedRegions { { 0, 0, renderLayout.modelW, renderLayout.modelH },
                                                                   { 0, 0, renderLayout.modelW, renderLayout.modelH } };
                        auto uc = DlssNr::Spatial::MakeGuideConstants(
                            l, packedRegions, { renderLayout.modelW, renderLayout.modelH }, 1, 1);
                        uc.mode = 104;
                        uc.padding[0] = 1;
                        DlssNr::Spatial::SetDepthPrecision(uc, quantization ? 2.f / 65535.f : 0.f);
                        auto uniformCb = f.cb(uc);
                        for (unsigned variant = 0; variant < 2; ++variant)
                            f.table(128 + variant * 24, depth[variant].Get(), depth[variant].Get(),
                                    motion[variant].Get(), nullptr, nullptr, privateDepth.Get(), privateMotion.Get());
                        ComPtr<ID3D12Resource> carrierCb[2];
                        for (unsigned variant = 0; variant < 2; ++variant)
                        {
                            DlssNrConstants c {};
                            c.Mode = 39;
                            c.Width = f.after.w;
                            c.Height = f.after.h;
                            c.TransferStrength = float(f.after.w);
                            c.ColourStrength = float(f.after.h);
                            c.MvScaleX = float(f.after.w) / renderLayout.modelW;
                            c.MvScaleY = float(f.after.h) / renderLayout.modelH;
                            carrierCb[variant] = f.cb(c);
                            f.table(136 + variant * 8, f.packedProxy.Get(), f.packedAnswer.Get(),
                                    reconstructPrivate ? privateMotion.Get() : motion[variant].Get(), nullptr, nullptr,
                                    f.after.carrier.Get(), f.after.temporalMotion.Get());
                        }
                        const std::string resolveName =
                            mapping < 0 ? "host_cached_packed_sdr" : "host_cached_packed_hdr0";
                        auto baselineResolve = r.pipeline(baseline, resolveName);
                        auto newResolve = r.pipeline(dir, resolveName);
                        auto metadata = CachedWarp(r, f.layout);
                        f.table(96, f.packedProxy.Get(), f.after.residual.Get(), f.images.inputs[2].Get(),
                                metadata.Get(), f.after.low.Get(), f.images.native.Get());
                        auto fused =
                            r.pipeline(dir, quantization ? "host_private_guides_quantized" : "host_private_guides");
                        auto oldFused = r.pipeline(baseline, "host_private_guides");
                        auto fusedConstants = uc;
                        fusedConstants.padding[1] = float(f.after.w) / renderLayout.modelW;
                        fusedConstants.padding[2] = float(f.after.h) / renderLayout.modelH;
                        auto fusedCb = f.cb(fusedConstants);
                        for (unsigned variant = 0; variant < 2; ++variant)
                        {
                            f.table(176 + variant * 16, f.packedProxy.Get(), depth[variant].Get(),
                                    motion[variant].Get(), f.packedAnswer.Get(), nullptr, privateDepth.Get(),
                                    f.after.temporalMotion.Get());
                            D3D12_UNORDERED_ACCESS_VIEW_DESC extra {};
                            extra.Format = f.after.carrier->GetDesc().Format;
                            extra.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                            auto extraCpu = r.heap->GetCPUDescriptorHandleForHeapStart();
                            extraCpu.ptr += SIZE_T(184 + variant * 16) * r.stride;
                            r.device->CreateUnorderedAccessView(f.after.carrier.Get(), nullptr, &extra, extraCpu);
                        }
                        const auto draw = [&](unsigned variant)
                        {
                            const auto& layout = renderLayout;
                            r.barrier(f.packedProxy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            f.dispatch(f.spatial.Get(), f.packCb.Get(), 0, f.layout.modelW, f.layout.modelH);
                            r.barrier(f.packedProxy.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                            f.dispatch(variant ? packed.Get() : point.Get(), constants[variant].Get(),
                                       112 + variant * 8, layout.modelW, layout.modelH);
                            if (plan[variant].pixels)
                            {
                                D3D12_RESOURCE_BARRIER b {};
                                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                                b.UAV.pResource = nullptr;
                                r.list->ResourceBarrier(1, &b);
                                r.list->SetPipelineState(variant ? correction.Get() : oldCorrection.Get());
                                r.list->Dispatch(plan[variant].groupsX, plan[variant].groupsY, 1);
                            }
                            if (!privateDlss)
                            {
                                D3D12_RESOURCE_BARRIER b {};
                                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                                r.list->ResourceBarrier(1, &b);
                                return;
                            }
                            for (auto* image : { depth[variant].Get(), motion[variant].Get() })
                                r.barrier(image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            if (reconstructPrivate)
                            {
                                f.dispatch(variant ? fused.Get() : oldFused.Get(), fusedCb.Get(), 176 + variant * 16,
                                           f.after.w, f.after.h);
                                r.barrier(privateDepth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            }
                            if (!reconstructPrivate)
                                f.dispatch(f.carrier.Get(), carrierCb[variant].Get(), 136 + variant * 8, f.after.w,
                                           f.after.h);
                            r.barrier(f.after.carrier.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            f.dispatch(f.carrier.Get(), f.after.decodeCb.Get(), 80, f.after.w, f.after.h);
                            r.barrier(f.after.residual.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            f.dispatch(f.low.Get(), f.after.lowCb.Get(), 88, (f.after.w + 7) / 8, (f.after.h + 7) / 8,
                                       16);
                            r.barrier(f.after.low.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            f.dispatch(variant ? newResolve.Get() : baselineResolve.Get(), f.after.resolveCb.Get(), 96,
                                       3840, 2160);
                            r.barrier(f.after.carrier.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            r.barrier(f.after.residual.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            r.barrier(f.after.low.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            D3D12_RESOURCE_BARRIER b {};
                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            r.list->ResourceBarrier(1, &b);
                            if (reconstructPrivate)
                            {
                                r.barrier(privateDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            }
                            for (auto* image : { depth[variant].Get(), motion[variant].Get() })
                                r.barrier(image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        };
                        const auto name = std::string(mapping < 0 ? "SDR" : "HDR0") + "-render" +
                                          std::to_string(renderH) + "-depth" + std::to_string(quantization);
                        MeasureCandidate(f.images, out, name.c_str(), modelPercent, [&] { draw(0); }, [&] { draw(1); });
                    }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
