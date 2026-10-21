#pragma once

// Copies diagnostic resources, never the game picture, from the producer's D3D12 queue
// into D3D11 SRVs. Both directions use a shared GPU fence. No per-frame CPU wait/readback.
#include "SharedFrame.h"
#include <array>

namespace native
{
class DebugPreviewDx11
{
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct Slot
    {
        SharedTexture texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Resource> source;
        uint64_t retiredAt = 0;
    };
    std::array<Slot, 6> _slots;
    std::array<Slot*, 2> _used {};
    unsigned _count = 0, _frame = 0;
    uint64_t _lastRead = 0;
    ComPtr<ID3D11Device> _device11;
    ComPtr<ID3D11DeviceContext4> _context;
    ComPtr<ID3D12Device> _device12;
    ComPtr<ID3D12CommandQueue> _queue;
    SharedFence _fence;
    std::string _error;

  public:
    void BeginFrame() { _count = 0; ++_frame; }
    ID3D11ShaderResourceView* Export(ID3D11Device* device11, ID3D12CommandQueue* queue,
        ID3D12Resource* source, DXGI_FORMAT format, D3D12_RESOURCE_STATES resting)
    {
        if (!source || !device11 || !queue) return nullptr;
        // Inline and enlarged views can refer to the same resource during one menu frame.
        for (unsigned i = 0; i < _count; ++i)
            if (_used[i]->source.Get() == source) return _used[i]->view.Get();
        if (_count == _used.size()) return nullptr;
        if (!_device11)
        {
            _device11 = device11; _queue = queue;
            ComPtr<ID3D11DeviceContext> context;
            device11->GetImmediateContext(&context);
            if (FAILED(context.As(&_context)) || FAILED(queue->GetDevice(IID_PPV_ARGS(&_device12))) ||
                !_fence.Create(_device12.Get(), device11))
            { _error = "Could not share the D3D12 diagnostic with this D3D11 device."; return nullptr; }
        }
        if (_device11.Get() != device11 || _queue.Get() != queue || !_fence.Ready())
        { _error = "The diagnostic graphics device changed. Restart the application."; return nullptr; }
        auto& slot = _slots[(_frame % 3) * 2 + _count];
        const auto completed = _fence.Fence12()->GetCompletedValue();
        if (completed == UINT64_MAX || completed < slot.retiredAt)
        { _error = "Waiting for the previous diagnostic image."; return nullptr; }
        auto desc = source->GetDesc();
        if (!slot.texture.Matches((uint32_t)desc.Width, desc.Height, format))
        {
            slot.view.Reset(); slot.texture.Reset();
            if (!slot.texture.Create(device11, (uint32_t)desc.Width, desc.Height, format, D3D11_BIND_SHADER_RESOURCE) ||
                !slot.texture.Open(_device12.Get()) ||
                FAILED(device11->CreateShaderResourceView(slot.texture.Tex11(), nullptr, &slot.view)))
            { _error = "Could not create the shared diagnostic image: " + slot.texture.Error(); return nullptr; }
        }
        if (!slot.commands)
        {
            if (FAILED(_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
                FAILED(_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                                   IID_PPV_ARGS(&slot.commands))) || FAILED(slot.commands->Close()))
            { _error = "Could not allocate diagnostic copy commands."; return nullptr; }
        }
        if (FAILED(slot.allocator->Reset()) || FAILED(slot.commands->Reset(slot.allocator.Get(), nullptr))) return nullptr;
        slot.source = source;
        D3D12_RESOURCE_BARRIER barriers[2] {};
        for (auto& b : barriers) { b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
        barriers[0].Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, resting, D3D12_RESOURCE_STATE_COPY_SOURCE};
        barriers[1].Transition = {slot.texture.Res12(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
        slot.commands->ResourceBarrier(2, barriers);
        slot.commands->CopyResource(slot.texture.Res12(), source);
        for (auto& b : barriers) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        slot.commands->ResourceBarrier(2, barriers);
        if (FAILED(slot.commands->Close())) return nullptr;
        ID3D12CommandList* commands[] = {slot.commands.Get()};
        if (_count == 0 && _lastRead && FAILED(queue->Wait(_fence.Fence12(), _lastRead))) return nullptr;
        queue->ExecuteCommandLists(1, commands);
        const auto copied = _fence.Next();
        slot.retiredAt = copied;
        if (FAILED(queue->Signal(_fence.Fence12(), copied)) || FAILED(_context->Wait(_fence.Fence11(), copied)))
        { _error = "Could not synchronize the diagnostic copy."; return nullptr; }
        _used[_count++] = &slot;
        _error.clear();
        return slot.view.Get();
    }
    // Called AFTER ImGui submitted its D3D11 reads, even when its main window is hidden.
    void EndFrame()
    {
        if (!_count || !_context) return;
        const auto read = _fence.Next();
        if (SUCCEEDED(_context->Signal(_fence.Fence11(), read)))
        {
            _lastRead = read;
            for (unsigned i = 0; i < _count; ++i) _used[i]->retiredAt = read;
            _context->Flush();
        }
        else
        {
            for (unsigned i = 0; i < _count; ++i) _used[i]->retiredAt = UINT64_MAX;
            _error = "The diagnostic read could not be fenced; its textures are retained.";
        }
        _count = 0;
    }
    ID3D12Fence* CopyFence() const { return _fence.Fence12(); }
    uint64_t CopiedValue() const { return _fence.Value(); }
    const std::string& Error() const { return _error; }
    bool Reset()
    {
        if(_fence.Ready() && _fence.Fence12()->GetCompletedValue()<_fence.Value())
        {
            HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);
            if(!event)return false;
            const HRESULT hr=_fence.Fence12()->SetEventOnCompletion(_fence.Value(),event);
            const bool done=SUCCEEDED(hr) && WaitForSingleObject(event,2000)==WAIT_OBJECT_0;
            CloseHandle(event);if(!done)return false;
        }
        for(auto& slot:_slots)
        {
            slot.view.Reset();slot.texture.Reset();slot.source.Reset();slot.commands.Reset();slot.allocator.Reset();slot.retiredAt=0;
        }
        _fence.Reset();_context.Reset();_queue.Reset();_device12.Reset();_device11.Reset();_lastRead=0;_count=0;
        return true;
    }
};
}
