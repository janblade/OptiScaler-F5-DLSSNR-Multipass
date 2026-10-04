#include "pch.h"

#include "GenericDepth_Dx12.h"

#include <Config.h>
#include <Util.h>

#include <detours/detours.h>

#include <menu/menu_overlay_dx.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D12GraphicsCommandList* This, UINT ThreadGroupCountX, UINT ThreadGroupCountY,
                                              UINT ThreadGroupCountZ);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteBundle)(ID3D12GraphicsCommandList* This,
                                                   ID3D12GraphicsCommandList* pCommandList);
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
PFN_ExecuteBundle o_ExecuteBundle = nullptr;
PFN_Dispatch o_Dispatch = nullptr;

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
    ID3D12Resource* resource = nullptr;
    bool readOnlyDepth = false;
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
    bool readOnlyDepth = false; // the view is read-only for depth, so the buffer is in the depth-read state while bound
};

struct ListState
{
    Stats* stats = nullptr;       // the depth buffer this list draws into now
    float viewportWidth = 0.0f;   // its main viewport
};

// Whether the hooks count. Cleared for good once the game is seen making an upscaler call, so a game that has one pays a
// relaxed load per call and nothing more.
std::atomic<bool> g_active { false };
std::atomic<bool> g_upscalerSeen { false };
std::atomic<bool> g_armed { false };
std::atomic<bool> g_overlayOn { false };

// What the hooks have seen since start, for the log: tells "the game has no depth buffer at this point" (a menu, a video)
// from "the hooks are blind" (descriptors created before they were installed, a path they do not cover).
std::atomic<uint64_t> g_countDsvCreated { 0 };
std::atomic<uint64_t> g_countOmSet { 0 };
std::atomic<uint64_t> g_countOmSetWithDepth { 0 };     // an OMSetRenderTargets that carried a depth descriptor
std::atomic<uint64_t> g_countOmSetUnknownDepth { 0 };  // ... of which the descriptor was not one the hooks had seen created
std::atomic<uint64_t> g_countDraws { 0 };              // every draw, whatever was bound
std::atomic<uint64_t> g_countExecIndirect { 0 };       // every ExecuteIndirect, whatever was bound
std::atomic<uint64_t> g_countExecBundle { 0 };         // every ExecuteBundle: draws recorded in a bundle are not seen by the
                                                       // direct list's draw hooks if the bundle's functions are other code
std::atomic<uint64_t> g_countDispatch { 0 };           // every Dispatch (a game that renders through compute draws little)
std::atomic<int> g_bundleSameDraw { -1 };              // 1 the bundle's Draw functions are the direct list's, 0 not, -1 unknown

// The copy of the picked depth buffer for the overlay (guarded by g_mutex like the rest).
struct Backup
{
    ID3D12Resource* resource = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
};

std::mutex g_mutex;
std::unordered_map<SIZE_T, DsvInfo> g_dsv;                       // CPU descriptor handle -> what it views
std::unordered_map<ID3D12GraphicsCommandList*, ListState> g_lists;
std::unordered_map<ID3D12Resource*, Stats> g_stats;              // node-stable: g_lists holds pointers into it
uint64_t g_bestSnapshotVertices = 0;                             // the busiest stretch before a clear, this frame
float g_pictureWidth = 0.0f;                                     // from the last present
Backup g_backup;
uint64_t g_backupFrame = 0;          // the frame a copy was last recorded in
bool g_srvDirty = true;
bool g_srvAllocated = false;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE g_srvCpu {};
D3D12_GPU_DESCRIPTOR_HANDLE g_srvGpu {};
std::vector<std::pair<ID3D12Resource*, uint64_t>> g_retired; // replaced copies, released a few frames later
uint64_t g_presents = 0;
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
    if (!g_active.load(std::memory_order_relaxed))
        return;

    g_countDraws.fetch_add(1, std::memory_order_relaxed);

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

