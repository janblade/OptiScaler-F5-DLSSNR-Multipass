#pragma once
#include <cstdint>

// Superior Scaling on native Vulkan (dlssnr/DlssNr_Superior_Vk.inl): when the Vulkan path runs it, and what the menu
// says when it does not. A pure rule, so the host test (tests/nr_superior_vk_rule_smoke.cpp) can state every case.
//
// The Vulkan path runs Superior's composition (Area downsample, matched guides, the private DLSS CNN or Transformer or
// none, low field, guided resolve), unpacked, in one model pass, below 100% Model resolution, and at 100% and above
// when [DlssNr] SuperiorAtNative applies (the private DLSS at native): the cases D3D12 runs the same way. Where it does
// not run, the ordinary Matched residual composition stays, as it was before Superior had a Vulkan path. What the
// private DLSS itself says (making it, falling back to No DLSS) is its own line, not an outcome here.
namespace DlssNr::SuperiorScaling::VkRule
{
enum class Outcome : uint8_t
{
    NotSelected,  // another Upscale Mode
    Runs,         // the No DLSS composition runs
    NotReduced,   // 100% and above without the private DLSS at native (No DLSS, Also at 100% and above off, ...)
    Packed,       // Compress screen edges packs the model's picture
    MorePasses,   // more than one model pass
    DetailReuse,  // Reuse detail is on
    DeviceLimits, // the GPU cannot hold the low field pass's shared memory
    PassFailed    // the passes could not be built on this device
};

struct Frame
{
    bool selected = false;
    bool reduced = false; // the model works on fewer pixels than the frame (below 100%)
    // [DlssNr] SuperiorAtNative applies (StabilizeAtNative: 100% and above, a private DLSS wanted and available, on
    // Vulkan no Compress screen edges): the private DLSS steadies the native raster, above 100% the pair the
    // supersample down-leg averaged to native.
    bool atNative = false;
    bool packed = false; // Compress screen edges is packing this frame
    unsigned passes = 1;
    bool detailReuse = false;
    bool deviceCanRun = true;
    bool passBuilt = true;
};

constexpr Outcome Decide(const Frame& f)
{
    return !f.selected                 ? Outcome::NotSelected
           : f.packed                  ? Outcome::Packed
           : !f.reduced && !f.atNative ? Outcome::NotReduced
           : f.passes > 1              ? Outcome::MorePasses
           : f.detailReuse             ? Outcome::DetailReuse
           : !f.deviceCanRun           ? Outcome::DeviceLimits
           : !f.passBuilt              ? Outcome::PassFailed
                                       : Outcome::Runs;
}

// The low field pass keeps a 36 x 68 tile of RGB floats in shared memory (superior/RawLow.hlsl). D3D12 packs a float3
// in 12 bytes (29,376); the std430 bound for a SPIR-V float3 array is the 16-byte stride, which is what a Vulkan driver
// may reserve, so the device must offer that.
constexpr uint32_t kLowFieldSharedBytes = 36u * 68u * 4u * 4u;
constexpr bool SharedMemoryEnough(uint32_t maxComputeSharedMemorySize)
{
    return maxComputeSharedMemorySize >= kLowFieldSharedBytes;
}

// The line under the Upscale Mode controls. Empty when nothing needs saying.
constexpr const char* Describe(Outcome outcome)
{
    switch (outcome)
    {
    case Outcome::NotSelected:
        return "";
    case Outcome::Runs:
        return "";
    case Outcome::NotReduced:
        return "On Vulkan, Superior Scaling works below 100% Model resolution, and at 100% and above with CNN or "
               "Transformer while Also at 100% and above is on; otherwise the Matched residual composition is used.";
    case Outcome::Packed:
        return "Superior Scaling does not run with Compress screen edges on Vulkan yet; Matched residual is used.";
    case Outcome::MorePasses:
        return "Superior Scaling runs one model pass on Vulkan; with more passes Matched residual is used.";
    case Outcome::DetailReuse:
        return "Superior Scaling and Reuse detail do not run together on Vulkan yet; Matched residual is used.";
    case Outcome::DeviceLimits:
        return "This GPU cannot hold Superior Scaling's low field pass; Matched residual is used.";
    case Outcome::PassFailed:
        return "Superior Scaling's Vulkan passes could not be built on this GPU; Matched residual is used.";
    }
    return "";
}
} // namespace DlssNr::SuperiorScaling::VkRule
