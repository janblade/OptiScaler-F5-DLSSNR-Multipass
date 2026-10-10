// Host check of native::PresentEpoch (native/FrameContract.h): the frame number native input hands to NR. Detail reuse
// reads any step other than 1 between two frames NR ran on as a skipped frame, and a count that stops moving keeps a new
// NR model on "Preparing". No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_present_epoch_smoke.cpp
#include "../OptiScaler/native/FrameContract.h"

#include <cstdio>

using native::Api;
using native::PresentEpoch;

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

// Runs `frames` frames of a game whose DXGI count moves `dxgiStep` and Vulkan count `vulkanStep` per frame, from the given
// starting counts, and says whether the epoch the source of `api` sees moved by exactly 1 every frame.
static bool StepsByOne(Api api, uint64_t dxgi, uint64_t vulkan, uint64_t dxgiStep, uint64_t vulkanStep, int frames = 50)
{
    uint64_t last = PresentEpoch(api, dxgi, vulkan);

    for (int i = 0; i < frames; ++i)
    {
        dxgi += dxgiStep;
        vulkan += vulkanStep;
        const uint64_t now = PresentEpoch(api, dxgi, vulkan);

        if (now != last + 1)
            return false;

        last = now;
    }

    return true;
}

int main()
{
    // A D3D11/D3D12 game: only DXGI presents tick.
    CHECK(StepsByOne(Api::D3D12, 100, 0, 1, 0));
    CHECK(StepsByOne(Api::D3D11, 100, 0, 1, 0));

    // A native Vulkan game without the bridge (NR only): only Vulkan presents tick.
    CHECK(StepsByOne(Api::Vulkan, 0, 100, 0, 1));

    // A dxvk game read through Vulkan: both tick once per frame (the sum stepped by 2).
    CHECK(StepsByOne(Api::Vulkan, 500, 480, 1, 1));

    // A native Vulkan game bridged to 3x frame generation: the DXGI count runs 3 per frame and ahead of the Vulkan one.
    CHECK(StepsByOne(Api::Vulkan, 9000, 3000, 3, 1));

    // ...then the bridge goes (NR only again): the DXGI count stops far ahead. The larger of the two would stand still.
    CHECK(StepsByOne(Api::Vulkan, 9000, 3000, 0, 1));

    if (fails == 0)
        printf("present epoch: ok\n");

    return fails == 0 ? 0 : 1;
}
