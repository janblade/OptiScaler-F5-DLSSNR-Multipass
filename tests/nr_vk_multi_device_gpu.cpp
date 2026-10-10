// GPU test for a Vulkan game that makes more devices after its swapchain (AC Odyssey on dxvk: a Vulkan device per Direct3D
// feature level it probes, 11_0 down to 9_1, plus D3D12 devices, all made and destroyed again). Real Vulkan devices and a
// real D3D12 device, no game and no OptiScaler: the decisions are the ones OptiScaler takes (native/VkDeviceRules.h,
// native/VkOutputFailureRule.h), wired here the way native/VkFrameSource.cpp, native/VkPresentBridge.cpp and
// hooks/D3D12_Hooks.cpp wire them.
//   - the game presents with device A and its queue; the interop (external memory and semaphore functions, the shared
//     picture and fence on a private D3D12 device) is held for A,
//   - the game makes and destroys six more Vulkan devices (one per feature level) and seven D3D12 devices,
//   - the present is answered with A (the queue's own device), never with the device made last,
//   - nothing is loaded on, or acted on for, any other device: no interop load, the interop and the shared picture and
//     fence survive, the bridge stays up, the current D3D12 device stays the bridge's,
//   - a present that does come from another live device is refused (and loads nothing), and that device going again
//     leaves the held interop alone,
//   - the bridge gives up its D3D12 swapchain when the device is gone or its present keeps failing, not before.
//
// Built with -DVK_MULTI_DEVICE_OLD the same test takes the decisions the code took before the fix (the last device made is
// the present's, any destroyed device resets the bridge, every D3D12 device replaces the current one): it fails.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 /Iexternal\vulkan\include tests\nr_vk_multi_device_gpu.cpp OptiScaler\native\SharedFrameVk.cpp
//      d3d12.lib dxgi.lib vulkan-1.lib /link /LIBPATH:OptiScaler\library\vulkan

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../OptiScaler/native/SharedFrameVk.h"
#include "../OptiScaler/native/VkDeviceRules.h"
#include "../OptiScaler/native/VkOutputFailureRule.h"

using Microsoft::WRL::ComPtr;

namespace
{

int g_failures = 0;

bool Check(const char* what, bool value)
{
    printf("  %-96s %s\n", what, value ? "ok" : "FAIL");

    if (!value)
        ++g_failures;

    return value;
}

struct Vk
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = 0;

    bool Init()
    {
        VkApplicationInfo app {};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo {};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &app;

        if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS)
            return false;

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());

        for (auto candidate : devices)
        {
            VkPhysicalDeviceProperties properties {};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            LUID luid {};

            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
                native::VkPhysicalDeviceLuid(candidate, luid))
            {
                physical = candidate;
                printf("Vulkan device: %s\n", properties.deviceName);
                break;
            }
        }

        if (physical == VK_NULL_HANDLE)
            return false;

        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> familyList(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, familyList.data());

        for (uint32_t i = 0; i < families; ++i)
        {
            if (familyList[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                family = i;
                break;
            }
        }

        return true;
    }

    bool MakeDevice(VkDevice* device, VkQueue* queue) const
    {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo {};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        const char* deviceExtensions[] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                           VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };

        VkPhysicalDeviceVulkan12Features features12 {};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.timelineSemaphore = VK_TRUE;

        VkDeviceCreateInfo deviceInfo {};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = &features12;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 2;
        deviceInfo.ppEnabledExtensionNames = deviceExtensions;

        if (vkCreateDevice(physical, &deviceInfo, nullptr, device) != VK_SUCCESS)
            return false;

        vkGetDeviceQueue(*device, family, 0, queue);
        return true;
    }
};

using Registry = native::VkDeviceRegistry<VkDevice, VkPhysicalDevice, VkQueue>;

