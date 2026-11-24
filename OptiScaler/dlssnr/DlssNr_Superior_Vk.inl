// Superior Scaling, Vulkan (menu: NR Options, Upscale Mode "Superior Scaling"; ini [DlssNr] Transfer=11).
//
// The No DLSS path of the D3D12 side (shaders/dlssnr/superior/Pipeline.inl, DlssNr_Dx12.cpp), step for step and from the
// same HLSL (tools/compile_superior_vk.ps1 builds it to SPIR-V; the D3D12 blobs are unchanged):
//   1. Area downsample of the encoded proxy to the model's raster, instead of the ordinary box downsample (v59's Area,
//      dlssnr_superior_area).
//   2. The game's depth and motion matched to the model's raster (SuperiorRun::MatchGuides: the uniform joint guides), so
//      the model is handed guides at its own size, with the vector scale taken from the matched grid.
//   3. The model, one pass.
//   4. The 1/8-size low field of the answer minus the model's input (host_low_raw), when the band gains are not both 1.
//   5. The resolve: the live composition (host_resolve_guided_raw) with the radius-one guided estimator reading the model's
//      raster, shaped by the low field.
// The private DLSS (CNN or Transformer, or none: No DLSS) is DlssNr_SuperiorPrivate_Vk.inl; Compose runs it between the
// model and the resolve, and falls back to the raw answer (No DLSS) while it is being made or when it cannot run.
//
// Runs below 100% Model resolution, and at 100% and above with a private DLSS (CNN or Transformer) under [DlssNr]
// SuperiorAtNative, as D3D12 does; unpacked, in one pass and without Reuse detail while that runs (DlssNr_SuperiorVkRule.h
// decides, and words each case it does not); otherwise the Matched residual composition the Vulkan path always had runs.
//
// Vulkan differences from D3D12, and why:
//   - Layouts instead of resource states: our own images are moved with Transition; the game's depth and motion are read
//     in SHADER_READ_ONLY_OPTIMAL and never moved (through a depth-only view when the game's depth view covers stencil too,
//     EdgeCompressionVk::DepthRead). The matched motion is RGBA32F: R32G32 is not a storage format every device offers.
//   - No GPU lifetime tracker: the images are made and freed with the device drained, as the rest of the Vulkan path does
//     (a first frame, a size change, or after the pass has not run for a few seconds).
//   - Compute and transfer commands only, so it is legal on the async-compute queue a game may present from.
//   - The live exposure travels in the motion slot (t3), where the Vulkan composition has always read it, not in t4.
//
// Included inside DlssNrFeature_Vk.cpp's anonymous namespace, after DlssNr_Spatial_Vk.inl. EvaluateAtSeamVk calls Begin
// where it decides the frame, and the rest around the model, all under g_vkMutex. The menu reads Status().
namespace SuperiorVk
{
namespace Rule = DlssNr::SuperiorScaling::VkRule;
namespace Sp = DlssNr::Spatial;

// The private DLSS (DlssNr_SuperiorPrivate_Vk.inl, included after this file).
namespace Private
{
bool Holds();
void Rebuild(VkDevice device, bool ngxGone = false);
void Release(bool deviceAlive);
void ShutdownOwnCore(bool deviceAlive, VkDevice device);
void Reselected(VkDevice device);
} // namespace Private

std::unique_ptr<DlssNrSuperior_Vk> pass;
bool passFailed = false;

OwnedImage depth;  // the game's depth matched to the model's raster, R32F
OwnedImage motion; // the game's motion matched the same way, RGBA32F (the model reads xy)
OwnedImage low;    // the 1/8-size low field, RGBA16F
unsigned int madeFor[2] = {};
unsigned int failedFor[2] = {}; // the raster whose images could not be made: not tried again until the raster changes
bool wasRunning = false;        // Superior ran last frame (EvaluateAtSeamVk: the model's history starts over on a change)
bool depthIsReal = false;   // the last MatchGuides matched the game's depth (not the depthless surface guides)
float depthPrecision = 0.f; // of the game's depth format, as the last MatchGuides saw it (the private DLSS's ghost field)

// When the pass last ran, for letting go of its images and passes once it has not for a while.
std::chrono::steady_clock::time_point lastRan {};
constexpr auto kIdleRelease = std::chrono::seconds(5);

std::mutex statusLock;
std::string status;
Rule::Outcome lastOutcome = Rule::Outcome::NotSelected;
bool outcomeSaid = false;
bool everSelected = false; // "not selected" is only worth a line after it was

std::string privateStatus; // what the private DLSS says (DlssNr_SuperiorPrivate_Vk.inl), under statusLock

std::string Status()
{
    std::lock_guard<std::mutex> lock(statusLock);
    return status.empty() ? privateStatus : privateStatus.empty() ? status : status + " " + privateStatus;
}

void Publish(const std::string& text)
{
    std::lock_guard<std::mutex> lock(statusLock);
    status = text;
}

bool HoldsImages() { return depth.Valid() || motion.Valid() || low.Valid(); }

void DestroyImages()
{
    for (OwnedImage* img : { &depth, &motion, &low })
        DestroyImage(*img);

    madeFor[0] = madeFor[1] = 0;
}

// Drains the device unless the frame has drained it already (the resize block does, before anything here runs).
void Drain(VkDevice device)
{
    if (device != VK_NULL_HANDLE && g_vkDrainedAt != g_vkSeamCalls)
    {
        vkDeviceWaitIdle(device);
        g_vkDrainedAt = g_vkSeamCalls;
    }
}

// The images for a model raster. Earlier frames may still read the old ones, so the device is drained first; that happens
// on the first frame, on a size change (the resize block has just drained for the same reason) and after an idle release.
bool EnsureImages(VkDevice device, unsigned int workW, unsigned int workH)
{
    if (depth.Valid() && motion.Valid() && low.Valid() && madeFor[0] == workW && madeFor[1] == workH)
        return true;

    // Failed at this raster: not retried every frame, only at another one (or after Superior is selected again).
    if (failedFor[0] == workW && failedFor[1] == workH)
        return false;

    if (HoldsImages())
        Drain(device);

    DestroyImages();
    const bool ok = CreateImage(depth, workW, workH, VK_FORMAT_R32_SFLOAT, true) &&
                    CreateImage(motion, workW, workH, VK_FORMAT_R32G32B32A32_SFLOAT, true) &&
                    CreateImage(low, (workW + 7) / 8, (workH + 7) / 8, VK_FORMAT_R16G16B16A16_SFLOAT, true);

    if (!ok)
    {
        DestroyImages();
        failedFor[0] = workW;
        failedFor[1] = workH;
        LOG_WARN("DLSS-NR Vulkan Superior Scaling: its images could not be made at {}x{}; Matched residual is used at "
                 "this size", workW, workH);
        return false;
    }

    madeFor[0] = workW;
    madeFor[1] = workH;
    return true;
}

// What the No DLSS composition dispatches (the private DLSS builds its own set when it is made).
constexpr DlssNrSuperior_Vk::Blob kBaseBlobs[] = {
    DlssNrSuperior_Vk::Blob::Area,
    DlssNrSuperior_Vk::Blob::SurfaceGuides,
    DlssNrSuperior_Vk::Blob::JointGuides,
    DlssNrSuperior_Vk::Blob::JointMinifiedGuides,
    DlssNrSuperior_Vk::Blob::JointGuidesQuantized,
    DlssNrSuperior_Vk::Blob::JointMinifiedGuidesQuantized,
    DlssNrSuperior_Vk::Blob::LowRaw,
    DlssNrSuperior_Vk::Blob::ResolveGuidedRaw,
};

// The pass and all its pipelines, made at the first frame it is wanted; a build that fails (or a dispatch that fails
// later, GiveUp) gives it up until Superior Scaling is selected again or NR is retried, so the frame runs the ordinary
// Matched residual step instead of Neural Rendering stopping.
bool PassReady(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice physicalDevice)
{
    if (passFailed)
        return false;

    if (!pass)
    {
        pass = std::make_unique<DlssNrSuperior_Vk>("Superior Scaling", device, physicalDevice);

        if (!pass->Ready() || !pass->Build(cmd, kBaseBlobs, std::size(kBaseBlobs)))
        {
            pass.reset();
            passFailed = true;
            LOG_WARN("DLSS-NR Vulkan: the Superior Scaling passes could not be built; Matched residual is used");
        }
    }

    return pass != nullptr && pass->Ready();
}

// A Superior dispatch failed while running: the frame falls back to the ordinary step (the caller), the rest of the
// selection too, and the model's history starts over. Said once per selection.
void GiveUp(const char* reason)
{
    if (!passFailed)
        LOG_WARN("DLSS-NR Vulkan Superior Scaling: {}; Matched residual is used until Superior Scaling is selected again",
                 reason);
    passFailed = true;
    g_vk.reset = true;
}

// The 310.8 runtime has one main network only, which changes the band gain tables (DlssNr_SuperiorScaling.h). Read from the
// model's file version once; D3D12 does the same when it makes its pipeline.
bool MainOnlyGains()
{
    static std::optional<bool> mainOnly;

    if (!mainOnly.has_value())
    {
        mainOnly = false;
        version_t version {};
        const auto snippet = FindSnippet();

        if (snippet.has_value() && Util::GetFileVersion(snippet->wstring(), &version))
            mainOnly = DlssNr::SuperiorScaling::MainOnlyRuntime(version.major, version.minor);
        else
            LOG_WARN("DLSS-NR Vulkan Superior Scaling: the model's version is unavailable; using the 310.9 preset "
                     "semantics for the frequency gains");
    }

    return *mainOnly;
}

// What a 16- or 24-bit integer depth cannot tell apart, in the units the guide shaders compare (the D3D12 table).
float DepthPrecision(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_R16_UNORM: // a colour image holding the depth (D3D12's table lists R16_UNORM too)
        return 2.f / 65535.f;
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return 2.f / 16777215.f;
    default:
        return 0.f;
    }
}

