// "Tune for this scene", Vulkan side: the GPU resources, the copies and the stats pass. The run itself -- the menu's
// requests, the sweep's EV per evaluation, the readback bookkeeping, the log -- is shared with D3D12
// (shaders/dlssnr/DlssNr_ExposureCalibrate_Run.h, which also describes the per-evaluation flow);
// DlssNr_ExposureCalibrate.h has the sweep and the why. Included by DlssNrFeature_Vk.cpp inside its anonymous
// namespace, after the image helpers.
//
// Differences from D3D12:
// - The copies are made by the stats shader's own copy mode (a straight load and store over the frame), not a
//   transfer: the game's image may not have been created for transfers, and a shader read needs nothing it does not
//   already have. They are always RGBA16F, whatever the game's format.
// - What a run gives back is destroyed kCalVkRetireFrames evaluations later, when the GPU can no longer be reading it
//   (Tick), the way D3D12 parks it.

namespace Cal = DlssNrExposureCalibrate;

// The stats grid (Cal::ReduceGrid has the layout): 256x64 RGBA32F, copied into a tightly packed buffer.
constexpr uint32_t kCalVkGridWidth = Cal::kGridTiles * Cal::kGridColumns;
constexpr uint32_t kCalVkGridHeight = Cal::kGridTiles;
constexpr VkDeviceSize kCalVkGridBytes = (VkDeviceSize) Cal::kGridRowFloats * sizeof(float) * Cal::kGridTiles;
// Frames in flight here are three; the readback delay (which allows for frame generation) is plenty.
constexpr unsigned long long kCalVkRetireFrames = Cal::kReadDelay;
constexpr VkFormat kCalVkCopyFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

class CalibrationVk final : public Cal::Backend
{
  public:
    OwnedImage input[2];
    OwnedImage output[2];
    bool inputReady[2] = {};
    bool outputReady[2] = {};
    OwnedImage grid;
    VkBuffer readback[Cal::kRing] = {};
    VkDeviceMemory readbackMemory[Cal::kRing] = {};
    void* mapped[Cal::kRing] = {};

    bool Held() const override { return grid.Valid(); }

    // HOST_COHERENT and mapped for as long as it lives, as the meter's ring is.
    bool Create() override
    {
        if (g_vk.device == VK_NULL_HANDLE ||
            !CreateImage(grid, kCalVkGridWidth, kCalVkGridHeight, VK_FORMAT_R32G32B32A32_SFLOAT, true))
        {
            Release();
            return false;
        }

        for (unsigned int i = 0; i < Cal::kRing; ++i)
        {
            VkBufferCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = kCalVkGridBytes;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(g_vk.device, &info, nullptr, &readback[i]) != VK_SUCCESS)
            {
                readback[i] = VK_NULL_HANDLE;
                Release();
                return false;
            }

            VkMemoryRequirements req {};
            vkGetBufferMemoryRequirements(g_vk.device, readback[i], &req);

            VkMemoryAllocateInfo alloc {};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = FindMemoryTypeIndex(
                req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            if (alloc.memoryTypeIndex == UINT32_MAX ||
                vkAllocateMemory(g_vk.device, &alloc, nullptr, &readbackMemory[i]) != VK_SUCCESS)
            {
                readbackMemory[i] = VK_NULL_HANDLE;
                Release();
                return false;
            }

            if (vkBindBufferMemory(g_vk.device, readback[i], readbackMemory[i], 0) != VK_SUCCESS ||
                vkMapMemory(g_vk.device, readbackMemory[i], 0, kCalVkGridBytes, 0, &mapped[i]) != VK_SUCCESS)
            {
                mapped[i] = nullptr;
                Release();
                return false;
            }
        }

        return true;
    }

    void Release() override
    {
        for (unsigned int i = 0; i < 2; ++i)
        {
            RetireImage(input[i]);
            RetireImage(output[i]);
            inputReady[i] = outputReady[i] = false;
        }

        RetireImage(grid);

        for (unsigned int i = 0; i < Cal::kRing; ++i)
        {
            if (readback[i] == VK_NULL_HANDLE && readbackMemory[i] == VK_NULL_HANDLE)
                continue;

            Retired r;
            r.buffer = readback[i];
            r.memory = readbackMemory[i];
            r.mapped = mapped[i] != nullptr;
            r.due = g_vk.frames + kCalVkRetireFrames;
            retired_.push_back(r);

            readback[i] = VK_NULL_HANDLE;
            readbackMemory[i] = VK_NULL_HANDLE;
            mapped[i] = nullptr;
        }
    }

    // An unmapped slot is an empty sample, which the sweep drops.
    Cal::Stats Reduce(unsigned int slot) override
    {
        if (mapped[slot] == nullptr)
        {
            Cal::Stats empty {};
            empty.detailBand = NAN;
            return empty;
        }

        return Cal::ReduceGrid(static_cast<const float*>(mapped[slot]));
    }

