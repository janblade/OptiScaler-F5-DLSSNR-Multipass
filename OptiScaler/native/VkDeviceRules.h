#pragma once

// Which Vulkan device a thing belongs to, for a game that makes more devices than the one it presents with. AC Odyssey on
// dxvk makes a Vulkan device per Direct3D feature level it probes (11_0 down to 9_1) and a D3D12 device or two, all after its
// swapchain, and destroys them again. Everything here is a decision that used to take the last device the game made (or any
// destroyed device) for the one in use: the interop was thrown away and then loaded from a dead device, the bridge was
// reset by a probe's destruction, and the D3D12 side followed the probe's D3D12 device. Header-only and free of Vulkan and
// D3D (the handle types are template parameters): a host test includes it.

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace native
{

// A game's devices and queues as seen at vkCreateDevice: the device and physical device a queue belongs to. A present is
// answered per queue, not with whichever device was made last.
template <class Device, class Physical, class Queue> class VkDeviceRegistry
{
  public:
    struct Entry
    {
        Device device {};
        Physical physical {};
        uint32_t family = 0;
    };

    void NoteDevice(Device device, Physical physical) { _physical[device] = physical; }

    void NoteQueue(Queue queue, Device device, uint32_t family)
    {
        Entry entry;
        entry.device = device;
        entry.family = family;
        const auto it = _physical.find(device);

        if (it != _physical.end())
            entry.physical = it->second;

        _queues[queue] = entry;
    }

    // False for a queue that was not seen.
    bool OfQueue(Queue queue, Entry* out) const
    {
        const auto it = _queues.find(queue);

        if (it == _queues.end())
            return false;

        *out = it->second;
        return true;
    }

    Physical PhysicalOf(Device device) const
    {
        const auto it = _physical.find(device);
        return it != _physical.end() ? it->second : Physical {};
    }

    void Forget(Device device)
    {
        _physical.erase(device);

        for (auto it = _queues.begin(); it != _queues.end();)
        {
            if (it->second.device == device)
                it = _queues.erase(it);
            else
                ++it;
        }
    }

  private:
    std::unordered_map<Device, Physical> _physical;
    std::unordered_map<Queue, Entry> _queues;
};

// What the frame source does with the device a present is on, given the one its interop (the external memory and semaphore
// functions, the shared picture and fence) was loaded for. One device at a time: another device's present is left alone
// until the held one is destroyed.
enum class InteropVerdict
{
    Keep,   // the held device is the present's
    Load,   // nothing is held: load this device
    Refuse  // another device is held and alive
};

template <class Device> InteropVerdict JudgeInteropDevice(Device held, Device present)
{
    if (held == Device {})
        return InteropVerdict::Load;

    return held == present ? InteropVerdict::Keep : InteropVerdict::Refuse;
}

// Whether the Vulkan bridge is concerned with the destruction of `destroyed`: it is the device the bridge presents on, or
// of one already let go of that still waits for its hidden swapchains. Any other device going leaves the bridge, the
// interop state and the shared D3D12 device alone.
template <class Device> bool BridgeConcernedBy(Device destroyed, Device bridge, const std::vector<Device>& retired)
{
    if (destroyed == Device {})
        return false;

    if (destroyed == bridge)
        return true;

    for (const auto& device : retired)
    {
        if (device == destroyed)
            return true;
    }

    return false;
}

// Whether a D3D12 device the game just made becomes the one the D3D12 side works on. Not while the Vulkan bridge's D3D12
// swapchain is up: its device is the current one, and a probe's would take the native input driver and frame generation's
// state away from it.
inline bool AdoptNewD3D12Device(bool vulkanBridgeOutputUp) { return !vulkanBridgeOutputUp; }

} // namespace native
