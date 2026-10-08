// Reuse detail between frames, Vulkan (menu: "Reuse detail between frames"; ini DetailReuse*).
//
// The same feature as the D3D12 side (shaders/dlssnr/DlssNr_DetailReuse.inl), step for step: every other frame skips
// the model -- all passes -- and moves the previous frame's detail onto this frame's input; a full frame after a reused
// one hands the model vectors composed over both frames; Fill, Steady and the debug view as there. When it may run,
// the cadence, the constants and the status are shared (dlssnr/DlssNrDetailReuseHost.h); this file owns the images and
// records the dispatches (shaders/dlssnr/DlssNrDetailReuse_Vk.h).
//
// What differs from D3D12, and why:
//   - Layouts instead of resource states. Our own images are moved with Transition: SHADER_READ_ONLY_OPTIMAL to be
//     read, GENERAL to be written. The game's depth and motion are read in SHADER_READ_ONLY_OPTIMAL and never moved:
//     the layout a game hands inputs to DLSS in is not stated anywhere, and that is the one FSR2Feature_Vk and the
//     exposure read in DlssNrFeature_Vk.cpp already rely on in these games.
//   - No deferred release on D3D12's terms. An image a frame still in flight may use is not destroyed at once but
//     retired: destroyed kRetireFrames NR frames later. A working-size change needs none of it -- the resize block
//     has drained the device by the time Prepare runs.
//   - A dead device (ShutdownVk(false)) abandons every handle, as the rest of the Vulkan path does.
//
// Included inside DlssNrFeature_Vk.cpp's anonymous namespace, after OwnedImage / CreateImage / DestroyImage /
// Transition. EvaluateAtSeamVk calls BeforeModel and AfterModel around its pass loop, all under g_vkMutex.
namespace DetailReuseVk
{
// What EvaluateAtSeamVk knows about this frame: the shared facts, and the Vulkan objects.
struct Frame : DlssNrDetailReuse::HostFrame
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkFormat answerFormat = VK_FORMAT_UNDEFINED; // the model answers' format
    OwnedImage* modelInput = nullptr;            // what the first pass reads
    OwnedImage* output = nullptr;                // the first pass's answer (g_vk.output)
    NVSDK_NGX_Resource_VK* motion = nullptr;     // the game's
    NVSDK_NGX_Resource_VK* depth = nullptr;      // the game's
};

// What BeforeModel decided.
struct Plan
{
    bool active = false;     // reuse runs on this route now
    bool reused = false;     // the model is skipped: the answer is in Frame::output, in GENERAL
    bool resetModel = false; // start the model's history over (it last ran two frames ago, without composed vectors)
    NVSDK_NGX_Resource_VK* motion = nullptr; // the vectors the model evaluates with, and their subrect origin
    unsigned int motionBaseX = 0, motionBaseY = 0;
};

DlssNrDetailReuse::Host host { "DLSS-NR Vulkan detail reuse" };
std::unique_ptr<DlssNrDetailReuse_Vk> pass;
bool passFailed = false;

// Every processed frame saves its detail; two sets alternate, one read this frame (cur), the other written.
OwnedImage detail[2];      // work size, RGBA16F: answer - input, a = valid
OwnedImage colourDepth[2]; // work size, RGBA32F: that frame's input colour, a = far-is-zero depth
unsigned int cur = 0;
OwnedImage prevMotion; // work size, RG32F: the last reused frame's vectors as uv displacement
OwnedImage composed;   // the motion image's size, RG32F: raw vectors over two frames
OwnedImage estimate;   // work size, RGBA16F: moved detail with its trust (Fill and Steady)
OwnedImage steadied;   // work size, the answers' format: the steadied answer (Steady)
unsigned int workWidth = 0, workHeight = 0;

