#include "pch.h"
#include "menu_overlay_base.h"
#include "menu_overlay_vk.h"

#include <Util.h>
#include <Config.h>
#include <SysUtils.h>

#include <imgui/imgui_impl_vulkan.h>
#include <native/NativeDriverVk.h>
#include <imgui/imgui_impl_win32.h>

// Diagnostic: the menu submits on the game's queue from the present thread. Says which thread, once per change, to compare with
// the thread ids in a Vulkan validation threading error.
static void LogMenuSubmitThread(const char* what, VkQueue queue)
{
    static thread_local bool logged = false;

    if (logged)
        return;

    logged = true;
    LOG_INFO("Vulkan menu {} on queue {:X}, thread {}", what, (uintptr_t) queue, GetCurrentThreadId());
}

// Vulkan overlay code adopted from here:
// https://gist.github.com/mem99/0ec31ca302927457f86b1d6756aaa8c4
// Need to check resize & recreate fixes

static bool _isInited = false;

static bool _vulkanObjectsCreated = false;
static std::mutex _vkCleanMutex;
static std::mutex _vkPresentMutex;

// Family of every queue, games can present from queues the menu can't submit to
static std::unordered_map<VkQueue, uint32_t> _queueFamilies;
static std::mutex _queueFamiliesMutex;

// imgui stuff
struct ImGui_ImplVulkan_InitInfo _ImVulkan_Info = {};
struct ImGui_ImplVulkanH_Frame* _ImVulkan_Frames = VK_NULL_HANDLE;
static VkSemaphore* _ImVulkan_Semaphores = VK_NULL_HANDLE;
static VkRenderPass _vkRenderPass = VK_NULL_HANDLE;
static uint32_t _scImageCount;

// Swapchain the menu's frames were made for, other swapchains are presented without the menu
static VkSwapchainKHR _menuSwapchain = VK_NULL_HANDLE;
static ULONG64 _frameCount;

// ImGui's renderer backend is the Vulkan one. Not a given: with the frame-generation bridge (native/VkPresentBridge.h)
// the menu moves to the D3D12 swapchain and its backend, and when the game then makes a swapchain the bridge does not
// take, ImGui_ImplVulkan_Shutdown with no Vulkan backend reads through a null pointer (RDR2, bridge with the game's
// upscaler).
static bool VulkanBackendIsUp()
{
    if (ImGui::GetCurrentContext() == nullptr)
        return false;

    const ImGuiIO& io = ImGui::GetIO();
    return io.BackendRendererUserData != nullptr && io.BackendRendererName != nullptr &&
           strcmp(io.BackendRendererName, "imgui_impl_vulkan") == 0;
}

