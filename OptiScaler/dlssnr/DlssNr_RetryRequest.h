#pragma once

// A request to retry NR after a failure, made on one thread (the menu) and consumed on another (the one that records NR).
// The menu must not touch the GPU or the backend's lock, so it only counts; the backend compares the count with the last
// one it handled at the start of its next evaluate, where it can drain its own device and rebuild.

#include <atomic>

namespace DlssNr
{
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
