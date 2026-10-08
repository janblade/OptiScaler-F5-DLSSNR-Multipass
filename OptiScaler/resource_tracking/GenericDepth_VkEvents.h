#pragma once

// The Vulkan depth observer's bookkeeping, with no Vulkan call in it: what the game made (images, views, framebuffers,
// render passes) and what each command buffer is doing, turned into native::DepthFinderCore events, and the core's
// snapshot requests turned into "copy this image, in this layout, now". resource_tracking/GenericDepth_Vk.cpp feeds it
// from the hooks and records the copies; tests/nr_generic_depth_vk_smoke.cpp replays event sequences through it.
//
// How Vulkan maps onto the core (docs/NATIVE-INPUT-ADAPTERS.md):
//   - A context is a command buffer. Its depth buffer is bound by vkCmdBeginRenderPass(2) (the subpass's depth
//     attachment, through the framebuffer's view) or vkCmdBeginRendering (pDepthAttachment's view), and unbound at the
//     matching end. A render pass never spans command buffers, so "a list closes while bound" does not happen.
//   - A clear is a depth attachment loaded with CLEAR (its clear value), vkCmdClearDepthStencilImage, or
//     vkCmdClearAttachments inside a pass.
//   - A copy cannot be recorded inside a render pass. The core's requests come at the end of a pass ("unbind"): the copy
//     goes after the end, in the attachment's final layout (dynamic rendering: the layout it rendered in). A clear request
//     at the start of a pass copies before the begin, in the attachment's initial layout; UNDEFINED there means the
//     contents are not kept, and there is nothing to copy. Requests that fall inside a pass (vkCmdClearAttachments, a
//     subpass with another depth attachment, a suspended dynamic pass) are dropped and counted.
//   - Draws recorded into a secondary command buffer that continues a render pass are kept with it, and counted for the
//     primary when vkCmdExecuteCommands runs them there.

#include "../native/DepthFinderCore.h"

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace GenericDepthVkEvents
{

inline bool IsDepthFormat(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default:
        return false;
    }
}