// The typeless format a copy of a depth format is made in, and the format its depth plane is read through.
bool BackupFormats(DXGI_FORMAT depth, DXGI_FORMAT* typeless, DXGI_FORMAT* view)
{
    switch (depth)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        *typeless = DXGI_FORMAT_R32_TYPELESS;
        *view = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        *typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
        *view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        *typeless = DXGI_FORMAT_R24G8_TYPELESS;
        *view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        *typeless = DXGI_FORMAT_R16_TYPELESS;
        *view = DXGI_FORMAT_R16_UNORM;
        return true;
    default:
        return false;
    }
}

// Under g_mutex. Makes the copy target when the depth buffer's size or format is new, retiring the old one for a few frames
// (the menu may still be reading it).
bool EnsureBackup(ID3D12Device* device, const D3D12_RESOURCE_DESC& source, DXGI_FORMAT typeless, DXGI_FORMAT view)
{
    if (g_backup.resource != nullptr && g_backup.width == source.Width && g_backup.height == source.Height &&
        g_backup.typeless == typeless)
        return true;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = source.Width;
    desc.Height = source.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = typeless;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* created = nullptr;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                               IID_PPV_ARGS(&created))))
        return false;

    if (g_backup.resource != nullptr)
        g_retired.emplace_back(g_backup.resource, g_presents);

    g_backup = Backup { created, (uint32_t) source.Width, source.Height, typeless, view };
    g_srvDirty = true;
    return true;
}

// Under g_mutex, from the clear hook, before the clear itself: the buffer is in the depth-write state a clear needs, so
// it goes to copy-source and back around one copy of its first subresource into the overlay's texture. The overlay's own
// texture rests in the shader-resource state. A multisampled buffer is skipped (it would need a resolve).
void RecordSnapshot(ID3D12GraphicsCommandList* list, ID3D12Resource* source, bool readOnlyDepth)
{
    const auto depthState = readOnlyDepth ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE;

    const auto desc = source->GetDesc();

    if (desc.SampleDesc.Count > 1)
        return;

    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN;

    if (!BackupFormats(desc.Format, &typeless, &view))
        return;

    ID3D12Device* device = nullptr;

    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
        return;

    const bool ready = EnsureBackup(device, desc, typeless, view);
    device->Release();

    if (!ready)
        return;

    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    };

    D3D12_RESOURCE_BARRIER in[2] = {
        barrier(source, depthState, D3D12_RESOURCE_STATE_COPY_SOURCE),
        barrier(g_backup.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)
    };
    list->ResourceBarrier(2, in);

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_backup.resource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER out[2] = {
        barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, depthState),
        barrier(g_backup.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    };
    list->ResourceBarrier(2, out);

    g_backupFrame = g_presents;
}

void STDMETHODCALLTYPE hkCreateDepthStencilView(ID3D12Device* This, ID3D12Resource* pResource,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    o_CreateDepthStencilView(This, pResource, pDesc, DestDescriptor);

    g_countDsvCreated.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard lock(g_mutex);

    if (pResource == nullptr)
    {
        g_dsv.erase(DestDescriptor.ptr);
        return;
    }

    const auto desc = pResource->GetDesc();
    const bool readOnly = pDesc != nullptr && (pDesc->Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0;
    g_dsv[DestDescriptor.ptr] = DsvInfo { pResource, (uint32_t) desc.Width, desc.Height, desc.Format, readOnly };
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

// What each hook does, apart from calling on to the original: shared by the hooks on the runtime's own functions above and
// by the thunks that go into a game command list's private vtable below.
void OnViewports(ID3D12GraphicsCommandList* This, UINT NumViewports, const D3D12_VIEWPORT* pViewports)
{
    // Only the main viewport matters, as in ReShade's add-on.
    if (NumViewports > 0 && pViewports != nullptr && g_active.load(std::memory_order_relaxed))
    {
        std::lock_guard lock(g_mutex);
        g_lists[This].viewportWidth = pViewports[0].Width;
    }
}

void OnClear(ID3D12GraphicsCommandList* This, D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags,
             FLOAT Depth)
{
    if ((ClearFlags & D3D12_CLEAR_FLAG_DEPTH) != 0 && g_active.load(std::memory_order_relaxed))
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

                        // The overlay's copy: only of the buffer picked last frame, and only at the busiest stretch, so
                        // the copy that is left at the end of the frame is the scene's.
                        if (g_overlayOn.load(std::memory_order_relaxed) && g_pick.valid &&
                            g_pick.id == (uint64_t) (size_t) found->second.resource)
                            RecordSnapshot(This, found->second.resource, false); // a clear needs depth-write
                    }

                    ++stats.clears;
                }
            }
        }
    }
}

