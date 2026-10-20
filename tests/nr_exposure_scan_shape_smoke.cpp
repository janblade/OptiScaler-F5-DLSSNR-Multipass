// Host check of DlssNr_ExposureScanShape.h (what the exposure scan takes as a candidate on every API, and how its list
// reuses slots). No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_exposure_scan_shape_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_ExposureScanShape.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace DlssNrExposureScanShape;

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

struct Slot
{
    bool alive = false;
    uint32_t generation = 0;
    bool moves = false;
    bool handed = false;
    int rank = 0;
    unsigned int reads = 0;
    int id = -1; // which resource of the replayed order holds the slot
    unsigned int inRange = 0;
    unsigned int notDecimal = 0;
};

struct Waiting
{
    int rank = 0;
    int id = -1;
};

// Replays the order a game made its candidates in, as the scan's adopt does (Core::AdoptLocked): a free slot, or one
// given up by a less likely shape. "b16": a buffer of 16 bytes, "t4": a texture of 4 texels.
static std::vector<std::string> Words(const std::string& order)
{
    std::vector<std::string> out;
    std::string word;
    for (char c : order + " ")
    {
        if (c != ' ')
            word += c;
        else if (!word.empty())
        {
            out.push_back(word);
            word.clear();
        }
    }
    return out;
}

static bool Adopt(std::vector<Slot>& slots, const std::string& word, int id)
{
    const bool isBuffer = word[0] == 'b';
    const int rank = Rank(isBuffer, (uint64_t) std::stoull(word.substr(1)));
    int slot = FreeSlot(slots, 64);

    if (slot < 0)
    {
        const int victim = EvictFor(slots, rank, [](const Slot&) { return true; });
        if (victim >= 0)
        {
            slots[victim].alive = false;
            slot = FreeSlot(slots, 64);
        }
    }

    if (slot < 0)
        return false;
    if ((size_t) slot == slots.size())
        slots.emplace_back();
    slots[slot] = { true, slots[slot].generation + 1, false, false, rank, 0, id };
    return true;
}

// Witcher 3's case (2026-10-20): `count` buffers made at once at load, before NR reads any; only `exposureId` holds a
// decimal number, the rest integers. The ones a full list turns away wait (Core::AdoptLocked); each NR second the
// candidates that held no decimal are let go of and those waiting take their slots (RotateLocked). Returns the seconds
// until the exposure is held, or -1.
static int Rotate(int count, int exposureId, int maxSeconds)
{
    std::vector<Slot> slots;
    std::vector<Waiting> waiting;

    for (int id = 0; id < count; ++id)
    {
        if (!Adopt(slots, "b24", id))
            waiting.push_back({ Rank(true, 24), id });
    }

    for (int second = 0; second <= maxSeconds; ++second)
    {
        for (Slot& s : slots)
        {
            if (s.alive && s.id == exposureId)
                return second;
        }

        // a second of reads: 60 per candidate
        for (Slot& s : slots)
        {
            if (!s.alive)
                continue;
            s.reads += 60;
            (s.id == exposureId ? s.inRange : s.notDecimal) += 60;
        }

        for (Slot& s : slots)
        {
            if (s.alive && HoldsNoDecimal(s))
                s.alive = false;
        }

        while (!waiting.empty() && FreeSlot(slots, 64) >= 0)
        {
            const int next = NextWaiting(waiting);
            const Waiting w = waiting[(size_t) next];
            waiting.erase(waiting.begin() + next);
            const int slot = FreeSlot(slots, 64);
            if ((size_t) slot == slots.size())
                slots.emplace_back();
            slots[slot] = { true, slots[slot].generation + 1, false, false, w.rank, 0, w.id };
        }
    }

    return -1;
}

