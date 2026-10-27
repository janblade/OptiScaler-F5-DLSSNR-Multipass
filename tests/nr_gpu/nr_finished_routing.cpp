#include "pch.h"
#include <mutex>
#include <thread>
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
namespace DlssNr
{
namespace Late
{
using Microsoft::WRL::ComPtr;
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
    puts("PASS: production routing, 64 generated presents excluded, app/passthrough exactly once, PQ metadata, lock "
         "order, NR off, shutdown");
    return 0;
}
catch (const std::exception& e)
{
    puts(e.what());
    return 1;
}