inline bool HasStencil(VkFormat format)
{
    return format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

inline bool ReadOnlyLayout(VkImageLayout layout)
{
    return layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
           layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
           layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
}

struct ImageInfo
{
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = 0;
};

struct AttachmentInfo
{
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct SubpassInfo
{
    int32_t depth = -1; // the depth attachment's index, -1 none
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct RenderPassInfo
{
    std::vector<AttachmentInfo> attachments;
    std::vector<SubpassInfo> subpasses;
};

// The copy a hook records into its command buffer.
struct Copy
{
    bool take = false;
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    const char* where = "";
};

// What a render pass begin carries, in plain terms (from VkRenderPassBeginInfo).
struct PassBegin
{
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    const VkImageView* views = nullptr; // an imageless framebuffer's views (VkRenderPassAttachmentBeginInfo), else null
    uint32_t viewCount = 0;
    const VkClearValue* clears = nullptr;
    uint32_t clearCount = 0;
    uint32_t areaWidth = 0;
};

// What a dynamic rendering begin carries (from VkRenderingInfo).
struct RenderingBegin
{
    VkImageView depthView = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    float clearDepth = 1.0f;
    bool resuming = false;
    bool suspending = false;
    uint32_t areaWidth = 0;
};

class Tracker
{
  public:
    explicit Tracker(native::DepthFinderCore& core) : _core(core) {}

    // ---- what the game made ---------------------------------------------------------------------------------------------
    void OnImage(VkImage image, const ImageInfo& info)
    {
        std::unique_lock lock(_resources);
        _images[(uint64_t) image] = info;
    }
    void OnImageDestroyed(VkImage image)
    {
        std::unique_lock lock(_resources);
        _images.erase((uint64_t) image);
    }
    void OnView(VkImageView view, VkImage image)
    {
        std::unique_lock lock(_resources);
        _views[(uint64_t) view] = (uint64_t) image;
    }
    void OnViewDestroyed(VkImageView view)
    {
        std::unique_lock lock(_resources);
        _views.erase((uint64_t) view);
    }
    // `views` empty for an imageless framebuffer (the views come with each begin).
    void OnFramebuffer(VkFramebuffer framebuffer, std::vector<VkImageView> views)
    {
        std::unique_lock lock(_resources);
        _framebuffers[(uint64_t) framebuffer] = std::move(views);
    }
    void OnFramebufferDestroyed(VkFramebuffer framebuffer)
    {
        std::unique_lock lock(_resources);
        _framebuffers.erase((uint64_t) framebuffer);
    }
    void OnRenderPass(VkRenderPass renderPass, RenderPassInfo info)
    {
        std::unique_lock lock(_resources);
        _renderPasses[(uint64_t) renderPass] = std::move(info);
    }
    void OnRenderPassDestroyed(VkRenderPass renderPass)
    {
        std::unique_lock lock(_resources);
        _renderPasses.erase((uint64_t) renderPass);
    }

    bool FindImage(VkImage image, ImageInfo* info) const
    {
        std::shared_lock lock(_resources);
        const auto found = _images.find((uint64_t) image);

        if (found == _images.end())
            return false;

        *info = found->second;
        return true;
    }

    // ---- what a command buffer does -------------------------------------------------------------------------------------

    // vkBeginCommandBuffer (and an implicit reset). A secondary buffer that continues a render pass keeps its draws.
    void Begin(VkCommandBuffer cmd, bool secondaryContinue)
    {
        std::lock_guard lock(_commands);
        auto& state = _states[(uint64_t) cmd];

        if (state.secondaryContinue)
            _secondaries.fetch_sub(1, std::memory_order_relaxed);

        state = CommandState {};
        state.secondaryContinue = secondaryContinue;

        if (secondaryContinue)
            _secondaries.fetch_add(1, std::memory_order_relaxed);
    }

    // vkEndCommandBuffer.
    void End(VkCommandBuffer cmd) { _core.OnContextEnd((uint64_t) cmd); }

    // vkFreeCommandBuffers, and the reset of a pool's buffers.
    void Forget(VkCommandBuffer cmd)
    {
        std::lock_guard lock(_commands);
        const auto found = _states.find((uint64_t) cmd);

        if (found == _states.end())
            return;

        if (found->second.secondaryContinue)
            _secondaries.fetch_sub(1, std::memory_order_relaxed);

        _states.erase(found);
    }

    // vkCmdBeginRenderPass(2): the copy to record BEFORE the begin (a clear request), if any.
    Copy BeginRenderPass(VkCommandBuffer cmd, const PassBegin& begin)
    {
        Copy copy;
        Attached depth;

        {
            std::shared_lock lock(_resources);
            const auto pass = _renderPasses.find((uint64_t) begin.renderPass);

            if (pass != _renderPasses.end())
                depth = ResolveLocked(pass->second, 0, begin);
        }

        _core.OnViewport((uint64_t) cmd, (float) begin.areaWidth);

        if (depth.image != VK_NULL_HANDLE && depth.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
        {
            const float value = depth.attachment >= 0 && (uint32_t) depth.attachment < begin.clearCount
                                    ? begin.clears[depth.attachment].depthStencil.depth
                                    : 1.0f;
            const auto request = _core.OnDepthClear((uint64_t) cmd, depth.buffer, value);
            copy = CopyFor(request, depth.image, depth.initialLayout);
        }

        Bind(cmd, depth, begin.renderPass, begin);
        return copy;
    }

    // vkCmdNextSubpass(2): the next subpass's depth. A request here falls inside the pass and is dropped.
    void NextSubpass(VkCommandBuffer cmd)
    {
        PassBegin begin;
        uint32_t subpass = 0;

        {
            std::lock_guard lock(_commands);
            auto& state = _states[(uint64_t) cmd];
            begin = state.begin;
            subpass = ++state.subpass;
        }

        Attached depth;

        {
            std::shared_lock lock(_resources);
            const auto pass = _renderPasses.find((uint64_t) begin.renderPass);

            if (pass != _renderPasses.end())
                depth = ResolveLocked(pass->second, subpass, begin);
        }

        const auto request =
            _core.OnDepthBound((uint64_t) cmd, depth.image != VK_NULL_HANDLE, depth.image != VK_NULL_HANDLE ? &depth.buffer : nullptr);
        Dropped(request);

        std::lock_guard lock(_commands);
        auto& state = _states[(uint64_t) cmd];
        state.depth = depth.image;
        state.finalLayout = depth.finalLayout;
    }

    // vkCmdEndRenderPass(2): the copy to record AFTER the end, if any.
    Copy EndRenderPass(VkCommandBuffer cmd)
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        {
            std::lock_guard lock(_commands);
            auto& state = _states[(uint64_t) cmd];
            image = state.depth;
            finalLayout = state.finalLayout;
            state.depth = VK_NULL_HANDLE;
            state.begin = PassBegin {};
        }

        const auto request = _core.OnDepthBound((uint64_t) cmd, false, nullptr);
        return CopyFor(request, image, finalLayout);
    }

    // vkCmdBeginRendering: the copy to record BEFORE the begin (a clear request), if any.
    Copy BeginRendering(VkCommandBuffer cmd, const RenderingBegin& begin)
    {
        Copy copy;
        Attached depth;

        {
            std::shared_lock lock(_resources);
            depth = FromViewLocked(begin.depthView, begin.layout);
        }

        _core.OnViewport((uint64_t) cmd, (float) begin.areaWidth);

        // A resumed pass goes on with the one suspended before it: neither a clear nor a copy point.
        if (depth.image != VK_NULL_HANDLE && begin.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && !begin.resuming)
        {
            const auto request = _core.OnDepthClear((uint64_t) cmd, depth.buffer, begin.clearDepth);
            copy = CopyFor(request, depth.image, begin.layout);
        }

        depth.finalLayout = begin.layout;
        Bind(cmd, depth, VK_NULL_HANDLE, PassBegin {});

        std::lock_guard lock(_commands);
        _states[(uint64_t) cmd].suspending = begin.suspending;
        return copy;
    }

    // vkCmdEndRendering: the copy to record AFTER the end, if any (none for a suspended pass, which goes on elsewhere).
    Copy EndRendering(VkCommandBuffer cmd)
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool suspending = false;

        {
            std::lock_guard lock(_commands);
            auto& state = _states[(uint64_t) cmd];
            image = state.depth;
            layout = state.finalLayout;
            suspending = state.suspending;
            state.depth = VK_NULL_HANDLE;
            state.suspending = false;
        }

        const auto request = _core.OnDepthBound((uint64_t) cmd, false, nullptr);

        if (suspending)
        {
            Dropped(request);
            return Copy {};
        }

        return CopyFor(request, image, layout);
    }

    // vkCmdClearDepthStencilImage (outside a pass): the copy to record BEFORE the clear, if any.
    Copy ClearImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, float value)
    {
        Attached depth;

        {
            std::shared_lock lock(_resources);
            depth = FromImageLocked(image);
        }

        if (depth.image == VK_NULL_HANDLE)
            return Copy {};

        return CopyFor(_core.OnDepthClear((uint64_t) cmd, depth.buffer, value), image, layout);
    }

    // vkCmdClearAttachments with the depth aspect: a clear of the bound depth, inside the pass (no copy possible).
    void ClearBound(VkCommandBuffer cmd, float value)
    {
        VkImage image = VK_NULL_HANDLE;

        {
            std::lock_guard lock(_commands);
            const auto found = _states.find((uint64_t) cmd);

            if (found != _states.end())
                image = found->second.depth;
        }

        if (image == VK_NULL_HANDLE)
            return;

        Attached depth;

        {
            std::shared_lock lock(_resources);
            depth = FromImageLocked(image);
        }

        if (depth.image != VK_NULL_HANDLE)
            Dropped(_core.OnDepthClear((uint64_t) cmd, depth.buffer, value));
    }

    void Draw(VkCommandBuffer cmd, uint64_t vertices, uint32_t instances)
    {
        if (_secondaries.load(std::memory_order_relaxed) == 0 || !KeepForSecondary(cmd, vertices, instances, false))
            _core.OnDraw((uint64_t) cmd, vertices, instances);
    }

    void Indirect(VkCommandBuffer cmd, uint32_t maxCount)
    {
        if (_secondaries.load(std::memory_order_relaxed) == 0 || !KeepForSecondary(cmd, maxCount, 1, true))
            _core.OnIndirect((uint64_t) cmd, maxCount);
    }

    void Viewport(VkCommandBuffer cmd, float width) { _core.OnViewport((uint64_t) cmd, width); }

    // vkCmdExecuteCommands: the secondaries' draws count for the primary's pass.
    void Execute(VkCommandBuffer primary, const VkCommandBuffer* secondaries, uint32_t count)
    {
        std::vector<KeptDraw> draws;

        {
            std::lock_guard lock(_commands);

            for (uint32_t i = 0; i < count; ++i)
            {
                const auto found = _states.find((uint64_t) secondaries[i]);

                if (found != _states.end())
                    draws.insert(draws.end(), found->second.draws.begin(), found->second.draws.end());
            }
        }

        for (const auto& draw : draws)
        {
            if (draw.indirect)
                _core.OnIndirect((uint64_t) primary, (uint32_t) draw.vertices);
            else
                _core.OnDraw((uint64_t) primary, draw.vertices, draw.instances);
        }
    }

    // Requests that fell inside a render pass, where no copy can be recorded.
    uint64_t DroppedRequests() const { return _dropped.load(std::memory_order_relaxed); }

  private:
    static constexpr size_t kMaxKeptDraws = 65536;

    struct KeptDraw
    {
        uint64_t vertices = 0;
        uint32_t instances = 0;
        bool indirect = false;
    };

    struct CommandState
    {
        VkImage depth = VK_NULL_HANDLE; // bound now
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        PassBegin begin;                // the render pass being recorded (not its pointers: only used for the ids)
        uint32_t subpass = 0;
        std::vector<VkImageView> views; // an imageless framebuffer's views, kept for NextSubpass
        bool suspending = false;
        bool secondaryContinue = false;
        std::vector<KeptDraw> draws;
    };

    struct Attached
    {
        VkImage image = VK_NULL_HANDLE;
        native::DepthBuffer buffer;
        int32_t attachment = -1;
        VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    Attached FromImageLocked(VkImage image) const
    {
        Attached out;
        const auto found = _images.find((uint64_t) image);

        if (found == _images.end() || !IsDepthFormat(found->second.format))
            return out;

        out.image = image;
        out.buffer.id = (uint64_t) image;
        out.buffer.width = found->second.width;
        out.buffer.height = found->second.height;
        out.buffer.format = (uint32_t) found->second.format;
        return out;
    }

    Attached FromViewLocked(VkImageView view, VkImageLayout layout) const
    {
        if (view == VK_NULL_HANDLE)
            return Attached {};

        const auto found = _views.find((uint64_t) view);

        if (found == _views.end())
            return Attached {};

        Attached out = FromImageLocked((VkImage) found->second);
        out.buffer.readOnlyDepth = ReadOnlyLayout(layout);
        return out;
    }

    Attached ResolveLocked(const RenderPassInfo& pass, uint32_t subpass, const PassBegin& begin) const
    {
        if (subpass >= pass.subpasses.size() || pass.subpasses[subpass].depth < 0)
            return Attached {};

        const uint32_t index = (uint32_t) pass.subpasses[subpass].depth;

        if (index >= pass.attachments.size())
            return Attached {};

        VkImageView view = VK_NULL_HANDLE;

        if (begin.views != nullptr && index < begin.viewCount)
            view = begin.views[index];
        else if (const auto fb = _framebuffers.find((uint64_t) begin.framebuffer);
                 fb != _framebuffers.end() && index < fb->second.size())
            view = fb->second[index];

        Attached out = FromViewLocked(view, pass.subpasses[subpass].layout);
        const auto& attachment = pass.attachments[index];
        out.attachment = (int32_t) index;
        out.loadOp = attachment.loadOp;
        out.initialLayout = attachment.initialLayout;
        out.finalLayout = attachment.finalLayout;
        return out;
    }

    void Bind(VkCommandBuffer cmd, const Attached& depth, VkRenderPass renderPass, const PassBegin& begin)
    {
        const bool has = depth.image != VK_NULL_HANDLE;
        Dropped(_core.OnDepthBound((uint64_t) cmd, has, has ? &depth.buffer : nullptr));

        std::lock_guard lock(_commands);
        auto& state = _states[(uint64_t) cmd];
        state.depth = depth.image;
        state.finalLayout = depth.finalLayout;
        state.subpass = 0;
        state.begin = begin;
        state.begin.renderPass = renderPass;

        // The views of an imageless framebuffer are the caller's memory: kept here for the next subpasses.
        if (begin.views != nullptr)
        {
            state.views.assign(begin.views, begin.views + begin.viewCount);
            state.begin.views = state.views.data();
        }
        else
            state.views.clear();

        state.begin.clears = nullptr;
        state.begin.clearCount = 0;
    }

    bool KeepForSecondary(VkCommandBuffer cmd, uint64_t vertices, uint32_t instances, bool indirect)
    {
        std::lock_guard lock(_commands);
        const auto found = _states.find((uint64_t) cmd);

        if (found == _states.end() || !found->second.secondaryContinue)
            return false;

        if (found->second.draws.size() < kMaxKeptDraws)
            found->second.draws.push_back({ vertices, instances, indirect });

        return true;
    }

    Copy CopyFor(const native::SnapshotRequest& request, VkImage image, VkImageLayout layout)
    {
        Copy copy;

        if (!request.take)
            return copy;

        // UNDEFINED and PREINITIALIZED keep no depth worth reading.
        if (image == VK_NULL_HANDLE || (uint64_t) image != request.id || layout == VK_IMAGE_LAYOUT_UNDEFINED ||
            layout == VK_IMAGE_LAYOUT_PREINITIALIZED)
        {
            _dropped.fetch_add(1, std::memory_order_relaxed);
            return copy;
        }

        copy.take = true;
        copy.image = image;
        copy.layout = layout;
        copy.where = request.where;
        return copy;
    }

    void Dropped(const native::SnapshotRequest& request)
    {
        if (request.take)
            _dropped.fetch_add(1, std::memory_order_relaxed);
    }

    native::DepthFinderCore& _core;

    mutable std::shared_mutex _resources;
    std::unordered_map<uint64_t, ImageInfo> _images;
    std::unordered_map<uint64_t, uint64_t> _views; // view -> image
    std::unordered_map<uint64_t, std::vector<VkImageView>> _framebuffers;
    std::unordered_map<uint64_t, RenderPassInfo> _renderPasses;

    std::mutex _commands;
    std::unordered_map<uint64_t, CommandState> _states;
    std::atomic<int64_t> _secondaries { 0 }; // command buffers recorded as render pass continuations, now
    std::atomic<uint64_t> _dropped { 0 };
};

} // namespace GenericDepthVkEvents