void OnIndirect(ID3D12GraphicsCommandList* This, UINT MaxCommandCount)
{
    if (g_active.load(std::memory_order_relaxed))
    {
        g_countExecIndirect.fetch_add(1, std::memory_order_relaxed);

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
}

// ---- Game command lists with a vtable of their own ----------------------------------------------------------------------
// Witcher 3 (2026-10-04): its command lists each carry a copy of the vtable in heap memory whose DrawInstanced and
// DrawIndexedInstanced point into another module's code, while OMSetRenderTargets is the runtime's own. The hooks on the
// runtime's functions then never see a draw. So the first time a list is seen, an entry of its table that is not the
// runtime's own is replaced by a thunk that counts and calls what was there. A table whose entries are the runtime's own is
// left alone: the hooks above already cover it, and patching it as well would count every draw twice.
enum Slot
{
    kDraw = 0,
    kDrawIndexed,
    kViewports,
    kClear,
    kIndirect,
    kSlots
};
constexpr int kSlotIndex[kSlots] = { 12, 13, 21, 47, 59 };
constexpr const char* kSlotName[kSlots] = { "DrawInstanced", "DrawIndexedInstanced", "RSSetViewports",
                                            "ClearDepthStencilView", "ExecuteIndirect" };

struct TablePatch
{
    PVOID previous[kSlots] {};
};

PVOID* g_installTable = nullptr; // the runtime's own table, read at install
std::unordered_map<PVOID*, TablePatch> g_patched;
std::unordered_map<PVOID*, bool> g_examined;
int g_patchLogs = 0;

PVOID PreviousOf(ID3D12GraphicsCommandList* list, int slot)
{
    PVOID* table = *(PVOID**) list;
    std::lock_guard lock(g_mutex);
    const auto found = g_patched.find(table);
    return found != g_patched.end() ? found->second.previous[slot] : nullptr;
}

void STDMETHODCALLTYPE hkTableDrawInstanced(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance,
                                            UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
    OnDraw(This, VertexCountPerInstance, InstanceCount);

    if (auto previous = (PFN_DrawInstanced) PreviousOf(This, kDraw))
        previous(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE hkTableDrawIndexedInstanced(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                                   UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
                                                   UINT StartInstanceLocation)
{
    OnDraw(This, IndexCountPerInstance, InstanceCount);

    if (auto previous = (PFN_DrawIndexedInstanced) PreviousOf(This, kDrawIndexed))
        previous(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                 StartInstanceLocation);
}

void STDMETHODCALLTYPE hkTableRSSetViewports(ID3D12GraphicsCommandList* This, UINT NumViewports,
                                             const D3D12_VIEWPORT* pViewports)
{
    OnViewports(This, NumViewports, pViewports);

    if (auto previous = (PFN_RSSetViewports) PreviousOf(This, kViewports))
        previous(This, NumViewports, pViewports);
}

void STDMETHODCALLTYPE hkTableClearDepthStencilView(ID3D12GraphicsCommandList* This,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
                                                    D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil,
                                                    UINT NumRects, const D3D12_RECT* pRects)
{
    if (g_active.load(std::memory_order_relaxed))
        OnClear(This, DepthStencilView, ClearFlags, Depth);

    if (auto previous = (PFN_ClearDepthStencilView) PreviousOf(This, kClear))
        previous(This, DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
}

void STDMETHODCALLTYPE hkTableExecuteIndirect(ID3D12GraphicsCommandList* This, ID3D12CommandSignature* pCommandSignature,
                                              UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer,
                                              UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer,
                                              UINT64 CountBufferOffset)
{
    if (g_active.load(std::memory_order_relaxed))
        OnIndirect(This, MaxCommandCount);

    if (auto previous = (PFN_ExecuteIndirect) PreviousOf(This, kIndirect))
        previous(This, pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer,
                 CountBufferOffset);
}

const PVOID kTableHooks[kSlots] = { (PVOID) hkTableDrawInstanced, (PVOID) hkTableDrawIndexedInstanced,
                                    (PVOID) hkTableRSSetViewports, (PVOID) hkTableClearDepthStencilView,
                                    (PVOID) hkTableExecuteIndirect };

void PatchListTable(ID3D12GraphicsCommandList* list)
{
    PVOID* table = *(PVOID**) list;

    if (table == nullptr || table == g_installTable)
        return;

    {
        std::lock_guard lock(g_mutex);

        if (!g_examined.emplace(table, true).second)
            return;
    }

    TablePatch patch;
    bool any = false;

    for (int slot = 0; slot < kSlots; ++slot)
    {
        PVOID entry = table[kSlotIndex[slot]];

        if (g_installTable != nullptr && entry != g_installTable[kSlotIndex[slot]] && entry != kTableHooks[slot])
        {
            patch.previous[slot] = entry;
            any = true;
        }
    }

    if (!any)
        return;

    {
        std::lock_guard lock(g_mutex);
        g_patched[table] = patch; // before the entries change, so a thunk that runs at once finds what to call
    }

    int patched = 0;

    for (int slot = 0; slot < kSlots; ++slot)
    {
        if (patch.previous[slot] == nullptr)
            continue;

        PVOID* entry = &table[kSlotIndex[slot]];
        DWORD old = 0;

        if (VirtualProtect(entry, sizeof(PVOID), PAGE_READWRITE, &old))
        {
            InterlockedExchangePointer(entry, kTableHooks[slot]);
            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(PVOID), old, &ignored);
            ++patched;
        }
    }

    if (g_patchLogs++ < 6)
    {
        std::string which;

        for (int slot = 0; slot < kSlots; ++slot)
            if (patch.previous[slot] != nullptr)
                which += std::format(" {}={:X}", kSlotName[slot], (size_t) patch.previous[slot]);

        LOG_INFO("Depth finder: a game command list has a vtable of its own ({:X}); {} entries now count through us, they "
                 "pointed at:{}",
                 (size_t) table, patched, which);
    }
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                            BOOL RTsSingleHandleToDescriptorRange,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    static std::atomic<int> logged { 0 };

    if (logged.load(std::memory_order_relaxed) < 4 && logged.fetch_add(1) < 4)
    {
        PVOID* table = *(PVOID**) This;
        LOG_INFO("Depth finder: a game command list {:X} uses vtable {:X} (hooks were read from {:X}, {}); DrawInstanced {:X} "
                 "(read {:X}), DrawIndexedInstanced {:X} (read {:X}), OMSetRenderTargets {:X} (read {:X})",
                 (size_t) This, (size_t) table, (size_t) g_installTable, table == g_installTable ? "the same" : "a DIFFERENT one",
                 (size_t) table[12], g_installTable ? (size_t) g_installTable[12] : 0, (size_t) table[13],
                 g_installTable ? (size_t) g_installTable[13] : 0, (size_t) table[46],
                 g_installTable ? (size_t) g_installTable[46] : 0);
    }

    if (g_active.load(std::memory_order_relaxed))
        PatchListTable(This);

    if (g_active.load(std::memory_order_relaxed))
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
                stats.resource = found->second.resource;
                stats.readOnlyDepth = found->second.readOnlyDepth;
                bound = &stats;
            }
        }

        // The picked buffer is often never cleared again in the frame it was drawn (Witcher 3 clears at the start of the
        // pass), so the clear never offers a snapshot. The moment the list moves off it is the other chance: the busiest
        // stretch of the frame is copied there, in the state the view says the buffer is in.
        Stats* previous = g_lists[This].stats;

        if (previous != nullptr && previous != bound && g_overlayOn.load(std::memory_order_relaxed) && g_pick.valid &&
            previous->resource != nullptr && g_pick.id == (uint64_t) (size_t) previous->resource &&
            previous->current.drawcalls != 0)
        {
            if (previous->current.vertices >= g_bestSnapshotVertices)
            {
                g_bestSnapshotVertices = previous->current.vertices;
                RecordSnapshot(This, previous->resource, previous->readOnlyDepth);
            }

            previous->current = DrawStats {};
        }

        g_lists[This].stats = bound;

        g_countOmSet.fetch_add(1, std::memory_order_relaxed);
        if (pDepthStencilDescriptor != nullptr)
        {
            g_countOmSetWithDepth.fetch_add(1, std::memory_order_relaxed);
            if (bound == nullptr)
                g_countOmSetUnknownDepth.fetch_add(1, std::memory_order_relaxed);
        }
    }

    o_OMSetRenderTargets(This, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange,
                         pDepthStencilDescriptor);
}

