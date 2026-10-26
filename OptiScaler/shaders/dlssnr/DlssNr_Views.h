#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Games that draw several views into one colour allocation (a triple-screen rig: one projection per monitor, side by
// side, plus one combined feature) call the upscaler more than once per frame with different active rectangles.
// NR keeps one model feature and one history, so those calls rebuild it against each other and never evaluate; a
// view that does not start at the origin falls back to a pass over the whole output as well.
//
// This decides whether a frame is like that, and which of its views NR works on. Nothing here touches D3D12: the
// caller hands in what it read from the NGX parameter block. Host-testable (tests/nr_views_smoke.cpp).
namespace DlssNr::Views
{
// [DlssNr] MultiViewTarget. Off: NR stands down while several views are drawn (the default; an untested layout never
// runs the model on the wrong picture). The others name a view by what it is, not by where it is in the texture, so the
// setting survives a resolution change.
enum class Pick : unsigned int
{
    Off = 0,
    Largest = 1,  // the biggest view that starts at the origin (a combined "whole picture" feature, else the first screen)
    Smallest = 2, // the smallest view that starts at the origin (the first screen)
};

constexpr unsigned int kPickCount = 3;

inline Pick PickFrom(unsigned int value)
{
    return value < kPickCount ? (Pick) value : Pick::Off;
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
};

enum class Verdict
{
    Run,       // not a multi-view frame, or this is the view that was picked
    StandDown, // a multi-view frame and nothing is picked (or the pick has no view to land on)
    OtherView, // a multi-view frame and another view is the picked one
};

constexpr size_t kMaxViews = 8;
// Epochs (presented frames) without two views in one of them before the layout counts as gone: the game's menu or a mode
// switch is not enough to forget it.
constexpr unsigned int kQuietEpochs = 300;
// A view not seen for this many epochs no longer counts as a candidate (a drifting render size leaves old rectangles).
constexpr unsigned int kStaleEpochs = 60;

struct Snapshot
{
    bool active = false; // several views are being drawn (or were, within kQuietEpochs)
    Pick pick = Pick::Off;
    size_t count = 0; // candidates, sorted for the menu: by x, then the larger first
    Rect rects[kMaxViews];
    unsigned int allocW = 0, allocH = 0;
    bool hasSelected = false;
    Rect selected;
    unsigned long long standDownEvaluates = 0;
    // The pick lands on a view that has to run before SR (a single screen of a shared output).
    bool selectedNeedsBeforeSr = false;
};

class Detector
{
  public:
    struct Result
    {
        Verdict verdict = Verdict::Run;
        bool addedCandidate = false; // a view just became a candidate: the caller logs its numbers
        bool latchedNow = false;     // the layout was found by this evaluate
        bool unlatchedNow = false;   // ... or went away
        unsigned int viewInEpoch = 0;
        // This evaluate is the picked view and the pick is a single screen of a shared output: it has to run before SR.
        bool needsBeforeSr = false;
    };

    Result Note(unsigned long long epoch, const Seen& seen, Pick pick)
    {
        std::lock_guard<std::mutex> lock(_lock);
        Result result;

        if (_haveEpoch && epoch != _epoch)
            result.unlatchedNow = EndEpoch();

        if (!_haveEpoch || epoch != _epoch)
        {
            _epoch = epoch;
            _haveEpoch = true;
            _epochCount = 0;
            _epochMulti = false;
        }

        // This epoch's distinct views.
        bool known = false;
        for (size_t i = 0; i < _epochCount; ++i)
            known = known || (_epochViews[i].rect == seen.rect && _epochViews[i].SameAllocation(seen));

        if (!known && _epochCount < kMaxViews)
            _epochViews[_epochCount++] = seen;

        result.viewInEpoch = (unsigned int) _epochCount;

        // Several views in one allocation: that is the layout. Two sizes in two different textures are a different
        // thing (a mirror, a portrait) that is not handled here.
        size_t sameAllocation = 0;
        for (size_t i = 0; i < _epochCount; ++i)
            sameAllocation += _epochViews[i].SameAllocation(seen) ? 1 : 0;

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

            // Every view of this epoch in the allocation is a candidate.
            for (size_t i = 0; i < _epochCount; ++i)
                if (_epochViews[i].SameAllocation(seen))
                    result.addedCandidate |= Remember(_epochViews[i]);
        }

