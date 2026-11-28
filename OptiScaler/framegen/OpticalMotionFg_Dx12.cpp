#include "pch.h"
#include "OpticalMotionFg_Dx12.h"

#include <Config.h>
#include <State.h>
#include <framegen/FGOutput.h>
#include <framegen/IFGFeature_Dx12.h>
#include <misc/DeferredRelease.h>
#include <native/VirtualUpscalerDriver.h>
#include <native/VkPresentBridge.h>
#include <output/SdrToHdr_ProxyDx12.h>

#include <wrl/client.h>

#include <array>
#include <atomic>
#include <mutex>
#include <optional>

using Microsoft::WRL::ComPtr;
using namespace DlssNr::OpticalMotion;

namespace FGOpticalMotion
{
namespace
{

// A resource a list recorded this frame may still read: let go of only once no queue can reach it
void Drop(ComPtr<ID3D12Resource>& held)
{
    if (auto* raw = held.Detach())
        DeferredRelease::Release(raw);
}

struct Frame
{
    bool optical = false; // the call described this frame's motion as the flow's
    uint32_t pictureWidth = 0, pictureHeight = 0;
    // The game's motion vectors, copied on its list at the call (the fallback), and what the call said about them
    ComPtr<ID3D12Resource> gameCopy;
    bool haveGame = false;
    uint32_t gameWidth = 0, gameHeight = 0;
    float scaleX = 1.0f, scaleY = 1.0f, jitterX = 0.0f, jitterY = 0.0f;
    bool resets = false;
    ComPtr<ID3D12Resource> flowHeld; // the flow tagged for this frame, kept until the slot is used again
};

struct Offer
{
    ComPtr<ID3D12Resource> motion;
    Present present; // identities only
};

std::mutex g_mutex;
std::array<Frame, BUFFER_COUNT> g_frames;
Offer g_offer;
// The calls describe their motion as the flow's. Atomic: XeFG's activation check reads it under frame generation's
// lock, which must never wait on g_mutex's holder (the present takes that lock, then g_mutex).
std::atomic<bool> g_applied = false;
// [XeFG] JitteredMV / HighResMV as they were before ApplyCreationFlags made them the flow's
bool g_creationFlagsSet = false;
std::optional<bool> g_savedJittered, g_savedHighRes;
Counts g_counts;
uint32_t g_fallbackLogs = 0;

bool EnsureCopy(ComPtr<ID3D12Resource>& copy, ID3D12Resource* source)
{
    auto desc = source->GetDesc();
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.Alignment = 0;

    if (copy)
    {
        const auto held = copy->GetDesc();

        if (held.Width == desc.Width && held.Height == desc.Height && held.Format == desc.Format &&
            held.MipLevels == desc.MipLevels && held.DepthOrArraySize == desc.DepthOrArraySize)
            return true;

        Drop(copy);
    }

    ComPtr<ID3D12Device> device;

    if (FAILED(source->GetDevice(IID_PPV_ARGS(&device))))
        return false;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&copy))))
    {
        LOG_WARN("Frame generation optical motion: could not make the copy of the game's motion vectors");
        return false;
    }

    copy->SetName(L"FGOpticalMotion game motion copy");
    return true;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

} // namespace

FgKeys Keys()
{
    auto* config = Config::Instance();
    auto& state = State::Instance();

    FgKeys keys;
    keys.optical = config->FGMotionSourceOptical.value_or_default();
    keys.fgEnabled = config->IsFGEnabled();
    keys.upscalerInput = state.activeFgInput == FGInput::Upscaler;
    keys.xefgApiOutput = UsesXeFGApi(state.activeFgOutput);
    keys.nativeDx12 = state.swapchainApi == API::DX12 && state.swapchainInteropApi == SwapchainInteropApi::None &&
                      !VkPresentBridge::OutputActive();
    keys.resourceFlip = config->FGResourceFlip.value_or_default();
    // XeFG reads display-size motion right only from a swap chain made for it (ApplyCreationFlags); F5Low takes the
    // motion's size from the tag itself
    keys.contextFits =
        state.currentFG == nullptr || state.activeFgOutput == FGOutput::F5Low || !state.currentFG->IsLowResMV();
    return keys;
}

bool Wanted() { return FgWanted(Keys()); }