// The slot the resource made after `before` ends up in, or -1: `atLoad` is made next, before NR reads anything; then
// NR runs and sees the exposure move (moves = true), and `later` is made.
static int Replay(const std::string& before, const std::string& exposure, const std::string& atLoad,
                  const std::string& later)
{
    std::vector<Slot> slots;
    int id = 0;
    for (const std::string& w : Words(before))
        Adopt(slots, w, id++);
    const int exposureId = id;
    Adopt(slots, exposure, id++);
    for (const std::string& w : Words(atLoad))
        Adopt(slots, w, id++);
    for (Slot& s : slots)
        s.moves = s.moves || (s.alive && s.id == exposureId);
    for (const std::string& w : Words(later))
        Adopt(slots, w, id++);
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].alive && slots[i].id == exposureId)
            return (int) i;
    return -1;
}

int main()
{
    // Buffers: Cyberpunk's exposure is in a 16-byte one; 128 is the bound (Nioh 3's junk was 256+)
    CHECK(!SmallBuffer(0));
    CHECK(!SmallBuffer(2)); // cannot hold the float read
    CHECK(SmallBuffer(4));
    CHECK(SmallBuffer(16));
    CHECK(SmallBuffer(128));
    CHECK(!SmallBuffer(129));

    // Textures: 1x1 (NBA, Cyberpunk) up to a 16x16 histogram
    CHECK(SmallTexture(1, 1));
    CHECK(SmallTexture(16, 16));
    CHECK(SmallTexture(256, 1));
    CHECK(!SmallTexture(17, 16));
    CHECK(!SmallTexture(0, 1));
    CHECK(!SmallTexture(1u << 31, 1u << 31)); // no overflow into a small product

    // DXGI formats: one or two float or typeless channels (NBA's handed exposure is R32_TYPELESS)
    CHECK(DxgiFloatFormat(kDxgiR32Typeless).bytes == 4);
    CHECK(DxgiFloatFormat(kDxgiR16Float).bytes == 2);
    CHECK(DxgiFloatFormat(kDxgiR16G16Typeless).bytes == 2);
    CHECK(DxgiFloatFormat(kDxgiR32G32Float).bytes == 4);
    CHECK(std::strcmp(DxgiFloatFormat(kDxgiR32Typeless).name, "R32_TYPELESS") == 0);
    CHECK(DxgiFloatFormat(10).bytes == 0); // R16G16B16A16_FLOAT: a picture
    CHECK(DxgiFloatFormat(42).bytes == 0); // R32_UINT: not a scale
    CHECK(DxgiFloatFormat(28).bytes == 0); // R8G8B8A8_UNORM

    // Vulkan formats: one or two float channels, the first channel's size
    CHECK(VkFloatFormat(kVkR16Sfloat).bytes == 2);
    CHECK(VkFloatFormat(kVkR32Sfloat).bytes == 4);
    CHECK(VkFloatFormat(kVkR16G16Sfloat).bytes == 2);
    CHECK(VkFloatFormat(kVkR32G32Sfloat).bytes == 4);
    CHECK(std::strcmp(VkFloatFormat(kVkR16Sfloat).name, "R16_SFLOAT") == 0);
    CHECK(VkFloatFormat(37).bytes == 0);  // R8G8B8A8_UNORM: a picture
    CHECK(VkFloatFormat(97).bytes == 0);  // R16G16B16A16_SFLOAT: a picture
    CHECK(VkFloatFormat(98).bytes == 0);  // R32_UINT: not a scale
    CHECK(VkFloatFormat(109).bytes == 0); // R32G32B32A32_SFLOAT

    // Slots: append under the cap, reuse a freed one first, -1 when full
    std::vector<Slot> slots;
    CHECK(FreeSlot(slots, 3) == 0);
    slots.push_back({ true, 1 });
    slots.push_back({ true, 1 });
    CHECK(FreeSlot(slots, 3) == 2);
    slots.push_back({ true, 1 });
    CHECK(FreeSlot(slots, 3) == -1);
    slots[1].alive = false;
    CHECK(FreeSlot(slots, 3) == 1);

    // A value copied for the old owner is not the new owner's
    CHECK(SameOwner(slots, 0, 1));
    CHECK(!SameOwner(slots, 1, 1)); // freed
    slots[1] = { true, 2 };         // taken again
    CHECK(!SameOwner(slots, 1, 1));
    CHECK(SameOwner(slots, 1, 2));
    CHECK(!SameOwner(slots, 7, 1)); // out of range

    // A full list makes room for a likelier shape (Rank, EvictFor), on the order three games made their candidates in
    // (the first 64 from each run's log). Each exposure came late: Cyberpunk 2077's copy (a 16-byte buffer) as
    // candidate 63, RDR2's 1x1 and Doom Eternal's 2x2 after the list was full -- they were shut out before.
    CHECK(Rank(false, 1) == 4 && Rank(false, 4) == 4 && Rank(false, 16) == 3 && Rank(false, 256) == 3);
    CHECK(Rank(true, 4) == 3 && Rank(true, 16) == 3 && Rank(true, 20) == 2 && Rank(true, 64) == 2);
    CHECK(Rank(true, 65) == 1 && Rank(true, 128) == 1);
    {
        const std::string cyberpunk =
            "b20 b20 b28 b4 b24 b96 b24 b24 b128 b4 b28 b28 b4 b96 b12 b96 b36 b36 b12 b4 b4 b4 b4 b4 b4 b24 b4 b96 b36 "
            "b36 b4 b4 b128 b128 b128 b128 b128 b128 b128 b128 b128 b128 b128 b128 b128 b128 b128 b128 b128 b128 b128 "
            "b128 b128 b128 b128 b128 b128 b128 b128 b128 t1 t1 b16 b96";
        const std::string rdr2 =
            "b16 b4 b56 b16 b16 b16 b16 b16 b16 b16 b16 b16 b16 b16 b48 b96 b60 b80 b12 b20 t1 b32 b32 b24 b68 b128 b128 "
            "b128 b12 b12 b32 b64 b4 b64 b96 b108 b48 b12 t256 t256 t256 t256 t256 t256 t256 t256 t1 t1 b4 b4 b4 b4 b4 "
            "b40 b32 b32 b16 b32 b64 b128 b64 b16 b64 b12";
        const std::string doom = "b112 b112 b128 b128 b112 b128 b128 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 "
                                 "t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t4 t16 t16 t16 t16 t16 t16 t16 t16 t16 "
                                 "t16 t16 t16 t16 t16 t16 t16 t16 t16 t16 t16 t16 t16 t16 t16 t16";
        // what keeps coming after the list is full: at load, the same shapes as before; once NR runs, also tiny
        // textures that are not the exposure
        std::string load, flood;
        for (int i = 0; i < 100; ++i)
        {
            load += i % 3 == 0 ? "b128 " : i % 3 == 1 ? "b16 " : "t16 ";
            flood += i % 4 == 0 ? "t4 " : i % 4 == 1 ? "b128 " : i % 4 == 2 ? "b16 " : "t1 ";
        }

        CHECK(Replay(cyberpunk, "b16", load, flood) >= 0);
        CHECK(Replay(rdr2 + " " + load, "t1", load + flood, flood) >= 0);
        CHECK(Replay(doom + " " + load, "t4", load + flood, flood) >= 0);

        // The limit: at load nothing has been read, so tiny textures made after a 16-byte exposure buffer push it out
        // when nothing smaller is left (Cyberpunk's own order did not do this; its 1x1 textures came first)
        CHECK(Replay(cyberpunk, "b16", flood, "") == -1);

        // a bigger buffer never pushes anything out, and a shape never pushes out its own rank
        std::string full;
        for (int i = 0; i < 64; ++i)
            full += "b128 ";
        CHECK(Replay(full, "b128", "", "") == -1);
        CHECK(Replay(full, "t1", "t1 t1", "") >= 0);

        // An exposure kept in a small struct buffer (32 bytes), made first in Cyberpunk's order: the 128-byte buffers
        // and its own size coming after do not push it out ...
        CHECK(Replay("", "b32", cyberpunk + " b32 b48 b64 b128 b128", "") >= 0);
        CHECK(Replay(cyberpunk, "b32", "b32 b48 b64 b128 b128", "") >= 0);
        // likelier shapes take the 128-byte buffers' slots first (it went first when every buffer over 16 bytes ranked
        // the same: the lowest slot)
        CHECK(Replay("", "b32", cyberpunk + " b16 b16 t1 t4", "") == 0);
        // ... but likelier shapes still can, once nothing bigger is left, before NR has read it (the known limit, as
        // for a 16-byte one below a flood of tiny textures)
        CHECK(Replay("", "b32", cyberpunk + " " + load, "") == -1);
    }
    {
        // A full list gives up only what its backend allows (Core::AdoptLocked: the newcomer's own API)
        std::vector<Slot> list;
        for (int i = 0; i < 64; ++i)
            Adopt(list, "b128", i);
        CHECK(EvictFor(list, 4, [](const Slot&) { return false; }) == -1);
        CHECK(EvictFor(list, 4, [](const Slot& s) { return s.id == 40; }) == 40);
    }
    {
        // a candidate seen moving, or the handed exposure, is never given up; of the rest, the one read longest
        std::vector<Slot> list;
        for (int i = 0; i < 64; ++i)
            Adopt(list, "b128", i);
        for (Slot& s : list)
            s.moves = true;
        list[5].moves = false;
        list[5].handed = true;
        CHECK(!Adopt(list, "t1", 100));
        list[9].moves = false;
        list[9].reads = 50;
        list[3].moves = false;
        list[3].reads = 10;
        CHECK(Adopt(list, "t1", 101));
        CHECK(list[9].id == 101);
        CHECK(list[5].id == 5);
    }
    {
        // A value that is not a decimal number: an integer read as a float (Witcher 3's 65536 is 9.18e-41), a
        // sentinel; not zero (a buffer not yet written), not a plausible exposure, not a NaN
        CHECK(NotADecimal(9.18355e-41f));
        CHECK(NotADecimal(1e6f) && NotADecimal(-1e6f) && NotADecimal(-1e-30f));
        CHECK(!NotADecimal(0.0f) && !NotADecimal(-0.0f));
        CHECK(!NotADecimal(0.5f) && !NotADecimal(1e-5f) && !NotADecimal(9000.0f) && !NotADecimal(-2.0f));
        float nan = 0.0f;
        nan = nan / nan;
        CHECK(!NotADecimal(nan));

        // It holds no decimal only after enough reads, and never once a plausible value came
        Slot s;
        s.notDecimal = kNotADecimalReads - 1;
        CHECK(!HoldsNoDecimal(s));
        s.notDecimal = kNotADecimalReads;
        CHECK(HoldsNoDecimal(s));
        s.inRange = 1;
        CHECK(!HoldsNoDecimal(s));
    }
    {
        // A full list gives up one that holds no decimal for any newcomer, before one of a lower rank; never the handed
        // exposure
        std::vector<Slot> list;
        for (int i = 0; i < 64; ++i)
            Adopt(list, "t1", i);
        CHECK(!Adopt(list, "b128", 100)); // all rank 4, none read
        list[7].notDecimal = kNotADecimalReads;
        list[7].reads = kNotADecimalReads;
        list[8].notDecimal = kNotADecimalReads;
        list[8].handed = true;
        CHECK(EvictFor(list, 1, [](const Slot&) { return true; }) == 7);
        list[3].rank = 1; // a lower rank, read longer
        list[3].reads = 500;
        CHECK(EvictFor(list, 4, [](const Slot&) { return true; }) == 7);

        // The waiting list: the likeliest shape first, then the first to arrive
        std::vector<Waiting> waiting = { { 2, 0 }, { 3, 1 }, { 3, 2 }, { 1, 3 } };
        CHECK(NextWaiting(waiting) == 1);
        CHECK(NextWaiting(std::vector<Waiting> {}) == -1);
    }
    {
        // Witcher 3: 387 buffers at once at load. Without waiting, one made after the first 64 was never seen; now the
        // list goes through them 64 a second
        CHECK(Rotate(387, 10, 10) == 0);
        CHECK(Rotate(387, 300, 10) == 4); // the fifth 64: 256..319
        CHECK(Rotate(387, 386, 10) == 6);
    }

    printf(fails == 0 ? "nr_exposure_scan_shape_smoke: PASS\n" : "nr_exposure_scan_shape_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
