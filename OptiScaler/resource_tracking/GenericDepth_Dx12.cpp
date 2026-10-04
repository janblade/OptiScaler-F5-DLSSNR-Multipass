#include "pch.h"

#include "GenericDepth_Dx12.h"

#include <Config.h>
#include <Util.h>

#include <detours/detours.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>

#ifndef STDMETHODCALLTYPE
#include <Unknwn.h>
#endif

// The counting rules (vertices weighed over draw calls, the viewport and workload tests on clears, the best snapshot, the
// reversed-Z hint from the clear value) are adapted from ReShade's Generic Depth add-on, Copyright (C) 2021 Patrick Mours,
// BSD-3-Clause; see Licenses/ReShade_GenericDepth_LICENSE.txt. This file only observes: it copies nothing yet.

namespace
{
typedef void(STDMETHODCALLTYPE* PFN_CreateDepthStencilView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                            const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CopyDescriptorsSimple)(ID3D12Device* This, UINT NumDescriptors,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                                           D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);
typedef void(STDMETHODCALLTYPE* PFN_CopyDescriptors)(ID3D12Device* This, UINT NumDestDescriptorRanges,
                                                     const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                                     const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                                     const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                                     const UINT* pSrcDescriptorRangeSizes,
                                                     D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors,
                                                        const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                                        BOOL RTsSingleHandleToDescriptorRange,
                                                        const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_ClearDepthStencilView)(ID3D12GraphicsCommandList* This,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
                                                           D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil,
                                                           UINT NumRects, const D3D12_RECT* pRects);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance,
                                                   UINT InstanceCount, UINT StartVertexLocation,
                                                   UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                                          UINT InstanceCount, UINT StartIndexLocation,
                                                          INT BaseVertexLocation, UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D12GraphicsCommandList* This, UINT NumViewports,
                                                    const D3D12_VIEWPORT* pViewports);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteIndirect)(ID3D12GraphicsCommandList* This,
                                                     ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount,
                                                     ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset,
                                                     ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset);

PFN_CreateDepthStencilView o_CreateDepthStencilView = nullptr;
PFN_CopyDescriptorsSimple o_CopyDescriptorsSimple = nullptr;
PFN_CopyDescriptors o_CopyDescriptors = nullptr;
PFN_OMSetRenderTargets o_OMSetRenderTargets = nullptr;
PFN_ClearDepthStencilView o_ClearDepthStencilView = nullptr;
PFN_DrawInstanced o_DrawInstanced = nullptr;
PFN_DrawIndexedInstanced o_DrawIndexedInstanced = nullptr;
PFN_RSSetViewports o_RSSetViewports = nullptr;
PFN_ExecuteIndirect o_ExecuteIndirect = nullptr;

struct DrawStats
{
    uint64_t vertices = 0;
    uint32_t drawcalls = 0;
    uint32_t drawcallsIndirect = 0;
    float lastViewportWidth = 0.0f;
};

// One depth-stencil buffer's counts for the frame being recorded.
struct Stats
{
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DrawStats total;
    DrawStats current;       // since the last clear
    uint32_t clears = 0;     // clears that came after real work
    int32_t bestClear = -1;  // the clear a snapshot would be taken at
    bool reversed = false;   // cleared to something other than 1.0
};