// What native::VkFrameSource holds for one device: the interop, the private D3D12 device, the shared fence and picture.
struct FrameSlot
{
    native::VkInterop interop;
    ComPtr<ID3D12Device> device12;
    native::SharedFenceVk fence;
    native::SharedImageVk picture;
    std::map<VkDevice, int> loads;
    VkDevice lastPresent = VK_NULL_HANDLE; // VkFrameSource's _device: the last present's device, refused or not

    void Release()
    {
        picture.Reset();
        fence.Reset();
        interop = native::VkInterop {};
    }

    // VkFrameSource::EnsureDevices
    bool Ensure(VkPhysicalDevice physical, VkDevice presentDevice, std::string& why)
    {
        lastPresent = presentDevice;

#ifdef VK_MULTI_DEVICE_OLD
        // Before the fix: whatever device the present is on replaces the held one, whether or not it is alive
        if (interop.device != presentDevice)
        {
            Release();
            ++loads[presentDevice];

            if (!interop.Load(physical, presentDevice, why))
                return false;
        }
#else
        switch (native::JudgeInteropDevice(interop.device, presentDevice))
        {
        case native::InteropVerdict::Refuse:
            why = "another device is held";
            return false;
        case native::InteropVerdict::Load:
        {
            native::VkInterop loaded;
            ++loads[presentDevice];

            if (!loaded.Load(physical, presentDevice, why))
                return false;

            Release();
            interop = loaded;
            break;
        }
        case native::InteropVerdict::Keep:
            break;
        }
#endif

        if (!device12)
        {
            LUID luid {};
            ComPtr<IDXGIFactory4> factory;

            if (!native::VkPhysicalDeviceLuid(physical, luid) || FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
                return false;

            ComPtr<IDXGIAdapter> adapter;

            if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
                FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12))))
                return false;
        }

        if (!fence.Ready() && !fence.Create(device12.Get(), interop))
        {
            why = fence.Error();
            return false;
        }

        if (!picture.Matches(256, 144, VK_FORMAT_B8G8R8A8_UNORM) &&
            !picture.Create(device12.Get(), interop, 256, 144, DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                            true))
        {
            why = picture.Error();
            return false;
        }

        return true;
    }

    // VkFrameSource::OnDeviceDestroyed
    void OnDeviceDestroyed(VkDevice device)
    {
#ifdef VK_MULTI_DEVICE_OLD
        if (interop.device == device || (lastPresent == device && device12 != nullptr))
#else
        if (native::ReleasedWithDevice(device, interop.device, lastPresent, device12 != nullptr))
#endif
            Release();

        if (lastPresent == device)
            lastPresent = VK_NULL_HANDLE;
    }
};

} // namespace

