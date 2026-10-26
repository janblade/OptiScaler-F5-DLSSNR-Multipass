#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Games that draw several views into one colour allocation (a triple-screen rig: one projection per monitor, side by
// side, plus one combined feature) call the upscaler more than once per frame with different active rectangles.
//
// This decides whether a frame is like that, gives each view a stable role (screen 1, 2, 3 from the left, or the whole
// picture), and says whether NR works on it. Each view NR works on keeps its own model and history (DlssNr_Dx12.cpp,
// the view slots), so the views never rebuild one model against each other. Nothing here touches D3D12: the caller hands
// in what it read from the NGX parameter block. Host-testable (tests/nr_views_smoke.cpp).
namespace DlssNr::Views
{
constexpr size_t kMaxViews = 8;    // screens, left to right
constexpr unsigned int kWholeBit = 8; // the view that covers the whole allocation (the game's combined picture)
constexpr unsigned int kSlots = kMaxViews + 1;

// [DlssNr] MultiViewScreens: bit i = screen i + 1 from the left, bit kWholeBit = the whole picture. 0 (the default):
// NR stands down while several views are drawn, so an untested layout never runs the model on the wrong picture.
inline bool Runs(uint32_t mask, unsigned int slot) { return slot < kSlots && (mask >> slot) & 1u; }

// Bit kOnePictureBit: all screens as one picture. After SR the views are side by side in the one shared output, so NR
// runs once there, on the frame's last view (FeedSlot), with the whole depth and motion allocations. Overrides the
// other bits.
constexpr unsigned int kOnePictureBit = 9;
inline bool OnePicture(uint32_t mask) { return (mask >> kOnePictureBit) & 1u; }

inline unsigned int CountRuns(uint32_t mask)
{
    unsigned int n = 0;
    for (unsigned int i = 0; i < kSlots; ++i)
        n += Runs(mask, i) ? 1 : 0;
    return n;
}

struct Rect
{
    unsigned int x = 0, y = 0, w = 0, h = 0;

    unsigned long long Area() const { return (unsigned long long) w * h; }
    bool AtOrigin() const { return x == 0 && y == 0; }
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// What one evaluate reads: the colour allocation and the active rectangle inside it.
struct Seen
{
    unsigned int allocW = 0, allocH = 0;
    int allocFormat = 0;
    Rect rect;

    bool SameAllocation(const Seen& o) const
    {
        return allocW == o.allocW && allocH == o.allocH && allocFormat == o.allocFormat;
    }
    bool Same(const Seen& o) const { return rect == o.rect && SameAllocation(o); }
    // The game's combined picture: the whole allocation, not one screen of it.
    bool Whole() const { return rect.AtOrigin() && rect.w == allocW && rect.h == allocH; }
};

enum class Verdict
{
    Run,       // not a multi-view frame, or a view NR works on
    StandDown, // a multi-view frame and no view is ticked
    OtherView, // a multi-view frame and this view is not ticked
};

// Epochs (presented frames) without two views in one of them before the layout counts as gone: the game's menu or a mode
// switch is not enough to forget it.
constexpr unsigned int kQuietEpochs = 300;
// ... unless only the whole picture is drawn: AC EVO draws one view per screen from the cockpit and one wide view from
// the outside cameras, and the wide view should not wait seconds for NR. A camera switch is held for this many epochs.
// Switching back is cheap: the caller keeps each view's model for a while, and the layout is remembered (Recalls).
constexpr unsigned int kWholeOnlyEpochs = 10;
// A view not seen for this many epochs no longer counts (a drifting render size leaves old rectangles).
constexpr unsigned int kStaleEpochs = 60;

struct ViewInfo
{
    Rect rect;
    unsigned int slot = 0; // screen index from the left, or kWholeBit
    bool runs = false;
};

struct Snapshot
{
    bool active = false; // several views are being drawn (or were, within kQuietEpochs)
    uint32_t mask = 0;
    size_t count = 0; // live views, screens left to right, then the whole picture
    ViewInfo views[kSlots];
    size_t screens = 0;
    unsigned int allocW = 0, allocH = 0;
    unsigned long long skippedEvaluates = 0;
};

class Detector
{
  public:
    struct Result
    {
        Verdict verdict = Verdict::Run;
        int slot = -1;               // the view's state slot while several views are drawn; -1: the single view's
        bool needsBeforeSr = false;  // one screen of a shared output: only the pre-SR placement keeps to its rectangle
        bool offOrigin = false;
        bool addedCandidate = false; // a view just became a candidate: the caller logs its numbers
        bool latchedNow = false;     // the layout was found by this evaluate
        bool unlatchedNow = false;   // ... or went away
        bool quickRelease = false;   // ... because only the whole picture was drawn (unlatchedNow)
        bool recalled = false;       // latchedNow on a screen of the layout seen before, from its first evaluate
        unsigned int viewInEpoch = 0;
    };

