#pragma once

// When the Vulkan bridge gives up its D3D12 swapchain. The D3D12 present is what the game's frame rate hangs on, so a
// present that keeps failing (the private D3D12 device was removed or reset, or the swapchain is invalid) must not be
// retried for ever: Present on such a swapchain can block for about a second per call, and the game then looks frozen with
// nothing in the log but the same line. A device that reports itself gone is given up at once. Any other failure only once
// it has lasted: a few in a row AND for a while, so a short run of failures (frame generation's swapchain being remade
// reports DEVICE_REMOVED from the wrapper for a frame or two) does not end frame generation for the session. A present
// that is merely still drawing is not a failure. Header-only and free of D3D and Vulkan: a host test includes it.

#include <cstdint>

namespace native
{
inline constexpr uint64_t kVkOutputFailuresAllowed = 3;
inline constexpr uint64_t kVkOutputFailureMs = 1000;

class VkOutputFailureRule
{
  public:
    // `failed`: the present returned an error (not "still drawing"). `deviceGone`: the D3D12 device reports it was removed.
    // `nowMs`: a steady clock in milliseconds. True once the bridge should be turned off; stays true after that.
    bool OnPresent(bool failed, bool deviceGone, uint64_t nowMs)
    {
        if (!failed)
        {
            _failures = 0;
            return _gaveUp;
        }

        if (_failures++ == 0)
            _firstFailureMs = nowMs;

        if (deviceGone || (_failures >= kVkOutputFailuresAllowed && nowMs - _firstFailureMs >= kVkOutputFailureMs))
            _gaveUp = true;

        return _gaveUp;
    }

    uint64_t Failures() const { return _failures; }
    bool GaveUp() const { return _gaveUp; }

  private:
    uint64_t _failures = 0;
    uint64_t _firstFailureMs = 0;
    bool _gaveUp = false;
};
} // namespace native
