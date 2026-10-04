#include "pch.h"

#include "GenericDepth_Dx12.h"

#include <Config.h>
#include <Util.h>

#include <detours/detours.h>

#include <menu/menu_overlay_dx.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <atomic>
#include <mutex>
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

// Whether the hooks count. Cleared for good once the game is seen making an upscaler call, so a game that has one pays a
// relaxed load per call and nothing more.
std::atomic<bool> g_active { false };
std::atomic<bool> g_upscalerSeen { false };
std::atomic<bool> g_armed { false };
std::atomic<bool> g_overlayOn { false };

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
void RecordSnapshot(ID3D12GraphicsCommandList* list, ID3D12Resource* source)
{
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
        barrier(source, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE),
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
        barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE),
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
    if (NumViewports > 0 && pViewports != nullptr && g_active.load(std::memory_order_relaxed))
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
                            RecordSnapshot(This, found->second.resource);
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
    if (g_active.load(std::memory_order_relaxed))
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

    if (g_upscalerSeen.load())
        ImGui::TextDisabled("The game makes its own upscaler call: the depth finder has stood down.");
    else if (!g_armed.load())
        ImGui::TextDisabled("Watching the game's depth buffers (%llu of %u frames)...", (unsigned long long) g_presents,
                            warmup);
    else if (!pick.valid)
        ImGui::TextDisabled("No depth buffer qualifies yet.");
    else
        ImGui::Text("Picked %ux%u, format %u, score %llu%s", pick.width, pick.height, pick.format,
                    (unsigned long long) pick.score, pick.reversed ? ", reversed-Z" : "");

    ImGui::TextDisabled("The log has the candidates (Depth finder lines).");

    if (!g_overlayOn.load())
    {
        ImGui::TextDisabled("Tick the depth checkbox above and restart to see the picked buffer.");
    }
    else if (Armed() && pick.valid)
    {
        std::lock_guard lock(g_mutex);

        if (g_backupFrame == 0 || g_backup.resource == nullptr)
        {
            ImGui::TextDisabled("Waiting for the picked buffer's first clear...");
        }
        else
        {
            EnsureOverlayView();

            if (g_srvAllocated && !g_srvDirty)
            {
                const float width = std::min(360.0f, ImGui::GetContentRegionAvail().x);
                const float height = width * (float) g_backup.height / (float) g_backup.width;
                ImGui::Image((ImTextureID) g_srvGpu.ptr, ImVec2(width, height));
                ImGui::TextDisabled("Raw depth: normal depth looks nearly white, reversed-Z shows near as bright.");
            }
            else
                ImGui::TextDisabled("The menu could not give the image a descriptor.");
        }
    }

    ImGui::TreePop();
}
} // namespace GenericDepthDx12
