#include "pch.h"
#include <mutex>
#include <thread>
#include "../../OptiScaler/shaders/dlssnr/DlssNr_FinishedReady.h"
#include "../../OptiScaler/dlssnr/DlssNr_StreamlinePicture.h"

// Compile the production routing entry points with a composition observer, using real DXGI buffers.
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("DXGI call failed");
}
static void Expect(bool ok, const char* message)
{
    if (!ok)
        throw std::runtime_error(message);
}
struct Setting
{
    bool value = true;
    bool value_or_default() const { return value; }
};
struct Config
{
    Setting DlssNrEnabled, DlssNrFinishedPicture;
    static Config* Instance()
    {
        static Config value;
        return &value;
    }
};
enum class SwapchainInteropApi
{
    None,
    Other
};
struct State
{
    uint64_t frameCount = 100;
    bool isShuttingDown = false;
    ::SwapchainInteropApi swapchainInteropApi = ::SwapchainInteropApi::None;
    static State& Instance()
    {
        static State value;
        return value;
    }
};
std::recursive_mutex g_nrMutex;
static bool NrLockIsHeld()
{
    bool held = false;
    std::thread probe(
        [&]
        {
            if (g_nrMutex.try_lock())
                g_nrMutex.unlock();
            else
                held = true;
        });
    probe.join();
    return held;
}
static unsigned gameBufferQueries = 0, composeRequests = 0;
static ID3D12Resource* expectedPicture = nullptr;
static ID3D12CommandQueue* expectedQueue = nullptr;
static bool expectedHandoff = false;
static DXGI_COLOR_SPACE_TYPE expectedSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
namespace SdrToHdr
{
HRESULT GameBuffer(IDXGISwapChain3* chain, UINT index, REFIID iid, void** resource)
{
    Expect(!NrLockIsHeld(), "Swapchain query under NR lock can invert Streamline's lock order");
    ++gameBufferQueries;
    return chain->GetBuffer(index, iid, resource);
}
} // namespace SdrToHdr
static bool nativeFgRunning = false;
namespace DlssNr::StreamlinePicture
{
bool NativeFgRunning() { return nativeFgRunning; }
bool Owns(IDXGISwapChain* chain) { return RenderQueue(chain) != nullptr; }
}
namespace DlssNr
{
namespace Late
{
using Microsoft::WRL::ComPtr;
struct Slot
{
    struct Frame { uint64_t SubmissionEpoch = 0; unsigned OutputWidth = 8, OutputHeight = 8; } frame;
    bool pending = false, submitted = false, expired = false, residualOnly = false;
    uint64_t serial = 0, ready = 0;
    ID3D12CommandList* producer = nullptr;
    ComPtr<ID3D12CommandQueue> producerQueue;
    ComPtr<ID3D12Fence> fence;
};
std::array<Slot, 4> slots;
bool reset = false;
uint64_t successes = 0, lastApplicationSuccesses = 0;
std::string status;
void Say(const char* message) { status = message; }
void Cancel() { reset = true; for (auto& slot : slots) { slot.expired = true; if (slot.submitted) slot.pending = false; } }
ID3D12CommandQueue* RealQueue(ID3D12CommandQueue* queue) { return queue; }
#include "FinishedSubmit.inl"
inline constexpr GUID colorSpaceKey = {
    0x34a31e7b, 0x84c5, 0x44ef, { 0xa7, 0x4d, 0x6b, 0xd3, 0x60, 0x8c, 0xe5, 0x22 }
};
} // namespace Late
static void ApplyFinishedColor(ID3D12Resource* picture, ID3D12CommandQueue* queue, DXGI_COLOR_SPACE_TYPE space,
                               bool handoff, const D3D12_RESOURCE_DESC& desc)
{
    Expect(NrLockIsHeld(), "Composition request lost the shared NR lock");
    Expect(picture == expectedPicture && queue == expectedQueue, "Routing selected the wrong picture or queue");
    Expect(space == expectedSpace && handoff == expectedHandoff, "Routing lost colour metadata or input policy");
    if (picture)
        Expect(desc.Width == picture->GetDesc().Width && desc.Format == picture->GetDesc().Format,
               "Routing lost the current picture description after resize");
    ++composeRequests;
}
void FinishedPictureUnavailable(const char* message);
#include "FinishedSelect.inl"
// Generated from the live Late.inl by the runner, without duplicating the implementation.
#include "FinishedRouting.inl"
} // namespace DlssNr
int main()
try
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q {};
    Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = desc.Height = 8;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferCount = 2;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    Check(factory->CreateSwapChainForComposition(queue.Get(), &desc, nullptr, &chain));
    ComPtr<IDXGISwapChain3> chain3;
    Check(chain.As(&chain3));
    ComPtr<ID3D12Resource> ordinary, application;
    Check(chain->GetBuffer(chain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&ordinary)));
    // Deliberately different from the display buffer: a handoff must use the passed app frame.
    Check(chain->GetBuffer((chain3->GetCurrentBackBufferIndex() + 1) % 2, IID_PPV_ARGS(&application)));
    expectedQueue = queue.Get();
    expectedPicture = ordinary.Get();
    const auto pq = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    Check(chain->SetPrivateData(DlssNr::Late::colorSpaceKey, sizeof(pq), &pq));
    expectedSpace = pq;
    DlssNr::ApplyToFinishedPicture(chain.Get(), queue.Get());
    Expect(composeRequests == 1 && gameBufferQueries == 1, "Ordinary unregistered chain did not compose once");
    nativeFgRunning = true;
    for (unsigned frame = 0; frame < 64; ++frame)
        DlssNr::ApplyToFinishedPicture(chain.Get(), queue.Get());
    Expect(composeRequests == 1 && gameBufferQueries == 1,
           "Unregistered native DLSS-G chain edited generated display frames");
    Expect(DlssNr::Late::status.find("no application-picture handoff") != std::string::npos,
           "Missing handoff pause was silent");
    nativeFgRunning = false;
    Check(chain->SetPrivateDataInterface(DlssNr::StreamlinePicture::renderQueueKey, queue.Get()));
    // Simulate many asynchronous/generated lower presents: none may ask to compose or query a buffer.
    for (unsigned i = 0; i < 64; ++i)
        DlssNr::ApplyToFinishedPicture(chain.Get(), queue.Get());
    Expect(composeRequests == 1 && gameBufferQueries == 1, "Registered lower/generated presents applied NR again");
    expectedPicture = application.Get();
    expectedHandoff = true;
    DlssNr::ApplyToStreamlinePicture(chain.Get(), application.Get(), queue.Get());
    Expect(composeRequests == 2 && gameBufferQueries == 1, "App handoff replaced the supplied picture");
    expectedPicture = ordinary.Get();
    expectedHandoff = false;
    DlssNr::ApplyToStreamlinePassthrough(chain.Get(), queue.Get());
    Expect(composeRequests == 3 && gameBufferQueries == 2, "Registered FG-off passthrough did not compose once");
    DlssNr::ApplyToFinishedPicture(chain.Get(), queue.Get());
    Expect(composeRequests == 3 && gameBufferQueries == 2, "Lower present duplicated FG-off passthrough");
    // Disabling NR lets the shared composition entry cancel pending work, without swapchain reads.
    Config::Instance()->DlssNrEnabled.value = false;
    expectedPicture = nullptr;
    expectedSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    DlssNr::ApplyToFinishedPicture(chain.Get(), queue.Get());
    Expect(composeRequests == 4 && gameBufferQueries == 2,
           "NR-off path queried a frame instead of requesting cancellation");
    Config::Instance()->DlssNrEnabled.value = true;
    State::Instance().isShuttingDown = true;
    DlssNr::ApplyToFinishedPicture(chain.Get(), queue.Get());
    DlssNr::ApplyToStreamlinePicture(chain.Get(), application.Get(), queue.Get());
    DlssNr::ApplyToStreamlinePassthrough(chain.Get(), queue.Get());
    Expect(composeRequests == 4 && gameBufferQueries == 2, "Shutdown guard allowed presentation work");
    // Exercise the actual slot-building loop, expiration and boundary functions too.
    State::Instance().isShuttingDown = false;
    auto& slots = DlssNr::Late::slots;
    for (unsigned i = 0; i < slots.size(); ++i)
    {
        slots[i].pending = slots[i].submitted = true;
        slots[i].expired = false;
        slots[i].serial = 10 + i;
        slots[i].ready = 1;
        slots[i].frame.SubmissionEpoch = 100;
        Check(device->CreateFence(i == 3 ? 0 : 1, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slots[i].fence)));
    }
    std::array<DlssNr::FinishedInput, 4> inputs {};
    size_t index = 0;
    const auto d = application->GetDesc();
    auto select = [&](bool handoff)
    {
        inputs = {};
        return DlssNr::SelectFinishedInput(queue.Get(), d, false, handoff, index, inputs);
    };
    Expect(select(true) == DlssNr::FinishedPick::NotReady && index == 3,
           "App handoff fell back from newest in-flight capture to previous frame's guides");
    slots[3].submitted = false;
    Expect(select(true) == DlssNr::FinishedPick::NotReady && index == 3,
           "Unsubmitted newest capture allowed previous guides");
    slots[3].submitted = true;
    slots[3].producerQueue = queue;
    Expect(select(true) == DlssNr::FinishedPick::Ready && index == 3,
           "Same-queue app capture was not ordered");
    slots[3].producerQueue.Reset();
    Check(slots[3].fence->Signal(1));
    Expect(select(true) == DlssNr::FinishedPick::Ready && index == 3,
           "Newest completed capture not accepted across queues");
    slots[2].submitted = false; // recorded, but not submitted by this app Present
    DlssNr::EndStreamlinePicture("Waiting for app picture");
    Expect(select(true) == DlssNr::FinishedPick::None,
           "Menu/declined Present left old guides eligible for the next app frame");
    Expect(slots[2].pending && slots[2].expired,
           "Unsubmitted expired capture lost its submission/fence tracking");
    slots[2].producer = reinterpret_cast<ID3D12CommandList*>(uintptr_t(1));
    ID3D12CommandList* lists[] = {slots[2].producer};
    DlssNr::Late::Submitted(queue.Get(), 1, lists);
    Expect(slots[2].submitted && !slots[2].pending, "Expired recording lost submission/fence tracking");
    HANDLE done = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    Check(slots[2].fence->SetEventOnCompletion(slots[2].ready, done));
    Expect(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0, "Expired submitted capture's fence was not signalled");
    CloseHandle(done);
    Expect(select(true) == DlssNr::FinishedPick::None, "Late submission revived an expired app capture");
    Expect(DlssNr::Late::reset, "Skipped app frame did not reset NR history");
    slots[0].pending = true;
    slots[0].expired = false;
    slots[0].frame.SubmissionEpoch = 98;
    Expect(select(false) == DlssNr::FinishedPick::None && !slots[0].pending,
           "Ordinary Present lost its epoch expiry");
    slots[0].pending = true;
    slots[0].expired = false;
    slots[0].frame.SubmissionEpoch = 100;
    Expect(select(true) == DlssNr::FinishedPick::Ready,
           "New capture after menu did not recover app-frame composition");
    std::cout << "PASS: production slot building/newest-only readiness, menu expiry, late submission and recovery\n";
    puts("PASS: production routing, 64 generated presents excluded, app/passthrough exactly once, PQ metadata, lock "
         "order, NR off, shutdown");
    return 0;
}
catch (const std::exception& e)
{
    puts(e.what());
    return 1;
}