// Decides this frame (and publishes and logs what changed). reduced: the model works on fewer pixels than the frame.
// Reuse detail's gate comes and goes with the frame rate and frame generation; Superior (and its history) must not follow a
// flicker, so a change of what the gate says is only followed once it has held for kReuseHoldFrames frames in a row.
constexpr unsigned kReuseHoldFrames = 8;
bool ReuseTakesFrame(bool raw)
{
    static bool held = false;
    static unsigned differing = 0;
    if (raw == held)
        differing = 0;
    else if (++differing >= kReuseHoldFrames)
    {
        held = raw;
        differing = 0;
    }
    return held;
}

// How Superior's raster relates to the model's (for the log): the model's own, native (100% with the private DLSS), or
// native over a supersampled model (above 100%).
enum class Raster
{
    Model,
    Native,
    Supersampled
};
Raster lastRaster = Raster::Model;

Rule::Outcome Begin(VkCommandBuffer cmd, const Config& cfg, VkDevice device, VkPhysicalDevice physicalDevice,
                    bool reduced, bool atNative, bool packed, unsigned int passes, unsigned int workW,
                    unsigned int workH, Raster raster = Raster::Model)
{
    Rule::Frame frame;
    frame.selected = DlssNr::SuperiorScaling::Enabled(cfg);
    frame.reduced = reduced;
    frame.atNative = atNative;
    frame.packed = packed;
    frame.passes = passes;
    // Reuse detail takes the frame only when it would actually run: its own gate (route, minimum frame rate, frame
    // generation, textures) said so on its last frame. The ini switch alone does not block Superior. Its gate sits in
    // DetailReuseVk::BeforeModel, after this point, so this reads what it decided last frame (published for the menu)
    // rather than moving the passes; the frame it flips costs one frame of the other composition.
    frame.detailReuse = ReuseTakesFrame(cfg.DlssNrDetailReuse.value_or_default() && DetailReuseVk::Published().why.empty());
    if (frame.selected)
    {
        // The device's limits do not change; asked once per device handle.
        static VkPhysicalDevice asked = VK_NULL_HANDLE;
        static bool canRun = true;
        if (asked != physicalDevice)
        {
            asked = physicalDevice;
            canRun = DlssNrSuperior_Vk::DeviceCanRun(physicalDevice);
        }
        frame.deviceCanRun = canRun;
    }
    Rule::Outcome outcome = Rule::Decide(frame);

    // Selecting Superior Scaling again retries what a failure gave up (no DLSS here, a creation that failed).
    {
        static bool wasSelected = false;
        if (frame.selected && !wasSelected)
        {
            passFailed = false;
            failedFor[0] = failedFor[1] = 0;
            Private::Reselected(device);
            // Private passes that would not build are tried afresh: the pass (and its flags) is made again.
            if (pass && !pass->PrivateReady())
            {
                Drain(device);
                Private::Rebuild(device);
                pass.reset();
            }
        }
        wasSelected = frame.selected;
    }

    // Only a run that could go ahead builds anything; the images are the last thing that can fail.
    if (outcome == Rule::Outcome::Runs && (!PassReady(cmd, device, physicalDevice) || !EnsureImages(device, workW, workH)))
        outcome = Rule::Outcome::PassFailed;
    if (frame.selected && passFailed)
        outcome = Rule::Outcome::PassFailed;

    Publish(Rule::Describe(outcome));

    if (!outcomeSaid || lastOutcome != outcome || (outcome == Rule::Outcome::Runs && lastRaster != raster))
    {
        outcomeSaid = true;
        lastOutcome = outcome;
        lastRaster = raster;

        if (outcome == Rule::Outcome::Runs)
            LOG_INFO("DLSS-NR Vulkan Superior Scaling: running, {}",
                     raster == Raster::Model ? "the model works on " + std::to_string(workW) + "x" + std::to_string(workH)
                     : raster == Raster::Native
                         ? "at native " + std::to_string(workW) + "x" + std::to_string(workH) +
                               " (Also at 100% and above): the private DLSS steadies the native raster"
                         : "above 100%: the model supersamples, the private DLSS steadies the native " +
                               std::to_string(workW) + "x" + std::to_string(workH) + " pair after the down-leg");
        else if (outcome != Rule::Outcome::NotSelected)
            LOG_INFO("DLSS-NR Vulkan Superior Scaling: {}", Rule::Describe(outcome));
        else if (everSelected)
            LOG_INFO("DLSS-NR Vulkan Superior Scaling: not selected");
    }
    everSelected = everSelected || outcome != Rule::Outcome::NotSelected;

    const auto now = std::chrono::steady_clock::now();

    if (outcome == Rule::Outcome::Runs)
        lastRan = now;
    else if (lastRan == std::chrono::steady_clock::time_point {} && (pass || HoldsImages()) && frame.selected)
        lastRan = now; // built but not run (a fallback right away): the idle clock still starts, so it is let go
    else if ((pass || HoldsImages() || Private::Holds()) && lastRan != std::chrono::steady_clock::time_point {} &&
             now - lastRan >= kIdleRelease && device != VK_NULL_HANDLE)
    {
        // Not run for a while (another Upscale Mode, 100%, a fallback): the images and passes go, with the device drained
        // once. A quick switch back finds them still here.
        Drain(device);
        Private::Rebuild(device);
        DestroyImages();
        pass.reset();
        lastRan = {};
    }

    return outcome;
}

