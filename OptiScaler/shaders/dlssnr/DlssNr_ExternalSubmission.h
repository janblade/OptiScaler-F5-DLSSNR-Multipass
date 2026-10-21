#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

// Protected by the caller's NR mutex. A ticket identifies a recording, not merely
// the reusable command-list object. Reset discards it; only a real queue submit
// publishes Completion. Independent of swapchain API and DXGI Present counters.
template<class Completion> class DlssNrExternalSubmission
{
  public:
    enum class Status { Recording, Submitted, Discarded };
    struct Recording
    {
        Status status = Status::Recording;
        uint64_t serial = 0;
        Completion completion {};
    };
    using Ticket = std::shared_ptr<Recording>;

    Ticket Record(const void* list)
    {
        if (!list) return {};
        auto& ticket = pending[list];
        if (!ticket)
        {
            ticket = std::make_shared<Recording>();
            ticket->serial = ++serial;
        }
        return ticket;
    }
    bool Contains(const void* list) const { return pending.find(list) != pending.end(); }
    bool Submitted(const void* list, Completion completion)
    {
        const auto it = pending.find(list);
        if (it == pending.end()) return false;
        it->second->completion = std::move(completion);
        it->second->status = Status::Submitted;
        pending.erase(it);
        return true;
    }
    void Reset(const void* list)
    {
        const auto it = pending.find(list);
        if (it == pending.end()) return;
        it->second->status = Status::Discarded;
        pending.erase(it);
    }
    void Clear()
    {
        for (auto& item : pending) item.second->status = Status::Discarded;
        pending.clear();
    }

  private:
    std::unordered_map<const void*, Ticket> pending;
    uint64_t serial = 0;
};
