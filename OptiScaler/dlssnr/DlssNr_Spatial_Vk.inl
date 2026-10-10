// Compress screen edges, Vulkan (menu: NR Input, "Compress screen edges"; ini [DlssNr] Spatial*).
//
// The same feature as the D3D12 side (shaders/dlssnr/DlssNr_Spatial.inl), step for step: the model works on a packed
// picture whose middle stays 1:1 and whose edges are squeezed (shaders/dlssnr/DlssNr_Spatial.h has the layout), the
// proxy, depth and motion are packed after the encode, and the packed model input and the model's answer are unpacked
// to the ordinary grid for the resolve, the SGSR1 up-leg and the supersample down-leg. Motion is packed by its end
// points, so the model gets a scale of 1. The dispatches are shaders/dlssnr/DlssNrSpatial_Vk.h.
//
// What differs from D3D12, and why:
//   - The ordinary grid is the one this backend works on without compression: the model resolution as a plain rounding
//     of the frame (this backend does not round to 16 pixels). The packed picture is on the 16-pixel grid either way.
//   - Layouts instead of resource states. Our own images are moved with Transition; the game's depth and motion are
//     read in SHADER_READ_ONLY_OPTIMAL and never moved (see DlssNr_DetailReuse_Vk.inl for why), through a depth-only view
//     when the game's depth view covers stencil too. The packed motion is RGBA32F: R32G32 is not a storage format every
//     device offers, and this pass asks for no optional feature.
//   - Surfaces are made in the resize block, which has drained the device; nothing here frees an image a frame in flight
//     may still read, except the per-frame depth-only view, which is retired like Reuse detail's.
//   - A dead device (ShutdownVk(false)) abandons every handle, as the rest of the Vulkan path does.
//   - There is no driver-proxy backend on Vulkan, so only a failure turns compression off.
//
// Included inside DlssNrFeature_Vk.cpp's anonymous namespace, after DlssNr_DetailReuse_Vk.inl. EvaluateAtSeamVk calls
// Begin where it decides the frame and the rest around the model, all under g_vkMutex. The menu reads Published().
namespace EdgeCompressionVk
{
namespace Sp = DlssNr::Spatial;

Sp::Tracker tracker;
std::unique_ptr<DlssNrSpatial_Vk> pass;
bool passFailed = false;

OwnedImage color;  // the packed colour (model input), RGBA16F
OwnedImage depth;  // the packed depth, R32F
OwnedImage motion; // the packed motion, RGBA32F
OwnedImage proxy;  // the packed model input unpacked to the ordinary grid, RGBA16F
OwnedImage answer; // the model's answer unpacked the same way
OwnedImage proxyNative;  // supersampling only: both averaged down to native
OwnedImage answerNative;
std::unique_ptr<OS_Vk> proxyDown; // the down-leg for the proxy; the answer goes through g_vk.superDown, the same filter
unsigned int madeFor[6] = {}; // packed w/h, ordinary w/h, native w/h the images above were made for
bool madeSuper = false;
bool madeActive = false;
bool madeReplace = false; // the reference picture (proxySmall below 100%/above) was kept for the Replace curves

OwnedImage depthOnly; // a depth-only view of the game's depth, when its own view covers stencil too
bool depthWarned = false, depthLogged = false;

std::mutex publishedLock;
Sp::Published published;

const Sp::Layout& Layout() { return tracker.layout(); }

void Publish(const Sp::Layout& l, Sp::Status status)
{
    Sp::Published p = Sp::Publish(l, status, true);
    std::lock_guard<std::mutex> lock(publishedLock);
    published = p;
}

Sp::Published Published()
{
    std::lock_guard<std::mutex> lock(publishedLock);
    return published;
}

// What makes last frame's packed picture a different one besides the layout: the formats and sizes that are packed.
uint64_t SignatureOf(VkFormat colourFormat, const NVSDK_NGX_Resource_VK* depthRes,
                     const NVSDK_NGX_Resource_VK* motionRes)
{
    uint64_t hash = Sp::Mix(0, (uint64_t) colourFormat);
    for (const NVSDK_NGX_Resource_VK* res : { depthRes, motionRes })
    {
        const auto& info = res->Resource.ImageViewInfo;
        for (const uint64_t v : { (uint64_t) info.Format, (uint64_t) info.Width, (uint64_t) info.Height })
            hash = Sp::Mix(hash, v);
    }
    return hash;
}

// Decides this frame. Returns whether the picture is packed. resetHistory is set when the packed picture is a different
// one from last frame's.
bool Begin(const Config& cfg, uint32_t width, uint32_t height, float scale, uint64_t signature, bool& resetHistory)
{
    const Sp::Settings settings = Sp::ReadSettings(cfg);
    const Sp::Tracker::Frame frame = tracker.Begin(settings, width, height, scale, false, false, signature);
    const Sp::Layout& next = tracker.layout();

    if (frame.resetHistory)
        resetHistory = true;

    Publish(next, frame.status);

    if (frame.changed)
    {
        if (frame.active)
            LOG_INFO("DLSS-NR Vulkan compress screen edges: on, the model works on {}x{} instead of {}x{} ({:.0f}% of "
                     "the pixels), middle {:.0f}%x{:.0f}% of the frame",
                     next.modelW, next.modelH, next.ordinaryW, next.ordinaryH,
                     100.0 * next.modelW * next.modelH / (double) (next.ordinaryW * (double) next.ordinaryH),
                     100.0 * (next.centerBounds.right - next.centerBounds.left),
                     100.0 * (next.centerBounds.bottom - next.centerBounds.top));
        else if (settings.enabled)
            LOG_INFO("DLSS-NR Vulkan compress screen edges: not applied ({})", Sp::Describe(frame.status));
        else
            LOG_INFO("DLSS-NR Vulkan compress screen edges: off");
    }

    return frame.active;
}

// Compression turned itself off; NR carries on from the next frame without it.
void TurnOff(Sp::Status why)
{
    tracker.TurnOff(why);
    Publish(tracker.layout(), why);
    LOG_WARN("DLSS-NR Vulkan compress screen edges turned itself off: {}; NR continues without it", Sp::Describe(why));
}

void Retry() { tracker.Retry(); }

// Whether the images made last time are not the ones this layout needs.
bool Stale(bool spatial, const Sp::Layout& l, bool supersample, bool replace)
{
    if (!spatial)
        return color.Valid() || madeActive;

    const unsigned int want[6] = { l.modelW, l.modelH, l.ordinaryW, l.ordinaryH, l.nativeW, l.nativeH };
    return !madeActive || !color.Valid() || !std::equal(std::begin(want), std::end(want), std::begin(madeFor)) ||
           madeSuper != supersample || madeReplace != replace;
}

void DestroyImages()
{
    for (OwnedImage* img : { &color, &depth, &motion, &proxy, &answer, &proxyNative, &answerNative })
        DestroyImage(*img);

    for (unsigned int& v : madeFor)
        v = 0;

    madeActive = false;
}

// The images for this layout, in the resize block (the device is drained). False if one could not be made.
bool CreateImages(const Sp::Layout& l, bool supersample, bool replace)
{
    DestroyImages();
    const VkFormat working = VK_FORMAT_R16G16B16A16_SFLOAT;

    const bool ok = CreateImage(color, l.modelW, l.modelH, working, true) &&
                    CreateImage(depth, l.modelW, l.modelH, VK_FORMAT_R32_SFLOAT, true) &&
                    CreateImage(motion, l.modelW, l.modelH, VK_FORMAT_R32G32B32A32_SFLOAT, true) &&
                    CreateImage(proxy, l.ordinaryW, l.ordinaryH, working, true) &&
                    CreateImage(answer, l.ordinaryW, l.ordinaryH, working, true) &&
                    (!supersample || (CreateImage(proxyNative, l.nativeW, l.nativeH, working, true) &&
                                      CreateImage(answerNative, l.nativeW, l.nativeH, working, true)));

    if (!ok)
    {
        DestroyImages();
        return false;
    }

    const unsigned int want[6] = { l.modelW, l.modelH, l.ordinaryW, l.ordinaryH, l.nativeW, l.nativeH };
    std::copy(std::begin(want), std::end(want), std::begin(madeFor));
    madeSuper = supersample;
    madeReplace = replace;
    madeActive = true;
    return true;
}

// The pass, made on first use; not retried once it failed.
bool PassReady(VkDevice device, VkPhysicalDevice physicalDevice)
{
    if (!pass && !passFailed)
    {
        pass = std::make_unique<DlssNrSpatial_Vk>("Compress screen edges", device, physicalDevice);

        if (!pass->Ready())
        {
            pass.reset();
            passFailed = true;
            LOG_WARN("DLSS-NR Vulkan: the Compress screen edges passes could not be built; NR runs on the whole picture");
        }
    }

    return pass != nullptr;
}

// This frame's read of the game's depth: its own view when that is depth only, else a depth-only view of its image made
// for this frame (a view with both aspects cannot be sampled; RDR2's is D32_SFLOAT_S8_UINT). Retired the next frame,
// since the game may reuse a handle value for another image.
DlssNrSpatial_Vk::Read DepthRead(VkDevice device, const NVSDK_NGX_Resource_VK* res)
{
    DetailReuseVk::Retire(depthOnly);

    const auto& info = res->Resource.ImageViewInfo;
    if (info.SubresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT)
        return { info.ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

    if (info.Image == VK_NULL_HANDLE || (info.SubresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0)
        return {};

    VkImageViewCreateInfo view {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = info.Image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.Format;
    view.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, info.SubresourceRange.baseMipLevel, 1,
                              info.SubresourceRange.baseArrayLayer, 1 };

    if (vkCreateImageView(device, &view, nullptr, &depthOnly.view) != VK_SUCCESS)
    {
        depthOnly.view = VK_NULL_HANDLE;
        if (!depthWarned)
        {
            depthWarned = true;
            LOG_WARN("DLSS-NR Vulkan compress screen edges: could not make a depth-only view of the game's depth");
        }
        return {};
    }

    if (!depthLogged)
    {
        depthLogged = true;
        LOG_INFO("DLSS-NR Vulkan compress screen edges: the game's depth view covers aspects {:#x}; reading it through "
                 "a depth-only view", (uint32_t) info.SubresourceRange.aspectMask);
    }

    return { depthOnly.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
}

// Shutdown. deviceAlive: the device is idle and everything is destroyed; otherwise it is gone and took every handle with
// it, so they are only forgotten.
void Release(bool deviceAlive)
{
    if (deviceAlive)
    {
        DestroyImages();
        DestroyImage(depthOnly);
        pass.reset();
        proxyDown.reset();
    }
    else
    {
        for (OwnedImage* img : { &color, &depth, &motion, &proxy, &answer, &proxyNative, &answerNative, &depthOnly })
            *img = OwnedImage {};
        pass.release(); // its pipelines and descriptors went with the device
        proxyDown.release();
    }

    for (unsigned int& v : madeFor)
        v = 0;

    madeActive = false;
    madeReplace = false;
    passFailed = false;
    tracker.Reset();
    std::lock_guard<std::mutex> lock(publishedLock);
    published = {};
}
} // namespace EdgeCompressionVk
