#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace native
{
// Compare COM identities, not arbitrary interface pointers. ReShade can expose a
// device proxy to the producer while the FG presenter uses its original device.
inline bool SameDebugComObject(IUnknown* a, IUnknown* b)
{
    if (!a || !b) return false;
    if (a == b) return true;
    Microsoft::WRL::ComPtr<IUnknown> identityA, identityB;
    return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&identityA))) &&
           SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&identityB))) &&
           identityA.Get() == identityB.Get();
}

inline bool SameDebugDevice(IUnknown* menuDevice, IUnknown* producerDevice,
                            ID3D12Fence* producerFence, ID3D12Fence* menuFence = nullptr)
{
    if (!menuDevice || !producerDevice) return false;
    if (SameDebugComObject(menuDevice, producerDevice)) return true;

    // The producer's submission fence belongs to the same allocation device as
    // its textures. GetDevice on that child exposes the original device behind
    // a ReShade device proxy. This is an ownership check; matching adapter LUIDs
    // alone would incorrectly accept resources from a different device.
    Microsoft::WRL::ComPtr<ID3D12Device> allocationDevice;
    if (!producerFence || FAILED(producerFence->GetDevice(IID_PPV_ARGS(&allocationDevice)))) return false;
    if (SameDebugComObject(menuDevice, allocationDevice.Get())) return true;
    // Either side can be wrapped. The menu completion fence is allocated by its
    // rendering device, and is also the fence passed back after that queue submits.
    Microsoft::WRL::ComPtr<ID3D12Device> menuAllocationDevice;
    return menuFence && SUCCEEDED(menuFence->GetDevice(IID_PPV_ARGS(&menuAllocationDevice))) &&
           SameDebugComObject(menuAllocationDevice.Get(), allocationDevice.Get());
}
}
