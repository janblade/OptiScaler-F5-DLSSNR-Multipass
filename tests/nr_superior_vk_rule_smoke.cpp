// Host check of dlssnr/DlssNr_SuperiorVkRule.h: when Superior Scaling runs on native Vulkan and what the menu says
// otherwise, and that the Upscale Mode combo lets it be selected there.
// cl /std:c++17 /EHsc /W4 tests/nr_superior_vk_rule_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"
#include "../OptiScaler/dlssnr/DlssNr_SuperiorVkRule.h"

#include <cstdio>
#include <cstring>

// The runtime state the shared header declares lives with the D3D12 pipeline; the rule needs none of it.
namespace DlssNr::SuperiorScaling
{
std::string Status() { return {}; }
std::string Unavailable() { return {}; }
bool SrUnavailable() { return false; }
void NgxDx12Shutdown() {}
} // namespace DlssNr::SuperiorScaling

int main()
{
    int fails = 0;
    const auto check = [&](bool ok, const char* what)
    {
        if (!ok)
        {
            std::printf("FAIL: %s\n", what);
            ++fails;
        }
    };
    using namespace DlssNr::SuperiorScaling;
    using VkRule::Decide;
    using VkRule::Frame;
    using VkRule::Outcome;

    Frame f;
    f.selected = true;
    f.reduced = true;
    check(Decide(f) == Outcome::Runs, "selected, reduced, unpacked, one pass: runs");

    Frame off = f;
    off.selected = false;
    check(Decide(off) == Outcome::NotSelected, "not selected: nothing");

    Frame native = f;
    native.reduced = false;
    check(Decide(native) == Outcome::NotReduced, "100% and above: the ordinary composition");

    // SuperiorAtNative (the D3D12 rule, StabilizeAtNative): at 100% and above it runs when the private DLSS steadies native.
    Frame atNative = native;
    atNative.atNative = true;
    check(Decide(atNative) == Outcome::Runs, "100% and above with the private DLSS at native: runs");
    atNative.packed = true;
    check(Decide(atNative) == Outcome::Packed, "at native but packed: Matched residual");
    atNative.packed = false;
    atNative.passes = 2;
    check(Decide(atNative) == Outcome::MorePasses, "at native, two passes: Matched residual");
    atNative.passes = 1;
    atNative.detailReuse = true;
    check(Decide(atNative) == Outcome::DetailReuse, "at native with Reuse detail running: Matched residual");
    // StabilizeAtNative itself: which frames give atNative on (the same function D3D12 calls).
    check(StabilizeAtNative(true, 1.0f, false, false, 1u, false), "100%, CNN: at native");
    check(StabilizeAtNative(true, 1.2f, false, false, 11u, false), "120%, Transformer: at native");
    check(!StabilizeAtNative(true, 1.0f, false, false, 0u, false), "No DLSS: Matched residual at 100%");
    check(!StabilizeAtNative(false, 1.0f, false, false, 1u, false), "Also at 100% off: Matched residual");
    check(!StabilizeAtNative(true, 1.0f, true, false, 1u, false), "Compress screen edges: Vulkan passes not supersampled (VkRule Packed)");
    check(!StabilizeAtNative(true, 1.0f, false, false, 1u, true), "no NGX DLSS: Matched residual");
    check(!StabilizeAtNative(true, 0.8f, false, false, 1u, false), "below 100%: the reduced path, not at native");
    check(StabilizeSupersampled(true, 1920, 1080, 2304, 1296) && !StabilizeSupersampled(true, 1920, 1080, 1920, 1080) &&
              !StabilizeSupersampled(false, 1920, 1080, 2304, 1296),
          "supersampling only above native, under at native");

    Frame packed = f;
    packed.packed = true;
    check(Decide(packed) == Outcome::Packed, "packed: Matched residual");
    packed.reduced = false;
    check(Decide(packed) == Outcome::Packed, "packed at 100%: still said as packed");

    Frame passes = f;
    passes.passes = 2;
    check(Decide(passes) == Outcome::MorePasses, "two passes: Matched residual");
    passes.passes = 1;
    check(Decide(passes) == Outcome::Runs, "one pass: runs");

    Frame reuse = f;
    reuse.detailReuse = true;
    check(Decide(reuse) == Outcome::DetailReuse, "Reuse detail on: Matched residual");

    Frame small = f;
    small.deviceCanRun = false;
    check(Decide(small) == Outcome::DeviceLimits, "a GPU without the shared memory");
    Frame failed = f;
    failed.passBuilt = false;
    check(Decide(failed) == Outcome::PassFailed, "passes that would not build");

    // The order of reasons: packed first, then more passes, then Reuse detail; the device is asked last.
    Frame both = f;
    both.packed = true;
    both.passes = 3;
    both.detailReuse = true;
    check(Decide(both) == Outcome::Packed, "packed comes first");
    both.packed = false;
    check(Decide(both) == Outcome::MorePasses, "then more passes");
    both.passes = 1;
    check(Decide(both) == Outcome::DetailReuse, "then Reuse detail");

    check(VkRule::SharedMemoryEnough(49152) && VkRule::SharedMemoryEnough(VkRule::kLowFieldSharedBytes) &&
              !VkRule::SharedMemoryEnough(VkRule::kLowFieldSharedBytes - 1) && !VkRule::SharedMemoryEnough(16384),
          "the shared memory the low field pass needs (the 16-byte float3 stride bound, 39168 bytes)");
    static_assert(VkRule::kLowFieldSharedBytes == 39168);

    // Words: each fallback says something and names what runs instead; running and not selected are silent (the private
    // DLSS's own state is a line of its own).
    check(std::strlen(VkRule::Describe(Outcome::NotSelected)) == 0, "not selected is silent");
    check(std::strlen(VkRule::Describe(Outcome::Runs)) == 0, "running is silent");
    for (const Outcome o : { Outcome::NotReduced, Outcome::Packed, Outcome::MorePasses, Outcome::DetailReuse,
                             Outcome::DeviceLimits, Outcome::PassFailed })
        check(std::strstr(VkRule::Describe(o), "Matched residual") != nullptr,
              "a fallback names what runs instead");

    // The combo: Superior is selectable on Vulkan (no argument for it any more); the other modes follow the scale.
    for (unsigned percent = 25; percent <= 200; ++percent)
        for (const unsigned mode : { 0u, 1u, 2u, 11u })
            check(CanSelectUpscaleMode(mode, percent / 100.f) ==
                      (mode == 11u || mode == 0u || (mode <= 2u && percent < 100)),
                  "Upscale Mode selection");

    if (fails == 0)
        std::printf("All Superior Vulkan rule checks passed\n");
    return fails ? 1 : 0;
}
