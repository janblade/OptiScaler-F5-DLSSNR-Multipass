#pragma once

// [FrameGen] MotionSource=optical on a native D3D12 game that calls its own upscaler (FGInput=upscaler, XeFG or F5Low
// output): frame generation takes Optical F5Low's flow, measured on the finished picture by the native producer
// (native/NativeDriver.cpp), instead of the game's motion vectors. The rules: dlssnr/DlssNr_OpticalMotion.h (Fg*).
//
// - At the game's upscaler call (inputs/FG/Upscaler_Inputs_Dx12.cpp UpscaleStart): the frame's motion is described as
//   the flow's (scale 1, no jitter, display-size, unjittered), the game's motion vectors are not tagged but copied for
//   a fallback. XeFG takes the kind of motion (display-size or render-size, jittered) only when its swap chain is made
//   (ApplyCreationFlags: volatile [XeFG] JitteredMV / HighResMV, the ini is untouched), so a switch on with the XeFG
//   output needs a restart; F5Low takes the size from each tag and switches at once. No context rebuild either way.
// - At the present (hooks/FG_Hooks.cpp FGPresent), after the producer ran for it and before frame generation's
//   Present: the flow is tagged on frame generation's UI list, which runs on the game's queue before its dispatch.
//   A present without a flow for the frame being generated tags the kept copy of the game's motion vectors with the
//   game's scale for that one frame (and a reset when they are not the flow's kind).

#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>

#include <dlssnr/DlssNr_OpticalMotion.h>

class IFGFeature_Dx12;
class IFeature_Dx12;

namespace FGOpticalMotion
{

// The keys on the config and the running frame generation, now
DlssNr::OpticalMotion::FgKeys Keys();
bool Wanted();
// What the menu says when it cannot run (nullptr: it can)
const char* RefusalText(DlssNr::OpticalMotion::FgRefusal refusal);

// The producer (present thread): this present's flow (RGBA16F at the picture's size, .xy in pixels, current to
// previous, resting in NON_PIXEL_SHADER_RESOURCE) measured on `picture` in a list already submitted to `queue`.
void Publish(ID3D12CommandQueue* queue, ID3D12Resource* picture, ID3D12Resource* motion, uint32_t width,
             uint32_t height, uint64_t epoch, bool reset);
void Clear();

// XeFG's swap chain creation (XeFG and F5Low outputs), before it reads [XeFG] JitteredMV / HighResMV: with the
// setting on, they are the flow's (volatile); off, back to what they were.
void ApplyCreationFlags();
// The swap chain in use was made for the flow (ApplyCreationFlags set the flags last): XeFG keeps reading display-size
// motion until it is made again, so switching back to the game's motion needs a restart there
bool MadeForFlow();

// The game's upscaler call, before EvaluateState: whether this frame's motion for frame generation is the flow.
bool AtUpscaleCall(IFGFeature_Dx12* fg, IFeature_Dx12* feature, bool severalViews);
// ... after StartNewFrame: what the frame at `index` is, and the game's motion vectors copied on its list for a
// fallback (`mv` may be null: no fallback then)
void KeepGameMotion(IFGFeature_Dx12* fg, int index, bool optical, ID3D12GraphicsCommandList* list, ID3D12Resource* mv,
                    D3D12_RESOURCE_STATES state, uint32_t width, uint32_t height, float scaleX, float scaleY,
                    float jitterX, float jitterY, bool lowRes, bool jittered, uint32_t pictureWidth,
                    uint32_t pictureHeight);

// FGPresent, with frame generation active, before fg->Present()
void AtPresent(IFGFeature_Dx12* fg, IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, uint64_t epoch);

// The calls describe their motion as the flow's (XeFG's activation check takes display-size motion then, like native
// AA)
bool ContextTakesFlow();

// For the menu: presents since the setting went on that took the flow / fell back to the game's motion
struct Counts
{
    uint64_t flow = 0, fallback = 0;
};
Counts GetCounts();

} // namespace FGOpticalMotion
