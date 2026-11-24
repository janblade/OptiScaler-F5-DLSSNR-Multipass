#pragma once
#include <windows.h>
#include <cstdint>

// In-process ABI, shared NT handles only: the producer and bridge use DIFFERENT D3D12 devices.
// A null buffer is a query. Handles are borrowed; each import retains its own kernel reference.
// Producer signals readyValue only after its Vulkan image-to-buffer submission. Consumer returns
// doneValue only after its copy was submitted and a completion signal was successfully queued.
struct FeederPresentV1
{
    uint32_t size = sizeof(FeederPresentV1);
    uint32_t version = 1;
    uint64_t swapchain = 0;
    uint64_t producerGeneration = 0;
    HANDLE buffer = nullptr;
    HANDLE readyFence = nullptr;
    uint64_t readyValue = 0;
    uint32_t width = 0, height = 0, rowPitch = 0, format = 0;
    uint32_t bridgeGeneration = 0;
    uint32_t displayed = 0;
    HANDLE doneFence = nullptr;
    uint64_t doneValue = 0;
};
using FeederPresentFnV1 = BOOL(WINAPI*)(FeederPresentV1*);
