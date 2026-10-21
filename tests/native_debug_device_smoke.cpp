// Regression: the FG queue may expose the original device while ReShade exposes
// a different COM identity to the producer. Its own submission fence proves ownership.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include "../OptiScaler/native/NativeDebugDevice.h"
using Microsoft::WRL::ComPtr;

struct ProxyIdentity final : IUnknown
{
    ULONG references = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
};

int main()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device, other;
    ComPtr<ID3D12Fence> fence, menuFence, foreignFence;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))) ||
        FAILED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&other))) ||
        FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))) ||
        FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&menuFence))) ||
        FAILED(other->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&foreignFence)))) return 2;
    ProxyIdentity proxy, menuProxy;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool value) { ++checks; if (!value) ++failures; };
    check(native::SameDebugDevice(device.Get(),device.Get(),fence.Get()));
    check(!native::SameDebugComObject(device.Get(),&proxy));
    check(native::SameDebugDevice(device.Get(),&proxy,fence.Get()));
    check(!native::SameDebugDevice(other.Get(),&proxy,fence.Get()));
    check(!native::SameDebugDevice(other.Get(),device.Get(),fence.Get()));
    check(!native::SameDebugDevice(device.Get(),&proxy,nullptr));
    check(!native::SameDebugDevice(nullptr,&proxy,fence.Get()));
    check(!native::SameDebugDevice(device.Get(),nullptr,fence.Get()));
    check(native::SameDebugDevice(&menuProxy,&proxy,fence.Get(),menuFence.Get()));
    check(!native::SameDebugDevice(&menuProxy,&proxy,fence.Get(),foreignFence.Get()));
    check(!native::SameDebugDevice(&menuProxy,&proxy,fence.Get(),nullptr));
    check(menuProxy.references == 1);
    check(proxy.references == 1);
    std::printf("Native debug device: %u checks, %u failures (original, proxy identity, distinct allocation device, nulls, COM lifetime)\n",checks,failures);
    return failures ? 1 : 0;
}