    void RetireImage(OwnedImage& img)
    {
        if (img.image == VK_NULL_HANDLE && img.view == VK_NULL_HANDLE && img.memory == VK_NULL_HANDLE)
            return;

        Retired r;
        r.image = img;
        r.due = g_vk.frames + kCalVkRetireFrames;
        retired_.push_back(r);
        img = OwnedImage {};
    }

    // Destroys what was given back once the GPU is done with it; `all` after the device went idle.
    void Tick(bool all = false)
    {
        if (retired_.empty() || g_vk.device == VK_NULL_HANDLE)
            return;

        for (auto it = retired_.begin(); it != retired_.end();)
        {
            if (!all && g_vk.frames < it->due)
            {
                ++it;
                continue;
            }

            DestroyImage(it->image);

            if (it->memory != VK_NULL_HANDLE)
            {
                if (it->mapped)
                    vkUnmapMemory(g_vk.device, it->memory);
                vkFreeMemory(g_vk.device, it->memory, nullptr);
            }

            if (it->buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(g_vk.device, it->buffer, nullptr);

            it = retired_.erase(it);
        }
    }

    // The device is gone, and every handle with it: forget them, destroy nothing.
    void Abandon()
    {
        for (unsigned int i = 0; i < 2; ++i)
        {
            input[i] = output[i] = OwnedImage {};
            inputReady[i] = outputReady[i] = false;
        }

        grid = OwnedImage {};

        for (unsigned int i = 0; i < Cal::kRing; ++i)
        {
            readback[i] = VK_NULL_HANDLE;
            readbackMemory[i] = VK_NULL_HANDLE;
            mapped[i] = nullptr;
        }

        retired_.clear();
    }

  private:
    struct Retired
    {
        OwnedImage image;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        bool mapped = false;
        unsigned long long due = 0;
    };

    std::vector<Retired> retired_;
};

CalibrationVk g_calVk;

// Automatic's own base white point from the readbacks, 0 without one.
float CalibrationVkOwnBase()
{
    return g_vk.autoExposureValue > 1e-8f ? g_vk.autoExposurePreExposure / g_vk.autoExposureValue : 0.0f;
}

// Following the game: the game's base white point times the learned calibration, 0 without a reading.
float CalibrationVkFollowedBase()
{
    return g_vk.pairGameExposure > 1e-8f
               ? g_vk.pairPreExposure / g_vk.pairGameExposure * DlssNrFollowGame::Instance().Scale()
               : 0.0f;
}

// The base white point the tuned source's Trim multiplies, 0 without one: Game exposure's is the same as the
// encode's, so a sweep measures the white the picture is made with (DlssNrGameScale::WhiteBase), Automatic's is its
// own metering, following the game's exposure or not (Cal::RelearnFollowOnStart has why).
float CalibrationVkBase(const Config& cfg)
{
    if (cfg.DlssNrWhitePointSource.value_or_default() == 1)
        return g_vk.gameExposure > 1e-6f ? DlssNrGameScale::WhiteBase(cfg.DlssNrGameExposureScale.value_or_default(),
                                                                      g_vk.gamePreExposure, g_vk.gameExposure)
                                         : 0.0f;

    return CalibrationVkOwnBase();
}

// Everything availability depends on, from this evaluation. This backend has no proxy backend, frame hold or
// finished-picture mode (it does not run NR in the last), so those never block here.
Cal::Situation CalibrationVkSituation(const Config& cfg, bool linearHdr, bool autoRunning, bool gameExposureNow,
                                      bool colourConverted)
{
    Cal::Situation s;
    s.source = cfg.DlssNrWhitePointSource.value_or_default();
    s.anchorKey = DlssNrGameScale::AnchorKey(g_vk.gamePreExposure, g_vk.gameExposure);
    s.hdr = linearHdr;
    s.colourConverted = colourConverted;
    s.autoRunning = autoRunning;
    s.followLocked = DlssNr::FollowGameOn(cfg) && DlssNrFollowGame::Instance().Locked();
    s.followDisagreementEv =
        g_vk.followingGame ? Cal::BaseDisagreementEv(CalibrationVkFollowedBase(), CalibrationVkOwnBase()) : 0.0f;
    s.gameExposureNow = gameExposureNow;
    s.gameExposureReading = g_vk.gameExposure > 1e-6f;
    // Where NR runs without a Tune: not Before SR under Ray Reconstruction, so no wait.
    s.beforeSrSet = cfg.DlssNrRunBeforeSr.value_or_default() && g_vk.beforeSrPlacement;

    return s;
}

// Each evaluation, before the white point is used: the pinned white point of this evaluation, 0 when no run is on.
float CalibrationVkBeginFrame(const Config& cfg, uint32_t width, uint32_t height, bool linearHdr, bool autoRunning,
                              bool gameExposureNow, bool colourConverted)
{
    g_calVk.Tick();

    if (!Cal::WantedNow())
    {
        Cal::IdleNow();
        return 0.0f;
    }

    Cal::BeginFrameNow(g_calVk, cfg, width, height, CalibrationVkSituation(cfg, linearHdr, autoRunning, gameExposureNow, colourConverted),
                       CalibrationVkBase(cfg), g_vk.frames);
    return Cal::TheRun().frameWhitePoint;
}

// The width x height corner of `from` (a view in `fromLayout`, at least that large) into `copy`, left readable by a
// shader. Needs the grid (it is bound, not used).
bool CalibrationVkCopy(VkCommandBuffer cmd, OwnedImage& copy, VkImageView from, VkImageLayout fromLayout,
                       uint32_t width, uint32_t height)
{
    if (from == VK_NULL_HANDLE || !g_calVk.grid.Valid())
        return false;

    // A copy from before a resize is not the same shape any more.
    if (copy.Valid() && (copy.width != width || copy.height != height))
        g_calVk.RetireImage(copy);

    if (!copy.Valid() && !CreateImage(copy, width, height, kCalVkCopyFormat, true))
        return false;

    Transition(cmd, copy, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmd, g_calVk.grid, VK_IMAGE_LAYOUT_GENERAL);

    // The stats shader's copy mode (dlssnr_detail_stats.hlsl, Vulkan only).
    DlssNrConstants params {};
    params.Mode = 1;
    params.Width = width;
    params.Height = height;

    if (!g_vk.pass->DispatchStatsCopy(cmd, params, from, fromLayout, copy.view, g_calVk.grid.view))
        return false;

    Transition(cmd, copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// After the encode: the untouched frame (the encode's copy, g_vk.keep).
void CalibrationVkCopyInput(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    Cal::RunState& run = Cal::TheRun();

    if (!Cal::Active(run, g_calVk))
        return;

    const unsigned int i = run.current;
    g_calVk.inputReady[i] = CalibrationVkCopy(cmd, g_calVk.input[i], g_vk.keep.view, g_vk.keep.layout, width, height);
}

// After the resolve: `edited` is the finished frame (a view in `editedLayout`), `modelInput` the picture the model was
// shown -- the full-size proxy, or its shrink when the model runs reduced; the stats pass reads it at the frame's
// coordinates scaled to its own size.
void CalibrationVkMeasure(VkCommandBuffer cmd, VkImageView edited, VkImageLayout editedLayout, OwnedImage& modelInput,
                          uint32_t width, uint32_t height)
{
    Cal::RunState& run = Cal::TheRun();

    if (!Cal::Active(run, g_calVk))
        return;

    const unsigned int i = run.current;
    const unsigned int prev = i ^ 1u;
    g_calVk.outputReady[i] = CalibrationVkCopy(cmd, g_calVk.output[i], edited, editedLayout, width, height);

    if (!run.measurePending)
        return;

    const bool haveAll = g_calVk.outputReady[i] && g_calVk.outputReady[prev] && g_calVk.inputReady[i] &&
                         g_calVk.inputReady[prev] && modelInput.Valid();
    const unsigned int slot = Cal::FreeSlot(run);

    if (!haveAll || slot == Cal::kRing || g_calVk.readback[slot] == VK_NULL_HANDLE)
        return; // left pending: the next BeginFrame returns the ticket empty

    // The stats shader's own layout over the first fields (dlssnr_detail_stats.hlsl): see the D3D12 side.
    DlssNrConstants params {};
    params.Mode = 0;
    params.WhitePoint = run.measureWhitePoint;
    params.Width = width;
    params.Height = height;
    const DlssNrProxyCurve::TuneThresholds damage =
        DlssNrProxyCurve::Thresholds(Config::Instance()->DlssNrReversibleMode.value_or_default());
    params.TransferStrength = damage.shoulder;
    params.ColourStrength = damage.floor;

    // The model's input is read as a storage image (the layout has four sampled bindings, the stats pass reads five).
    Transition(cmd, modelInput, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmd, g_calVk.grid, VK_IMAGE_LAYOUT_GENERAL);

    if (!g_vk.pass->DispatchDetailStats(cmd, params, g_calVk.output[i].view, g_calVk.output[prev].view,
                                        g_calVk.input[i].view, g_calVk.input[prev].view, modelInput.view,
                                        g_calVk.grid.view))
        return;

    Transition(cmd, g_calVk.grid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { kCalVkGridWidth, kCalVkGridHeight, 1 };
    vkCmdCopyImageToBuffer(cmd, g_calVk.grid.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_calVk.readback[slot], 1,
                           &region);

    VkBufferMemoryBarrier toHost {};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = g_calVk.readback[slot];
    toHost.offset = 0;
    toHost.size = kCalVkGridBytes;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0,
                         nullptr);

    Cal::Submitted(run, slot, g_vk.frames);
}

// NR shutting down. `deviceAlive`: the device went idle and the resources can be destroyed now; otherwise the device
// is gone and they are forgotten.
void CalibrationVkShutdown(bool deviceAlive)
{
    if (!deviceAlive)
        g_calVk.Abandon();

    Cal::ShutdownNow(g_calVk);

    if (deviceAlive)
        g_calVk.Tick(true);
}