    Result Note(unsigned long long epoch, const Seen& seen, uint32_t mask)
    {
        std::lock_guard<std::mutex> lock(_lock);
        Result result;

        if (_haveEpoch && epoch != _epoch)
        {
            result.unlatchedNow = EndEpoch();
            result.quickRelease = result.unlatchedNow && _releasedQuick;
        }

        if (!_haveEpoch || epoch != _epoch)
        {
            _epoch = epoch;
            _haveEpoch = true;
            _epochCount = 0;
            _epochMulti = false;
            _epochWhole = false;
            _epochScreen = false;
        }

        if (seen.Whole())
            _epochWhole = true;
        else
            _epochScreen = true;

        // This epoch's distinct views.
        bool known = false;
        for (size_t i = 0; i < _epochCount; ++i)
            known = known || _epochViews[i].Same(seen);

        if (!known && _epochCount < kSlots)
            _epochViews[_epochCount++] = seen;

        result.viewInEpoch = (unsigned int) _epochCount;
        result.offOrigin = !seen.rect.AtOrigin();

        // Several views in one allocation: that is the layout. Two sizes in two different textures are a different
        // thing (a mirror, a portrait) that is not handled here.
        size_t sameAllocation = 0;
        for (size_t i = 0; i < _epochCount; ++i)
            sameAllocation += _epochViews[i].SameAllocation(seen) ? 1 : 0;

        // A screen of the layout seen before: the layout is back (the cockpit camera again), found on its first view
        // instead of after one evaluate run as a single view.
        if (!_latched && !seen.Whole() && Recalls(seen))
        {
            _latched = true;
            result.latchedNow = result.recalled = true;
            _quiet = 0;
            _candidateCount = 0;
            for (size_t i = 0; i < _knownCount; ++i)
                _candidates[_candidateCount++] = { _known[i], _epoch };
        }

        if (sameAllocation >= 2)
        {
            _epochMulti = true;
            _quiet = 0;

            if (!_latched)
            {
                _latched = true;
                result.latchedNow = true;
                _candidateCount = 0;
            }

            for (size_t i = 0; i < _epochCount; ++i)
                if (_epochViews[i].SameAllocation(seen))
                    result.addedCandidate |= Remember(_epochViews[i]);
        }

        if (_latched)
        {
            for (size_t i = 0; i < _candidateCount; ++i)
                if (_candidates[i].seen.Same(seen))
                    _candidates[i].lastEpoch = _epoch;
        }

        _mask = mask;

        if (!_latched)
            return result;

        const int slot = SlotOf(seen);
        result.slot = slot;
        result.needsBeforeSr = !seen.Whole();

        if (mask == 0 || slot < 0)
        {
            result.verdict = Verdict::StandDown;
            ++_skipped;
            return result;
        }

        if (Runs(mask, (unsigned int) slot))
            return result;

        result.verdict = Verdict::OtherView;
        ++_skipped;
        return result;
    }

    Snapshot Status() const
    {
        std::lock_guard<std::mutex> lock(_lock);
        Snapshot s;
        s.active = _latched;
        s.mask = _mask;
        s.skippedEvaluates = _skipped;

        if (!_latched)
            return s;

        Seen screens[kMaxViews];
        const size_t n = Screens(screens);
        for (size_t i = 0; i < n; ++i)
            s.views[s.count++] = { screens[i].rect, (unsigned int) i, Runs(_mask, (unsigned int) i) };
        s.screens = n;

        for (size_t i = 0; i < _candidateCount; ++i)
            if (!Stale(_candidates[i]) && _candidates[i].seen.Whole())
            {
                s.views[s.count++] = { _candidates[i].seen.rect, kWholeBit, Runs(_mask, kWholeBit) };
                break;
            }

        for (size_t i = 0; i < _candidateCount; ++i)
            if (!Stale(_candidates[i]))
            {
                s.allocW = _candidates[i].seen.allocW;
                s.allocH = _candidates[i].seen.allocH;
                break;
            }

        return s;
    }