int main()
{
    Vk vk;

    if (!vk.Init())
    {
        printf("no Vulkan device with a LUID\n");
        return 2;
    }

    // The game's device and queue, registered as hkvkCreateDevice does
    Registry registry;
    VkDevice deviceA = VK_NULL_HANDLE;
    VkQueue queueA = VK_NULL_HANDLE;

    if (!vk.MakeDevice(&deviceA, &queueA))
    {
        printf("device A could not be made (external memory / semaphore win32 missing?)\n");
        return 2;
    }

    registry.NoteDevice(deviceA, vk.physical);
    registry.NoteQueue(queueA, deviceA, vk.family);
    VkDevice lastMade = deviceA; // what the old code kept as "the" device

    FrameSlot slot;
    std::string why;

    // The bridge as OnDeviceDestroying sees it: up on A, A's D3D12 device current
    bool bridgeUp = true;
    ID3D12Device* const bridgeDevice12 = nullptr; // set below once the slot made it
    (void) bridgeDevice12;

    printf("Present on device A\n");

    auto presentDevice = [&](VkQueue queue)
    {
#ifdef VK_MULTI_DEVICE_OLD
        (void) queue;
        return lastMade;
#else
        Registry::Entry entry;
        return registry.OfQueue(queue, &entry) ? entry.device : lastMade;
#endif
    };

    bool ok = Check("first present: the interop is loaded and the shared fence and picture are made",
                    slot.Ensure(vk.physical, presentDevice(queueA), why));

    if (!ok)
    {
        printf("  (%s)\n", why.c_str());
        return 2;
    }

    ID3D12Resource* const pictureBefore = slot.picture.Res12();
    ID3D12Fence* const fenceBefore = slot.fence.Fence12();
    ID3D12Device* const current12Before = slot.device12.Get();
    ID3D12Device* currentD3D12 = slot.device12.Get(); // State::currentD3D12Device, the bridge's

    Check("the interop is held for device A", slot.interop.device == deviceA);

    printf("The game makes and destroys six more Vulkan devices (one per feature level 11_0 .. 9_1)\n");

    int wrongPresentDevice = 0;
    int bridgeResets = 0;

    for (int level = 0; level < 6; ++level)
    {
        VkDevice probe = VK_NULL_HANDLE;
        VkQueue probeQueue = VK_NULL_HANDLE;

        if (!vk.MakeDevice(&probe, &probeQueue))
        {
            Check("probe device made", false);
            break;
        }

        registry.NoteDevice(probe, vk.physical);
        registry.NoteQueue(probeQueue, probe, vk.family);
        lastMade = probe;

        // The game presents on A in between, as it does every frame
        if (presentDevice(queueA) != deviceA)
            ++wrongPresentDevice;

        slot.Ensure(vk.physical, presentDevice(queueA), why);

        // vkDestroyDevice: hkvkDestroyDevice's hooks
        slot.OnDeviceDestroyed(probe);
#ifdef VK_MULTI_DEVICE_OLD
        const bool concerned = true; // the bridge reset on any device
#else
        const bool concerned = native::BridgeConcernedBy(probe, deviceA, std::vector<VkDevice> {});
#endif
        if (concerned)
        {
            bridgeUp = false;
            ++bridgeResets;
        }

        registry.Forget(probe);
        vkDestroyDevice(probe, nullptr);

        if (lastMade == probe)
            lastMade = deviceA; // the old code had no way back: it keeps the destroyed handle
#ifdef VK_MULTI_DEVICE_OLD
        lastMade = probe;
#endif
    }

    printf("The game makes and destroys seven D3D12 devices\n");
    int d3d12Adopted = 0; // times a probe's device replaced the current one

    for (int i = 0; i < 7; ++i)
    {
        ComPtr<ID3D12Device> probe12;
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        LUID luid {};

        if (!native::VkPhysicalDeviceLuid(vk.physical, luid) || FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
            FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&probe12))))
        {
            Check("probe D3D12 device made", false);
            break;
        }

#ifdef VK_MULTI_DEVICE_OLD
        currentD3D12 = probe12.Get();
        ++d3d12Adopted;
#else
        if (native::AdoptNewD3D12Device(bridgeUp))
        {
            currentD3D12 = probe12.Get();
            ++d3d12Adopted;
        }
#endif
        // destroyed again; the old code leaves a dangling pointer in the current device
        probe12.Reset();
    }

    printf("Checks\n");
    Check("every present was answered with device A (the queue's own), not the device made last", wrongPresentDevice == 0);
    Check("the interop is still held for device A", slot.interop.device == deviceA);
    Check("the shared fence and picture were not thrown away", slot.fence.Ready() && slot.picture.Res12() == pictureBefore &&
                                                                   slot.fence.Fence12() == fenceBefore);
    Check("the bridge was not reset by a probe device going", bridgeUp && bridgeResets == 0);
    Check("the current D3D12 device is still the bridge's (no probe device was adopted)",
          currentD3D12 == current12Before && d3d12Adopted == 0);

    int loadsOnOthers = 0;

    for (const auto& [device, count] : slot.loads)
    {
        if (device != deviceA)
            loadsOnOthers += count;
    }

    Check("no interop load happened on any other device", loadsOnOthers == 0);
    Check("device A was loaded exactly once", slot.loads[deviceA] == 1);

