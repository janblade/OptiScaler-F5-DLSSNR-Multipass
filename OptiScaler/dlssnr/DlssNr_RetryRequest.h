#pragma once

// A request to retry NR after a failure, made on one thread (the menu) and consumed on another (the one that records NR).
// The menu must not touch the GPU or the backend's lock, so it only counts; the backend compares the count with the last
// one it handled at the start of its next evaluate, where it can drain its own device and rebuild.

#include <atomic>

namespace DlssNr
{
// What a consumed retry does about the device it finds. Release: the device the failure happened on is still the game's,
// so drain it and let go of NR's objects. Abandon: the game made a new device since (the old one is destroyed and took
// every handle with it), so nothing may be called on it; the handles are only forgotten. None: nothing to retry.
enum class RetryStep { None, Release, Abandon };

constexpr RetryStep DecideRetry(bool requested, bool failed, bool permanent, bool haveDevice, bool deviceChanged)
{
    if (!requested || !failed || permanent)
        return RetryStep::None;

    return haveDevice && deviceChanged ? RetryStep::Abandon : RetryStep::Release;
}

class RetryRequest
{
  public:
    void Request() { _generation.fetch_add(1, std::memory_order_relaxed); }

    // True once for each request (however many were made since the last call). `handled` is the consumer's own record.
    bool Consume(unsigned int& handled) const
    {
        const unsigned int now = _generation.load(std::memory_order_relaxed);

        if (now == handled)
            return false;

        handled = now;
        return true;
    }

  private:
    std::atomic<unsigned int> _generation { 0 };
};
} // namespace DlssNr