    // The view whose evaluate comes last in a frame while the layout is drawn: the whole picture when the game draws it,
    // else the rightmost screen. By then every view's input and output of the frame are written. -1: no layout.
    int FeedSlot() const
    {
        std::lock_guard<std::mutex> lock(_lock);
        if (!_latched)
            return -1;

        for (size_t i = 0; i < _candidateCount; ++i)
            if (!Stale(_candidates[i]) && _candidates[i].seen.Whole())
                return (int) kWholeBit;

        Seen screens[kMaxViews];
        const size_t n = Screens(screens);
        return n == 0 ? -1 : (int) n - 1;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(_lock);
        _haveEpoch = false;
        _epoch = 0;
        _epochCount = 0;
        _epochMulti = false;
        _latched = false;
        _quiet = 0;
        _candidateCount = 0;
        _mask = 0;
        _skipped = 0;
        _epochWhole = _epochScreen = false;
        _wholeOnly = 0;
        _releasedQuick = false;
        _knownCount = 0;
    }

  private:
    struct Candidate
    {
        Seen seen;
        unsigned long long lastEpoch = 0;
    };

    bool Stale(const Candidate& c) const { return _epoch > c.lastEpoch && _epoch - c.lastEpoch > kStaleEpochs; }

    // Returns true when the layout is gone after this epoch.
    bool EndEpoch()
    {
        if (!_latched || _epochMulti)
        {
            _quiet = _epochMulti ? 0 : _quiet;
            _wholeOnly = 0;
            return false;
        }

        _wholeOnly = _epochWhole && !_epochScreen ? _wholeOnly + 1 : 0;
        const bool quick = _wholeOnly >= kWholeOnlyEpochs;

        if (++_quiet < kQuietEpochs && !quick)
            return false;

        // Left for the whole picture alone (a camera switch): the screens are remembered for when they come back. Left
        // because one view was drawn for a long time: forgotten, or that view would bring the layout straight back.
        Seen screens[kMaxViews];
        _knownCount = quick ? Screens(screens) : 0;
        std::copy(screens, screens + _knownCount, _known);

        _latched = false;
        _candidateCount = 0;
        _quiet = 0;
        _wholeOnly = 0;
        _releasedQuick = quick;
        return true;
    }

    bool Remember(const Seen& seen)
    {
        for (size_t i = 0; i < _candidateCount; ++i)
        {
            if (_candidates[i].seen.Same(seen))
            {
                _candidates[i].lastEpoch = _epoch;
                return false;
            }
        }

        // Full: replace the oldest.
        if (_candidateCount == kSlots)
        {
            size_t victim = 0;
            for (size_t i = 1; i < _candidateCount; ++i)
                if (_candidates[i].lastEpoch < _candidates[victim].lastEpoch)
                    victim = i;
            _candidates[victim] = { seen, _epoch };
            return true;
        }

        _candidates[_candidateCount++] = { seen, _epoch };
        return true;
    }

    // The live views that are not the whole picture, left to right (then top to bottom).
    size_t Screens(Seen (&out)[kMaxViews]) const
    {
        size_t n = 0;
        for (size_t i = 0; i < _candidateCount && n < kMaxViews; ++i)
            if (!Stale(_candidates[i]) && !_candidates[i].seen.Whole())
                out[n++] = _candidates[i].seen;

        std::sort(out, out + n, [](const Seen& a, const Seen& b)
                  { return a.rect.x != b.rect.x ? a.rect.x < b.rect.x : a.rect.y < b.rect.y; });
        return n;
    }

    int SlotOf(const Seen& seen) const
    {
        if (seen.Whole())
            return (int) kWholeBit;

        Seen screens[kMaxViews];
        const size_t n = Screens(screens);
        for (size_t i = 0; i < n; ++i)
            if (screens[i].Same(seen))
                return (int) i;
        return -1;
    }

    mutable std::mutex _lock;
    bool _haveEpoch = false;
    unsigned long long _epoch = 0;
    Seen _epochViews[kSlots];
    size_t _epochCount = 0;
    bool _epochMulti = false;
    bool _epochWhole = false;  // the whole picture was drawn in this epoch
    bool _epochScreen = false; // ... or a view that is not the whole picture

    bool _latched = false;
    unsigned int _wholeOnly = 0; // epochs in a row with only the whole picture
    bool _releasedQuick = false; // the last release was for the whole picture alone
    Seen _known[kMaxViews];      // the screens of the last layout, for when it comes back
    size_t _knownCount = 0;

    bool Recalls(const Seen& seen) const
    {
        for (size_t i = 0; i < _knownCount; ++i)
            if (_known[i].Same(seen))
                return true;
        return false;
    }
    unsigned int _quiet = 0;
    Candidate _candidates[kSlots];
    size_t _candidateCount = 0;
    uint32_t _mask = 0;
    unsigned long long _skipped = 0;
};
} // namespace DlssNr::Views
