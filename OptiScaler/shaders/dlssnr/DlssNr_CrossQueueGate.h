#pragma once
// Finished Picture when the game renders on another queue than it presents on (DlssNr_Late.inl).
//
// The guides NR needs are copied on the game's render queue, inside the list that calls DLSS; NR runs at Present on
// the present queue. A CPU check at Present almost always finds that copy unfinished in a GPU-bound game (the CPU is
// ahead), so every frame was skipped (Skyrim with Community Shaders). Instead the present queue now waits on the GPU
// for a gate fence the render queue signals right after that list. The game's own picture comes from the same DLSS
// call, so its present waits for that work anyway, and the extra wait costs nothing.
//
// The one danger: a render queue whose signal sits behind a wait on this very present (the game's own DLSS-G does
// this; tests/nr_finished_queue_smoke.cpp). Then the wait never ends. The watchdog below waits for each gate on the
// CPU; when one is not reached within the timeout it signals the gate itself, which frees the present queue (that one
// frame reads guides still being copied), turns the GPU wait off for the rest of the session and hands back the
// capture's serial, so its slot is never used again (its copy may still be pending, and its fence now runs out of
// order). Header-only, so the WARP test (tests/nr_finished_gate_smoke.cpp) runs the real code.
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace DlssNr::CrossQueueGate
{
class Watchdog
{
  public:
    explicit Watchdog(std::chrono::milliseconds timeout) : timeout(timeout) {}

    ~Watchdog()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
        if (event)
            SetEvent(event);
        if (worker.joinable())
            worker.join();
        if (event)
            CloseHandle(event);
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    // False once a gate had to be freed: from then on frames still rendering at present are skipped, as before.
    bool Enabled() const { return !off.load(); }

    // Watch a GPU wait just queued on the present queue for `gate` to reach `value`. False when the watchdog could not
    // start: the caller must then not queue the wait (it calls this before Wait).
    bool Watch(ID3D12Fence* gate, uint64_t value, uint64_t serial)
    {
        if (!gate || off.load())
            return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (!event)
        {
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event)
                return false;
        }
        if (!worker.joinable())
        {
            try
            {
                worker = std::thread([this] { Run(); });
            }
            catch (...)
            {
                return false;
            }
        }
        items.push_back({ gate, value, serial, std::chrono::steady_clock::now() + timeout });
        wake.notify_one();
        return true;
    }

    // Serials of the captures whose gate the watchdog freed since the last call.
    std::vector<uint64_t> TakeAbandoned()
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<uint64_t> taken;
        taken.swap(abandoned);
        return taken;
    }

    // Gates still being watched (tests).
    size_t Watching()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return items.size();
    }

  private:
    struct Item
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> gate;
        uint64_t value = 0, serial = 0;
        std::chrono::steady_clock::time_point deadline;
    };

    void Run()
    {
        for (;;)
        {
            Item item;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stop || !items.empty(); });
                if (stop)
                    return;
                item = items.front();
            }
            // UINT64_MAX: the device was removed, nothing is waiting any more.
            const auto reached = [&] { return item.gate->GetCompletedValue() >= item.value; };
            bool done = reached();
            while (!done)
            {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    item.deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0)
                    break;
                // The event is shared: an older registration can wake it early, so check the value again.
                if (FAILED(item.gate->SetEventOnCompletion(item.value, event)))
                    break;
                WaitForSingleObject(event, (DWORD) std::min<long long>(left.count(), 100));
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stop)
                        return;
                }
                done = reached();
            }
            if (!done)
            {
                // Record first, signal second: once the present queue moves on, the slot's fence can look finished,
                // and the next capture must already find it abandoned.
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    abandoned.push_back(item.serial);
                }
                off.store(true);
                item.gate->Signal(item.value);
            }
            std::lock_guard<std::mutex> lock(mutex);
            items.pop_front();
        }
    }

    const std::chrono::milliseconds timeout;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Item> items;
    std::vector<uint64_t> abandoned;
    std::thread worker;
    HANDLE event = nullptr;
    std::atomic<bool> off { false };
    bool stop = false;
};
} // namespace DlssNr::CrossQueueGate
