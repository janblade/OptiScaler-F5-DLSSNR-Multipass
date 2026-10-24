#pragma once

// XeFG's frame IDs. No D3D, no Config: a host test can include it.
//
// Intel: XeLL's markers and xefgSwapChainSetPresentId carry the same frame ID, and XeFG tags the frame's resources and
// constants with it too. We keep that for our own XeLL, whose IDs rise by 1 a frame: a frame takes the ID of the frame
// being rendered, picked at the frame's first use and kept for all of its tags and its present. The game's own Reflex
// IDs are not followed: they skip (a tester's Forza Motorsport log: 42% of the steps are 2 or more) and are read from
// the latest marker, not the frame's, and after a skip XeFG shows only the real frame. XeFG's IDs rise by exactly 1 a
// frame whatever XeLL says. xefg_swapchain.h asks for an ID unique among the frames XeFG holds (frames may even reach it
// out of order), so only an ID it gave one of its newest frames is refused.

#include <cstdint>

namespace xefg
{
// The IDs XeFG gave its newest frames
struct FrameIdHistory
{
    static constexpr int kSize = 8; // well past the frames XeFG holds (IFGFeature's 4 slots)

    uint32_t ids[kSize] = {};
    int count = 0;
    int next = 0;

    bool Contains(uint32_t id) const
    {
        for (int i = 0; i < count; ++i)
        {
            if (ids[i] == id)
                return true;
        }

        return false;
    }

    void Add(uint32_t id)
    {
        ids[next] = id;
        next = (next + 1) % kSize;

        if (count < kSize)
            ++count;
    }
};

// Whose XeLL an ID comes from
enum class XellIdSource
{
    None,
    Ours, // our low latency's markers: one ID a frame, rising by 1
    Game  // the game's own Reflex markers
};

struct FrameIdPick
{
    uint32_t id = 0;
    bool followsXell = false;
    bool steppedBack = false; // below the last XeLL ID followed (the game restarted its IDs): XeFG's history is reset
    bool jumped = false;      // our XeLL's ID went up by more than 1: numbered on its own
    bool picked = false;      // picked by this call (not the frame's cached ID)
};

// Slots match IFGFeature's frame slots (frame number % kSlots)
template <int kSlots> class FrameIds
{
  public:
    // The ID of FG frame `fgFrame`: the cached one when the frame already has one, else picked now with `xellId`
    FrameIdPick For(uint64_t fgFrame, uint32_t xellId, XellIdSource source)
    {
        const auto slot = (int) (fgFrame % kSlots);

        if (_slotFrame[slot] == fgFrame && _slot[slot].id != 0)
            return _slot[slot];

        auto pick = Pick(xellId, source);
        _slotFrame[slot] = fgFrame;
        _slot[slot] = pick;
        _lastId = pick.id;
        _recent.Add(pick.id);
        pick.picked = true;
        return pick;
    }

    uint32_t LastFollowedXell() const { return _lastFollowedXell; }

  private:
    FrameIdPick Pick(uint32_t xellId, XellIdSource source)
    {
        // Only our XeLL is followed, and only while its ID is XeFG's last + 1. The first one (nothing followed since our
        // XeLL started) and a step back (the game restarted its IDs) are followed too. An ID XeFG holds, or one that
        // stays the same (XeLL's frame did not advance), is not.
        if (xellId == 0 || source != XellIdSource::Ours)
        {
            _lastFollowedXell = 0;
            return { Next(), false, false, false };
        }

        const bool first = _lastFollowedXell == 0;
        const bool steppedBack = !first && xellId < _lastFollowedXell;

        if ((first || steppedBack || xellId == _lastId + 1) && !_recent.Contains(xellId))
        {
            _lastFollowedXell = xellId;
            return { xellId, true, steppedBack, false };
        }

        return { Next(), false, false, !first && xellId > _lastId + 1 };
    }

    uint32_t Next()
    {
        uint32_t id = 0;

        do
        {
            id = ++_lastId;
        } while (id == 0 || _recent.Contains(id));

        return id;
    }

    uint64_t _slotFrame[kSlots] = {};
    FrameIdPick _slot[kSlots] = {};
    FrameIdHistory _recent;
    uint32_t _lastId = 0;
    uint32_t _lastFollowedXell = 0;
};
} // namespace xefg