        if (_latched)
        {
            // A candidate keeps its freshness while it keeps being drawn.
            for (size_t i = 0; i < _candidateCount; ++i)
                if (_candidates[i].seen.rect == seen.rect && _candidates[i].seen.SameAllocation(seen))
                    _candidates[i].lastEpoch = _epoch;
        }

        _pick = pick;

        if (!_latched)
        {
            result.verdict = Verdict::Run;
            return result;
        }

        Seen selected;
        if (pick == Pick::Off || !Select(pick, selected))
        {
            result.verdict = Verdict::StandDown;
            ++_standDown;
            return result;
        }

        if (selected.rect == seen.rect && selected.SameAllocation(seen))
        {
            result.verdict = Verdict::Run;
            result.needsBeforeSr = pick == Pick::Smallest;
            return result;
        }

        result.verdict = Verdict::OtherView;
        ++_standDown;
        return result;
    }

    Snapshot Status() const
    {
        std::lock_guard<std::mutex> lock(_lock);
        Snapshot s;
        s.active = _latched;
        s.pick = _pick;
        s.standDownEvaluates = _standDown;

        if (!_latched)
            return s;

        Seen live[kMaxViews];
        size_t n = 0;
        for (size_t i = 0; i < _candidateCount; ++i)
            if (!Stale(_candidates[i]))
                live[n++] = _candidates[i].seen;

        // The menu lists them left to right, the larger one first where two start together.
        std::sort(live, live + n, [](const Seen& a, const Seen& b)
                  { return a.rect.x != b.rect.x ? a.rect.x < b.rect.x : a.rect.Area() > b.rect.Area(); });

        s.count = n;
        for (size_t i = 0; i < n; ++i)
            s.rects[i] = live[i].rect;

        if (n > 0)
        {
            s.allocW = live[0].allocW;
            s.allocH = live[0].allocH;
        }

        Seen selected;
        if (_pick != Pick::Off && Select(_pick, selected))
        {
            s.hasSelected = true;
            s.selected = selected.rect;
            s.selectedNeedsBeforeSr = _pick == Pick::Smallest;
        }

        return s;
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
        _pick = Pick::Off;
        _standDown = 0;
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
            return false;

        if (++_quiet < kQuietEpochs)
            return false;

        _latched = false;
        _candidateCount = 0;
        _quiet = 0;
        return true;
    }

    bool Remember(const Seen& seen)
    {
        for (size_t i = 0; i < _candidateCount; ++i)
        {
            if (_candidates[i].seen.rect == seen.rect && _candidates[i].seen.SameAllocation(seen))
            {
                _candidates[i].lastEpoch = _epoch;
                return false;
            }
        }

        // Full: replace a stale one, else the oldest.
        if (_candidateCount == kMaxViews)
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

    // The picked view: only a view at the origin can be staged, so the choice is made among those.
    bool Select(Pick pick, Seen& out) const
    {
        bool found = false;
        for (size_t i = 0; i < _candidateCount; ++i)
        {
            const Candidate& c = _candidates[i];
            if (Stale(c) || !c.seen.rect.AtOrigin() || c.seen.rect.Area() == 0)
                continue;

            const bool better = !found ||
                                (pick == Pick::Largest ? c.seen.rect.Area() > out.rect.Area()
                                                       : c.seen.rect.Area() < out.rect.Area());
            if (better)
            {
                out = c.seen;
                found = true;
            }
        }
        return found;
    }

    mutable std::mutex _lock;
    bool _haveEpoch = false;
    unsigned long long _epoch = 0;
    Seen _epochViews[kMaxViews];
    size_t _epochCount = 0;
    bool _epochMulti = false;

    bool _latched = false;
    unsigned int _quiet = 0;
    Candidate _candidates[kMaxViews];
    size_t _candidateCount = 0;
    Pick _pick = Pick::Off;
    unsigned long long _standDown = 0;
};
} // namespace DlssNr::Views
