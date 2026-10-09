#pragma once

// What the Vulkan bridge's D3D12 present does when this frame's picture did not reach the back buffer. The D3D12 present
// is what paces the game and keeps the window alive, so a persistent failure must still present; but the swapchain is
// flip-discard, so a present without a copy shows (and lets frame generation interpolate from) a back buffer with undefined
// or stale contents. The first few failures in a row are therefore skipped, so the last good frame stays on screen.
// Header-only and free of D3D and Vulkan: a host test includes it.

#include <cstdint>

namespace native
{
enum class VkCopyOutcome
{
    Copied,     // the picture is in the back buffer
    NoPicture,  // the frame source had none this frame
    CopyFailed  // there was a picture and putting it into the back buffer failed
};

inline constexpr uint64_t kVkCopyFailuresSkipped = 3;

class VkCopyFailureRule
{
  public:
    struct Decision
    {
        bool present = true;    // present the D3D12 swapchain this frame
        bool warn = false;      // say it now (at 1, 10, 100 ... failures in a row); `failures` has the count
        bool recovered = false; // pictures came back after `failures` frames without one
        uint64_t failures = 0;
    };

    Decision OnFrame(VkCopyOutcome outcome)
    {
        Decision decision;

        if (outcome == VkCopyOutcome::Copied)
        {
            decision.recovered = _failures != 0;
            decision.failures = _failures;
            _failures = 0;
            _everCopied = true;
            return decision;
        }

        // No picture before the first one ever arrived is just a game that is still loading: nothing is wrong, nothing
        // to keep on screen, nothing to say. A copy that fails is counted from the start.
        if (!_everCopied && outcome == VkCopyOutcome::NoPicture)
            return decision;

        ++_failures;
        decision.failures = _failures;
        decision.present = !(_everCopied && _failures <= kVkCopyFailuresSkipped);
        decision.warn = _failures == 1 || _failures == 10 || _failures == 100 || _failures == 1000 || _failures == 10000;
        return decision;
    }

  private:
    uint64_t _failures = 0;
    bool _everCopied = false;
};
} // namespace native
