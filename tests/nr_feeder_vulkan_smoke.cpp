// Functional x64 Vulkan/Feeder/OptiScaler test application. Run with the local
// modified Feeder + ReShade layers and OptiScaler winmm proxy, in an isolated folder.
// No engine DLSS integration, DXGI swapchain or game assets. GPU waits here belong
// to this deterministic test harness, not to the production motion pipeline.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

static LONG CALLBACK CaptureValidationFault(EXCEPTION_POINTERS* exception)
{
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        HANDLE file = CreateFileW(L"vulkan-smoke-crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION info {GetCurrentThreadId(), exception, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo), &info, nullptr, nullptr);
            CloseHandle(file);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(1, CaptureValidationFault);
    timeBeginPeriod(1); // load the OptiScaler proxy as an ordinary x64 application
    WNDCLASSW wc {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FeederVulkanValidation";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"Feeder Vulkan NR validation", WS_POPUP,
        -30000, -30000, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    const char* extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app; ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = extensions;
    VkInstance instance;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return 2;
    VkWin32SurfaceCreateInfoKHR sci { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = wc.hInstance; sci.hwnd = window;
    VkSurfaceKHR surface;
    if (vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface) != VK_SUCCESS) return 3;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance, &count, physicals.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = 0;
    for (auto candidate : physicals)
    {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            VkBool32 present = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            { physical = candidate; family = i; break; }
        }
        if (physical) break;
    }
    if (!physical) return 4;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
    VkPhysicalDeviceVulkan12Features features { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    features.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &features; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 3; dci.ppEnabledExtensionNames = deviceExtensions;
    VkDevice device;
    if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS) return 5;
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data());
    auto format = formats.front();
    for (auto f : formats) if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { format = f; break; }
    VkSwapchainCreateInfoKHR swci { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swci.surface = surface; swci.minImageCount = (std::max)(2u, caps.minImageCount);
    if (caps.maxImageCount) swci.minImageCount = (std::min)(swci.minImageCount, caps.maxImageCount);
    swci.imageFormat = format.format; swci.imageColorSpace = format.colorSpace;
    swci.imageExtent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{640, 360};
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; swci.presentMode = VK_PRESENT_MODE_FIFO_KHR; swci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain;
    if (vkCreateSwapchainKHR(device, &swci, nullptr, &swapchain) != VK_SUCCESS) return 6;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
    VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = uint64_t(swci.imageExtent.width) * swci.imageExtent.height * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer upload;
    if (vkCreateBuffer(device, &bci, nullptr, &upload) != VK_SUCCESS) return 7;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, upload, &requirements);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    VkMemoryAllocateInfo mai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = requirements.size; mai.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mai.memoryTypeIndex = i; break; }
    VkDeviceMemory uploadMemory;
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &uploadMemory) != VK_SUCCESS) return 8;
    vkBindBufferMemory(device, upload, uploadMemory, 0);
    unsigned char* mapped;
    if (vkMapMemory(device, uploadMemory, 0, VK_WHOLE_SIZE, 0, (void**)&mapped) != VK_SUCCESS) return 9;
    VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = family;
    VkCommandPool pool; vkCreateCommandPool(device, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer commands; vkAllocateCommandBuffers(device, &cai, &commands);
    VkSemaphoreCreateInfo semInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired; vkCreateSemaphore(device, &semInfo, nullptr, &acquired);
    std::vector<VkSemaphore> rendered(images.size());
    for (auto& semaphore : rendered) vkCreateSemaphore(device, &semInfo, nullptr, &semaphore);
    const int frames = argc > 1 ? std::atoi(argv[1]) : 1200;
    for (int frame = 0; frame < frames; ++frame)
    {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        for (uint32_t y = 0; y < swci.imageExtent.height; ++y)
            for (uint32_t x = 0; x < swci.imageExtent.width; ++x)
            {
                uint32_t hash = (x + frame/2) * 374761393u + (y + frame/5) * 668265263u;
                hash = (hash ^ (hash >> 13)) * 1274126177u;
                const unsigned char value = 40 + ((hash >> 24) * 175) / 255;
                const size_t offset = (size_t(y) * swci.imageExtent.width + x) * 4;
                mapped[offset] = mapped[offset+1] = mapped[offset+2] = value; mapped[offset+3] = 255;
            }
        uint32_t index;
        if (vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index) != VK_SUCCESS) return 10;
        vkResetCommandBuffer(commands, 0);
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; vkBeginCommandBuffer(commands, &begin);
        VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[index]; barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy region {}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        region.imageExtent = {swci.imageExtent.width, swci.imageExtent.height, 1};
        vkCmdCopyBufferToImage(commands, upload, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.oldLayout = barrier.newLayout; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,nullptr,0,nullptr,1,&barrier);
        vkEndCommandBuffer(commands);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&acquired; submit.pWaitDstStageMask=&stage;
        submit.commandBufferCount=1; submit.pCommandBuffers=&commands;
        submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&rendered[index];
        if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return 11;
        VkPresentInfoKHR present { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount=1; present.pWaitSemaphores=&rendered[index];
        present.swapchainCount=1; present.pSwapchains=&swapchain; present.pImageIndices=&index;
        if (vkQueuePresentKHR(queue, &present) != VK_SUCCESS) return 12;
        if (vkQueueWaitIdle(queue) != VK_SUCCESS) return 13;
        Sleep(12);
    }
    printf("Vulkan: rendered %d frames with no DXGI swapchain\n", frames);
    vkDeviceWaitIdle(device);
    for (auto semaphore : rendered) vkDestroySemaphore(device, semaphore, nullptr);
    vkDestroySemaphore(device, acquired, nullptr); vkDestroyCommandPool(device, pool, nullptr);
    vkUnmapMemory(device, uploadMemory); vkDestroyBuffer(device, upload, nullptr); vkFreeMemory(device, uploadMemory, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr); vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr); vkDestroyInstance(instance, nullptr);
    DestroyWindow(window); timeEndPeriod(1);
    return 0;
}
