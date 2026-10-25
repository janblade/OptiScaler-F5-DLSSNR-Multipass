#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace xefg
{
// The frames XeFG sent to be shown and the game presents that sent them (xefgSwapChainGetLastPresentStatus), running
// totals in one word so a reader never mixes two presents: presents in the low 24 bits, frames above. Both wrap;
// readers take differences masked to their width.
constexpr uint64_t kShownPresentBits = 24;
constexpr uint64_t kShownPresentMask = (1ull << kShownPresentBits) - 1;
constexpr uint64_t kShownFrameMask = (1ull << (64 - kShownPresentBits)) - 1;

inline void AddShownPresent(std::atomic<uint64_t>& total, uint32_t frames)
{
    auto old = total.load(std::memory_order_relaxed);
    uint64_t next = 0;

    do
    {
        const auto shown = ((old >> kShownPresentBits) + frames) & kShownFrameMask;
        const auto presents = ((old & kShownPresentMask) + 1) & kShownPresentMask;
        next = (shown << kShownPresentBits) | presents;
    } while (!total.compare_exchange_weak(old, next, std::memory_order_relaxed));
}

// Frames XeFG sent to show per real frame, from the presents' own counts: the count asked for is not what is sent (a
// frame XeFG could not generate). It is what XeFG hands its presenter, not what the display shows. Smoothed; Restart
// on a count change. 0 until a present was seen.
class ShownPerFrame
{
  public:
    // A count change takes XeFG up to a frame to apply, so the presents of the first 2 samples after it are not used
    static constexpr int kSkipSamples = 2;

    // `packed`: the totals of AddShownPresent. The first call only takes the baselines.
    void Update(uint64_t packed)
    {
        const auto shownTotal = packed >> kShownPresentBits;
        const auto presentTotal = packed & kShownPresentMask;
        const auto presents = (presentTotal - _presentTotal) & kShownPresentMask;
        const auto shown = (shownTotal - _shownTotal) & kShownFrameMask;
        const bool baselined = _baselined;
        _presentTotal = presentTotal;
        _shownTotal = shownTotal;
        _baselined = true;
        _sample = -1.0;

        if (!baselined || presents == 0)
            return;

        if (_skip > 0)
        {
            --_skip;
            return;
        }

        _sample = (double) shown / (double) presents;
        _value = _known ? _value + 0.1 * (_sample - _value) : _sample;
        _known = true;
    }

    // Count changed: the smoothed value starts over, the first samples are not used
    void Restart()
    {
        _known = false;
        _value = 0.0;
        _sample = -1.0;
        _skip = kSkipSamples;
    }

    // Dynamic MFG was off (or this is a new XeFG): the totals moved on without us, take new baselines
    void Rebase()
    {
        Restart();
        _baselined = false;
    }

    // 0: not known. A stall (0 frames sent) reads as a small value, not as unknown.
    double Value() const { return _known ? std::max(_value, 0.01) : 0.0; }
    // The last Update's own shown per present; < 0 if it had none
    double Sample() const { return _sample; }

  private:
    uint64_t _shownTotal = 0;
    uint64_t _presentTotal = 0;
    bool _baselined = false;
    bool _known = false;
    int _skip = 0;
    double _value = 0.0;
    double _sample = -1.0;
};
} // namespace xefg
