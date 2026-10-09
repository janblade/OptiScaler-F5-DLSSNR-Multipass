#pragma once

// The Vulkan flavour of Reflex's Sleep (NvAPI_Vulkan_*, the pre-VK_NV_low_latency2 interface) is a pair: the device has to
// be initialised as a low-latency device once, which hands back a timeline semaphore, and each frame's Sleep is given a
// value the driver signals on that semaphore once it is time to go on -- the app has to wait for it. One device's state and
// the order of calls, with the calls themselves passed in: no NVAPI, no Vulkan, so a host test can drive it.
//
// Anything short of a working pair leaves the device "unavailable" and makes no further call: a Sleep without the
// semaphore signals a null handle in fakenvapi (the vendor-neutral Reflex stand-in), which crashes.

#include <cstdint>
#include <functional>

namespace native::lowlatency
{
enum class VkSleepState
{
    Unknown,
    Ready,
    Unavailable
};

enum class VkSleepWhy
{
    None,
    NoTimelineSemaphores, // the device was made without the timeline semaphore feature
    InitFailed,           // the init call failed (initStatus has its NVAPI status)
    NoSemaphore,          // the init call succeeded and returned no semaphore
    WaitTimedOut          // the Sleep's value never arrived, several frames in a row
};

struct VkSleepSync
{
    VkSleepState state = VkSleepState::Unknown;
    VkSleepWhy why = VkSleepWhy::None;
    int initStatus = 0; // NVAPI status of the init call, 0 = OK
    void* semaphore = nullptr;
    uint64_t value = 0; // the last value given to Sleep; the next frame's is one more
    bool timeoutWarned = false;
    uint32_t timeouts = 0; // waits in a row that timed out
};

// What the sync calls. All return success as 0 / true.
struct VkSleepCalls
{
    std::function<bool()> timelineSemaphoresOn;      // the device has the timeline semaphore feature
    std::function<int(void** semaphore)> init;       // NvAPI_Vulkan_InitLowLatencyDevice
    std::function<int(uint64_t value)> sleep;        // NvAPI_Vulkan_Sleep
    std::function<bool(void* semaphore, uint64_t value, uint64_t timeoutNs)> wait; // vkWaitSemaphores; true = reached
};

// How long the present thread waits for a Sleep's value; the signal is made inside the Sleep call, so this is never a
// real wait unless something is wrong.
inline constexpr uint64_t kVkSleepWaitNs = 100ull * 1000 * 1000;
inline constexpr uint32_t kVkSleepTimeoutsBeforeGivingUp = 3;

// Initialises the device once; true when Sleep may be called. Makes no NVAPI call at all without the timeline feature,
// and none ever again after a failure.
inline bool VkSleepReady(VkSleepSync& sync, const VkSleepCalls& calls)
{
    if (sync.state != VkSleepState::Unknown)
        return sync.state == VkSleepState::Ready;

    if (!calls.timelineSemaphoresOn || !calls.timelineSemaphoresOn())
    {
        sync.state = VkSleepState::Unavailable;
        sync.why = VkSleepWhy::NoTimelineSemaphores;
        return false;
    }

    void* semaphore = nullptr;
    sync.initStatus = calls.init ? calls.init(&semaphore) : -1;

    if (sync.initStatus != 0)
    {
        sync.state = VkSleepState::Unavailable;
        sync.why = VkSleepWhy::InitFailed;
        return false;
    }

    if (semaphore == nullptr)
    {
        sync.state = VkSleepState::Unavailable;
        sync.why = VkSleepWhy::NoSemaphore;
        return false;
    }

    sync.semaphore = semaphore;
    sync.state = VkSleepState::Ready;
    return true;
}

enum class VkSleepResult
{
    Unavailable, // nothing was called
    Slept,
    SleepFailed, // the Sleep call itself failed: no wait
    TimedOut     // the value never arrived (sync.timeoutWarned says whether this is the first time)
};

// One frame's Sleep: the next value, the call, and the wait for that value on the semaphore.
inline VkSleepResult VkSleepFrame(VkSleepSync& sync, const VkSleepCalls& calls, bool* firstTimeout = nullptr)
{
    if (firstTimeout != nullptr)
        *firstTimeout = false;

    if (!VkSleepReady(sync, calls))
        return VkSleepResult::Unavailable;

    const uint64_t value = sync.value + 1;
    sync.value = value;

    if (calls.sleep(value) != 0)
        return VkSleepResult::SleepFailed;

    if (calls.wait(sync.semaphore, value, kVkSleepWaitNs))
    {
        sync.timeouts = 0;
        return VkSleepResult::Slept;
    }

    if (!sync.timeoutWarned)
    {
        sync.timeoutWarned = true;

        if (firstTimeout != nullptr)
            *firstTimeout = true;
    }

    // A wait that never ends would cost its whole timeout every frame: after a few in a row the device gives up
    if (++sync.timeouts >= kVkSleepTimeoutsBeforeGivingUp)
    {
        sync.state = VkSleepState::Unavailable;
        sync.why = VkSleepWhy::WaitTimedOut;
    }

    return VkSleepResult::TimedOut;
}
} // namespace native::lowlatency
