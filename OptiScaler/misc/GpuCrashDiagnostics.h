#pragma once

#include <Config.h>
#include <Logger.h>
#include <d3d12.h>
#include <mutex>
#include <string>

namespace GpuCrashDiagnostics
{
inline std::string Narrow(const wchar_t* text)
{
    if (!text) return "(unnamed)";
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "(unnamed)";
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

// DRED must be enabled before the first device, including capability probes. It does not require the debug layer.
inline void Enable()
{
    wchar_t enabled[2] {};
    if (GetEnvironmentVariableW(L"F5LOW_DRED", enabled, 2) != 1 || enabled[0] != L'1')
        return;
    static std::once_flag once;
    std::call_once(once, [] {
        ID3D12DeviceRemovedExtendedDataSettings1* settings = nullptr;
        const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
        if (SUCCEEDED(hr)) {
            settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            settings->Release();
            LOG_INFO("F5Low GPU crash diagnostics enabled (F5LOW_DRED=1)");
        } else LOG_WARN("DRED settings unavailable: 0x{:X}", (unsigned)hr);
    });
}

inline void Dump(ID3D12Device* device)
{
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) return;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs {};
    const HRESULT hr = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
    LOG_ERROR("DRED breadcrumb query: 0x{:X}", (unsigned)hr);
    unsigned nodes = 0;
    if (SUCCEEDED(hr)) for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node && nodes++ < 256; node = node->pNext) {
        const UINT completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
        if (completed >= node->BreadcrumbCount) continue;
        const auto name = node->pCommandListDebugNameA ? std::string(node->pCommandListDebugNameA) :
                                                       Narrow(node->pCommandListDebugNameW);
        LOG_ERROR("DRED pending command list {}: completed {} of {} operations", name, completed, node->BreadcrumbCount);
        const UINT begin = completed > 4 ? completed - 4 : 0;
        const UINT end = (std::min)(node->BreadcrumbCount, completed + 8);
        for (UINT i = begin; node->pCommandHistory && i < end; ++i)
            LOG_ERROR("DRED operation {}: {}{}", i, (unsigned)node->pCommandHistory[i], i == completed ? " <- next" : "");
        for (UINT i = 0; node->pBreadcrumbContexts && i < node->BreadcrumbContextsCount; ++i) {
            const auto& context = node->pBreadcrumbContexts[i];
            if (context.BreadcrumbIndex <= end && context.BreadcrumbIndex + 12 >= completed)
                LOG_ERROR("DRED context {}: {}", context.BreadcrumbIndex, Narrow(context.pContextString));
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault))) {
        LOG_ERROR("DRED page fault VA: 0x{:X}", fault.PageFaultVA);
        for (int freed = 0; freed < 2; ++freed) {
            auto* allocation = freed ? fault.pHeadRecentFreedAllocationNode : fault.pHeadExistingAllocationNode;
            for (unsigned i = 0; allocation && i < 16; ++i, allocation = allocation->pNext) {
                const auto name = allocation->ObjectNameA ? std::string(allocation->ObjectNameA) : Narrow(allocation->ObjectNameW);
                LOG_ERROR("DRED allocation {}: {} type {}", freed ? "freed" : "existing", name, (unsigned)allocation->AllocationType);
            }
        }
    }
    dred->Release();
}
}
