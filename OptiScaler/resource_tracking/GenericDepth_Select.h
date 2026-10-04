#pragma once

// Picks the scene's depth buffer out of the depth-stencil buffers a game used in one frame, for the native input producer
// (DLSS-NR in a game that makes no upscaler call, so there is no depth to take from one). Plain data in, one pick out: no
// D3D12 here, so tests/nr_generic_depth_smoke.cpp runs it without a game or a GPU. GenericDepth_Dx12.cpp fills it in.
//
// The rules are adapted from ReShade's Generic Depth add-on (examples/09-depth/generic_depth_addon.cpp in
// github.com/crosire/reshade, Copyright (C) 2021 Patrick Mours, BSD-3-Clause; the licence is in
// Licenses/ReShade_GenericDepth_LICENSE.txt): the scene's depth is the buffer with the most vertices drawn to it in a frame
// (draw calls instead when indirect draws dominate), whose size has the picture's shape. A buffer that is only cleared has
// no draws; a shadow map is square or far from the picture's shape. It is a heuristic: a game it gets wrong is a per-game
// limitation, as it is for ReShade.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace GenericDepthSelect
{

struct Candidate
{
    uint64_t id = 0;                 // the resource, as the caller identifies it
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;             // DXGI_FORMAT, only reported
    uint64_t vertices = 0;           // vertices (indices for indexed draws) times instances, over the frame
    uint32_t drawcalls = 0;          // draw calls over the frame, indirect ones included
    uint32_t drawcallsIndirect = 0;  // of which indirect
    uint32_t clears = 0;             // depth clears seen
    int32_t bestClear = -1;          // the clear whose stretch before it drew the most: the snapshot to take (-1 none)
    bool reversed = false;           // cleared to something other than 1.0, so near is 1.0 (reversed-Z)
};

struct Pick
{
    bool valid = false;
    uint64_t id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint64_t score = 0;
    int32_t bestClear = -1;
    bool reversed = false;
};

struct Rules
{
    // ReShade's "similar aspect ratio": the shapes differ by at most this (absolute width/height), and both sides are
    // within this range of the picture's. ReShade's range stops at 1.85 (a buffer down to 0.54x of the picture); that was
    // widened to 4 after Witcher 3, whose scene depth is 1280x720 under a 2560x1440 picture (2.0) and whose shadow maps are
    // square, which the shape test already removes.
    float aspectDelta = 0.1f;
    float minRatio = 0.5f;
    float maxRatio = 4.0f;
    uint32_t warmupFrames = 2;       // a buffer must have been in use this many frames before it can win
    uint32_t quietDrawcalls = 8;     // a frame whose only buffer drew this little is not a real frame (emulators present more)
    float keepFraction = 0.75f;      // the previous pick stays unless something beats it by more than this (1/keep)
    uint32_t holdFrames = 90;        // a pick is kept this many frames in a row while nothing qualifies, then dropped
};

// The metric a buffer is ranked by: vertices, or draw calls when more than a third of them are indirect (the vertex
// count is then not accurate).
inline uint64_t Score(const Candidate& c)
{
    return c.drawcallsIndirect < c.drawcalls / 3 ? c.vertices : (uint64_t) c.drawcalls;
}

inline bool SimilarAspect(float w, float h, float pictureW, float pictureH, const Rules& rules)
{
    if (w == 0.0f || h == 0.0f)
        return true;

    const float wRatio = pictureW / w;
    const float hRatio = pictureH / h;
    const float delta = pictureW / pictureH - w / h;

    return std::fabs(delta) <= rules.aspectDelta && wRatio <= rules.maxRatio && wRatio >= rules.minRatio &&
           hRatio <= rules.maxRatio && hRatio >= rules.minRatio;
}

inline bool Eligible(const Candidate& c, uint32_t pictureWidth, uint32_t pictureHeight, const Rules& rules)
{
    if (c.width == 0 || c.height == 0 || pictureWidth == 0 || pictureHeight == 0)
        return false;

    // Unused, or only a fullscreen pass or two.
    if (c.drawcalls == 0 || (c.vertices <= 3 && c.drawcallsIndirect == 0))
        return false;

    return SimilarAspect((float) c.width, (float) c.height, (float) pictureWidth, (float) pictureHeight, rules);
}

// Distance in size to the picture, for breaking a tie: the nearer the better.
inline double SizeGap(const Candidate& c, uint32_t pictureWidth, uint32_t pictureHeight)
{
    const double pictureArea = (double) pictureWidth * (double) pictureHeight;
    const double area = (double) c.width * (double) c.height;
    return std::fabs(std::log(area / pictureArea));
}

// Keeps the last pick between frames, so two buffers with similar counts (a depth pre-pass and the main pass) do not trade
// places every frame, and a buffer must have been in use for a couple of frames before it can win (ReShade skips those that
// only just appeared).
class Selector
{
  public:
    Pick Update(const std::vector<Candidate>& frame, uint32_t pictureWidth, uint32_t pictureHeight)
    {
        // Very little activity: not a rendered frame (a second present, an emulator presenting between frames).
        if (frame.size() == 1 && frame[0].drawcalls <= _rules.quietDrawcalls)
            return _pick;

        ++_frame;

        // A new picture size means the old pick was measured against another one.
        if (pictureWidth != _lastWidth || pictureHeight != _lastHeight)
        {
            _pick = Pick {};
            _misses = 0;
            _lastWidth = pictureWidth;
            _lastHeight = pictureHeight;
        }

        std::unordered_map<uint64_t, uint64_t> seen;
        seen.reserve(frame.size());

        const Candidate* best = nullptr;
        const Candidate* previous = nullptr;

        for (const auto& c : frame)
        {
            const auto first = _firstSeen.find(c.id);
            seen[c.id] = first != _firstSeen.end() ? first->second : _frame;

            if (!Eligible(c, pictureWidth, pictureHeight, _rules))
                continue;

            // Not until it has been in use for a few frames.
            if (_frame < seen[c.id] + _rules.warmupFrames)
                continue;

            if (_pick.valid && c.id == _pick.id)
                previous = &c;

            if (best == nullptr || Score(c) > Score(*best) ||
                (Score(c) == Score(*best) &&
                 SizeGap(c, pictureWidth, pictureHeight) < SizeGap(*best, pictureWidth, pictureHeight)))
                best = &c;
        }

        _firstSeen = std::move(seen);

        const Candidate* chosen = best;

        if (previous != nullptr && best != nullptr && (float) Score(*previous) >= _rules.keepFraction * (float) Score(*best))
            chosen = previous;

        if (chosen == nullptr)
        {
            // Nothing qualifies this frame: a pick that was there stays for a while, so a buffer that drops out for a frame
            // or two (a menu, a cut) does not make the answer flicker.
            if (_pick.valid && _misses < _rules.holdFrames)
                ++_misses;
            else
            {
                _pick = Pick {};
                _misses = 0;
            }
        }
        else
        {
            _misses = 0;
            _pick = Pick { true,           chosen->id,        chosen->width,     chosen->height,
                           chosen->format, Score(*chosen),    chosen->bestClear, chosen->reversed };
        }

        return _pick;
    }

    const Pick& Current() const { return _pick; }
    void Reset()
    {
        _pick = Pick {};
        _firstSeen.clear();
        _frame = 0;
        _misses = 0;
        _lastWidth = 0;
        _lastHeight = 0;
    }
    Rules& Settings() { return _rules; }

  private:
    Rules _rules;
    Pick _pick;
    uint64_t _frame = 0;
    uint32_t _misses = 0;                  // frames in a row the pick was held with nothing qualifying
    uint32_t _lastWidth = 0;
    uint32_t _lastHeight = 0;
    std::unordered_map<uint64_t, uint64_t> _firstSeen; // id -> the frame it first appeared
};

} // namespace GenericDepthSelect
