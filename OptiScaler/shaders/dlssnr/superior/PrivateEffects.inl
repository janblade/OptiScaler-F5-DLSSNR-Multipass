// Included after Superior's dispatch and NGX creation helpers.
void UpdateResidentCnn(Generation& g, ID3D12GraphicsCommandList* cmd, const DlssNrFrameInfo& frame)
{
    using namespace DlssNr::SuperiorScaling;
    if (g.actualPreset != CnnD)
        return;
    if (!g.residentAttempted)
    {
        g.residentAttempted = true;
        auto resident = std::make_shared<ResidentCnn>();
        resident->ngxEpoch = g.ngxEpoch;
        if (NVNGXProxy::D3D12_AllocateParameters()(&resident->parameters) != NVSDK_NGX_Result_Success ||
            !resident->parameters)
            return;
        SetCreationParameters(g, resident->parameters);
        resident->parameters->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
        const auto result = CreateFeature(resident->parameters, resident->feature, cmd, CnnE);
        // An aliased handle still belongs to the primary instance.
        if (resident->feature == g.feature)
            resident->feature = nullptr;
        if (result != NVSDK_NGX_Result_Success || !resident->feature)
        {
            GetState().lifetime.Retire([resident] {});
            LOG_WARN("Superior Scaling: resident CNN E unavailable, keeping CNN D");
            return;
        }
        resident->creation = Util::RealObjectIdentity(cmd);
        resident->createEpoch = frame.SubmissionEpoch;
        g.resident = std::move(resident);
    }
    if (!g.resident || g.resident->ready)
        return;
    if (g.resident->submitted && frame.SubmissionEpoch != g.resident->createEpoch)
    {
        g.motionProbe = std::make_unique<MotionProbe>();
        g.resident->ready = true;
    }
    else if (++g.resident->waitedFrames > kCreationWaitFrames)
    {
        auto abandoned = std::move(g.resident);
        GetState().lifetime.Retire([abandoned] {});
    }
}

std::shared_ptr<MotionSample> NewMotionSample(ID3D12Device* device)
{
    auto sample = std::make_shared<MotionSample>();
    if (!Ensure(sample->result, device, DXGI_FORMAT_R32G32_UINT, 1, 1))
        return {};
    auto texture = sample->result.Get()->GetDesc();
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &sample->footprint, nullptr, nullptr, &bytes);
    auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&sample->readback))))
        return {};
    return sample;
}