// How much of a frame arrived with no detail to move, for DlssNrDetailReuse::MotionGuard: the Coverage pass sums the
// trust over a small grid, copied into one of a ring of host-visible buffers and read kRetireFrames NR frames later --
// the same wait this file already uses for anything a frame in flight may still be reading. There is no submission of
// ours to hang a fence on here, so this is both simpler and slower to react than D3D12's fence: the pause starts about
// kRetireFrames rendered frames (a quarter of a second at 30 fps) after the motion does, where D3D12 takes two or
// three. One slot per frame of that wait, so every frame still gets a reading and the guard's resume wait is the 0.3 s
// it is documented as; fewer slots would halve the rate of readings and double the wait.
constexpr unsigned int kCoverageTiles = kDlssNrDetailReuseCoverageTiles;
constexpr unsigned int kCoverageBytes = kCoverageTiles * kCoverageTiles * 2 * sizeof(float);
constexpr unsigned int kCoverageSlots = 9; // one per frame of the wait; kRetireFrames is declared below, so the two
                                           // are tied together by a static_assert there

OwnedImage coverage;            // kCoverageTiles^2, RG32F: (sum of 1 - trust, pixels measured) per tile
bool measureWithSteady = false; // a held frame whose steadiness pass carries the coverage measurement

struct CoverageSlot
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    unsigned long long due = 0; // the NR frame from which it may be read
    bool pending = false;
};

CoverageSlot coverageSlots[kCoverageSlots];
unsigned int coverageNext = 0;
bool coverageFailed = false; // the grid or its buffers could not be made: measure no more, but leave Fill alone

// A depth-only view of the game's depth image, when the view the game gave NGX covers stencil too: a view with both
// aspects cannot be sampled (RDR2: D32_SFLOAT_S8_UINT, depth and stencil -- read as it was, the depth test saw garbage
// and dropped nearly every pixel). Only the view is ours (image and memory stay null, so DestroyImage frees just it).
// Made anew every NR frame and retired the same frame: the game may destroy its image and get the same handle value
// back for a new one, so a view kept across frames could outlive what it views.
OwnedImage depthOnly;
DlssNrDetailReuse_Vk::Read depthRead;     // this frame's depth read (PrepareDepth)
bool depthLogged = false, depthWarned = false;

// Images a frame still in flight may read, destroyed once that many NR frames have gone by.
constexpr unsigned long long kRetireFrames = 8;
static_assert(kCoverageSlots == kRetireFrames + 1, "one coverage readback slot per frame of the wait, so every frame "
                                                  "still gets a reading");
struct Retired
{
    OwnedImage image;
    unsigned long long at = 0;
};
std::vector<Retired> retired;

void Retire(OwnedImage& img)
{
    if (img.image != VK_NULL_HANDLE || img.view != VK_NULL_HANDLE)
        retired.push_back({ img, g_vk.frames + kRetireFrames });
    img = OwnedImage {};
}

void DestroyRetired(bool all)
{
    for (auto it = retired.begin(); it != retired.end();)
    {
        if (all || g_vk.frames >= it->at)
        {
            DestroyImage(it->image);
            it = retired.erase(it);
        }
        else
            ++it;
    }
}

void RetireExtras()
{
    Retire(estimate);
    Retire(steadied);
}

// A copy into a coverage buffer may be in flight, so the buffers go the way the images do: put aside now, unmapped and
// destroyed once kRetireFrames NR frames have gone by (as DlssNr_ExposureCalibrate_Vk.inl retires its own readbacks).
struct RetiredBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool mapped = false;
    unsigned long long at = 0;
};
std::vector<RetiredBuffer> retiredBuffers;

