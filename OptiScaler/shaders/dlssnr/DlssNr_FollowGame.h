#pragma once

// Automatic exposure following the game's own exposure, on frames the game has not exposed yet.
//
// On an unexposed frame the game hands DLSS the exposure it is about to apply in its tone-mapping pass. That value is
// the game's own adaptation, frame by frame: it cannot be fooled by letterboxing, menus, fades or a flash, and it moves
// exactly when the picture on screen moves. Automatic's meter is not the game's, though -- it maps the scene's
// log-average to middle grey (0.18) and RDR2 maps it to about 1.7, a steady 3.2 EV apart in gameplay (measured
// 2026-09-25, -3.14..-3.27 over 70 s). So the game's exposure is not used as it is: the offset between the two is learned
// here, once per session, and the base white point Automatic uses becomes the game's times that offset. The brightness
// slider keeps its meaning (the same EV is the same picture), and only the frame-to-frame movement is the game's.
//
// The offset is the median of log2(Automatic base white point / the game's) over kWindow readings, taken only while the
// frame reads unexposed (DlssNr_AutoTrimDefault.h), and locked from then on. The meter ignores black tiles, so a
// letterboxed cutscene reads the same offset as gameplay; a few loading-screen or menu readings are outvoted by the
// median, and a reading more than kMaxEv from zero is not taken at all.
//
// Locked is not frozen. A game whose exposure never moves (NBA 2K27: 1.3195 all session) drifts from Automatic as the
// view changes, and one learned once at startup sat 1.6 EV from where it settled in gameplay. Track() keeps the offset
// within reach: while Automatic and the game stay within kTrackStartEv of it nothing moves, and brightness follows the
// game exactly; once they have stayed further apart for kTrackPersistMs (a flash or a camera cut is shorter), the
// offset eases toward where they are, at most kTrackRateEvPerSecond, until it is within kTrackStopEv. Readings are
// smoothed over kTrackSmoothSeconds first, so a single frame decides nothing. Held still while Tune measures.
//
// Only for a game whose own exposure never moves. In one whose exposure does (RDR2), the median above is what keeps
// menus, loading screens and fades from dragging the offset, and a menu freezes the game's exposure too, so "steady
// for a moment" would not tell them apart: once the game's exposure has changed kGameMovesChanges separate times
// (each a step of kGameStepEv from where it last settled), Track() stops easing for good (until the offset is learned
// again). One change is not adapting: NBA 2K27 reports 1 while it loads and 1.3195 from then on. A gap in the readings (a pause, alt-tab,
// frames without the game's exposure) starts the wait and the smoothing over, so it cannot count as a disagreement
// that lasted.
//
// Header-only and free of D3D types so it can be exercised on the host (tests/nr_follow_game_smoke.cpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace DlssNrFollowGame
{
constexpr unsigned kWindow = 120; // readings before the offset locks (about 2 s)
constexpr float kMaxEv = 10.0f;   // |offset| beyond this is not a calibration, it is a broken reading

// Track(): the band the offset is left alone in, how long a disagreement must last, how fast the offset eases, and
// where it stops easing.
constexpr float kTrackStartEv = 0.75f;
constexpr unsigned long long kTrackPersistMs = 1500;
constexpr float kTrackRateEvPerSecond = 0.25f;
constexpr float kTrackStopEv = 0.25f;
constexpr float kTrackSmoothSeconds = 0.5f;
constexpr float kGameStepEv = 0.1f;             // a change of the game's own exposure
constexpr unsigned kGameMovesChanges = 3;       // this many changes: the game adapts by itself, no easing
constexpr unsigned long long kTrackGapMs = 250; // no reading for longer than this: start the wait over

// What a Track() call did, for the log.
struct TrackEvent
{
    bool started = false;     // the offset began to ease
    bool settled = false;     // ... and has arrived
    bool stopped = false;     // ... or was stopped on the way (`why`)
    bool gameMoves = false;   // the game's own exposure moved: easing is off from now on
    const char* why = "";     // stopped: "a Tune is measuring", "the readings stopped", "the game's exposure moves"
    float fromEv = 0.0f;      // the offset when it began (started) or began this ease (settled, stopped)
    float toEv = 0.0f;        // the smoothed reading it eases toward (started), the offset now (settled, stopped)
};

class Calibration
{
  public:
    // One pair of base white points from the same frame. True when this reading locked the offset.
    bool Feed(float autoBaseWhitePoint, float gameBaseWhitePoint)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (locked_ || !(autoBaseWhitePoint > 0.0f) || !(gameBaseWhitePoint > 0.0f) ||
            !std::isfinite(autoBaseWhitePoint) || !std::isfinite(gameBaseWhitePoint))
            return false;

        const float ev = std::log2(autoBaseWhitePoint / gameBaseWhitePoint);

        if (!std::isfinite(ev) || std::fabs(ev) > kMaxEv)
            return false;

        window_[filled_++] = ev;

        if (filled_ < kWindow)
            return false;

        std::array<float, kWindow> sorted = window_;
        std::nth_element(sorted.begin(), sorted.begin() + kWindow / 2, sorted.end());
        offsetEv_ = sorted[kWindow / 2];
        locked_ = true;
        smoothedEv_ = offsetEv_;
        lastMs_ = 0;
        outSinceMs_ = 0;
        easing_ = false;
        gameSeen_ = false;
        gameChanges_ = 0;
        gameMoves_ = false;
        return true;
    }

    // Once locked, one pair from the same frame at time nowMs: keep the offset within reach of it (see the header).
    // `hold` keeps the offset where it is and forgets any disagreement building up (Tune is measuring).
    TrackEvent Track(float autoBaseWhitePoint, float gameBaseWhitePoint, unsigned long long nowMs, bool hold)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        TrackEvent event;

        if (!locked_ || !(autoBaseWhitePoint > 0.0f) || !(gameBaseWhitePoint > 0.0f) ||
            !std::isfinite(autoBaseWhitePoint) || !std::isfinite(gameBaseWhitePoint))
            return event;

        const float ev = std::log2(autoBaseWhitePoint / gameBaseWhitePoint);

        if (!std::isfinite(ev) || std::fabs(ev) > kMaxEv)
            return event;

        // Stops an ease on the way, for the log.
        const auto stop = [this, &event](const char* why) {
            if (easing_)
            {
                event.stopped = true;
                event.why = why;
                event.fromEv = easeFromEv_;
                event.toEv = offsetEv_;
            }
            easing_ = false;
            outSinceMs_ = 0;
        };

        // The game's own exposure: once it keeps changing, this game adapts by itself and the median rules (header).
        const float gameEv = std::log2(gameBaseWhitePoint);
        if (!gameSeen_)
        {
            gameSeen_ = true;
            gameSettledEv_ = gameEv;
        }
        else if (std::fabs(gameEv - gameSettledEv_) > kGameStepEv)
        {
            gameSettledEv_ = gameEv;
            ++gameChanges_;
        }
        if (!gameMoves_ && gameChanges_ >= kGameMovesChanges)
        {
            gameMoves_ = true;
            event.gameMoves = true;
            stop("the game's exposure moves");
        }

        // A gap in the readings: the smoothed value is of another moment, and the wait counted wall time.
        if (lastMs_ != 0 && nowMs > lastMs_ && nowMs - lastMs_ > kTrackGapMs)
        {
            stop("the readings stopped");
            smoothedEv_ = ev;
            lastMs_ = nowMs;
            return event;
        }

        const float dt = lastMs_ != 0 && nowMs > lastMs_ ? std::min((float) (nowMs - lastMs_) / 1000.0f, 0.1f) : 0.0f;
        lastMs_ = nowMs;
        smoothedEv_ += (ev - smoothedEv_) * std::min(dt / kTrackSmoothSeconds, 1.0f);

        if (hold)
        {
            stop("a Tune is measuring");
            return event;
        }

        if (gameMoves_)
            return event;

        if (!easing_)
        {
            if (std::fabs(smoothedEv_ - offsetEv_) <= kTrackStartEv)
            {
                outSinceMs_ = 0;
                return event;
            }

            if (outSinceMs_ == 0)
                outSinceMs_ = nowMs;

            if (nowMs - outSinceMs_ < kTrackPersistMs)
                return event;

            easing_ = true;
            easeFromEv_ = offsetEv_;
            event.started = true;
            event.fromEv = offsetEv_;
            event.toEv = smoothedEv_;
        }

        const float gap = smoothedEv_ - offsetEv_;
        const float most = kTrackRateEvPerSecond * dt;
        offsetEv_ += std::clamp(gap, -most, most);

        if (std::fabs(smoothedEv_ - offsetEv_) < kTrackStopEv)
        {
            easing_ = false;
            outSinceMs_ = 0;
            event.settled = true;
            event.fromEv = easeFromEv_;
            event.toEv = offsetEv_;
        }

        return event;
    }

    // The offset is easing toward Automatic right now.
    bool Easing() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return easing_;
    }

    // The game's own exposure has moved since the offset was learned: easing is off (see the header).
    bool GameMoves() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return gameMoves_;
    }

    bool Locked() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return locked_;
    }

    // log2(Automatic / game), valid once Locked().
    float OffsetEv() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return offsetEv_;
    }

    // What the game's base white point is multiplied by. 1 until locked.
    float Scale() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return locked_ ? std::exp2(offsetEv_) : 1.0f;
    }

    unsigned Readings() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return filled_;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        window_ = {};
        filled_ = 0;
        locked_ = false;
        offsetEv_ = 0.0f;
        smoothedEv_ = 0.0f;
        lastMs_ = 0;
        outSinceMs_ = 0;
        easing_ = false;
        easeFromEv_ = 0.0f;
        gameSeen_ = false;
        gameChanges_ = 0;
        gameMoves_ = false;
    }

  private:
    mutable std::mutex mutex_;
    std::array<float, kWindow> window_ {};
    unsigned filled_ = 0;
    bool locked_ = false;
    float offsetEv_ = 0.0f;
    // Track()
    float smoothedEv_ = 0.0f;
    unsigned long long lastMs_ = 0;
    unsigned long long outSinceMs_ = 0; // when the smoothed reading left the band, 0 while inside it
    bool easing_ = false;
    float easeFromEv_ = 0.0f;
    bool gameSeen_ = false;
    float gameSettledEv_ = 0.0f; // the game's own exposure where it last changed to
    unsigned gameChanges_ = 0;   // ... and how many times it has changed since the offset was learned
    bool gameMoves_ = false;
};

inline Calibration& Instance()
{
    static Calibration calibration;
    return calibration;
}
} // namespace DlssNrFollowGame