static void SetVkObjectName(VkDevice device, VkInstance instance, VkObjectType objectType, uint64_t objectHandle,
                            const char* name)
{
    static PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT = nullptr;

    if (vkSetDebugUtilsObjectNameEXT == nullptr)
        vkSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT"));

    VkDebugUtilsObjectNameInfoEXT info {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = objectType;
    info.objectHandle = objectHandle;
    info.pObjectName = name;

    vkSetDebugUtilsObjectNameEXT(device, &info);
}

static void CreateVulkanObjects(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                                const VkSwapchainCreateInfoKHR* pCreateInfo, VkSwapchainKHR* pSwapchain)
{
    LOG_FUNC();

    if (device == VK_NULL_HANDLE || pCreateInfo == nullptr || *pSwapchain == VK_NULL_HANDLE)
    {
        LOG_WARN(
            "device({0:X}) == VK_NULL_HANDLE || pCreateInfo({1:X}) == nullptr || *pSwapchain({2:X}) == VK_NULL_HANDLE",
            (UINT64) device, (UINT64) pCreateInfo, (UINT64) *pSwapchain);
        return;
    }

    if (_vulkanObjectsCreated)
    {
        LOG_DEBUG("_vulkanObjectsCreated, releasing objects");

        NativeMotionVk::ReleaseDebugViews(); // its textures are the backend's descriptor sets

        if (VulkanBackendIsUp())
            ImGui_ImplVulkan_Shutdown(false);

        MenuOverlayVk::DestroyVulkanObjects(false);

        _vulkanObjectsCreated = false;
    }

    // Initialize ImGui
    if (!MenuOverlayBase::IsInited() || MenuOverlayBase::Handle() != hwnd)
    {
        if (MenuOverlayBase::IsInited())
            MenuOverlayBase::Shutdown();

        LOG_DEBUG("MenuOverlayBase::Init");
        MenuOverlayBase::Init(hwnd, false);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = static_cast<float>(pCreateInfo->imageExtent.width);
    io.DisplaySize.y = static_cast<float>(pCreateInfo->imageExtent.height);

    VkResult result;

    // Get swapchain image count. The driver may make more images than the game asked for (minImageCount is a floor),
    // so everything per image below is sized from this count. _scImageCount takes it only once the images are read:
    // the present path divides by it.
    uint32_t imageCount = 0;
    result = vkGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, NULL);
    if (result != VK_SUCCESS)
    {
        LOG_ERROR("vkGetSwapchainImagesKHR error: {0:X}", (UINT) result);
        return;
    }

    LOG_DEBUG("Swapchain image count: {}, game's minImageCount: {}", imageCount, pCreateInfo->minImageCount);

    // ImGui's Vulkan backend needs at least two images; with fewer the menu stays off on this swapchain
    if (imageCount < 2)
    {
        LOG_ERROR("Swapchain has only {} image(s), no menu on this swapchain", imageCount);
        return;
    }

    std::vector<VkImage> images(imageCount);
    result = vkGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, images.data());
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || imageCount < 2)
    {
        LOG_ERROR("vkGetSwapchainImagesKHR error: {0:X}", (UINT) result);
        return;
    }

    _scImageCount = imageCount;

    // Alloc ImGui frame structure/semaphores for every image.
    // For convenience, I am using ImGui_ImplVulkanH_Frame in imgui_impl_vulkan.h
    // A swapchain made again lands here with the previous arrays still allocated, sized for its image count
    if (!_vulkanObjectsCreated)
    {
        IM_FREE(_ImVulkan_Frames);
        IM_FREE(_ImVulkan_Semaphores);
        _ImVulkan_Frames = (ImGui_ImplVulkanH_Frame*) IM_ALLOC(sizeof(ImGui_ImplVulkanH_Frame) * _scImageCount);
        _ImVulkan_Semaphores = (VkSemaphore*) IM_ALLOC(sizeof(VkSemaphore) * _scImageCount);
    }

    // Select queue family.
    uint32_t queueFamily = 0;
    {
        // get count
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, NULL);

        // get queues
        if (count > 0)
        {
            std::vector<VkQueueFamilyProperties> queues(count);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, queues.data());

            // find graphic queue
            for (uint32_t i = 0; i < count; i++)
            {
                if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                {
                    queueFamily = i;
                    break;
                }
            }
        }
        else
        {
            LOG_WARN("PD Queue property count is 0!");
            return;
        }
    }

    // Get device queue
    VkQueue queue;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    // Create the render pool
    VkDescriptorPool pool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize sampler_pool_size = {};
        sampler_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_pool_size.descriptorCount = 8; // required by ImGui 1.92

        VkDescriptorPoolCreateInfo desc_pool_info = {};
        desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        desc_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        desc_pool_info.maxSets = 8;
        desc_pool_info.poolSizeCount = 1;
        desc_pool_info.pPoolSizes = &sampler_pool_size;

        result = vkCreateDescriptorPool(device, &desc_pool_info, NULL, &pool);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkCreateDescriptorPool error: {0:X}", (UINT) result);
            return;
        }
    }

    // Create the render pass
    {
        VkAttachmentDescription attachment_desc = {};

        attachment_desc.format = pCreateInfo->imageFormat;
        attachment_desc.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment_desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment_desc.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment_desc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color_attachment = {};
        color_attachment.attachment = 0;
        color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_attachment;

        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo render_pass_info = {};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments = &attachment_desc;
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;
        render_pass_info.dependencyCount = 1;
        render_pass_info.pDependencies = &dependency;

        result = vkCreateRenderPass(device, &render_pass_info, NULL, &_vkRenderPass);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkCreateRenderPass error: {0:X}", (UINT) result);
            return;
        }
    }

    // Create The Image Views
    {
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;

        info.format = pCreateInfo->imageFormat;
        info.components.r = VK_COMPONENT_SWIZZLE_R;
        info.components.g = VK_COMPONENT_SWIZZLE_G;
        info.components.b = VK_COMPONENT_SWIZZLE_B;
        info.components.a = VK_COMPONENT_SWIZZLE_A;

        VkImageSubresourceRange image_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        info.subresourceRange = image_range;

        for (uint32_t i = 0; i < _scImageCount; i++)
        {
            ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];
            fd->Backbuffer = images[i];
            info.image = fd->Backbuffer;

            result = vkCreateImageView(device, &info, NULL, &fd->BackbufferView);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateImageView error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_IMAGE_VIEW, (UINT64) fd->BackbufferView,
                            "ImGui Backbuffer View");
