// Automatic exposure's eye adaptation (DlssNr_ExposureAdapt.h): eases the exposure the encode and resolve read toward
// this evaluation's meter reading, instead of jumping to it.
//
// Separate from dlssnr.hlsl so the shared shader and its SPIR-V twin stay as they are. It reuses that shader's root
// signature and descriptor table on D3D12 (DlssNr_Dx12::DispatchExposureAdapt) and its descriptor set layout on Vulkan
// (DlssNr_Vk::DispatchExposureAdapt: the reading in gSource's binding, the eased value in gTarget's). One thread: the
// exposure is one texel.
//
// The law is DlssNrExposureAdapt::Ease, mirrored line for line; change them together. The host works out the two
// blends (1 - exp(-dt / tau), one per direction) and whether to snap, so the frame clock stays on the CPU.

#ifdef VK_MODE
[[vk::binding(0, 0)]]
#endif
cbuffer Params : register(b0)
{
    // Overlays the first fields of DlssNrConstants: Mode, WhitePoint, Width, Height.
    uint mode;
    float blend;       // the share of the way to the reading when the scene got brighter (reading below the eased value)
    uint snap;         // 1: take the reading at once (a cut, or the eased value is stale)
    float blendDarker; // ... and when it got darker
};

#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> gReading : register(t0); // the meter's reading this evaluation (DlssNrMode_AutoExposure); 0 = none
#ifdef VK_MODE
[[vk::binding(5, 0)]] [[vk::image_format("r32f")]]
#endif
RWTexture2D<float> gEased  : register(u0); // the eased exposure: the last evaluation's on entry, this one's on exit

bool Valid(float e) { return isfinite(e) && e > 1e-8 && e < 1e8; }

[numthreads(1, 1, 1)]
void CSMain()
{
    const float reading = gReading.Load(int3(0, 0, 0)).r;
    const float previous = gEased[uint2(0, 0)];
    float eased;

    if (!Valid(reading))
    {
        eased = snap == 0u && Valid(previous) ? previous : 0.0;
    }
    else if (snap != 0u || !Valid(previous))
    {
        eased = reading;
    }
    else
    {
        const float b = reading < previous ? blend : blendDarker;
        const float t = isfinite(b) ? saturate(b) : 0.0;
        eased = exp2(log2(previous) + (log2(reading) - log2(previous)) * t);
    }

    gEased[uint2(0, 0)] = eased;
}