struct DsvInfo
{
    ID3D12Resource* resource = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct ListState
{
    Stats* stats = nullptr;       // the depth buffer this list draws into now
    float viewportWidth = 0.0f;   // its main viewport
};

std::mutex g_mutex;
std::unordered_map<SIZE_T, DsvInfo> g_dsv;                       // CPU descriptor handle -> what it views
std::unordered_map<ID3D12GraphicsCommandList*, ListState> g_lists;
std::unordered_map<ID3D12Resource*, Stats> g_stats;              // node-stable: g_lists holds pointers into it
uint64_t g_bestSnapshotVertices = 0;                             // the busiest stretch before a clear, this frame
float g_pictureWidth = 0.0f;                                     // from the last present
GenericDepthSelect::Selector g_selector;
GenericDepthSelect::Pick g_pick;
UINT g_dsvIncrement = 0;
uint64_t g_frames = 0;
uint64_t g_lastLoggedFrame = 0;
uint64_t g_lastLoggedPick = 0;
bool g_installed = false;

constexpr uint64_t kLogEveryFrames = 600;

void AddDraw(DrawStats& s, uint64_t vertices, uint32_t drawcalls, bool indirect)
{
    s.vertices += vertices;
    s.drawcalls += drawcalls;
    if (indirect)
        s.drawcallsIndirect += drawcalls;
}

void OnDraw(ID3D12GraphicsCommandList* list, uint64_t vertices, uint32_t instances)
{
    std::lock_guard lock(g_mutex);

    const auto found = g_lists.find(list);

    if (found == g_lists.end() || found->second.stats == nullptr)
        return;

    auto& state = found->second;
    const uint64_t count = vertices * instances;

    AddDraw(state.stats->total, count, 1, false);
    AddDraw(state.stats->current, count, 1, false);

    // A fullscreen rectangle (two triangles) does not update the viewport the last real draw used.
    if (!(vertices == 6 && instances == 1))
        state.stats->current.lastViewportWidth = state.viewportWidth;
}

void STDMETHODCALLTYPE hkCreateDepthStencilView(ID3D12Device* This, ID3D12Resource* pResource,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    o_CreateDepthStencilView(This, pResource, pDesc, DestDescriptor);

    std::lock_guard lock(g_mutex);

    if (pResource == nullptr)
    {
        g_dsv.erase(DestDescriptor.ptr);
        return;
    }

    const auto desc = pResource->GetDesc();
    g_dsv[DestDescriptor.ptr] = DsvInfo { pResource, (uint32_t) desc.Width, desc.Height, desc.Format };
}

void CopyDsv(SIZE_T dest, SIZE_T source)
{
    const auto found = g_dsv.find(source);

    if (found != g_dsv.end())
    {
        const auto info = found->second;
        g_dsv[dest] = info;
    }
    else
    {
        g_dsv.erase(dest);
    }
}

void STDMETHODCALLTYPE hkCopyDescriptorsSimple(ID3D12Device* This, UINT NumDescriptors,
                                               D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                               D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                               D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    o_CopyDescriptorsSimple(This, NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart,
                            DescriptorHeapsType);

    if (DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || g_dsvIncrement == 0)
        return;

    std::lock_guard lock(g_mutex);

    for (UINT i = 0; i < NumDescriptors; ++i)
        CopyDsv(DestDescriptorRangeStart.ptr + (SIZE_T) i * g_dsvIncrement,
                SrcDescriptorRangeStart.ptr + (SIZE_T) i * g_dsvIncrement);
}

void STDMETHODCALLTYPE hkCopyDescriptors(ID3D12Device* This, UINT NumDestDescriptorRanges,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                         const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                         const UINT* pSrcDescriptorRangeSizes,
                                         D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    o_CopyDescriptors(This, NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                      NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
                      DescriptorHeapsType);

    if (DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || g_dsvIncrement == 0 ||
        pDestDescriptorRangeStarts == nullptr || pSrcDescriptorRangeStarts == nullptr)
        return;

    std::lock_guard lock(g_mutex);

    // The copy walks the two lists of ranges in step, one descriptor at a time; a null size array means one each.
    UINT destRange = 0, destUsed = 0, srcRange = 0, srcUsed = 0;

    while (destRange < NumDestDescriptorRanges && srcRange < NumSrcDescriptorRanges)
    {
        const UINT destSize = pDestDescriptorRangeSizes != nullptr ? pDestDescriptorRangeSizes[destRange] : 1;
        const UINT srcSize = pSrcDescriptorRangeSizes != nullptr ? pSrcDescriptorRangeSizes[srcRange] : 1;

        CopyDsv(pDestDescriptorRangeStarts[destRange].ptr + (SIZE_T) destUsed * g_dsvIncrement,
                pSrcDescriptorRangeStarts[srcRange].ptr + (SIZE_T) srcUsed * g_dsvIncrement);

        if (++destUsed >= destSize)
        {
            ++destRange;
            destUsed = 0;
        }

        if (++srcUsed >= srcSize)
        {
            ++srcRange;
            srcUsed = 0;
        }
    }
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                            BOOL RTsSingleHandleToDescriptorRange,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    {
        std::lock_guard lock(g_mutex);

        Stats* bound = nullptr;

        if (pDepthStencilDescriptor != nullptr)
        {
            const auto found = g_dsv.find(pDepthStencilDescriptor->ptr);

            if (found != g_dsv.end() && found->second.resource != nullptr)
            {
                auto& stats = g_stats[found->second.resource];
                stats.width = found->second.width;
                stats.height = found->second.height;
                stats.format = found->second.format;
                bound = &stats;
            }
        }

        g_lists[This].stats = bound;
    }

    o_OMSetRenderTargets(This, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange,
                         pDepthStencilDescriptor);
}

void STDMETHODCALLTYPE hkRSSetViewports(ID3D12GraphicsCommandList* This, UINT NumViewports,
                                        const D3D12_VIEWPORT* pViewports)
{
    // Only the main viewport matters, as in ReShade's add-on.
    if (NumViewports > 0 && pViewports != nullptr)
    {
        std::lock_guard lock(g_mutex);
        g_lists[This].viewportWidth = pViewports[0].Width;
    }

    o_RSSetViewports(This, NumViewports, pViewports);
}

void STDMETHODCALLTYPE hkClearDepthStencilView(ID3D12GraphicsCommandList* This,
                                               D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
                                               D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil,
                                               UINT NumRects, const D3D12_RECT* pRects)
{
    if ((ClearFlags & D3D12_CLEAR_FLAG_DEPTH) != 0)
    {
        std::lock_guard lock(g_mutex);

        const auto found = g_dsv.find(DepthStencilView.ptr);

        if (found != g_dsv.end() && found->second.resource != nullptr)
        {
            auto& stats = g_stats[found->second.resource];
            stats.width = found->second.width;
            stats.height = found->second.height;
            stats.format = found->second.format;

            // Reversed-Z games clear to 0.0 (or anything but 1.0).
            if (Depth != 1.0f)
                stats.reversed = true;

            // A clear with no work before it (the start of a frame) means nothing.
            if (stats.current.drawcalls != 0)
            {
                const DrawStats stretch = stats.current;
                stats.current = DrawStats {};

                // A clear after a render into a small viewport (a mirror, a portal) is not the scene; ReShade's rule.
                const bool real = stretch.lastViewportWidth > 1024.0f || stretch.lastViewportWidth == 0.0f ||
                                  g_pictureWidth <= 1024.0f;

                if (real)
                {
                    // The busiest stretch of the frame is the one to snapshot; ties go to the later one, so a scene
                    // drawn first into a shadow map and then for real picks the real one.
                    const bool best = stretch.vertices >= g_bestSnapshotVertices;

                    if (best)
                    {
                        g_bestSnapshotVertices = stretch.vertices;
                        stats.bestClear = (int32_t) stats.clears;
                    }

                    ++stats.clears;
                }
            }
        }
    }

    o_ClearDepthStencilView(This, DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
}

void STDMETHODCALLTYPE hkDrawInstanced(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance,
                                       UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
    OnDraw(This, VertexCountPerInstance, InstanceCount);
    o_DrawInstanced(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                              UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
                                              UINT StartInstanceLocation)
{
    OnDraw(This, IndexCountPerInstance, InstanceCount);
    o_DrawIndexedInstanced(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                           StartInstanceLocation);
}

// ExecuteIndirect carries draws, dispatches and mesh dispatches alike, and the command signature that says which is not
// followed here: with a depth buffer bound it is counted as indirect draws (up to MaxCommandCount), which is a guess for a
// dispatch issued while one happens to be bound. ReShade's add-on tells them apart through the API's own indirect type.
void STDMETHODCALLTYPE hkExecuteIndirect(ID3D12GraphicsCommandList* This, ID3D12CommandSignature* pCommandSignature,
                                         UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer,
                                         UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer,
                                         UINT64 CountBufferOffset)
{
    {
        std::lock_guard lock(g_mutex);

        const auto found = g_lists.find(This);

        if (found != g_lists.end() && found->second.stats != nullptr)
        {
            auto* stats = found->second.stats;
            AddDraw(stats->total, 0, MaxCommandCount, true);
            AddDraw(stats->current, 0, MaxCommandCount, true);
            stats->current.lastViewportWidth = found->second.viewportWidth;
        }
    }

    o_ExecuteIndirect(This, pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer,
                      CountBufferOffset);
}

void LogCandidates(const std::vector<GenericDepthSelect::Candidate>& frame, const GenericDepthSelect::Pick& pick,
                   uint32_t pictureWidth, uint32_t pictureHeight)
{
    auto sorted = frame;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return GenericDepthSelect::Score(a) > GenericDepthSelect::Score(b); });

    LOG_INFO("Depth finder: frame {}, picture {}x{}, {} depth buffer(s) in use{}", g_frames, pictureWidth,
             pictureHeight, sorted.size(), pick.valid ? "" : ", none qualifies");

    const size_t shown = std::min<size_t>(sorted.size(), 8);

    for (size_t i = 0; i < shown; ++i)
    {
        const auto& c = sorted[i];
        LOG_INFO("Depth finder:   {}{:X}  {}x{}  format {}  {} vertices, {} draws ({} indirect), {} clear(s), best "
                 "clear {}{}",
                 pick.valid && pick.id == c.id ? "-> " : "   ", c.id, c.width, c.height, c.format, c.vertices,
                 c.drawcalls, c.drawcallsIndirect, c.clears, c.bestClear, c.reversed ? ", reversed-Z" : "");
    }
}
} // namespace