#endif
        }
    }

    // Create frame Buffer
    {
        VkImageView attachment[1];
        VkFramebufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = _vkRenderPass;
        info.attachmentCount = 1;
        info.pAttachments = attachment;

        info.width = pCreateInfo->imageExtent.width;
        info.height = pCreateInfo->imageExtent.height;

        info.layers = 1;

        for (uint32_t i = 0; i < _scImageCount; i++)
        {
            ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];
            attachment[0] = fd->BackbufferView;
            result = vkCreateFramebuffer(device, &info, NULL, &fd->Framebuffer);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateFramebuffer error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_FRAMEBUFFER, (UINT64) fd->Framebuffer,
                            "ImGui Backbuffer Framebuffer");
#endif
        }
    }

    // Create command pools, command buffers, fences, and semaphores for every image
    for (uint32_t i = 0; i < _scImageCount; i++)
    {
        ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];
        VkSemaphore* fsd = &_ImVulkan_Semaphores[i];
        {
            VkCommandPoolCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            info.queueFamilyIndex = queueFamily;
            result = vkCreateCommandPool(device, &info, NULL, &fd->CommandPool);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateCommandPool error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_COMMAND_POOL, (UINT64) fd->CommandPool,
                            "ImGui Backbuffer Command Pool");
#endif
        }

        {
            VkCommandBufferAllocateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            info.commandPool = fd->CommandPool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;
            result = vkAllocateCommandBuffers(device, &info, &fd->CommandBuffer);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkAllocateCommandBuffers error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_COMMAND_BUFFER, (UINT64) fd->CommandBuffer,
                            "ImGui Backbuffer Command Buffer");
#endif
        }

        {
            VkFenceCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            result = vkCreateFence(device, &info, NULL, &fd->Fence);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateFence error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_FENCE, (UINT64) fd->Fence, "ImGui Backbuffer Fence");
#endif
        }

        {
            VkSemaphoreCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            result = vkCreateSemaphore(device, &info, NULL, fsd);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateSemaphore error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_SEMAPHORE, (UINT64) *fsd, "ImGui Backbuffer Semaphore");
