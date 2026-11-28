// Headless D3D12/WARP test of the Finished Picture cross-queue gate (DlssNr_CrossQueueGate.h): a present queue that
// waits on the GPU for the render queue's guide copies. Normal case: the wait ends by itself, the watchdog stays out.
// Deadlock case (the render queue's signal sits behind a wait on this very present, as tests/nr_finished_queue_smoke.cpp
// sets up): the watchdog frees the present queue within its timeout, turns the wait off and hands back the capture.
// No game or NVIDIA runtime is loaded.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include "../OptiScaler/shaders/dlssnr/DlssNr_CrossQueueGate.h"

using Microsoft::WRL::ComPtr;
using namespace std::chrono;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 call failed"); }
static void Expect(bool yes, const char* why) { if (!yes) throw std::runtime_error(why); }

static bool WaitFence(ID3D12Fence* fence, UINT64 value, DWORD ms)
{
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Expect(event != nullptr, "Could not create completion event");
    Check(fence->SetEventOnCompletion(value, event));
    const bool done = WaitForSingleObject(event, ms) == WAIT_OBJECT_0;
    CloseHandle(event);
    return done;
}

int main() try
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    D3D12_COMMAND_QUEUE_DESC desc {};
    ComPtr<ID3D12CommandQueue> render, present;
    Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&render)));
    Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&present)));
    const auto fence = [&] {
        ComPtr<ID3D12Fence> f;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)));
        return f;
    };

    DlssNr::CrossQueueGate::Watchdog watchdog(milliseconds(300));

    // 1. Ordinary frames: the render work finishes a little after the present was queued. The present waits for it.
    {
        auto gate = fence(), release = fence(), presented = fence();
        for (UINT64 frame = 1; frame <= 8; ++frame)
        {
            Check(render->Wait(release.Get(), frame)); // the render work still running at present
            Check(render->Signal(gate.Get(), frame));
            Expect(gate->GetCompletedValue() < frame, "Render work finished before the present (test setup)");
            Expect(watchdog.Watch(gate.Get(), frame, frame), "Watchdog refused a gate");
            Check(present->Wait(gate.Get(), frame));
            Check(present->Signal(presented.Get(), frame));
            std::this_thread::sleep_for(milliseconds(20));
            Expect(presented->GetCompletedValue() < frame, "Present ran before the render work finished");
            Check(release->Signal(frame));
            Expect(WaitFence(presented.Get(), frame, 2000), "Present never ran after the render work finished");
        }
        for (int i = 0; i < 100 && watchdog.Watching() != 0; ++i)
            std::this_thread::sleep_for(milliseconds(5));
        Expect(watchdog.Watching() == 0, "Watchdog still watching finished gates");
        Expect(watchdog.Enabled() && watchdog.TakeAbandoned().empty(), "Watchdog fired on ordinary frames");
    }

    // 2. The render queue's signal waits for this present: without the watchdog these queues never move again.
    {
        auto gate = fence(), presentGate = fence(), presented = fence();
        const UINT64 frame = 1, serial = 42;
        Check(render->Wait(presentGate.Get(), frame));
        Check(render->Signal(gate.Get(), frame));
        const auto start = steady_clock::now();
        Expect(watchdog.Watch(gate.Get(), frame, serial), "Watchdog refused a gate");
        Check(present->Wait(gate.Get(), frame));
        Check(present->Signal(presentGate.Get(), frame));
        Check(present->Signal(presented.Get(), frame));
        const bool freed = WaitFence(presented.Get(), frame, 3000);
        if (!freed) gate->Signal(frame); // cleanup even on regression failure
        Expect(freed, "Watchdog did not free the deadlocked present queue");
        const auto took = duration_cast<milliseconds>(steady_clock::now() - start).count();
        Expect(took >= 250, "Present ran before the watchdog's timeout");
        Expect(WaitFence(gate.Get(), frame, 2000), "Render queue never ran after the present was freed");
        const auto abandoned = watchdog.TakeAbandoned();
        Expect(abandoned.size() == 1 && abandoned[0] == serial, "Freed capture not handed back");
        Expect(!watchdog.Enabled(), "Wait still on after a freed gate");
        Expect(!watchdog.Watch(gate.Get(), frame + 1, serial + 1), "Watched a gate after the wait was turned off");
        printf("deadlock freed after %lld ms\n", (long long) took);
    }
    puts("PASS finished-picture cross-queue gate: present waits for unfinished render work, watchdog frees a present "
         "the render queue waits on, then turns the wait off");
    return 0;
}
catch (const std::exception& e) { puts(e.what()); return 1; }