const char* RefusalText(FgRefusal refusal)
{
    switch (refusal)
    {
    case FgRefusal::NotNativeDx12:
        return "Optical F5Low motion for frame generation needs a native DirectX 12 game.";
    case FgRefusal::NotUpscalerInput:
        return "Optical F5Low motion for frame generation needs FG Input \"Upscaler\" (frame generation fed from the "
               "game's upscaler).";
    case FgRefusal::Output:
        return "Optical F5Low motion for frame generation works with the XeFG and F5Low outputs only (FSR frame "
               "generation and DLSS-G keep the game's motion).";
    case FgRefusal::ResourceFlip:
        return "Optical F5Low motion for frame generation does not work with \"Flip motion and depth\" (Resource "
               "Flip): the measured motion is never flipped.";
    case FgRefusal::NeedsRestart:
        return "XeFG takes Optical F5Low motion from its next start: save the settings and restart the game.";
    case FgRefusal::None:
        break;
    }

    return nullptr;
}

void Publish(ID3D12CommandQueue* queue, ID3D12Resource* picture, ID3D12Resource* motion, uint32_t width,
             uint32_t height, uint64_t epoch)
{
    std::lock_guard lock(g_mutex);
    g_offer.motion = motion;
    g_offer.present = { queue, picture, epoch, width, height };
}

void Clear()
{
    std::lock_guard lock(g_mutex);
    g_offer = Offer {};
}

void ApplyCreationFlags()
{
    auto* config = Config::Instance();
    auto& state = State::Instance();
    const bool optical = config->FGMotionSourceOptical.value_or_default() && state.activeFgInput == FGInput::Upscaler &&
                         UsesXeFGApi(state.activeFgOutput);

    std::lock_guard lock(g_mutex);

    if (optical == g_creationFlagsSet)
        return;

    g_creationFlagsSet = optical;

    if (optical)
    {
        g_savedJittered = static_cast<const std::optional<bool>&>(config->FGXeFGJitteredMV);
        g_savedHighRes = static_cast<const std::optional<bool>&>(config->FGXeFGHighResMV);
        config->FGXeFGJitteredMV.set_volatile_value(false);
        config->FGXeFGHighResMV.set_volatile_value(true);
        LOG_INFO("Frame generation motion: the swap chain is made for Optical F5Low's flow (display-size, unjittered)");
        return;
    }

    if (g_savedJittered.has_value())
        config->FGXeFGJitteredMV.set_volatile_value(*g_savedJittered);
    else
        config->FGXeFGJitteredMV.reset_volatile();

    if (g_savedHighRes.has_value())
        config->FGXeFGHighResMV.set_volatile_value(*g_savedHighRes);
    else
        config->FGXeFGHighResMV.reset_volatile();
}

bool MadeForFlow()
{
    std::lock_guard lock(g_mutex);
    return g_creationFlagsSet;
}

bool AtUpscaleCall(IFGFeature_Dx12* fg, IFeature_Dx12* feature, bool severalViews)
{
    const bool optical = FgAtCall(Wanted(), native::IsVirtualUpscalerFeature(feature), severalViews);

    std::lock_guard lock(g_mutex);

    if (optical == g_applied.load())
        return optical;

    g_applied = optical;

    for (auto& frame : g_frames)
        frame.optical = false;

    if (optical)
    {
        g_counts = {};
        g_fallbackLogs = 0;
    }

    // No rebuild: the motion's kind is the swap chain's (ApplyCreationFlags), and each frame says its own scale
    LOG_INFO("Frame generation motion: {} ({})",
             optical ? "Optical F5Low's flow, measured on the finished picture" : "the game's motion vectors again",
             fg != nullptr ? fg->Name() : "no frame generation");
    return optical;
}

