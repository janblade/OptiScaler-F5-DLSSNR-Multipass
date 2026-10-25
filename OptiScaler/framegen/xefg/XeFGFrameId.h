#pragma once

// XeFG's frame IDs. No D3D, no Config: a host test can include it.
//
// Intel: XeLL's markers and xefgSwapChainSetPresentId carry the same frame ID, and XeFG tags the frame's resources and
// constants with it too. We keep that for our own XeLL, whose IDs rise by 1 a frame: a frame takes the ID of the frame
// being rendered, picked at the frame's first use and kept for all of its tags and its present. The game's own Reflex
// IDs are not followed: they skip (a tester's Forza Motorsport log: 42% of the steps are 2 or more) and are read from
// the latest marker, not the frame's, and after a skip XeFG shows only the real frame. XeFG's IDs rise by exactly 1 a
// frame whatever XeLL says, except where a pause or a restart of XeLL's IDs resets XeFG's history anyway: there our
// XeLL is followed again. xefg_swapchain.h asks for an ID unique among the frames XeFG holds (frames may even reach it
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

// Why a frame took XeFG's own counter although our XeLL gave an ID
enum class OwnReason
{
    None,
    Jumped, // the ID went up by more than 1
    Unusable // the ID is not above XeFG's last (it repeats, or XeFG still holds it)
};

struct FrameIdPick
{
    uint32_t id = 0;
    bool followsXell = false;
    bool resetHistory = false; // a followed ID that is not XeFG's last + 1: XeFG's history is reset
    OwnReason ownReason = OwnReason::None;
    bool picked = false; // picked by this call (not the frame's cached ID)
};

// Slots match IFGFeature's frame slots (frame number % kSlots)
template <int kSlots> class FrameIds
{
  public:
    // The ID of FG frame `fgFrame`: the cached one when the frame already has one, else picked now with `xellId`.
    // `resetFrame`: XeFG's history is reset on this frame anyway. `canReset`: a reset is honoured at all.
    FrameIdPick For(uint64_t fgFrame, uint32_t xellId, XellIdSource source, bool resetFrame = false,
                    bool canReset = true)
    {
        const auto slot = (int) (fgFrame % kSlots);

        if (_slotFrame[slot] == fgFrame && _slot[slot].id != 0)
            return _slot[slot];

        // Frames passed without a pick (FG paused or skipped): our XeLL ran on without us
        const bool gap = _lastPickedFrame != 0 && fgFrame > _lastPickedFrame + 1;
        auto pick = Pick(xellId, source, canReset && (resetFrame || gap), canReset);

        if (fgFrame > _lastPickedFrame)
            _lastPickedFrame = fgFrame;

        _slotFrame[slot] = fgFrame;
        _slot[slot] = pick;
        _lastId = pick.id;
        _recent.Add(pick.id);
        pick.picked = true;
        return pick;
    }

    uint32_t LastFollowedXell() const { return _lastFollowedXell; }
    uint32_t LastId() const { return _lastId; }

  private:
    FrameIdPick Pick(uint32_t xellId, XellIdSource source, bool resync, bool canReset)
    {
        // Only our XeLL is followed. Mid-stream that is only while its ID is XeFG's last + 1. Where XeFG's history is
        // reset anyway (`resync`), after the first one, or on a step back (our XeLL restarted its IDs), any ID is
        // followed, and the reset is marked. An ID XeFG holds, or one that stays the same, is not followed.
        if (xellId == 0 || source != XellIdSource::Ours)
        {
            _lastFollowedXell = 0;
            return { Next(), false, false, OwnReason::None, false };
        }

        const bool step = xellId == _lastId + 1;
        const bool first = _lastFollowedXell == 0;
        const bool back = !first && xellId < _lastFollowedXell;
        const bool free = canReset ? (first || back || resync) : (first && _lastId == 0);

        if (!_recent.Contains(xellId) && (step || free))
        {
            _lastFollowedXell = xellId;
            return { xellId, true, _lastId != 0 && !step, OwnReason::None, false };
        }

        return { Next(), false, false, xellId > _lastId + 1 ? OwnReason::Jumped : OwnReason::Unusable, false };
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
    uint64_t _lastPickedFrame = 0;
    uint32_t _lastId = 0;
    uint32_t _lastFollowedXell = 0;
};
} // namespace xefg
