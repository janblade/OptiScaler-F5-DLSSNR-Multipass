#pragma once

// Internal to the exposure scan: the candidate list the D3D12 (DlssNr_ExposureScan.cpp), D3D11
// (DlssNr_ExposureScan_Dx11.cpp) and Vulkan (DlssNr_ExposureScan_Vk.cpp) backends share.
//
// Each backend finds candidates on its own API's creation calls and copies their values on its own API's command
// stream, into a readback ring of kSlots slots indexed by candidate slot. What is shared: the list, every candidate's
// statistics, the readout and the recording. The recording's row and the movers log are written once per NR frame, by
// the pass that runs NR: the D3D12 one (also for a D3D11 or Vulkan game whose NR runs on D3D12: the D3D11 bridge, the
// native producer) or the Vulkan one (DlssNrFeature_Vk.cpp).
//
// Everything here is called under Mutex().

#include "DlssNr_ExposureScan.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace DlssNr
{
namespace ExposureScan
{
namespace Core
{
// Enough for engines that crowd the real exposure in with dozens of tiny buffers (Cyberpunk's arrived at slot 61); see
// the D3D12 file for the history.
inline constexpr size_t kMaxCandidates = 64;

// Readback ring depth: the slot read is this many frames behind the slot written, so a read never waits on the GPU.
inline constexpr unsigned int kSlots = 4;

struct Tracked
{
    Api api = Api::D3D12;
    // D3D12/D3D11: the resource, a reference held while alive. Vulkan: the VkBuffer or VkImage handle (no reference
    // exists in Vulkan: the backend drops it when the game destroys it or frees its memory).
    void* handle = nullptr;
    void* device = nullptr; // D3D11: the ID3D11Device it was made on; Vulkan: the VkDevice. D3D12: unused.
    uint64_t memory = 0;    // Vulkan: the VkDeviceMemory it is bound to
    uint32_t layout = 0;    // Vulkan image: the layout the game's last recorded barrier left it in (0: not known)
    bool alive = false;
    uint32_t generation = 0; // counts the slot's owners (DlssNr_ExposureScanShape.h)

    std::string shape;
    bool isBuffer = false;
    unsigned int bytes = 4; // size of the value's first channel: 2 (half) or 4 (float)
    uint32_t format = 0;    // D3D: the DXGI_FORMAT, Vulkan: the VkFormat (textures/images only)

    float latest = 0.0f;
    float lowest = 0.0f;
    float highest = 0.0f;
    unsigned int reads = 0;
    unsigned int inRange = 0; // reads that could plausibly be an exposure
    bool moves = false;
    bool handed = false; // this is the exposure texture the game hands to the upscaler: the known answer
};

std::mutex& Mutex();

// Set while this thread holds Mutex() and calls into its API (nests: a depth count) (a staging resource made, a copy
// recorded): a creation hook reached from inside -- the scan's own resources, or a D3D11On12 device's D3D12 ones --
// passes by instead of taking the lock again, which would deadlock (it is not recursive).
class HeldScope
{
  public:
    HeldScope();
    ~HeldScope();
};

bool Held();

// The scan is wanted (WhitePointSource 2, ScanExposure or RecordExposureScan). Adopting also needs NR on (Active).
bool Wanted();
bool Active();

// Whether the resource being created comes from the game (DlssNr_ExposureScanSource.h); `caller` is the creation
// call's return address.
bool FromGameLocked(void* caller);

// Counts a resource looked at by any backend's creation hook (the readout's "examined").
void ExaminedLocked();

std::vector<Tracked>& ListLocked();

// Takes a free slot for a new candidate; D3D12/D3D11 handles get a reference. Returns the slot, or -1 when already
// tracked or full (said once).
int AdoptLocked(Api api, void* handle, void* device, const std::string& shape, unsigned int bytes, bool isBuffer,
                uint32_t format);

// Lets go of a candidate: its reference (D3D) is released and its slot freed.
void DropLocked(size_t slot);

// A value read back for `slot`, copied when the slot's generation was `generation`: decoded (2 bytes = half,
// 4 = float) into the candidate's statistics, unless the slot has another owner by now.
void ConsumeLocked(size_t slot, uint32_t generation, const void* bytes);

// Once per NR frame, by the pass that runs NR: marks the handed candidate, writes the recording's row, logs the movers.
void EndFrameLocked(const FrameExposure& frame);

// The game's own handed exposure for this NR frame when the pass only sees OptiScaler's copy of it (the D3D11 bridge
// hands NR a D3D12 copy of the game's D3D11 texture): EndFrameLocked marks this one instead, then forgets it.
void NoteHandedLocked(Api api, const void* handle);

bool AnyAliveLocked(Api api);

// The shape's suffix in the readout for a non-D3D12 candidate (" [D3D11]", " [Vulkan]").
const char* ApiSuffix(Api api);

float HalfToFloat(uint16_t h);
} // namespace Core
} // namespace ExposureScan
} // namespace DlssNr