void STDMETHODCALLTYPE hkRSSetViewports(ID3D12GraphicsCommandList* This, UINT NumViewports,
                                        const D3D12_VIEWPORT* pViewports)
{
    OnViewports(This, NumViewports, pViewports);

    o_RSSetViewports(This, NumViewports, pViewports);
}

void STDMETHODCALLTYPE hkClearDepthStencilView(ID3D12GraphicsCommandList* This,
                                               D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
                                               D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil,
                                               UINT NumRects, const D3D12_RECT* pRects)
{
    OnClear(This, DepthStencilView, ClearFlags, Depth);

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
    OnIndirect(This, MaxCommandCount);

    o_ExecuteIndirect(This, pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer,
                      CountBufferOffset);
}

void STDMETHODCALLTYPE hkDispatch(ID3D12GraphicsCommandList* This, UINT ThreadGroupCountX, UINT ThreadGroupCountY,
                                    UINT ThreadGroupCountZ)
{
    if (g_active.load(std::memory_order_relaxed))
        g_countDispatch.fetch_add(1, std::memory_order_relaxed);

    o_Dispatch(This, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

void STDMETHODCALLTYPE hkExecuteBundle(ID3D12GraphicsCommandList* This, ID3D12GraphicsCommandList* pCommandList)
{
    if (g_active.load(std::memory_order_relaxed))
        g_countExecBundle.fetch_add(1, std::memory_order_relaxed);

    o_ExecuteBundle(This, pCommandList);
}

void LogCandidates(const std::vector<GenericDepthSelect::Candidate>& frame, const GenericDepthSelect::Pick& pick,
                   uint32_t pictureWidth, uint32_t pictureHeight)
{
    auto sorted = frame;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return GenericDepthSelect::Score(a) > GenericDepthSelect::Score(b); });

    LOG_INFO("Depth finder: frame {}, picture {}x{}, {} depth buffer(s) in use{}", g_frames, pictureWidth,
             pictureHeight, sorted.size(), pick.valid ? "" : ", none qualifies");
    LOG_INFO("Depth finder:   hooks so far: {} depth views created, {} OMSetRenderTargets ({} with a depth descriptor, "
             "{} of those unknown to us), {} draws, {} ExecuteIndirect, {} ExecuteBundle (bundle draw code is the direct "
             "list's: {}), {} Dispatch",
             g_countDsvCreated.load(), g_countOmSet.load(), g_countOmSetWithDepth.load(),
             g_countOmSetUnknownDepth.load(), g_countDraws.load(), g_countExecIndirect.load(), g_countExecBundle.load(),
             g_bundleSameDraw.load() < 0 ? "unknown" : g_bundleSameDraw.load() ? "yes" : "no",
             g_countDispatch.load());

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
    o_ExecuteBundle = (PFN_ExecuteBundle) listTable[27];
    o_Dispatch = (PFN_Dispatch) listTable[14];
    g_installTable = listTable;

    // Is a bundle's DrawInstanced the same code as the direct list's? If not, draws recorded in bundles are invisible to the
    // draw hooks (a bundle inherits the caller's depth buffer, so they would have to be counted at ExecuteBundle).
    {
        ID3D12CommandAllocator* bundleAllocator = nullptr;
        ID3D12GraphicsCommandList* bundle = nullptr;

        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundleAllocator))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundleAllocator, nullptr,
                                                IID_PPV_ARGS(&bundle))))
        {
            ID3D12GraphicsCommandList* realBundle = nullptr;

            if (!Util::CheckForRealObject(__FUNCTION__, bundle, (IUnknown**) &realBundle))
                realBundle = bundle;

            PVOID* bundleTable = *(PVOID**) realBundle;
            g_bundleSameDraw = (bundleTable[12] == listTable[12] && bundleTable[13] == listTable[13]) ? 1 : 0;
            bundle->Close();
        }

        if (bundle != nullptr)
            bundle->Release();
        if (bundleAllocator != nullptr)
            bundleAllocator->Release();
    }

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
    DetourAttach(&(PVOID&) o_ExecuteBundle, hkExecuteBundle);
    DetourAttach(&(PVOID&) o_Dispatch, hkDispatch);

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
        o_ExecuteBundle = nullptr;
        o_Dispatch = nullptr;
        return;
    }

    g_overlayOn = Config::Instance()->DlssNrNativeDepthOverlay.value_or_default();
    g_active = true;
    g_installed = true;
    LOG_INFO("Depth finder: observing the game's depth buffers{}, after {} frames of warm-up; it stands down if the game "
             "makes an upscaler call",
             g_overlayOn.load() ? " (the overlay's copy is recorded into the game's command list)"
                                : " (nothing is changed)",
             Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default());
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

    {
        std::lock_guard lock(g_mutex);
        ++g_presents;

        // Copies replaced a few frames ago are no longer being read by the menu.
        for (auto it = g_retired.begin(); it != g_retired.end();)
        {
            if (g_presents > it->second + 4)
            {
                it->first->Release();
                it = g_retired.erase(it);
            }
            else
                ++it;
        }

        if (g_upscalerSeen.load())
        {
            // Stand down for good: stop counting, drop the tracking and the pick.
            g_active = false;
            g_armed = false;
            g_stats.clear();
            g_lists.clear();
            g_pick = GenericDepthSelect::Pick {};
            g_selector.Reset();
            return;
        }
    }

    // Counting has gone on through the warm-up, but nothing is picked or reported until it is over.
    if (g_presents <= Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default())
        return;

    g_armed = true;

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