void UpdateMotion(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* motion,
                  DlssNr::GuideRegion region, const DlssNrFrameInfo& frame, double now, bool hold)
{
    auto& probe = *g.motionProbe;
    const unsigned rw = frame.MotionVectorsLowResolution ? frame.RenderSubrectWidth : frame.OutputWidth;
    const unsigned rh = frame.MotionVectorsLowResolution ? frame.RenderSubrectHeight : frame.OutputHeight;
    const float normal = frame.OutputHeight ? 1080.f / float(frame.OutputHeight) : 0.f;
    const float sx = DlssNr::SuperiorScaling::MotionScale(frame.MvScaleX, frame.OutputWidth, rw, region.width) * normal;
    const float sy =
        DlssNr::SuperiorScaling::MotionScale(frame.MvScaleY, frame.OutputHeight, rh, region.height) * normal;
    const bool suspended =
        hold || !motion || !region.valid() || !std::isfinite(sx) || !std::isfinite(sy) || sx == 0.f || sy == 0.f;
    const auto old = probe.region;
    if (probe.suspended != suspended || old.x != region.x || old.y != region.y || old.width != region.width ||
        old.height != region.height || sx != probe.scaleX || sy != probe.scaleY)
    {
        ++probe.context;
        probe.selection.Invalidate();
        probe.region = region;
        probe.scaleX = sx;
        probe.scaleY = sy;
        probe.suspended = suspended;
        probe.nextSample = 0;
    }
    if (suspended)
        return;

    // Process completions in recording order even when a slot was recycled out of order.
    std::array<MotionSample*, 8> complete {};
    unsigned count = 0;
    for (auto& sample : probe.samples)
        if (sample && sample->ready)
            complete[count++] = sample.get();
    std::sort(complete.begin(), complete.begin() + count,
              [](auto* a, auto* b) { return a->measuredAt < b->measuredAt; });
    for (unsigned i = 0; i < count; ++i)
    {
        auto& sample = *complete[i];
        sample.ready = false;
        if (sample.context != probe.context || now - sample.measuredAt > Private::FreshSeconds)
            continue;
        uint32_t* mapped = nullptr;
        const D3D12_RANGE read { 0, 2 * sizeof(uint32_t) };
        if (FAILED(sample.readback->Map(0, &read, reinterpret_cast<void**>(&mapped))))
            continue;
        if (mapped[0] == Private::MotionSamples && mapped[1] <= Private::MotionSamples)
            probe.selection.Observe(mapped[1], sample.measuredAt);
        else
            probe.selection.Invalidate();
        const D3D12_RANGE written {};
        sample.readback->Unmap(0, &written);
    }

    if (now < probe.nextSample)
        return;
    probe.nextSample = now + 1. / 30.; // Enough samples to resolve a 50 ms quiet interval.
    auto slot = std::find_if(probe.samples.begin(), probe.samples.end(),
                             [](const auto& sample) { return !sample || !sample->busy; });
    if (slot == probe.samples.end())
        return;
    if (!*slot)
        *slot = NewMotionSample(g.device.Get());
    const auto& sample = *slot;
    if (!sample)
        return;
    // The readback is written in the game list. Recycle only after Reset/destruction and every replay's fence.
    Record(cmd);
    if (!GetState().lifetime.AfterRecording(cmd,
                                            [sample](bool submitted)
                                            {
                                                sample->ready = submitted && sample->recorded;
                                                sample->busy = false;
                                                sample->input.Reset();
                                            }))
        return;

    sample->busy = true;
    sample->recorded = false;
    Constants constants {};
    Fields::SetPrivateMotion(constants, region.x, region.y, region.width, region.height, sx, sy);
    if (!shader.DispatchSuperiorPass(cmd, constants, motion, nullptr, nullptr, nullptr, nullptr, nullptr,
                                     sample->result.Get(), nullptr))
        return;
    sample->result.To(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from {}, to {};
    from.pResource = sample->result.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = sample->readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = sample->footprint;
    cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    sample->result.To(cmd, Uav); // identical initial/final state for a replayed or discarded recording
    sample->input = motion;
    sample->recorded = true;
    sample->measuredAt = now;
    sample->context = probe.context;
}

bool UpdateGhostHistory(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth,
                        ID3D12Resource* proxy, ID3D12Resource* answer, const DlssNrFrameInfo& frame, bool reset,
                        float precision)
{
    if (!g.ghosts)
        g.ghosts = std::make_unique<GhostHistory>();
    auto& h = *g.ghosts;
    const auto identity = Util::RealObjectIdentity(cmd);
    if (h.initialized && h.epoch == frame.SubmissionEpoch && (h.commands == identity || h.submitted) && !reset)
        return true;

    Constants c {};
    const bool previous = h.initialized && h.submitted && !reset;
    Fields::SetPrivateGhost(c, g.workWidth, g.workHeight, g.inverted, previous, precision);
    const unsigned w = c.Width, ht = c.Height;
    auto ensure = [&](Texture& t, DXGI_FORMAT format) { return Ensure(t, g.device.Get(), format, w, ht); };
    if (!ensure(h.reconstructed, DXGI_FORMAT_R32_UINT) || !ensure(h.field, DXGI_FORMAT_R16_FLOAT) ||
        !ensure(h.expanded, DXGI_FORMAT_R16G16B16A16_FLOAT))
        return false;
    for (unsigned i = 0; i < 2; ++i)
        if (!ensure(h.state[i], DXGI_FORMAT_R32G32_UINT) || !ensure(h.raw[i], DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            !ensure(h.reference[i], DXGI_FORMAT_R16G16B16A16_FLOAT))
            return false;

    h.commands = identity;
    h.submitted = false;
    h.initialized = false;
    const unsigned next = previous ? h.index ^ 1u : 0u;
    if (!previous)
        h.state[1].To(cmd, Srv); // bound but unread until a previous frame exists
    auto* old = h.state[previous ? h.index : 1].Get();
    if (previous)
    {
        c.Mode = DlssNrMode_PrivateGhostClear;
        if (!Dispatch(shader, cmd, c, depth, nullptr, nullptr, nullptr, nullptr, nullptr, h.reconstructed, nullptr,
                      nullptr, true))
            return false;
    }
    c.Mode = DlssNrMode_PrivateGhostScatter;
    if (!Dispatch(shader, cmd, c, depth, g.temporalMotion.Get(), old, nullptr, nullptr, nullptr, h.reconstructed,
                  &h.state[next], nullptr, false, true))
        return false;
    c.Mode = DlssNrMode_PrivateGhostAge;
    if (!Dispatch(shader, cmd, c, depth, g.temporalMotion.Get(), old, h.reconstructed.Get(), nullptr, nullptr,
                  h.state[next], &h.state[next]))
        return false;
    c.Mode = DlssNrMode_PrivateGhostReference;
    if (!Dispatch(shader, cmd, c, h.state[next].Get(), previous ? h.raw[h.index].Get() : proxy,
                  previous ? h.reference[h.index].Get() : proxy, proxy, answer, depth, h.raw[next], &h.reference[next]))
        return false;

    c.Mode = DlssNrMode_PrivateGhostDilate;
    if (!Dispatch(shader, cmd, c, h.state[next].Get(), h.reference[next].Get(), nullptr, nullptr, nullptr, nullptr,
                  h.field, &h.expanded))
        return false;
    h.index = next;
    h.epoch = frame.SubmissionEpoch;
    h.initialized = true;
    return true;
}

bool RejectTransformerGhosts(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd,
                             const Constants& baseDecode, ID3D12Resource* depth, ID3D12Resource* proxy,
                             ID3D12Resource* answer, const DlssNrFrameInfo& frame, bool reset, float precision)
{
    Constants decode = baseDecode;
    if (!UpdateGhostHistory(g, shader, cmd, depth, proxy, answer, frame, reset, precision))
    {
        if (g.ghosts)
            g.ghosts->initialized = false;
        return Dispatch(shader, cmd, decode, g.temporalOutput.Get(), nullptr, nullptr, nullptr, nullptr, nullptr,
                        g.carrier);
    }

    auto& h = *g.ghosts;
    decode.Mode = DlssNrMode_PrivateGhostDecode;
    decode.Passthrough = g.inverted;
    if (!Dispatch(shader, cmd, decode, g.temporalOutput.Get(), h.expanded.Get(), proxy, depth, answer, h.field.Get(),
                  g.carrier))
        return false;

    if (!h.checkedTypedLoad)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support { DXGI_FORMAT_R16G16B16A16_FLOAT };
        h.typedLoad =
            SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
            (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
        h.checkedTypedLoad = true;
    }
    const unsigned w = g.workWidth, ht = g.workHeight;
    if (!Ensure(h.rings, g.device.Get(), DXGI_FORMAT_R32G32_FLOAT, (w + 3) / 4, (ht + 3) / 4))
        return true; // unfeathered mask remains valid
    Constants c {};
    Fields::SetPrivateRings(c, w, ht, g.inverted, precision, false);
    if (!Dispatch(shader, cmd, c, g.carrier.Get(), nullptr, nullptr, depth, nullptr, nullptr, h.rings))
        return true;
    Fields::SetPrivateRings(c, w, ht, g.inverted, precision, true);
    if (h.typedLoad)
    {
        c.Mode = DlssNrMode_PrivateGhostFeatherInPlace;
        if (!Dispatch(shader, cmd, c, proxy, h.rings.Get(), proxy, depth, answer, nullptr, g.carrier))
            g.carrier.To(cmd, Srv);
    }
    else if (Ensure(h.featherOutput, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, w, ht) &&
             Dispatch(shader, cmd, c, g.carrier.Get(), h.rings.Get(), proxy, depth, answer, nullptr, h.featherOutput))
        std::swap(g.carrier, h.featherOutput);
    return true;
}