namespace GenericDepthDx12
{
bool Installed() { return g_installed; }

void Install(ID3D12Device* device)
{
    if (device == nullptr || g_installed || !Config::Instance()->DlssNrNativeDepthFinder.value_or_default())
        return;

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
    {
        LOG_WARN("Depth finder: could not make a command allocator, not installed");
        return;
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))))
    {
        allocator->Release();
        LOG_WARN("Depth finder: could not make a command list, not installed");
        return;
    }

    ID3D12GraphicsCommandList* realList = nullptr;

    if (!Util::CheckForRealObject(__FUNCTION__, list, (IUnknown**) &realList))
        realList = list;

    ID3D12Device* realDevice = nullptr;

    if (!Util::CheckForRealObject(__FUNCTION__, device, (IUnknown**) &realDevice))
        realDevice = device;

    PVOID* deviceTable = *(PVOID**) realDevice;
    PVOID* listTable = *(PVOID**) realList;

    o_CreateDepthStencilView = (PFN_CreateDepthStencilView) deviceTable[21];
    o_CopyDescriptors = (PFN_CopyDescriptors) deviceTable[23];
    o_CopyDescriptorsSimple = (PFN_CopyDescriptorsSimple) deviceTable[24];

    // ID3D12GraphicsCommandList vtable: DrawInstanced 12, DrawIndexedInstanced 13, RSSetViewports 21,
    // OMSetRenderTargets 46, ClearDepthStencilView 47, ExecuteIndirect 59 (checked against the SDK header with offsetof).
    o_DrawInstanced = (PFN_DrawInstanced) listTable[12];
    o_DrawIndexedInstanced = (PFN_DrawIndexedInstanced) listTable[13];
    o_RSSetViewports = (PFN_RSSetViewports) listTable[21];
    o_OMSetRenderTargets = (PFN_OMSetRenderTargets) listTable[46];
    o_ClearDepthStencilView = (PFN_ClearDepthStencilView) listTable[47];
    o_ExecuteIndirect = (PFN_ExecuteIndirect) listTable[59];

    g_dsvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    DetourAttach(&(PVOID&) o_CreateDepthStencilView, hkCreateDepthStencilView);
    DetourAttach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);
    DetourAttach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);
    DetourAttach(&(PVOID&) o_DrawInstanced, hkDrawInstanced);
    DetourAttach(&(PVOID&) o_DrawIndexedInstanced, hkDrawIndexedInstanced);
    DetourAttach(&(PVOID&) o_RSSetViewports, hkRSSetViewports);
    DetourAttach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);
    DetourAttach(&(PVOID&) o_ClearDepthStencilView, hkClearDepthStencilView);
    DetourAttach(&(PVOID&) o_ExecuteIndirect, hkExecuteIndirect);

    const auto result = DetourTransactionCommit();

    list->Close();
    list->Release();
    allocator->Release();

    if (result != NO_ERROR)
    {
        LOG_ERROR("Depth finder: hooking failed ({:X}), not installed", (UINT) result);
        o_CreateDepthStencilView = nullptr;
        o_CopyDescriptors = nullptr;
        o_CopyDescriptorsSimple = nullptr;
        o_DrawInstanced = nullptr;
        o_DrawIndexedInstanced = nullptr;
        o_RSSetViewports = nullptr;
        o_OMSetRenderTargets = nullptr;
        o_ClearDepthStencilView = nullptr;
        o_ExecuteIndirect = nullptr;
        return;
    }

    g_installed = true;
    LOG_INFO("Depth finder: observing the game's depth buffers (nothing is changed)");
}

