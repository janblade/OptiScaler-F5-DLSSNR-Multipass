#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace DlssNr::InterPass
{
struct ScheduleGains
{
    float high = 1.0f, low = 1.0f;
};
inline bool GapWorsens(ScheduleGains current, ScheduleGains next)
{
    const double before = std::abs(double(current.high) - current.low);
    const double after = std::abs(double(next.high) - next.low);
    return std::isfinite(before) && std::isfinite(after) && after > 0.15f && after > before;
}
inline unsigned CorrectionCount(uint32_t mask)
{
    unsigned count = 0;
    for (; mask; mask &= mask - 1u)
        ++count;
    return count;
}
struct GainSchedule
{
    static constexpr unsigned Capacity = 32;
    std::array<ScheduleGains, Capacity> gains {};
    unsigned passes = 0;
    bool enabled = false, compoundHigh = true, wholeLow = false;
    uint32_t expectedMask = 0;
    unsigned expectedCovered = 0;

    ScheduleGains Block(unsigned covered, unsigned completed) const
    {
        if (completed <= covered || completed > passes)
            return {};
        double high = compoundHigh ? 1.0 : gains[completed - 1u].high;
        double low = 0.0;
        const unsigned lowFirst = wholeLow ? 0u : covered;
        for (unsigned pass = lowFirst; pass < completed; ++pass)
        {
            low += gains[pass].low;
            if (compoundHigh && pass >= covered)
                high *= gains[pass].high;
        }
        const float h = static_cast<float>(high), l = static_cast<float>(low / (completed - lowFirst));
        return { std::isfinite(h) ? h : 1.0f, std::isfinite(l) ? l : 1.0f };
    }
    bool Due(unsigned pass, unsigned covered) const
    {
        // Correction follows this model and precedes the next; never after the last model.
        if (!enabled || pass >= passes || pass + 1u >= passes || covered > pass)
            return false;
        return GapWorsens(Block(covered, pass + 1u), Block(covered, pass + 2u));
    }
};
template <class GainForPass>
inline GainSchedule MakeGainSchedule(unsigned passes, bool enabled, bool compoundHigh, bool wholeLow,
                                     GainForPass gainForPass)
{
    GainSchedule result;
    result.passes = passes < GainSchedule::Capacity ? passes : GainSchedule::Capacity;
    result.enabled = enabled;
    result.compoundHigh = compoundHigh;
    result.wholeLow = wholeLow;
    if (!enabled)
        return result;
    for (unsigned pass = 0; pass < result.passes; ++pass)
    {
        const auto g = gainForPass(pass);
        result.gains[pass] = { std::isfinite(g.high) ? g.high : 1.0f, std::isfinite(g.low) ? g.low : 1.0f };
    }
    // Predict the successful schedule for history resets, UI and benchmark validation.
    // Runtime Due uses the actual covered count if a correction fails.
    for (unsigned pass = 0; pass + 1u < result.passes; ++pass)
        if (result.Due(pass, result.expectedCovered))
        {
            result.expectedMask |= uint32_t(1) << pass;
            result.expectedCovered = pass + 1u;
        }
    return result;
}
} // namespace DlssNr::InterPass
