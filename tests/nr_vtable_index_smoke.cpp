// Host check of VtableIndex.h, the lock-free pointer index of the D3D12 depth finder's patched vtables: lookups, a full index,
// and readers that look up while another thread inserts. No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_vtable_index_smoke.cpp
#include "../OptiScaler/resource_tracking/VtableIndex.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using native::VtableIndex;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

// A key like a heap pointer: 16-byte aligned, not 0.
static const void* KeyOf(size_t i)
{
    return (const void*) (uintptr_t) (0x7FF600000000ull + i * 0x40);
}

int main()
{
    // Lookups: a miss, a hit, a null key, a repeated key.
    {
        VtableIndex<16> index;

        CHECK(index.Find(KeyOf(1)) == -1);
        CHECK(index.Find(nullptr) == -1);
        CHECK(index.Insert(KeyOf(1), 7));
        CHECK(index.Find(KeyOf(1)) == 7);
        CHECK(index.Find(KeyOf(2)) == -1);
        CHECK(!index.Insert(KeyOf(1), 9));
        CHECK(index.Find(KeyOf(1)) == 7);
        CHECK(!index.Insert(nullptr, 1));
        CHECK(index.Find(nullptr) == -1);
    }

    // A full index refuses the next key and still finds every one it holds (the slots are twice the entries, so a miss ends).
    {
        constexpr size_t kMax = 16;
        VtableIndex<kMax> index;

        for (size_t i = 0; i < kMax; ++i)
            CHECK(index.Insert(KeyOf(i), (int) i));

        CHECK(!index.Insert(KeyOf(kMax), 99));

        for (size_t i = 0; i < kMax; ++i)
            CHECK(index.Find(KeyOf(i)) == (int) i);

        for (size_t i = kMax; i < kMax + 64; ++i)
            CHECK(index.Find(KeyOf(i)) == -1);
    }

    // The size the finder uses: every key comes back with its own number, whatever collided on the way.
    {
        constexpr size_t kMax = 65536;
        static VtableIndex<kMax> index;

        for (size_t i = 0; i < kMax; ++i)
            CHECK(index.Insert(KeyOf(i), (int) i));

        bool allFound = true;

        for (size_t i = 0; i < kMax; ++i)
            allFound = allFound && index.Find(KeyOf(i)) == (int) i;

        CHECK(allFound);

        bool noneFound = true;

        for (size_t i = kMax; i < kMax * 2; ++i)
            noneFound = noneFound && index.Find(KeyOf(i)) == -1;

        CHECK(noneFound);
        CHECK(!index.Insert(KeyOf(kMax), 1));
    }

    // Readers find while another thread inserts: a key is either not in yet or comes back with its own number, and one whose
    // insert was announced (release, after the insert) is always found.
    {
        constexpr size_t kMax = 20000;
        constexpr int kReaders = 4;

        for (int round = 0; round < 5; ++round)
        {
            // A fresh index per round, on the heap (a million bytes of slots).
            const auto owner = std::make_unique<VtableIndex<kMax>>();
            VtableIndex<kMax>& index = *owner;
            const size_t base = (size_t) round * kMax;

            std::atomic<size_t> announced { 0 };
            std::atomic<bool> done { false };
            std::atomic<int> wrong { 0 };

            std::vector<std::thread> readers;

            for (int r = 0; r < kReaders; ++r)
                readers.emplace_back(
                    [&, r]
                    {
                        size_t probe = (size_t) r * 7919;

                        while (!done.load(std::memory_order_acquire))
                        {
                            const size_t known = announced.load(std::memory_order_acquire);

                            // Everything announced is there.
                            for (size_t k = 0; k < 64 && known != 0; ++k)
                            {
                                const size_t i = (probe + k * 131) % known;

                                if (index.Find(KeyOf(base + i)) != (int) (base + i))
                                    wrong.fetch_add(1);
                            }

                            // Anything at all is absent or right (what the writer is inserting right now may be either).
                            for (size_t k = 0; k < 64; ++k)
                            {
                                const size_t i = (probe + k * 977) % kMax;
                                const int found = index.Find(KeyOf(base + i));

                                if (found != -1 && found != (int) (base + i))
                                    wrong.fetch_add(1);
                            }

                            probe += 101;
                        }
                    });

            // Under one writer, as the caller's lock makes it.
            for (size_t i = 0; i < kMax; ++i)
            {
                const bool inserted = index.Insert(KeyOf(base + i), (int) (base + i));
                CHECK(inserted);
                announced.store(i + 1, std::memory_order_release);
            }

            done.store(true, std::memory_order_release);

            for (auto& t : readers)
                t.join();

            CHECK(wrong.load() == 0);
        }
    }

    printf(fails == 0 ? "vtable index: ok\n" : "vtable index: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
