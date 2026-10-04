#pragma once

// Picks the scene's depth buffer out of the depth-stencil buffers a game used in one frame, for the native input producer
// (DLSS-NR in a game that makes no upscaler call, so there is no depth to take from one). Plain data in, one pick out: no
// D3D12 here, so tests/nr_generic_depth_smoke.cpp runs it without a game or a GPU. GenericDepth_Dx12.cpp fills it in.
//
// The shape is the one ReShade's Generic Depth add-on uses, as a heuristic, not a guarantee: the scene's depth is the buffer
// that is drawn to most between two clears and whose size matches the picture. Shadow maps are drawn to a lot too, but they
// are square or far from the picture's shape; a depth buffer that is only cleared and never drawn to has no draws at all.
// A game this gets wrong is a per-game limitation (as it is for ReShade), not something to chase to zero.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace GenericDepthSelect
{

struct Candidate
{
    uint64_t id = 0;      // the resource, as the caller identifies it
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;  // DXGI_FORMAT, only reported
    uint64_t draws = 0;   // draws into it this frame, all clears together (reported)
    uint64_t peakDraws = 0; // the most draws between two clears (or from the last clear to the end of the frame)
    uint32_t clears = 0;
};

struct Pick
{
    bool valid = false;
    uint64_t id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint64_t score = 0;
};

// How a buffer qualifies. The defaults are wide on purpose: a game's depth is often at its render resolution, below the
// picture, and the draw count is what separates it from the rest.
struct Rules
{
    float aspectTolerance = 0.06f; // width/height against the picture's, as a fraction
    float minSizeFraction = 0.25f; // narrowest allowed, as a fraction of the picture's width
    float maxSizeFraction = 2.0f;  // widest allowed (supersampled depth)
    uint64_t minDraws = 8;         // fewer than this between two clears is not a scene
    float keepFraction = 0.75f;    // the previous pick stays unless something beats it by more than this (1/keep)
};

inline bool Eligible(const Candidate& c, uint32_t pictureWidth, uint32_t pictureHeight, const Rules& rules)
{
    if (c.width == 0 || c.height == 0 || pictureWidth == 0 || pictureHeight == 0)
        return false;

    if (c.peakDraws < rules.minDraws)
        return false;

    const float pictureAspect = (float) pictureWidth / (float) pictureHeight;
    const float aspect = (float) c.width / (float) c.height;

    if (std::fabs(aspect - pictureAspect) > rules.aspectTolerance * pictureAspect)
        return false;

    const float size = (float) c.width / (float) pictureWidth;
    return size >= rules.minSizeFraction && size <= rules.maxSizeFraction;
}

// Distance in size to the picture, for breaking a tie: the nearer the better.
inline double SizeGap(const Candidate& c, uint32_t pictureWidth, uint32_t pictureHeight)
{
    const double pictureArea = (double) pictureWidth * (double) pictureHeight;
    const double area = (double) c.width * (double) c.height;
    return std::fabs(std::log(area / pictureArea));
}

// Keeps the last pick between frames, so two buffers with similar counts (a depth pre-pass and the main pass) do not trade
// places every frame.
class Selector
{
  public:
    Pick Update(const std::vector<Candidate>& frame, uint32_t pictureWidth, uint32_t pictureHeight)
    {
        const Candidate* best = nullptr;
        const Candidate* previous = nullptr;

        for (const auto& c : frame)
        {
            if (!Eligible(c, pictureWidth, pictureHeight, _rules))
                continue;

            if (_pick.valid && c.id == _pick.id)
                previous = &c;

            if (best == nullptr || c.peakDraws > best->peakDraws ||
                (c.peakDraws == best->peakDraws &&
                 SizeGap(c, pictureWidth, pictureHeight) < SizeGap(*best, pictureWidth, pictureHeight)))
                best = &c;
        }

        const Candidate* chosen = best;

        if (previous != nullptr && best != nullptr &&
            (float) previous->peakDraws >= _rules.keepFraction * (float) best->peakDraws)
            chosen = previous;

        if (chosen == nullptr)
            _pick = Pick {};
        else
            _pick = Pick { true, chosen->id, chosen->width, chosen->height, chosen->format, chosen->peakDraws };

        return _pick;
    }

    const Pick& Current() const { return _pick; }
    void Reset() { _pick = Pick {}; }
    Rules& Settings() { return _rules; }

  private:
    Rules _rules;
    Pick _pick;
};

} // namespace GenericDepthSelect
