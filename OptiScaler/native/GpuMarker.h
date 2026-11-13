#pragma once

// Named points on a D3D12 command list for the DRED report ([Log] D3D12Dred, Util.cpp LogDredReport): after a GPU hang
// the report shows the operations of the unfinished lists with these names between them. Header-only, so the standalone
// tests that compile NativeProducer.cpp and DlssNr_GpuTime.h alone build without Util or Config. Off (nothing recorded)
// until with_dx12.cpp EnableDredOnce has turned DRED's breadcrumb contexts on; always off in those tests.

#include <d3d12.h>

#include <atomic>
#include <cwchar>

namespace GpuMarkers
{

inline std::atomic<bool> g_on { false };

inline bool On() { return g_on.load(std::memory_order_relaxed); }

inline void Mark(ID3D12GraphicsCommandList* list, const wchar_t* name)
{
    if (list == nullptr || name == nullptr || !On())
        return;

    // Metadata 0: a wide string (PIX's WINPIX_EVENT_UNICODE_VERSION), which DRED keeps as the breadcrumb's context
    list->SetMarker(0, name, (UINT) ((std::wcslen(name) + 1) * sizeof(wchar_t)));
}

} // namespace GpuMarkers