void KeepGameMotion(IFGFeature_Dx12* fg, int index, bool optical, ID3D12GraphicsCommandList* list, ID3D12Resource* mv,
                    D3D12_RESOURCE_STATES state, uint32_t width, uint32_t height, float scaleX, float scaleY,
                    float jitterX, float jitterY, bool lowRes, bool jittered, uint32_t pictureWidth,
                    uint32_t pictureHeight)
{
    if (fg == nullptr || index < 0 || index >= BUFFER_COUNT)
        return;

    std::lock_guard lock(g_mutex);
    auto& frame = g_frames[index];
    frame.optical = optical;
    frame.haveGame = false;
    frame.pictureWidth = pictureWidth;
    frame.pictureHeight = pictureHeight;
    Drop(frame.flowHeld);

    if (!optical)
    {
        Drop(frame.gameCopy);
        return;
    }

    if (mv == nullptr || list == nullptr || !EnsureCopy(frame.gameCopy, mv))
        return;

    // The copy rests in COPY_DEST, and is tagged in it (frame generation hands it back in the state it was given)
    Transition(list, mv, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(frame.gameCopy.Get(), mv);
    Transition(list, mv, D3D12_RESOURCE_STATE_COPY_SOURCE, state);

    frame.haveGame = true;
    frame.gameWidth = width;
    frame.gameHeight = height;
    frame.scaleX = scaleX;
    frame.scaleY = scaleY;
    frame.jitterX = jitterX;
    frame.jitterY = jitterY;
    frame.resets = FgFallbackResets(lowRes, jittered);
}

void AtPresent(IFGFeature_Dx12* fg, IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, uint64_t epoch)
{
    if (fg == nullptr || swapChain == nullptr || queue == nullptr)
        return;

    std::lock_guard lock(g_mutex);

    if (!g_applied.load())
        return;

    const int index = fg->GetIndexWillBeDispatched();
    auto& frame = g_frames[index];

    if (!frame.optical)
        return;

    // This present's picture, compared by identity with the one the flow was measured on: the game's picture as the
    // frame source takes it (native/Dx12FrameSource.cpp; HDR output's proxy back buffer while it converts the chain)
    Present now { queue, nullptr, epoch, 0, 0 };
    {
        ComPtr<IDXGISwapChain3> chain3;
        ComPtr<ID3D12Resource> picture;

        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&chain3))) &&
            SUCCEEDED(SdrToHdr::GameBuffer(swapChain, chain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&picture))))
        {
            const auto desc = picture->GetDesc();
            now.picture = picture.Get();
            now.width = (uint32_t) desc.Width;
            now.height = desc.Height;
        }
    }

    const bool flowForThis = g_offer.motion && SamePresent(g_offer.present, now);
    const bool fits = g_offer.present.width == frame.pictureWidth && g_offer.present.height == frame.pictureHeight;
    const bool latest = index == fg->GetIndex();
    const auto tag = FgTagAtPresent(frame.optical, latest, flowForThis, fits);

    ID3D12GraphicsCommandList* list = fg->GetUICommandList(index);

    if (list == nullptr)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LOG_WARN("Frame generation optical motion: no list of frame generation's to tag the motion on");
        }
        return;
    }

    Dx12Resource resource {};
    resource.type = FG_ResourceType::Velocity;
    resource.cmdList = list;
    resource.frameIndex = index;
    resource.validity = FG_ResourceValidity::ValidNow;

    if (tag == FgTag::Flow)
    {
        frame.flowHeld = g_offer.motion;
        resource.resource = frame.flowHeld.Get();
        resource.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        resource.width = g_offer.present.width;
        resource.height = g_offer.present.height;
        ++g_counts.flow;
    }
    else
    {
        // The game's motion with the game's scale for this one frame (and no interpolation across it when they are
        // not the flow's kind): never an older flow, nor no motion, which XeFG takes as a missing input and pauses on
        ++g_counts.fallback;

        if (g_fallbackLogs < 8 || g_counts.fallback % 300 == 0)
        {
            ++g_fallbackLogs;
            LOG_INFO("Frame generation optical motion: no flow for this frame ({}), the game's motion vectors for it "
                     "(flow {} / game {} so far)",
                     !g_offer.motion ? "none offered"
                     : !latest       ? "frame generation runs behind the latest frame"
                     : !flowForThis  ? "the flow is another present's"
                                     : "the picture is another size than frame generation's",
                     g_counts.flow, g_counts.fallback);
            LOG_INFO("Frame generation optical motion: flow {}x{} (present {}), frame generation picture {}x{}, "
                     "this present {}x{} (present {}), game motion {}x{}{}",
                     g_offer.present.width, g_offer.present.height, g_offer.present.epoch, frame.pictureWidth,
                     frame.pictureHeight, now.width, now.height, epoch, frame.gameWidth, frame.gameHeight,
                     frame.resets ? ", reset (the game's motion is render-size)" : "");
        }

        if (!frame.haveGame)
            return;

        resource.resource = frame.gameCopy.Get();
        resource.state = D3D12_RESOURCE_STATE_COPY_DEST;
        resource.width = frame.gameWidth;
        resource.height = frame.gameHeight;
        fg->SetMVScale(frame.scaleX, frame.scaleY, index);
        fg->SetJitter(frame.jitterX, frame.jitterY, index);

        if (frame.resets)
            fg->SetReset(1, index);
    }

    fg->SetResource(&resource);
}

bool ContextTakesFlow() { return g_applied.load(); }

Counts GetCounts()
{
    std::lock_guard lock(g_mutex);
    return g_counts;
}

} // namespace FGOpticalMotion
