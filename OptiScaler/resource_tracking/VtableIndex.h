#pragma once

// A lock-free index from a pointer to a small number, for a table that is only ever added to: the D3D12 depth finder keeps one
// entry per game command list vtable it patched, and finds it again on every call of a patched function
// (GenericDepth_Dx12.cpp). No Direct3D in here, so tests/nr_vtable_index_smoke.cpp runs it on the host.
//
// Open addressing with linear probing over a power-of-two number of slots, at least twice the most that can be inserted, so a
// probe ends within a few slots at an empty one. A slot is written whole (the value first, then the key with release) and is
// never changed or removed, so a reader that finds a key (acquire) reads the value that goes with it, and one that reaches an
// empty slot knows the key is not in. Inserts are one at a time: the caller holds its own lock. Finds take none.

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace native
{

template <size_t MaxEntries> class VtableIndex
{
  public:
    static constexpr size_t kCapacity = std::bit_ceil(MaxEntries * 2);

    // The value stored for `key`, or -1 when it is not in. Any thread, any time.
    int Find(const void* key) const
    {
        const uintptr_t wanted = (uintptr_t) key;

        if (wanted == 0)
            return -1;

        for (size_t i = Home(wanted);; i = (i + 1) & (kCapacity - 1))
        {
            const uintptr_t found = _slots[i].key.load(std::memory_order_acquire);

            if (found == wanted)
                return _slots[i].value.load(std::memory_order_relaxed);

            if (found == 0)
                return -1;
        }
    }

    // Under the caller's lock. False (and nothing changes) when the key is null or already in, or MaxEntries are in.
    bool Insert(const void* key, int value)
    {
        const uintptr_t wanted = (uintptr_t) key;

        if (wanted == 0 || _count >= MaxEntries)
            return false;

        for (size_t i = Home(wanted);; i = (i + 1) & (kCapacity - 1))
        {
            const uintptr_t found = _slots[i].key.load(std::memory_order_relaxed);

            if (found == wanted)
                return false;

            if (found == 0)
            {
                _slots[i].value.store(value, std::memory_order_relaxed);
                _slots[i].key.store(wanted, std::memory_order_release);
                ++_count;
                return true;
            }
        }
    }

  private:
    struct Slot
    {
        std::atomic<uintptr_t> key { 0 }; // 0: empty
        std::atomic<int> value { -1 };
    };

    static size_t Home(uintptr_t key)
    {
        // Objects are 8 or 16 bytes apart at the least: the low bits say nothing, and the multiply spreads the rest.
        constexpr int kBits = std::countr_zero(kCapacity);
        return (size_t) ((((uint64_t) key >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kBits));
    }

    Slot _slots[kCapacity];
    size_t _count = 0;
};

} // namespace native