void DestroyRetiredBuffers(bool all)
{
    for (auto it = retiredBuffers.begin(); it != retiredBuffers.end();)
    {
        if (all || g_vk.frames >= it->at)
        {
            if (it->mapped && it->memory != VK_NULL_HANDLE)
                vkUnmapMemory(g_vk.device, it->memory);
            if (it->buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(g_vk.device, it->buffer, nullptr);
            if (it->memory != VK_NULL_HANDLE)
                vkFreeMemory(g_vk.device, it->memory, nullptr);
            it = retiredBuffers.erase(it);
        }
        else
            ++it;
    }
}

void RetireCoverageBuffers()
{
    for (CoverageSlot& slot : coverageSlots)
    {
        if (slot.buffer != VK_NULL_HANDLE || slot.memory != VK_NULL_HANDLE)
            retiredBuffers.push_back({ slot.buffer, slot.memory, slot.mapped != nullptr, g_vk.frames + kRetireFrames });
        slot = CoverageSlot {};
    }
    coverageNext = 0;
}

// Built the first frame the measurement is wanted; without it reuse simply never pauses. Any slots from before are put
// aside first: overwriting their handles would leak them, and a slot still marked pending would be read as if the new,
// unwritten buffer held a measurement.
bool MakeCoverage(const Frame& f)
{
    RetireCoverageBuffers();
    if (!CreateImage(coverage, kCoverageTiles, kCoverageTiles, VK_FORMAT_R32G32_SFLOAT, true))
        return false;

    for (CoverageSlot& slot : coverageSlots)
    {
        VkBufferCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = kCoverageBytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(f.device, &info, nullptr, &slot.buffer) != VK_SUCCESS)
        {
            slot.buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(f.device, slot.buffer, &req);

        VkMemoryAllocateInfo alloc {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = FindMemoryTypeIndex(
            req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(f.device, &alloc, nullptr, &slot.memory) != VK_SUCCESS)
        {
            slot.memory = VK_NULL_HANDLE;
            return false;
        }
        if (vkBindBufferMemory(f.device, slot.buffer, slot.memory, 0) != VK_SUCCESS ||
            vkMapMemory(f.device, slot.memory, 0, kCoverageBytes, 0, &slot.mapped) != VK_SUCCESS)
        {
            slot.mapped = nullptr;
            return false;
        }
    }
    return true;
}

// Every slot whose copy is certainly done: the share of that frame with no detail to move, to the guard.
void CollectCoverage()
{
    for (CoverageSlot& slot : coverageSlots)
    {
        if (!slot.pending || slot.mapped == nullptr || g_vk.frames < slot.due)
            continue;
        const auto* tiles = static_cast<const float*>(slot.mapped);
        double dropped = 0.0, pixels = 0.0;
        for (unsigned int i = 0; i < kCoverageTiles * kCoverageTiles; ++i)
        {
            // A tile whose numbers are not finite is left out rather than poisoning the frame's share.
            if (std::isfinite(tiles[2 * i]) && std::isfinite(tiles[2 * i + 1]))
            {
                dropped += tiles[2 * i];
                pixels += tiles[2 * i + 1];
            }
        }
        if (pixels > 0.0)
            host.RecordDropped((float) std::clamp(dropped / pixels, 0.0, 1.0));
        slot.pending = false;
    }
}

void RetireAll()
{
    for (unsigned int i = 0; i < 2; ++i)
    {
        Retire(detail[i]);
        Retire(colourDepth[i]);
    }
    Retire(prevMotion);
    Retire(composed);
    RetireExtras();
    Retire(coverage);
    RetireCoverageBuffers();
    workWidth = workHeight = 0;
    host.cadence.Drop();
}

// The pass, built on first use; its shader writes storage images without a format, which the device must allow. Null
// when it is ready, else why not.
const char* ShaderReady(const Frame& f)
{
    if (pass != nullptr)
        return nullptr;
    if (!DlssNr::VkExt::WritesWithoutFormat(f.device))
    {
        if (!passFailed)
            LOG_WARN("DLSS-NR Vulkan detail reuse: this device was created without shaderStorageImageWriteWithoutFormat "
                     "(NR was off when the game created it, or the device does not offer it); every frame runs the "
                     "model");
        passFailed = true;
        return "needs a restart with NR on (the device lacks a feature it needs)";
    }
    if (!passFailed)
    {
        pass = std::make_unique<DlssNrDetailReuse_Vk>("DLSS-NR detail reuse", f.device, f.physicalDevice);
        if (!pass->Ready())
        {
            pass.reset();
            passFailed = true;
            LOG_WARN("DLSS-NR Vulkan detail reuse: the reuse pass could not be built; every frame runs the model");
        }
    }
    return pass != nullptr ? nullptr : "its shader could not be built";
}

// Allocates or retires the images for this frame. Returns whether reuse can run. keep: reuse is held off for now
// (frame generation, the frame rate), so the images stay.
bool Prepare(const Frame& f, bool wanted, bool keep, float steady, float fill, bool measure)
{
    if (wanted && (!detail[0].Valid() || workWidth != f.workWidth || workHeight != f.workHeight))
    {
        RetireAll();
        bool allocated = true;
        for (unsigned int i = 0; i < 2; ++i)
            allocated = allocated &&
                        CreateImage(detail[i], f.workWidth, f.workHeight, VK_FORMAT_R16G16B16A16_SFLOAT, false) &&
                        CreateImage(colourDepth[i], f.workWidth, f.workHeight, VK_FORMAT_R32G32B32A32_SFLOAT, false);
        allocated = allocated && CreateImage(prevMotion, f.workWidth, f.workHeight, VK_FORMAT_R32G32_SFLOAT, false);
        if (!allocated)
        {
            RetireAll();
            host.AllocationFailed(f, false);
            LOG_ERROR("DLSS-NR Vulkan detail reuse: could not allocate its history images; every frame runs the model");
            return false;
        }
        workWidth = f.workWidth;
        workHeight = f.workHeight;
        cur = 0;
        host.TexturesRebuilt();
        LOG_INFO("DLSS-NR Vulkan detail reuse: on, model {}x{}", f.workWidth, f.workHeight);
    }
    else if (!wanted)
    {
        if (detail[0].Valid() && !keep)
            RetireAll();
        return false;
    }

    // The composed vectors go to the model in the motion subrect's place, so they are sized like the motion image: a
    // render-size change moves the subrect inside it without a rebuild. Handed to the model, so its NGX wrapper is used.
    const unsigned int wantWidth = std::max(f.motionAllocWidth, f.motionWidth);
    const unsigned int wantHeight = std::max(f.motionAllocHeight, f.motionHeight);
    if (!composed.Valid() || composed.width != wantWidth || composed.height != wantHeight)
    {
        Retire(composed);
        if (!CreateImage(composed, wantWidth, wantHeight, VK_FORMAT_R32G32_SFLOAT, false))
        {
            RetireAll();
            host.AllocationFailed(f, false);
            LOG_ERROR("DLSS-NR Vulkan detail reuse: could not allocate its motion image; every frame runs the model");
            return false;
        }
    }

    // The coverage grid and its readback ring, only while the pause on fast motion is on.
    // A failure here disables the measurement alone: host.AllocationFailed would also turn off Fill and steadiness,
    // which share the estimate image, and losing fill is a visible loss for a measurement nobody asked for.
    if (measure && !coverage.Valid() && !coverageFailed && !MakeCoverage(f))
    {
        Retire(coverage);
        RetireCoverageBuffers();
        coverageFailed = true;
        LOG_WARN("DLSS-NR Vulkan detail reuse: could not allocate the coverage grid; it will not pause on fast motion");
    }
    else if (!measure && coverage.Valid())
    {
        Retire(coverage);
        RetireCoverageBuffers();
    }

    const bool needEstimate = steady > 0.0f || fill > 0.0f || (measure && coverage.Valid());
    if (needEstimate && !estimate.Valid() && !host.ExtrasFailed())
    {
        if (!CreateImage(estimate, f.workWidth, f.workHeight, VK_FORMAT_R16G16B16A16_SFLOAT, false))
        {
            host.AllocationFailed(f, true);
            LOG_WARN("DLSS-NR Vulkan detail reuse: could not allocate the estimate image; no fill and no steadiness");
        }
    }
    else if (!needEstimate && estimate.Valid())
    {
        Retire(estimate);
    }
    if (steady > 0.0f && estimate.Valid() && !host.ExtrasFailed() &&
        (!steadied.Valid() || steadied.format != f.answerFormat))
    {
        Retire(steadied);
        if (!CreateImage(steadied, f.workWidth, f.workHeight, f.answerFormat, false))
        {
            host.AllocationFailed(f, true);
            LOG_WARN("DLSS-NR Vulkan detail reuse: could not allocate the steadiness image; full frames stay as the "
                     "model made them");
        }
    }
    else if (steady <= 0.0f && steadied.Valid())
    {
        Retire(steadied);
    }
    return true;
}

// The game's image as a sampled read (see the top of the file for the layout).
DlssNrDetailReuse_Vk::Read Game(const NVSDK_NGX_Resource_VK* res)
{
    return { res != nullptr ? res->Resource.ImageViewInfo.ImageView : VK_NULL_HANDLE,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
}

// This frame's read of the game's depth: its own view when that is depth only, else a depth-only view of its image
// made for this frame. False when the depth cannot be read (reuse then does not run this frame).
bool PrepareDepth(const Frame& f)
{
    Retire(depthOnly); // last frame's; destroyed once the frames that read it are done
    depthRead = {};
    if (f.depth == nullptr)
        return false;

    const auto& info = f.depth->Resource.ImageViewInfo;
    if (info.SubresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT)
    {
        depthRead = Game(f.depth);
        return depthRead.view != VK_NULL_HANDLE;
    }
    if (info.Image == VK_NULL_HANDLE || (info.SubresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0)
        return false;

    VkImageViewCreateInfo view {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = info.Image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.Format;
    view.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, info.SubresourceRange.baseMipLevel, 1,
                              info.SubresourceRange.baseArrayLayer, 1 };

    if (vkCreateImageView(f.device, &view, nullptr, &depthOnly.view) != VK_SUCCESS)
    {
        depthOnly.view = VK_NULL_HANDLE;
        if (!depthWarned)
        {
            depthWarned = true;
            LOG_WARN("DLSS-NR Vulkan detail reuse: could not make a depth-only view of the game's depth");
        }
        return false;
    }

    if (!depthLogged)
    {
        depthLogged = true;
        LOG_INFO("DLSS-NR Vulkan detail reuse: the game's depth view covers aspects {:#x}; reading it through a "
                 "depth-only view", (uint32_t) info.SubresourceRange.aspectMask);
    }
    depthRead = { depthOnly.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    return true;
}

DlssNrDetailReuse_Vk::Read GameDepth(const Frame&) { return depthRead; }

DlssNrDetailReuse_Vk::Read Own(const Frame& f, OwnedImage* img)
{
    if (img == nullptr || !img->Valid())
        return {};
    Transition(f.cmd, *img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return { img->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
}

VkImageView Written(const Frame& f, OwnedImage* img)
{
    if (img == nullptr || !img->Valid())
        return VK_NULL_HANDLE;
    Transition(f.cmd, *img, VK_IMAGE_LAYOUT_GENERAL);
    return img->view;
}

// One dispatch: t0-t5 as given, u0 and u1 written. Reads are listed before writes so no image is both.
bool Run(const Frame& f, DlssNrDetailReuseMode mode, unsigned int width, unsigned int height,
         const DlssNrDetailReuse_Vk::Read (&reads)[DlssNrDetailReuse_Vk::kReads], OwnedImage* target,
         OwnedImage* second)
{
    host.params.Mode = mode;
    const VkImageView u0 = Written(f, target);
    const VkImageView u1 = Written(f, second);
    return u0 != VK_NULL_HANDLE && pass->Dispatch(f.cmd, host.params, width, height, reads, u0, u1);
}

// How much of the frame the estimate has no detail for, into the grid and on to a readback slot. The estimate must be
// readable.
void MeasureCoverage(const Frame& f)
{
    if (!coverage.Valid())
        return;
    const DlssNrDetailReuse_Vk::Read reads[DlssNrDetailReuse_Vk::kReads] = { {}, Own(f, &estimate), {}, {}, {} };
    // One 8x8 thread group per tile of the grid, whatever the working size.
    const unsigned int threads = kCoverageTiles * 8;
    if (!Run(f, DlssNrDetailReuse_Coverage, threads, threads, reads, &coverage, nullptr))
        return;

    CoverageSlot* slot = nullptr;
    for (unsigned int i = 0; i < kCoverageSlots; ++i)
    {
        CoverageSlot& candidate = coverageSlots[(coverageNext + i) % kCoverageSlots];
        if (!candidate.pending && candidate.mapped != nullptr)
        {
            slot = &candidate;
            coverageNext = (coverageNext + i + 1) % kCoverageSlots;
            break;
        }
    }
    if (slot == nullptr) // every slot waiting: this frame is not measured, which the guard reads as no reading
        return;

    Transition(f.cmd, coverage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { kCoverageTiles, kCoverageTiles, 1 };
    vkCmdCopyImageToBuffer(f.cmd, coverage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot->buffer, 1, &region);

    VkBufferMemoryBarrier toHost {};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = slot->buffer;
    toHost.offset = 0;
    toHost.size = kCoverageBytes;
    vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0,
                         nullptr);

    slot->due = g_vk.frames + kRetireFrames;
    slot->pending = true;
}

// The moved detail with its trust into `estimate`, then `mode` (Fill or Steady) from it into `target`. measure: also
// read off how much of the frame had no detail to move, while the estimate is readable anyway.
bool EstimateThen(const Frame& f, DlssNrDetailReuseMode mode, OwnedImage* second, OwnedImage* target, bool measure)
{
    {
        const DlssNrDetailReuse_Vk::Read reads[DlssNrDetailReuse_Vk::kReads] = {
            Own(f, f.modelInput), Own(f, &detail[cur]), Own(f, &colourDepth[cur]), Game(f.motion), GameDepth(f)
        };
        if (!Run(f, DlssNrDetailReuse_Estimate, f.workWidth, f.workHeight, reads, &estimate, nullptr))
            return false;
    }
    if (measure)
        MeasureCoverage(f);
    const DlssNrDetailReuse_Vk::Read reads[DlssNrDetailReuse_Vk::kReads] = {
        Own(f, f.modelInput), Own(f, second), Own(f, &estimate), {}, GameDepth(f)
    };
    return Run(f, mode, f.workWidth, f.workHeight, reads, target, nullptr);
}

// A full frame while reuse is paused: nothing is reused, but the measurement carries on so the guard sees the picture
// calm down.
void MeasureOnly(const Frame& f)
{
    if (!coverage.Valid() || !estimate.Valid())
        return;
    const DlssNrDetailReuse_Vk::Read reads[DlssNrDetailReuse_Vk::kReads] = { Own(f, f.modelInput), Own(f, &detail[cur]),
                                                                             Own(f, &colourDepth[cur]), Game(f.motion),
                                                                             GameDepth(f) };
    if (Run(f, DlssNrDetailReuse_Estimate, f.workWidth, f.workHeight, reads, &estimate, nullptr))
        MeasureCoverage(f);
}

Plan BeforeModel(const Frame& f)
{
    Plan plan;
    plan.motion = f.motion;
    plan.motionBaseX = f.motionBaseX;
    plan.motionBaseY = f.motionBaseY;

    DestroyRetired(false);
    DestroyRetiredBuffers(false);
    depthRead = {};
    // Anything the GPU has finished measuring, before this frame records more: the guard reads it in Gate.
    CollectCoverage();

    // Asked only while reuse is on and nothing else holds it off: the shader, then this frame's depth.
    const DlssNrDetailReuse::Wanted want = host.Gate(f, [&]() -> const char* {
        if (const char* why = ShaderReady(f))
            return why;
        return PrepareDepth(f) ? nullptr : "the game's depth cannot be read";
    });
    plan.active = Prepare(f, want.wanted, want.keep, want.steady, want.fill, want.measure);

    // Once: what the game's depth and motion are, since they are read here as sampled images in a layout nobody states.
    // A depth view that includes the stencil aspect cannot be sampled.
    static bool saidGuides = false;
    if (plan.active && !saidGuides && f.depth != nullptr && f.motion != nullptr)
    {
        saidGuides = true;
        const auto& d = f.depth->Resource.ImageViewInfo;
        const auto& m = f.motion->Resource.ImageViewInfo;
        LOG_INFO("DLSS-NR Vulkan detail reuse: game depth format {} aspect {:#x} {}x{} (subrect {}x{} at {},{}, {}), "
                 "motion format {} aspect {:#x} {}x{} (subrect {}x{} at {},{}, game scale {:.3f}, {:.3f})",
                 (int) d.Format, (uint32_t) d.SubresourceRange.aspectMask, d.Width, d.Height, f.depthWidth,
                 f.depthHeight, f.depthBaseX, f.depthBaseY, f.depthInverted ? "inverted" : "standard", (int) m.Format,
                 (uint32_t) m.SubresourceRange.aspectMask, m.Width, m.Height, f.motionWidth, f.motionHeight,
                 f.motionBaseX, f.motionBaseY, f.mvScaleX, f.mvScaleY);
    }
    host.Decide(f, plan.active, want);
    const auto& decision = host.decision;

    if (decision.kind == DlssNrDetailReuse::Kind::Reuse)
    {
        if (want.fill > 0.0f && estimate.Valid())
        {
            // Moved detail with its trust first; Fill composes it and fills where it was dropped.
            plan.reused = EstimateThen(f, DlssNrDetailReuse_Fill, &estimate, f.output, want.measure);
        }
        else
        {
            // Fill is off. The frame is still measured when asked for, in a pass of its own, so the reuse below stays
            // exactly what it was.
            if (want.measure)
                MeasureOnly(f);

            const DlssNrDetailReuse_Vk::Read reads[DlssNrDetailReuse_Vk::kReads] = {
                Own(f, f.modelInput), Own(f, &detail[cur]), Own(f, &colourDepth[cur]), Game(f.motion), GameDepth(f)
            };
            plan.reused = Run(f, DlssNrDetailReuse_Reproject, f.workWidth, f.workHeight, reads, f.output, nullptr);
        }

        if (plan.reused)
        {
            // Kept for the next full frame, which composes it with its own vectors for the model's history. Without
            // it that frame cannot compose, so the cadence starts over and the model's history is reset instead.
            const DlssNrDetailReuse_Vk::Read reads[DlssNrDetailReuse_Vk::kReads] = {
                Game(f.motion), {}, {}, Game(f.motion), {}
            };
            if (!Run(f, DlssNrDetailReuse_SaveMotion, f.workWidth, f.workHeight, reads, &prevMotion, nullptr))
                host.cadence.Drop();
        }
        else
        {
            // Nothing was written: run the model this frame and start the cadence over.
            host.cadence.ReuseFailed();
            static bool warnedReuse = false;
            if (!warnedReuse)
            {
                warnedReuse = true;
                LOG_WARN("DLSS-NR Vulkan detail reuse: the reuse pass could not be recorded; running the model");
            }
        }
    }

    // A full frame while reuse is paused: keep measuring, so the guard knows when the picture has calmed down. With
    // steadiness on, AfterModel's own Estimate carries the measurement instead of running a second identical one.
    measureWithSteady = false;
    if (!plan.reused && plan.active && want.measure && want.hold && decision.historyUsable)
    {
        if (host.params.Steady > 0.0f && estimate.Valid() && steadied.Valid())
            measureWithSteady = true;
        else
            MeasureOnly(f);
    }

    // A full frame after a reused frame: the model last ran two frames ago, so it gets the vectors over both.
    bool composedReady = false;
    if (!plan.reused && decision.kind == DlssNrDetailReuse::Kind::Full && decision.composeMotion)
    {
        const DlssNrDetailReuse_Vk::Read reads[DlssNrDetailReuse_Vk::kReads] = {
            Game(f.motion), {}, {}, Game(f.motion), Own(f, &prevMotion)
        };
        if (Run(f, DlssNrDetailReuse_Compose, f.motionWidth, f.motionHeight, reads, &composed, nullptr))
        {
            // The model reads it like its other inputs.
            Transition(f.cmd, composed, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            composedReady = true;
            plan.motion = &composed.ngx;
            plan.motionBaseX = plan.motionBaseY = 0;
        }
    }

    plan.resetModel = host.Finish(f, plan.active, plan.reused, composedReady);
    return plan;
}

// After the model passes, or the reuse. succeeded: *finalAnswer holds a valid answer. May replace it with the steadied
// one.
void AfterModel(const Frame& f, const Plan& plan, bool succeeded, OwnedImage*& finalAnswer)
{
    const auto& decision = host.decision;

    // A full frame with last frame's history: pull the model's new detail toward the moved previous detail, as far as
    // that is trusted, so this frame and the reused one next to it differ less.
    if (!plan.reused && succeeded && finalAnswer != nullptr && decision.historyUsable && host.params.Steady > 0.0f &&
        estimate.Valid() && steadied.Valid() &&
        EstimateThen(f, DlssNrDetailReuse_Steady, finalAnswer, &steadied, measureWithSteady))
        finalAnswer = &steadied;

    // Every processed frame keeps its detail (answer - input), input colour and depth for the next frame. Not a reused
    // frame painted by the debug view: its colours are not detail.
    if (decision.captureHistory)
    {
        bool captured = false;
        if (succeeded && finalAnswer != nullptr && depthRead.view != VK_NULL_HANDLE &&
            !(plan.reused && host.params.DebugView != 0))
        {
            const unsigned int write = 1u - cur;
            const DlssNrDetailReuse_Vk::Read reads[DlssNrDetailReuse_Vk::kReads] = {
                Own(f, f.modelInput), Own(f, finalAnswer), GameDepth(f), {}, {}
            };
            captured = Run(f, DlssNrDetailReuse_Capture, f.workWidth, f.workHeight, reads, &detail[write],
                           &colourDepth[write]);
            if (captured)
                cur = write;
        }
        host.cadence.Captured(captured);
    }
}

void RecordGpuTime(double ms) { host.RecordGpuTime(ms); }

DlssNr::DetailReuseInfo Status() { return host.Status(); }

// For the menu, without g_vkMutex.
DlssNr::DetailReuseInfo Published() { return host.Published(DlssNr::VkFrameClock()); }

// Shutdown. deviceAlive: the device is idle and everything is destroyed; otherwise it is gone and took every handle
// with it, so they are only forgotten.
void Release(bool deviceAlive)
{
    OwnedImage* const images[] = { &detail[0], &detail[1], &colourDepth[0], &colourDepth[1], &prevMotion, &composed,
                                   &estimate, &steadied, &depthOnly, &coverage };
    for (OwnedImage* img : images)
    {
        if (deviceAlive)
            DestroyImage(*img);
        *img = OwnedImage {};
    }
    if (deviceAlive)
    {
        RetireCoverageBuffers();
        DestroyRetiredBuffers(true);
        DestroyRetired(true);
        pass.reset();
    }
    else
    {
        // The device is gone: its images, buffers and mappings went with it, so they are abandoned, never destroyed.
        for (CoverageSlot& slot : coverageSlots)
            slot = CoverageSlot {};
        coverageNext = 0;
        retiredBuffers.clear();
        retired.clear();
        pass.release(); // its pipeline and descriptors went with the device
    }
    coverageFailed = false;
    measureWithSteady = false;
    passFailed = false;
    depthRead = {};
    workWidth = workHeight = 0;
    cur = 0;
    host.Reset();
}
} // namespace DetailReuseVk