void OnPresent(IDXGISwapChain* swapChain)
{
    if (!g_installed || swapChain == nullptr)
        return;

    DXGI_SWAP_CHAIN_DESC desc {};

    if (FAILED(swapChain->GetDesc(&desc)))
        return;

    std::vector<GenericDepthSelect::Candidate> frame;

    {
        std::lock_guard lock(g_mutex);

        g_pictureWidth = (float) desc.BufferDesc.Width;
        g_bestSnapshotVertices = 0;
        frame.reserve(g_stats.size());

        for (auto it = g_stats.begin(); it != g_stats.end();)
        {
            auto& stats = it->second;

            if (stats.total.drawcalls == 0 && stats.clears == 0)
            {
                // Not touched this frame: forget it, and any command list still pointing at it.
                for (auto& list : g_lists)
                    if (list.second.stats == &stats)
                        list.second.stats = nullptr;

                it = g_stats.erase(it);
                continue;
            }

            GenericDepthSelect::Candidate c;
            c.id = (uint64_t) (size_t) it->first;
            c.width = stats.width;
            c.height = stats.height;
            c.format = (uint32_t) stats.format;
            c.vertices = stats.total.vertices;
            c.drawcalls = stats.total.drawcalls;
            c.drawcallsIndirect = stats.total.drawcallsIndirect;
            c.clears = stats.clears;
            c.bestClear = stats.bestClear;
            c.reversed = stats.reversed;
            frame.push_back(c);

            // The next frame starts from nothing; the bound pointer stays valid because the entry stays.
            stats.total = DrawStats {};
            stats.current = DrawStats {};
            stats.clears = 0;
            stats.bestClear = -1;
            stats.reversed = false;
            ++it;
        }
    }

    ++g_frames;

    const auto pick = g_selector.Update(frame, desc.BufferDesc.Width, desc.BufferDesc.Height);

    const bool pickChanged = (pick.valid ? pick.id : 0) != g_lastLoggedPick;
    const bool dueForLog = g_frames - g_lastLoggedFrame >= kLogEveryFrames || g_lastLoggedFrame == 0;

    if (pickChanged || dueForLog)
    {
        LogCandidates(frame, pick, desc.BufferDesc.Width, desc.BufferDesc.Height);
        g_lastLoggedFrame = g_frames;
        g_lastLoggedPick = pick.valid ? pick.id : 0;
    }

    std::lock_guard lock(g_mutex);
    g_pick = pick;
}

GenericDepthSelect::Pick CurrentPick()
{
    std::lock_guard lock(g_mutex);
    return g_pick;
}
} // namespace GenericDepthDx12