#ifndef VK_MULTI_DEVICE_OLD
    printf("A present from another live device\n");
    VkDevice other = VK_NULL_HANDLE;
    VkQueue otherQueue = VK_NULL_HANDLE;

    if (vk.MakeDevice(&other, &otherQueue))
    {
        registry.NoteDevice(other, vk.physical);
        registry.NoteQueue(otherQueue, other, vk.family);
        Check("it is refused", !slot.Ensure(vk.physical, presentDevice(otherQueue), why));
        Check("nothing was loaded for it, and device A's interop is untouched",
              slot.loads[other] == 0 && slot.interop.device == deviceA && slot.fence.Ready());

        // The refused device goes again while A is held: the last present came from it, and the rule before the review
        // fix (`_device == device && _device12 != nullptr`) released A's interop, picture and fence with it
        Check("the rule before the review fix would have released A's interop here",
              slot.interop.device == deviceA && slot.lastPresent == other && slot.device12 != nullptr);
        slot.OnDeviceDestroyed(other);
        Check("the refused device going leaves device A's interop, picture and fence alone",
              slot.interop.device == deviceA && slot.fence.Ready() && slot.picture.Matches(256, 144, VK_FORMAT_B8G8R8A8_UNORM));
        registry.Forget(other);
        vkDestroyDevice(other, nullptr);
        Check("device A is still answered and kept", slot.Ensure(vk.physical, presentDevice(queueA), why) &&
                                                         slot.interop.device == deviceA && slot.loads[deviceA] == 1);

        // The held device goes: another one may be taken on
        VkDevice third = VK_NULL_HANDLE;
        VkQueue thirdQueue = VK_NULL_HANDLE;

        if (vk.MakeDevice(&third, &thirdQueue))
        {
            registry.NoteDevice(third, vk.physical);
            registry.NoteQueue(thirdQueue, third, vk.family);
            Check("a third device presenting is refused while A is held",
                  !slot.Ensure(vk.physical, presentDevice(thirdQueue), why) && slot.loads[third] == 0);
            slot.OnDeviceDestroyed(deviceA);
            Check("when device A is destroyed the interop is released", slot.interop.device == VK_NULL_HANDLE);
            Check("the third device is then taken on (one load)",
                  slot.Ensure(vk.physical, presentDevice(thirdQueue), why) && slot.interop.device == third &&
                      slot.loads[third] == 1);
            slot.Release();
            registry.Forget(third);
            vkDestroyDevice(third, nullptr);
        }
    }
#endif

    printf("The bridge gives up its D3D12 swapchain\n");
    {
        native::VkOutputFailureRule rule;
        Check("a present that works does not give up", !rule.OnPresent(false, false, 0));
        Check("one failure does not", !rule.OnPresent(true, false, 10));
        Check("a success in between starts the count again", !rule.OnPresent(false, false, 20) &&
                                                                !rule.OnPresent(true, false, 30) &&
                                                                !rule.OnPresent(true, false, 40));
        Check("a third failure in a row within a second does not (a swapchain being remade)",
              !rule.OnPresent(true, false, 50));
        Check("failures that go on for a second give up", !rule.OnPresent(true, false, 900) && rule.OnPresent(true, false, 1031));
        Check("and stays given up", rule.OnPresent(false, false, 1040));

        native::VkOutputFailureRule brief;
        Check("two failures a second apart are not enough on their own",
              !brief.OnPresent(true, false, 0) && !brief.OnPresent(true, false, 2000));

        native::VkOutputFailureRule gone;
        Check("a removed device gives up at once", gone.OnPresent(true, true, 0));
    }

    slot.Release();
    vkDestroyDevice(deviceA, nullptr);
    vkDestroyInstance(vk.instance, nullptr);

    printf(g_failures == 0 ? "PASS nr_vk_multi_device_gpu\n" : "FAIL nr_vk_multi_device_gpu (%d checks)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
