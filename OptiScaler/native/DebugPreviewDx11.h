#pragma once

// The D3D11 menu's copy of the Optical F5Low debug view (native/FlowDiagnosticUi.h). The producer draws it on its own
// D3D12 device; the D3D11 game's menu (ImGui_ImplDX11) needs a D3D11 view of it. Each menu frame that shows it, the
// picture is copied on the producer's queue into a shared texture, and the D3D11 context waits for that copy on a
// shared fence; the copy waits for D3D11's last read of the same texture the same way. Three textures in turn, so a
// copy never writes one D3D11 may still read. No CPU wait or readback per frame. From PR #232 (TechvibeCoder07).

#include "SharedFrame.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <string>

namespace native
{

class DebugPreviewDx11
{
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

  public:
    // Once per menu frame, before the menu is drawn. A new D3D11 device (the menu was rebuilt) starts over.
    void BeginFrame(ID3D11Device* device)
    {
        if (_device11 != nullptr && _device11.Get() != device)
            Reset();

        _menuDevice = device;
        _exported = nullptr;
    }

    // A D3D11 view of `source` (the producer's preview, R8G8B8A8_UNORM, in `resting` between frames) for this menu
    // frame, or null when it cannot be made (Error() says why). The same source twice in one frame gives the same view.
    ID3D11ShaderResourceView* Export(ID3D12CommandQueue* queue, ID3D12Resource* source, D3D12_RESOURCE_STATES resting)
    {
        if (_menuDevice == nullptr || queue == nullptr || source == nullptr)
            return nullptr;

        if (_exported != nullptr && _exported->source.Get() == source)
            return _exported->view.Get();

        if (_device11 == nullptr)
        {
            ComPtr<ID3D11DeviceContext> context;
            _menuDevice->GetImmediateContext(&context);

            if (FAILED(context.As(&_context)) || FAILED(queue->GetDevice(IID_PPV_ARGS(&_device12))) ||
                !_fence.Create(_device12.Get(), _menuDevice))
            {
                _error = "The debug view cannot be shared with this D3D11 device.";
                _context.Reset();
                _device12.Reset();
                return nullptr;
            }

            _device11 = _menuDevice;
            _queue = queue;
        }

        if (_queue.Get() != queue)
        {
            _error = "The motion estimate's device changed; the debug view shows again after a restart.";
            return nullptr;
        }

        Slot& slot = _slots[_frame % _slots.size()];
        const uint64_t completed = _fence.Fence12()->GetCompletedValue();

        if (completed == UINT64_MAX || completed < slot.copied)
        {
            _error = "Waiting for the last copy of the debug view.";
            return nullptr;
        }

        const D3D12_RESOURCE_DESC desc = source->GetDesc();

        if (!slot.texture.Matches((uint32_t) desc.Width, desc.Height, DXGI_FORMAT_R8G8B8A8_UNORM))
        {
            slot.view.Reset();
            slot.texture.Reset();

            if (!slot.texture.Create(_device11.Get(), (uint32_t) desc.Width, desc.Height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                     D3D11_BIND_SHADER_RESOURCE) ||
                !slot.texture.Open(_device12.Get()) ||
                FAILED(_device11->CreateShaderResourceView(slot.texture.Tex11(), nullptr, &slot.view)))
            {
                _error = "Could not make the debug view's shared picture: " + slot.texture.Error();
                slot.view.Reset();
                slot.texture.Reset();
                return nullptr;
            }
        }

        if (slot.list == nullptr &&
            (FAILED(_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
             FAILED(_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&slot.list))) ||
             FAILED(slot.list->Close())))
        {
            _error = "Could not make the debug view's copy commands.";
            slot.list.Reset();
            slot.allocator.Reset();
            return nullptr;
        }

        // The slot's last copy is done (checked above): its allocator is free.
        if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr)))
            return nullptr;

        D3D12_RESOURCE_BARRIER barriers[2] {};

        for (auto& barrier : barriers)
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

        barriers[0].Transition = { source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, resting,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE };
        barriers[1].Transition = { slot.texture.Res12(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST };
        slot.list->ResourceBarrier(2, barriers);
        slot.list->CopyResource(slot.texture.Res12(), source);

        for (auto& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);

        slot.list->ResourceBarrier(2, barriers);

        if (FAILED(slot.list->Close()))
            return nullptr;

        // D3D11's last read (of the last frame's copy) is signalled before this copy signals: one fence, two queues,
        // its value may only grow. That read covers this slot's older read too.
        if (_lastRead != 0 && FAILED(queue->Wait(_fence.Fence12(), _lastRead)))
            return nullptr;

        ID3D12CommandList* lists[] = { slot.list.Get() };
        queue->ExecuteCommandLists(1, lists);
        const uint64_t copied = _fence.Next();

        if (FAILED(queue->Signal(_fence.Fence12(), copied)) || FAILED(_context->Wait(_fence.Fence11(), copied)))
        {
            _error = "Could not order the debug view's copy.";
            slot.copied = UINT64_MAX; // never reused: the copy's end is unknown
            return nullptr;
        }

        slot.copied = copied;
        slot.source = source;
        _exported = &slot;
        _error.clear();
        return slot.view.Get();
    }

    // Once per menu frame, after the menu's D3D11 draw was submitted: marks the end of its read of this frame's copy.
    void EndFrame()
    {
        if (_exported != nullptr && _context != nullptr)
        {
            const uint64_t read = _fence.Next();

            if (SUCCEEDED(_context->Signal(_fence.Fence11(), read)))
                _lastRead = read;
            else
                _exported->copied = UINT64_MAX; // the read's end is unknown: never reused
        }

        // On to the next slot every menu frame, so one slot that cannot be used again blanks only every third frame.
        ++_frame;

        _exported = nullptr;
        _menuDevice = nullptr;
    }

    const std::string& Error() const { return _error; }

    // When the menu's device goes away: waits (up to 2 s) for the last copy, then lets every D3D11 and D3D12 object go,
    // the device references with them.
    void Reset()
    {
        if (_fence.Ready() && _fence.Fence12()->GetCompletedValue() < _fence.Value())
        {
            if (HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr))
            {
                if (SUCCEEDED(_fence.Fence12()->SetEventOnCompletion(_fence.Value(), event)))
                    WaitForSingleObject(event, 2000);

                CloseHandle(event);
            }
        }

        for (Slot& slot : _slots)
        {
            slot.view.Reset();
            slot.texture.Reset();
            slot.list.Reset();
            slot.allocator.Reset();
            slot.source.Reset();
            slot.copied = 0;
        }

        _fence.Reset();
        _context.Reset();
        _queue.Reset();
        _device12.Reset();
        _device11.Reset();
        _exported = nullptr;
        _menuDevice = nullptr;
        _frame = 0;
        _lastRead = 0;
    }

  private:
    struct Slot
    {
        SharedTexture texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Resource> source;
        uint64_t copied = 0; // the fence value its last copy signalled
    };

    std::array<Slot, 3> _slots;
    Slot* _exported = nullptr; // this menu frame's copy
    uint64_t _frame = 0;
    uint64_t _lastRead = 0;              // the fence value D3D11 signalled after its last read
    ID3D11Device* _menuDevice = nullptr; // this menu frame's (BeginFrame)
    ComPtr<ID3D11Device> _device11;
    ComPtr<ID3D11DeviceContext4> _context;
    ComPtr<ID3D12Device> _device12;
    ComPtr<ID3D12CommandQueue> _queue;
    SharedFence _fence;
    std::string _error;
};

} // namespace native