#endif
        }
    }

    // Initialize ImGui and upload fonts
    {
        _ImVulkan_Info.Instance = instance;
        _ImVulkan_Info.PhysicalDevice = pd;
        _ImVulkan_Info.Device = device;
        _ImVulkan_Info.QueueFamily = queueFamily;
        _ImVulkan_Info.Queue = queue;
        _ImVulkan_Info.DescriptorPool = pool;
        _ImVulkan_Info.Subpass = 0;
        _ImVulkan_Info.MinImageCount = std::clamp(pCreateInfo->minImageCount, 2u, _scImageCount);
        _ImVulkan_Info.ImageCount = _scImageCount;
        _ImVulkan_Info.Allocator = NULL;
        _ImVulkan_Info.RenderPass = _vkRenderPass;

        // Another renderer backend still owns ImGui (the bridge's D3D12 menu was not let go): starting the Vulkan one
        // over it would leave both pointing at one context. No menu on this swapchain rather than a crash.
        if (ImGui::GetIO().BackendRendererUserData != nullptr && !VulkanBackendIsUp())
        {
            LOG_WARN("Vulkan menu: ImGui's renderer backend is still {}; the menu is not drawn on this swapchain",
                     ImGui::GetIO().BackendRendererName != nullptr ? ImGui::GetIO().BackendRendererName : "another one");
            return;
        }

        bool initResult = ImGui_ImplVulkan_Init(&_ImVulkan_Info);
        LOG_DEBUG("ImGui_ImplVulkan_Init result: {}", initResult);

        if (!initResult)
            return;

        // Upload Fonts
        // Use any command queue
        VkCommandPool command_pool = _ImVulkan_Frames[0].CommandPool;
        VkCommandBuffer command_buffer = _ImVulkan_Frames[0].CommandBuffer;
        result = vkResetCommandPool(device, command_pool, 0);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkBeginCommandBuffer error: {0:X}", (UINT) result);
            return;
        }

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer, &begin_info);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkBeginCommandBuffer error: {0:X}", (UINT) result);
            return;
        }

        // initResult = ImGui_ImplVulkan_CreateFontsTexture();
        // LOG_DEBUG("ImGui_ImplVulkan_CreateFontsTexture result: {}", initResult);

        VkSubmitInfo end_info = {};
        end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        end_info.commandBufferCount = 1;
        end_info.pCommandBuffers = &command_buffer;

        result = vkEndCommandBuffer(command_buffer);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkEndCommandBuffer error: {0:X}", (UINT) result);
            return;
        }

        LogMenuSubmitThread("end-of-frame submit", queue);
        result = vkQueueSubmit(queue, 1, &end_info, VK_NULL_HANDLE);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkQueueSubmit error: {0:X}", (UINT) result);
            return;
        }

        result = vkDeviceWaitIdle(device);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkDeviceWaitIdle error: {0:X}", (UINT) result);
            return;
        }
    }

    _vulkanObjectsCreated = true;
    State::Instance().menuOverlayIsVulkan = true;
    LOG_FUNC_RESULT(_vulkanObjectsCreated);
}

void MenuOverlayVk::DestroyVulkanObjects(bool shutdown)
{
    State::Instance().menuOverlayIsVulkan = false;
    if (_ImVulkan_Info.Device == VK_NULL_HANDLE)
        return;

    // The Optical F5Low debug view's images go with the menu's; their descriptor sets only through a live backend.
    NativeMotionVk::ReleaseDebugViews(VulkanBackendIsUp());

    if (!shutdown)
        LOG_FUNC();

    _vkCleanMutex.lock();

    auto result = vkDeviceWaitIdle(_ImVulkan_Info.Device);
    if (result != VK_SUCCESS && !shutdown)
        LOG_WARN("vkDeviceWaitIdle error: {0:X}", (UINT) result);

    if (shutdown)
    {
        if (_vkRenderPass)
            vkDestroyRenderPass(_ImVulkan_Info.Device, _vkRenderPass, VK_NULL_HANDLE);

        if (_ImVulkan_Info.DescriptorPool)
            vkDestroyDescriptorPool(_ImVulkan_Info.Device, _ImVulkan_Info.DescriptorPool, VK_NULL_HANDLE);
    }

    for (uint32_t i = 0; i < _ImVulkan_Info.ImageCount; i++)
    {
        ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];

        if (fd->Fence != VK_NULL_HANDLE)
        {
            vkDestroyFence(_ImVulkan_Info.Device, fd->Fence, VK_NULL_HANDLE);
            fd->Fence = VK_NULL_HANDLE;
        }

        if (fd->CommandBuffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(_ImVulkan_Info.Device, fd->CommandPool, 1, &fd->CommandBuffer);
            fd->CommandBuffer = VK_NULL_HANDLE;
        }

        if (fd->CommandPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(_ImVulkan_Info.Device, fd->CommandPool, VK_NULL_HANDLE);
            fd->CommandPool = VK_NULL_HANDLE;
        }

        if (fd->BackbufferView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(_ImVulkan_Info.Device, fd->BackbufferView, VK_NULL_HANDLE);
            fd->BackbufferView = VK_NULL_HANDLE;
        }

        if (fd->Framebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(_ImVulkan_Info.Device, fd->Framebuffer, VK_NULL_HANDLE);
            fd->Framebuffer = VK_NULL_HANDLE;
        }

        if (_ImVulkan_Semaphores[i] != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(_ImVulkan_Info.Device, _ImVulkan_Semaphores[i], VK_NULL_HANDLE);
            _ImVulkan_Semaphores[i] = VK_NULL_HANDLE;
        }
    }

    _ImVulkan_Info = {};

    _vkCleanMutex.unlock();
}