// 1. The Area downsample of the encoded proxy (native size) to the model's raster, into proxySmall.
bool Area(VkCommandBuffer cmd, OwnedImage& proxy, OwnedImage& proxySmall, unsigned int workW, unsigned int workH,
          float jitterX = 0.f, float jitterY = 0.f)
{
    DlssNr::Superior::DlssNrConstants c {};
    c.Mode = DlssNr::Superior::DlssNrMode_Downsample;
    c.Width = workW;
    c.Height = workH;
    if (jitterX != 0.f || jitterY != 0.f)
    {
        c.MvScaleX = jitterX;
        c.MvScaleY = jitterY;
        c.GuideWidth = 1u;
    }
    c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;

    Transition(cmd, proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, proxySmall, VK_IMAGE_LAYOUT_GENERAL);

    DlssNrSuperior_Vk::Reads reads {};
    reads[0] = { proxy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    return pass->Dispatch(cmd, DlssNrSuperior_Vk::Blob::Area, &c, sizeof(c), (workW + 7) / 8, (workH + 7) / 8, reads,
                          proxySmall.view);
}

// 2. The game's depth and motion matched to the model's raster (D3D12: SuperiorRun::MatchGuides, the uniform joint guides
// with the bounded-footprint correction where the guides shrink). Leaves `depth` and `motion` readable.
bool MatchGuides(VkCommandBuffer cmd, VkDevice device, const NVSDK_NGX_Resource_VK* gameDepth,
                 const NVSDK_NGX_Resource_VK* gameMotion, const DlssNr::GuideRegions& guides, unsigned int width,
                 unsigned int height, unsigned int workW, unsigned int workH, bool depthInverted, float gameMvX,
                 float gameMvY, DlssNr::GuideExtent reference)
{
    // Superior reads a depth held as a colour image too (as D3D12 reads any texture); Compress screen edges does not.
    const auto depthRead = EdgeCompressionVk::DepthRead(device, gameDepth, true);
    const bool haveDepth = depthRead.view != VK_NULL_HANDLE && guides.depth.valid();

    Sp::Layout layout {};
    layout.nativeW = width;
    layout.nativeH = height;
    layout.modelW = workW;
    layout.modelH = workH;
    layout.warp.x = pw::BuildAxis(width, width, workW, 1, 0, 0);
    layout.warp.y = pw::BuildAxis(height, height, workH, 1, 0, 0);

    auto c = Sp::MakeGuideConstants(layout, guides, reference, gameMvX, gameMvY);
    c.mode = 104u; // uniform: the model's raster is a plain scaling of the frame
    c.padding[0] = haveDepth ? 1.f : 0.f;
    if (!haveDepth)
    {
        c.depthRect[0] = c.depthRect[1] = 0.f;
        c.depthRect[2] = c.depthRect[3] = 1.f;
    }
    c.unused = depthInverted ? 1.f : 0.f;
    depthIsReal = haveDepth;
    depthPrecision = haveDepth ? DepthPrecision(gameDepth->Resource.ImageViewInfo.Format) : 0.f;
    Sp::SetDepthPrecision(c, depthPrecision);

    using Blob = DlssNrSuperior_Vk::Blob;
    const bool quantized = c.warp.reserved != 0;
    const bool joint = haveDepth; // padding[0] > .5
    const Sp::MinifiedMotionDispatch minified = joint ? Sp::PrepareJointMinification(c) : Sp::MinifiedMotionDispatch {};
    const Blob primary = joint ? (quantized ? Blob::JointGuidesQuantized : Blob::JointGuides) : Blob::SurfaceGuides;
    const Blob correction = joint && minified.pixels ? (quantized ? Blob::JointMinifiedGuidesQuantized
                                                                   : Blob::JointMinifiedGuides)
                                                     : Blob::Count;

    Transition(cmd, depth, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmd, motion, VK_IMAGE_LAYOUT_GENERAL);

    DlssNrSuperior_Vk::Reads reads {};
    reads[1] = { haveDepth ? depthRead.view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    reads[2] = { gameMotion->Resource.ImageViewInfo.ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

    if (!pass->DispatchGuides(cmd, primary, correction, &c, sizeof(c), (workW + 7) / 8, (workH + 7) / 8,
                              minified.groupsX, minified.groupsY, reads, depth.view, motion.view))
        return false;

    Transition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// 4 and 5. The low field (unless both gains are 1) and the resolve. `proxy` and `answer` are the model's input and answer
// at the model's raster, readable; with signedResidual the second is the private DLSS's decoded signed residual instead
// of the model's raw answer (the D3D12 resolve's "decoded signed DLSS residual" case). `resolve` is the composition's
// constants, as the ordinary resolve would take them.
bool Finish(VkCommandBuffer cmd, const Config& cfg, const DlssNrConstants& resolveIn, float workScale,
            unsigned int width, unsigned int height, OwnedImage& proxy, OwnedImage& answer, bool signedResidual,
            OwnedImage& keep, VkImageView exposureView, VkImageView target, bool directSr = false,
            OwnedImage* workAnswer = nullptr, bool directSrPerf = false)
{
    DlssNrConstants resolve = resolveIn; // the band gains and flags below are Superior's, not the ordinary resolve's
    VkImageView auxView = VK_NULL_HANDLE;
    if (directSr)
    {
        DlssNr::Superior::Fields::SetDirectSrResolve(resolve, workAnswer != nullptr,
                                                     cfg.DlssNrSuperiorNativeDetail.value_or_default(), directSrPerf,
                                                     cfg.DlssNrSuperiorDirectSrSharpness.value_or_default());
        if (workAnswer && !directSrPerf)
        {
            Transition(cmd, *workAnswer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            auxView = workAnswer->view;
        }
    }
    else
    {
        // The same gains D3D12 reads for one pass over the frame's native size: no pass has run before this one.
        const auto scales = DlssNr::SuperiorScaling::MakeFrequencyScales(width, height, proxy.width, proxy.height,
                                                                         proxy.width, proxy.height);
        const auto gains = DlssNr::SuperiorScaling::BlockGains(cfg, 0u, 1u, scales, MainOnlyGains());
        const bool wantLow = gains.high != 1.0f || gains.low != 1.0f;

        if (wantLow)
        {
            DlssNr::Superior::DlssNrConstants c {};
            c.Mode = DlssNr::Superior::DlssNrMode_GuidedResidualLow;
            c.Transfer = signedResidual ? 1u : 0u; // a raw model answer, or the decoded signed residual
            c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;
            c.Width = (proxy.width + 7) / 8;
            c.Height = (proxy.height + 7) / 8;

            Transition(cmd, proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmd, answer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmd, low, VK_IMAGE_LAYOUT_GENERAL);

            DlssNrSuperior_Vk::Reads reads {};
            reads[0] = { proxy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            reads[1] = { answer.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            if (!pass->Dispatch(cmd, signedResidual ? DlssNrSuperior_Vk::Blob::LowSigned : DlssNrSuperior_Vk::Blob::LowRaw,
                                &c, sizeof(c), (c.Width + 7) / 8,
                                (c.Height + DlssNr::SuperiorScaling::LowGroupHeight - 1) /
                                    DlssNr::SuperiorScaling::LowGroupHeight,
                                reads, low.view))
                return false;

            Transition(cmd, low, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            auxView = low.view;
        }

        DlssNr::Superior::Fields::SetBandGains(resolve, gains.high, gains.low);
        DlssNr::SuperiorScaling::SetGuidedRange(resolve, workScale);
        DlssNr::Superior::Fields::SetResolveFlags(resolve, wantLow, !signedResidual);
    }

    Transition(cmd, proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, answer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    DlssNrSuperior_Vk::Reads reads {};
    reads[0] = { proxy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    reads[1] = { answer.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    reads[2] = { keep.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    reads[3] = { exposureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    reads[5] = { auxView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

    using B = DlssNrSuperior_Vk::Blob;
    B blob = B::ResolveGuidedRaw;
    if (signedResidual || directSr)
    {
        // The D3D12 choice (SuperiorResolvePipeline): the specialization for the colour mapping in force, else the generic
        // resolve for SDR or HDR.
        const int normal = DlssNr::SuperiorScaling::NormalResolveVariant(false, false, resolve.Passthrough,
                                                                       resolve.ReversibleMode, resolve.DebugView,
                                                                       resolve.CompareMode, resolve.ApplyModel);
        blob = normal >= 0 ? (B) ((uint32_t) B::NormalSdr + (uint32_t) normal)
                           : (resolve.Passthrough != 0 ? B::ResolveSdr : B::ResolveHdr);
    }

    if (pass->Dispatch(cmd, blob, &resolve, sizeof(resolve), (width + 7) / 8, (height + 7) / 8, reads, target))
        return true;

    // A specialization that would not build: the generic residual resolve (D3D12 does the same).
    if (DlssNrSuperior_Vk::Optional(blob))
        return pass->Dispatch(cmd, resolve.Passthrough != 0 ? B::ResolveSdr : B::ResolveHdr, &resolve, sizeof(resolve),
                              (width + 7) / 8, (height + 7) / 8, reads, target);
    return false;
}

// Shutdown. deviceAlive: the device is idle and everything is destroyed; otherwise it went with the device and took every
// handle, so they are only forgotten.
void Release(bool deviceAlive)
{
    Private::Release(deviceAlive);
    if (deviceAlive)
    {
        DestroyImages();
        pass.reset();
    }
    else
    {
        for (OwnedImage* img : { &depth, &motion, &low })
            *img = OwnedImage {};
        madeFor[0] = madeFor[1] = 0;
        pass.release(); // its pipelines and descriptors went with the device
    }

    passFailed = false;
    failedFor[0] = failedFor[1] = 0;
    wasRunning = false;
    everSelected = false;
    lastRan = {};
    outcomeSaid = false;
    Publish("");
}
} // namespace SuperiorVk
