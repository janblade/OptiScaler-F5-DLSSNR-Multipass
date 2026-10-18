// Host check of DlssNr_ExposureScanShape.h (what the exposure scan takes as a candidate on every API, and how its list
// reuses slots). No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_exposure_scan_shape_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_ExposureScanShape.h"

#include <cstdio>
#include <cstring>

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
};

int main()
{
    // Buffers: Cyberpunk's exposure is in a 16-byte one; 128 is the bound (Nioh 3's junk was 256+)
    CHECK(!SmallBuffer(0));
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

    printf(fails == 0 ? "nr_exposure_scan_shape_smoke: PASS\n" : "nr_exposure_scan_shape_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