void NoteUpscalerCall()
{
    if (!g_installed)
        return;

    if (!g_upscalerSeen.exchange(true))
        LOG_INFO("Depth finder: the game makes its own upscaler call; the finder stands down");
}

bool Armed() { return g_installed && g_armed.load() && !g_upscalerSeen.load(); }

// Under g_mutex. The overlay's texture needs a descriptor in the menu's heap; both come and go with the menu.
static void EnsureOverlayView()
{
    ID3D12DescriptorHeap* heap = MenuOverlayDx::SrvHeap();

    if (heap == nullptr || g_backup.resource == nullptr)
        return;

    if (heap != g_srvHeap)
    {
        g_srvHeap = heap;
        g_srvAllocated = false;
        g_srvDirty = true;
    }

    if (!g_srvAllocated)
    {
        if (!MenuOverlayDx::AllocSrv(&g_srvCpu, &g_srvGpu))
            return;

        g_srvAllocated = true;
        g_srvDirty = true;
    }

    if (!g_srvDirty)
        return;

    ID3D12Device* device = nullptr;

    if (FAILED(g_backup.resource->GetDevice(IID_PPV_ARGS(&device))))
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = g_backup.view;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    // The depth on every colour channel, so it reads as gray rather than red.
    srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    srv.Texture2D.MipLevels = 1;
    srv.Texture2D.PlaneSlice = 0;

    device->CreateShaderResourceView(g_backup.resource, &srv, g_srvCpu);
    device->Release();
    g_srvDirty = false;
}