void MenuOverlayVk::DeviceCreated(VkDevice device, const VkDeviceCreateInfo* pCreateInfo)
{
    std::lock_guard lock(_queueFamiliesMutex);

    for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
    {
        auto& info = pCreateInfo->pQueueCreateInfos[i];

        // Queues with flags need vkGetDeviceQueue2, not presented from in practice
        if (info.flags != 0)
            continue;

        for (uint32_t index = 0; index < info.queueCount; index++)
        {
            VkQueue queue = VK_NULL_HANDLE;
            vkGetDeviceQueue(device, info.queueFamilyIndex, index, &queue);
            _queueFamilies[queue] = info.queueFamilyIndex;
        }
    }
}

bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)
{
    LOG_FUNC();

    if (!_vulkanObjectsCreated)
        return true;

    if (!MenuOverlayBase::IsInited() || _ImVulkan_Info.Device == VK_NULL_HANDLE)
        return true;

    if (pPresentInfo->swapchainCount == 0)
        return false;

    if (pPresentInfo->pSwapchains[0] != _menuSwapchain)
        return true;

    // std::lock_guard<std::mutex> lock(_vkPresentMutex);
    LOG_DEBUG("rendering menu, swapchain count: {0}", pPresentInfo->swapchainCount);

    ImGuiIO& io = ImGui::GetIO();
    (void) io;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    _frameCount++;

    {
        auto semaphoreIndex = _frameCount % _scImageCount;

        // With DLSSG the present comes from Streamline's thread, while the game's queue can be waiting on that
        // thread. The presenting queue avoids a deadlock, when the menu can submit to it.
        VkQueue menuQueue = _ImVulkan_Info.Queue;
        {
            std::lock_guard lock(_queueFamiliesMutex);
            if (auto it = _queueFamilies.find(queue);
                it != _queueFamilies.end() && it->second == _ImVulkan_Info.QueueFamily)
            {
                menuQueue = queue;
            }
        }

        ImGui_ImplVulkan_SetQueue(menuQueue);
        ImGui_ImplVulkan_NewFrame();

        // The Optical F5Low debug view's copy goes into this swap chain image's slot: its last submit must be done
        // before the copy is handed out again (only while the view can be shown; the fence is waited below anyway).
        const uint32_t imageIndex = pPresentInfo->pImageIndices[0];

        if (NativeMotionVk::DebugViewOn() && imageIndex < _scImageCount)
        {
            vkWaitForFences(_ImVulkan_Info.Device, 1, &_ImVulkan_Frames[imageIndex].Fence, VK_TRUE, UINT64_MAX);
            NativeMotionVk::BeginDebugFrame(_ImVulkan_Info.Device, _ImVulkan_Info.PhysicalDevice,
                                            _ImVulkan_Info.QueueFamily, imageIndex);
        }

        if (State::Instance().delayMenuRenderBy > 0)
            State::Instance().delayMenuRenderBy--;

        if (MenuOverlayBase::RenderMenu())
        {
            if (State::Instance().delayMenuRenderBy == 0)
            {
                uint32_t idx = pPresentInfo->pImageIndices[0];
                ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[idx];

                if (auto wResult = vkWaitForFences(_ImVulkan_Info.Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
                    wResult != VK_SUCCESS)
                {
                    LOG_ERROR("vkWaitForFences error: {}", (int32_t) wResult);
                    return true;
                }
                vkResetFences(_ImVulkan_Info.Device, 1, &fd->Fence);

                {
                    vkResetCommandPool(_ImVulkan_Info.Device, fd->CommandPool, 0);
                    VkCommandBufferBeginInfo info = {};
                    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    vkBeginCommandBuffer(fd->CommandBuffer, &info);
                }

                NativeMotionVk::RecordDebugTransfers(fd->CommandBuffer);

                {
                    VkRenderPassBeginInfo info = {};
                    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                    info.renderPass = _vkRenderPass;
                    info.framebuffer = fd->Framebuffer;
                    info.renderArea.extent.width = static_cast<uint32_t>(ImGui::GetIO().DisplaySize.x);
                    info.renderArea.extent.height = static_cast<uint32_t>(ImGui::GetIO().DisplaySize.y);
                    vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
                }

                ImGui::Render();
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fd->CommandBuffer);

                // Submit command buffer
                vkCmdEndRenderPass(fd->CommandBuffer);
                auto ecbResult = vkEndCommandBuffer(fd->CommandBuffer);
                if (ecbResult != VK_SUCCESS)
                {
                    LOG_ERROR("vkQueueSubmit error: {0:X}", (UINT) ecbResult);
                    return false;
                }

                // Submit queue and semaphores
                LOG_DEBUG("waitSemaphoreCount: {0}", pPresentInfo->waitSemaphoreCount);
                VkPipelineStageFlags waitStages[8] = { VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT };

                VkSubmitInfo submit_info = {};
                submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit_info.commandBufferCount = 1;
                submit_info.pCommandBuffers = &fd->CommandBuffer;
                submit_info.pWaitDstStageMask = waitStages;
                submit_info.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
                submit_info.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
                submit_info.signalSemaphoreCount = 1;
                submit_info.pSignalSemaphores = &_ImVulkan_Semaphores[semaphoreIndex];

                // With the debug view's copy this frame the submit also waits for it (a timeline semaphore, at the
                // transfer that reads it); every wait then gets its own stage and value.
                std::vector<VkSemaphore> debugWaits;
                std::vector<VkPipelineStageFlags> debugStages;
                std::vector<uint64_t> debugValues;
                VkTimelineSemaphoreSubmitInfo debugTimeline { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };

                if (const VkSemaphore copied = NativeMotionVk::DebugWaitSemaphore(); copied != VK_NULL_HANDLE)
                {
                    debugWaits.assign(pPresentInfo->pWaitSemaphores,
                                      pPresentInfo->pWaitSemaphores + pPresentInfo->waitSemaphoreCount);
                    debugStages.assign(debugWaits.size(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                    debugValues.assign(debugWaits.size(), 0);
                    debugWaits.push_back(copied);
                    debugStages.push_back(VK_PIPELINE_STAGE_TRANSFER_BIT);
                    debugValues.push_back(NativeMotionVk::DebugWaitValue());
                    debugTimeline.waitSemaphoreValueCount = (uint32_t) debugValues.size();
                    debugTimeline.pWaitSemaphoreValues = debugValues.data();
                    submit_info.pNext = &debugTimeline;
                    submit_info.waitSemaphoreCount = (uint32_t) debugWaits.size();
                    submit_info.pWaitSemaphores = debugWaits.data();
                    submit_info.pWaitDstStageMask = debugStages.data();
                }

                LogMenuSubmitThread("menu draw submit", menuQueue);
                auto qResult = vkQueueSubmit(menuQueue, 1, &submit_info, fd->Fence);
                NativeMotionVk::EndDebugFrame();
                if (qResult != VK_SUCCESS)
                {
                    LOG_ERROR("vkQueueSubmit error: {0:X}", (UINT) qResult);
                    return false;
                }

                pPresentInfo->waitSemaphoreCount = 1;
                pPresentInfo->pWaitSemaphores = &_ImVulkan_Semaphores[semaphoreIndex];
            }
            else
            {
                // To make RenderMenu happy as it expects this
                ImGui::Render();
            }
        }

        NativeMotionVk::EndDebugFrame();
    }

    return true;
}

void MenuOverlayVk::HandOverToBridge()
{
    // Bridged from the game's first swapchain: no Vulkan menu was made, only what a previous one left
    if (!_vulkanObjectsCreated)
    {
        DestroyVulkanObjects(false);
        return;
    }

    LOG_INFO("Vulkan bridge: the menu moves from the game's Vulkan swapchain to the D3D12 one");

    // The backend frees its textures' descriptor sets from our pool, so it goes first; nothing of it may be in flight
    if (_ImVulkan_Info.Device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(_ImVulkan_Info.Device);

    NativeMotionVk::ReleaseDebugViews(); // its textures are the backend's descriptor sets

    if (VulkanBackendIsUp())
        ImGui_ImplVulkan_Shutdown(false); // the font texture is made again by the D3D12 backend

    // Render pass and descriptor pool included: the next Vulkan menu (if the bridge ever ends) makes its own
    DestroyVulkanObjects(true);

    IM_FREE(_ImVulkan_Frames);
    _ImVulkan_Frames = nullptr;
    IM_FREE(_ImVulkan_Semaphores);
    _ImVulkan_Semaphores = nullptr;
    _vkRenderPass = VK_NULL_HANDLE;
    _scImageCount = 0;

    _vulkanObjectsCreated = false;
    _isInited = false;
}

void MenuOverlayVk::CreateSwapchain(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                                    const VkSwapchainCreateInfoKHR* pCreateInfo,
                                    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    LOG_FUNC();

    if (OnOtherWindow(hwnd))
    {
        LOG_DEBUG("Menu is on window {:X}, skipping swapchain of {:X}", (UINT64) MenuOverlayBase::Handle(),
                  (UINT64) hwnd);
        return;
    }

    if (MenuOverlayBase::Handle() != hwnd)
    {
        LOG_DEBUG("MenuOverlayBase::Handle() != _hwnd");

        if (MenuOverlayBase::IsInited())
        {
            NativeMotionVk::ReleaseDebugViews(); // its textures are the backend's descriptor sets
            if (VulkanBackendIsUp())
                ImGui_ImplVulkan_Shutdown(false);

            LOG_DEBUG("MenuOverlayBase::Shutdown();");
            MenuOverlayBase::Shutdown();
        }

        LOG_DEBUG("MenuOverlayBase::Init({0:X})", (UINT64) hwnd);
        MenuOverlayBase::Init(hwnd, false);
    }

    CreateVulkanObjects(device, pd, instance, hwnd, pCreateInfo, pSwapchain);

    if (_ImVulkan_Info.Device != VK_NULL_HANDLE)
    {
        _menuSwapchain = *pSwapchain;
        _isInited = true;
        MenuOverlayBase::VulkanReady();
        LOG_DEBUG("Vulkan ready");
    }
}

bool MenuOverlayVk::OnOtherWindow(HWND hwnd)
{
    auto menuHwnd = MenuOverlayBase::Handle();
    return MenuOverlayBase::IsInited() && menuHwnd != hwnd && IsWindow(menuHwnd);
}
