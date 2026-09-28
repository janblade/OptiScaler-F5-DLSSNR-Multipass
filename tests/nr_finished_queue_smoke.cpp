// Headless D3D12/WARP regression: no present may wait for render work gated by that same present.
// Ported from wilsjo2 dd2b7906 (tests/nr_finished_queue_smoke.cpp). No game or NVIDIA runtime is loaded.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/shaders/dlssnr/DlssNr_FinishedReady.h"

using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 call failed"); }
static void Expect(bool yes, const char* why) { if (!yes) throw std::runtime_error(why); }

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
    ComPtr<ID3D12Fence> presentGate, inputReady;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentGate)));
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&inputReady)));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Expect(event != nullptr, "Could not create completion event");
    for (UINT64 frame = 1; frame <= 16; ++frame)
    {
        // A submitted producer signal can sit behind a wait for this presentation.
        Check(render->Wait(presentGate.Get(), frame));
        Check(render->Signal(inputReady.Get(), frame));
        const auto completed = inputReady->GetCompletedValue();
        const bool crossQueueReady = DlssNr::FinishedInputReady(false, completed, frame);
        const bool sameQueueReady = DlssNr::FinishedInputReady(true, completed, frame);
        const bool olderInputReady = DlssNr::FinishedInputReady(false, completed, frame - 1);
        // Do not enqueue present->Wait(inputReady, frame): that would deadlock these queues.
        Check(present->Signal(presentGate.Get(), frame));
        Check(inputReady->SetEventOnCompletion(frame, event));
        const auto result = WaitForSingleObject(event, 5000);
        if (result != WAIT_OBJECT_0) presentGate->Signal(frame); // cleanup even on regression failure
        Expect(result == WAIT_OBJECT_0, "Present/render queues stalled");
        Expect(!crossQueueReady, "Accepted future render input on the presentation queue");
        Expect(sameQueueReady, "Rejected ordered same-queue input");
        Expect(olderInputReady, "Rejected the previous completed input");
        Expect(DlssNr::FinishedInputReady(false, inputReady->GetCompletedValue(), frame),
               "Completed cross-queue input remained unavailable");
    }
    CloseHandle(event);
    Expect(!DlssNr::FinishedInputReady(false, UINT64_MAX, 1), "Accepted a removed device");
    Expect(!DlssNr::FinishedInputReady(true, UINT64_MAX, 1), "Same-queue bypassed device removal");

    // Only the newest eligible slot may be composed; an older finished one holds the previous frame's guides.
    using DlssNr::FinishedInput;
    using DlssNr::FinishedPick;
    size_t index = 99;
    FinishedInput none[2] {};
    Expect(DlssNr::PickFinishedInput(none, 2, index) == FinishedPick::None, "Picked from no eligible slot");
    FinishedInput stale[3] { { true, 5, false, 5, 5 }, { true, 6, false, 5, 6 }, { false, 7, true, 0, 7 } };
    Expect(DlssNr::PickFinishedInput(stale, 3, index) == FinishedPick::NotReady && index == 1,
           "Fell back to the previous frame's finished guides");
    FinishedInput sameQueue[2] { { true, 5, true, 0, 5 }, { true, 6, true, 0, 6 } };
    Expect(DlssNr::PickFinishedInput(sameQueue, 2, index) == FinishedPick::Ready && index == 1,
           "Rejected the newest same-queue slot");
    FinishedInput done[2] { { true, 6, false, 6, 6 }, { true, 5, false, 6, 5 } };
    Expect(DlssNr::PickFinishedInput(done, 2, index) == FinishedPick::Ready && index == 0,
           "Rejected the newest completed cross-queue slot");
    FinishedInput removed[1] { { true, 6, true, UINT64_MAX, 6 } };
    Expect(DlssNr::PickFinishedInput(removed, 1, index) == FinishedPick::NotReady, "Composed after device removal");

    // Finished picture pauses only for the game's own DLSS-G.
    Expect(DlssNr::GameFrameGenerationOn(false, false, true, 0), "Missed the game's DLSS-G mode");
    Expect(DlssNr::GameFrameGenerationOn(false, false, false, 5), "Missed DLSS-G evaluates without a mode call");
    Expect(!DlssNr::GameFrameGenerationOn(false, false, false, 0), "Paused with DLSS-G off");
    Expect(!DlssNr::GameFrameGenerationOn(true, false, true, 5), "Paused OptiScaler's own FG path");
    Expect(!DlssNr::GameFrameGenerationOn(false, true, true, 0), "Paused while OptiScaler keeps the game's DLSS-G off");
    puts("PASS finished-picture queue readiness: presentation dependency, same-queue order, completed input, device "
         "removal, newest-slot pick, game DLSS-G pause");
    return 0;
}
catch (const std::exception& e) { puts(e.what()); return 1; }