void DrawDebugUi()
{
    auto* config = Config::Instance();

    if (!ImGui::TreeNode("Depth finder (experimental)##depthfinder"))
        return;

    // Both switches are read when the game's device is created, so a change here is saved with the rest and applies at the
    // next start.
    bool finder = config->DlssNrNativeDepthFinder.value_or_default();

    if (ImGui::Checkbox("Find the scene's depth##depthfinder", &finder))
        config->DlssNrNativeDepthFinder = finder;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "First step toward DLSS-NR in a game with no DLSS, FSR or XeSS: watches the game's depth\n"
                                "buffers (DirectX 12) and picks the scene's. It only observes, and it stands down for good if\n"
                                "the game makes an upscaler call of its own. The log lists the candidates.\n"
                                "Applies at the next start: save the settings and restart the game.");

    if (!finder && !g_installed)
    {
        ImGui::TreePop();
        return;
    }

    bool overlay = config->DlssNrNativeDepthOverlay.value_or_default();

    if (ImGui::Checkbox("Show the picked depth here##depthfinder", &overlay))
        config->DlssNrNativeDepthOverlay = overlay;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Debug. Copies the picked depth buffer at its busiest clear, recorded into the game's own\n"
                                "command list, and shows it below. Leave it off unless you are checking the pick.\n"
                                "Applies at the next start.");

    if (finder != g_installed)
        ImGui::TextDisabled("Takes effect after saving and restarting the game.");

    if (!g_installed)
    {
        ImGui::TreePop();
        return;
    }

    const auto pick = CurrentPick();
    const uint32_t warmup = Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default();

    // Every branch writes exactly one short line and the picture has a box of its own size, so nothing below moves when the
    // pick changes.
    if (g_upscalerSeen.load())
        ImGui::TextDisabled("Stood down: the game has an upscaler of its own.");
    else if (!g_armed.load())
        ImGui::TextDisabled("Watching (%llu of %u frames)...", (unsigned long long) g_presents, warmup);
    else if (!pick.valid)
        ImGui::TextDisabled("No depth buffer qualifies yet.");
    else
        ImGui::Text("Picked %ux%u, format %u%s", pick.width, pick.height, pick.format,
                    pick.reversed ? ", reversed-Z" : "");

    ImGui::TextDisabled("The log has the candidates (Depth finder lines).");

    if (!g_overlayOn.load())
    {
        ImGui::TextDisabled("Tick the depth checkbox above and restart to see the picked buffer.");
    }
    else
    {
        const float boxWidth = 360.0f;
        const float boxHeight = boxWidth * 9.0f / 16.0f;
        bool drawn = false;

        if (Armed() && pick.valid)
        {
            std::lock_guard lock(g_mutex);

            if (g_backupFrame != 0 && g_backup.resource != nullptr)
            {
                EnsureOverlayView();

                if (g_srvAllocated && !g_srvDirty)
                {
                    ImGui::Image((ImTextureID) g_srvGpu.ptr, ImVec2(boxWidth, boxHeight));
                    drawn = true;
                }
            }
        }

        if (!drawn)
            ImGui::Dummy(ImVec2(boxWidth, boxHeight));

        ImGui::TextDisabled(drawn ? "Raw depth; reversed-Z shows near as bright." : "Waiting for the picked buffer...");
    }

    ImGui::TreePop();
}
} // namespace GenericDepthDx12
