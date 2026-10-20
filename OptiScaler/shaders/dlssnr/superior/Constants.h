#pragma once
#include <cstdint>

// v59 ABI; intentionally distinct from janblade's composition constants.
namespace DlssNr::Superior
{
enum DlssNrMode : uint32_t
{
    DlssNrMode_Encode = 0,         // the frame -> a tone-mapped proxy, plus an untouched copy
    DlssNrMode_Resolve = 1,        // proxy + the model's answer + the untouched copy -> the edited frame
    DlssNrMode_Downsample = 2,     // the proxy -> a smaller proxy, when the model works below full size
    DlssNrMode_EncodeResidual = 5, // NR-composed minus original; signed difference encoded around 0.5
    DlssNrMode_ApplyResidual = 6,  // decode private DLSS result and add to clean SR output
    DlssNrMode_UnitExposure = 7,   // constant exposure for the private DLSS feature
    DlssNrMode_ClampProxy = 8,     // restore the encoded RGB range between model passes
    DlssNrMode_EncodeProxyResidual = 9,
    DlssNrMode_Meter = 3,
    DlssNrMode_AutoExposure = 11,
    DlssNrMode_ResizePrivateGuides = 10,
    DlssNrMode_EncodeResizeField = 12,
    // Direct DLSS: copy NR50 into the private-DLSS carrier and pack P50 + an NR-suppression gate.
    DlssNrMode_EncodeDirectDetail = 13,
    // Generic bilinear source->target resize, used by Direct NR A/B upscaler tests.
    DlssNrMode_UpscaleBilinear = 14,
    // Direct NR detail recovery: write only the NR-retention gate to alpha; no NR50 carrier copy.
    DlssNrMode_EncodeDirectDetailMaskOnly = 15,
    // Detail-quality lab: one low-resolution pass between proxy downscale and NVIDIA NR.
    DlssNrMode_ExperimentPrefilter = 16,
    // Detail-quality lab: one low-resolution pass after NVIDIA NR, before enlargement/resolve.
    DlssNrMode_ExperimentNrPostfilter = 17,
    // P100-guided residual: collapse E50=NR50-P50 to an 1/8-size positive-lobe low-frequency field.
    DlssNrMode_GuidedResidualLow = 18,
    // Legacy temporal residual carrier encoder; retained for captures/backward experiments.
    DlssNrMode_EncodeRawResidualCarrier = 19,
    // Temporal carrier first-stage analysis: per ~32x32 source tile strict/robust safe-K statistics.
    DlssNrMode_TemporalCarrierLimits = 20,
    // Reduce temporal carrier statistics to one pixel.
    DlssNrMode_TemporalCarrierReduce = 21,
    // Encode nonlinear/linear residual or image-anchored carrier using the same-frame GPU K.
    DlssNrMode_TemporalCarrierEncode = 22,
    // Decode the private DLAA output back to a signed raw-domain E50 residual.
    DlssNrMode_TemporalCarrierDecode = 23,
    // Sum-reduce diagnostic counts emitted beside K-limit tiles.
    DlssNrMode_TemporalCarrierDiagnosticsReduce = 24,
    // Build only the compressed image anchor for the paired independent DLAA baseline.
    DlssNrMode_TemporalAnchorEncode = 25,
    // Exact raw texture copy used only to snapshot the pre-DLAA carrier for debug display.
    DlssNrMode_TemporalCarrierDebugCopy = 26,
    // Between NR passes: reconstruct P100 + guided cumulative residual into a native P100 scratch.
    DlssNrMode_InterPassGuidedP100 = 27,
    // Between NR passes: evaluate the same corrected P100 procedurally while downsampling to working-res.
    DlssNrMode_InterPassGuidedWorking = 28,
    // Final post-NR colour matching: sparse paired OKLab statistics from original and composed output.
    DlssNrMode_FinalColourStats = 29,
    // Reduce paired statistics, derive correction and temporally smooth it entirely on the GPU.
    DlssNrMode_FinalColourReduce = 30,
    // Apply global chromaticity plus Saturation/Vibrance recovery to the already composed NR output.
    DlssNrMode_FinalColourApply = 31,
    // v5 classical inter-pass: same downfilter as Mode 2, but clamp/sanitize
    // directly to the working input, avoiding the extra ClampProxy dispatch.
    DlssNrMode_DownsampleClampProxy = 32,
    // Opt-in approximation: one native guide and one 3x3 residual estimate per working pixel.
    DlssNrMode_InterPassFastGuided = 33,
    DlssNrMode_InterPassLow36 = 34,
    DlssNrMode_InterPassFastCross5 = 36,
    DlssNrMode_SuperiorCarrierEncode = 37, // fixed clamp(0.5 + N-P, -1, 2), no gain analysis
    DlssNrMode_SuperiorCarrierDecode = 38, // fixed DLAA(carrier)-0.5, no gain texture
    DlssNrMode_SuperiorCarrierMotion = 39  // fixed carrier plus exact private motion resize
};

struct alignas(256) DlssNrConstants
{
    uint32_t Mode;
    float WhitePoint;

    uint32_t Width;
    uint32_t Height;

    // How much of the model's edit lands, and how much of it is allowed to be colour rather than
    // luminance. Separating the two is what keeps saturated highlights from shifting hue.
    float TransferStrength;
    float ColourStrength;

    uint32_t DebugView;

    // A ceiling on how far a pixel may be brightened. The transfer is a ratio, and a ratio against a
    // near-black proxy pixel is unbounded without one.
    float MaxRatio;

    // Set when the game's buffer is already tone-mapped, in which case there is nothing to convert
    // and the transfer is the identity.
    uint32_t Passthrough;

    float MvScaleX;
    float MvScaleY;

    // Depth and motion vectors come from the upscaler's inputs and so may be at render resolution
    // while colour and output are at display resolution.
    uint32_t GuideWidth;
    uint32_t GuideHeight;

    // Showing the pass against itself. 0 off, 1 side by side, 2 a wipe.
    //
    // Both are drawn by the resolve rather than by a pass of their own, because the resolve is the
    // one place that already holds the frame as the upscaler produced it and the frame the model
    // edited. Comparing them anywhere else would mean keeping a second copy of one of them.
    uint32_t CompareMode;
    float CompareSplit;

    // How much of the frame side by side shows. 1 fits the whole thing at its right shape and
    // letterboxes what is left over; 2 fills the half and crops to the middle instead.
    float CompareZoom;

    // Which side the edited frame is on. Swapping matters because the eye is not even-handed about
    // left and right, so a difference can look like an improvement purely from where it sits.
    uint32_t CompareSwap;

    // Below-size reconstruction: 0 classic, 1/2 matched residual spatial/DLSS,
    // 3/4 relative lighting and chromaticity spatial/DLSS. Native size bypasses resizing.
    //
    // Classic composes the model's own low-resolution picture against the full-resolution frame, so
    // the two disagree by the blur the downsample introduced as well as by the edit -- and the
    // composition reads that disagreement as headroom the frame has and the model never saw. Matched
    // residual takes only the model's *difference* from low resolution and lays it on the frame's own
    // full-resolution proxy, so the two pictures being compared are at the same scale and the only
    // thing carried up from small is the edit itself.
    //
    // The idea and the cube-scaled residual are hhkbble's, from the multi-pass PR against this fork.
    uint32_t Transfer; // 7 = internal DX12 Direct-NR fused resolve; 8 = internal P100-guided residual resolve.

    // What the debug views are multiplied by on their way out.
    //
    // They have to be scaled into the frame's units or the game's tonemapper shows them wrong, but
    // scaling them by the live white point makes the instrument move with the thing being measured:
    // two captures at different exposures then differ by the exposure, whatever the edit did. This
    // is the user's own multiplier, independent of the image's brightness.
    float DebugScale;

    // The reversible-proxy mode. 0 soft knee + our composition (default), 1 unclipped Neutwo proxy +
    // our composition, 2 Neutwo proxy + pure-inverse replace (model's answer straight back, no
    // composition). Trailing field, mirroring the shader's cbuffer, so the layout stays a flat run of
    // 4-byte scalars that C++ and HLSL agree on.
    uint32_t ReversibleMode;

    // 0 = output the clean upscaler frame (the pass still runs, so Hold frame keeps a frozen frame
    // to A/B against), 1 = apply the model's edit. Trailing scalar, mirrored in the shader cbuffer.
    uint32_t ApplyModel;

    // Reuses the former Reserved slot, so the 256-byte shared constant-buffer layout and all
    // subsequent offsets remain unchanged. Percentage: 100 = uncapped darkening, 0 = no darkening.
    float MaxDarkening;
    float ResidualScale; // Scene pre-exposure used to encode/decode the private residual carrier.
    // Optional colour-based final-composition mask. Not the runtime's semantic mask.
    uint32_t SkinProtection;
    uint32_t ShowSkinMask;
    float SkinDetail;
    float SkinColour;
    float EnvironmentDetail;
    float EnvironmentColour;

    // ResidualAcrossRR v2 only (dlssnr_residual.hlsl). History blend rate for the MV-reprojected
    // accumulator, 0..1. Read only by that separate shader; dlssnr.hlsl never declares it. Appended
    // here rather than in a new struct so DispatchResidualPass reuses the existing constant upload --
    // it lands inside the 256-byte alignas padding, so sizeof(DlssNrConstants) is unchanged.
    float ResidualBlend;
    uint32_t ResidualHistoryValid;
    uint32_t ResidualMotionBaseX;
    uint32_t ResidualMotionBaseY;
    float ReplaceDetailStrength;
    float ModelWorkScale;
    float ResidualConfidenceSensitivity;
    uint32_t ExposureMode;
    float PreExposure;
    float ExposureTrim;
    float ExposureProtection;
    uint32_t ExposureAnchorCount;
    uint32_t ExposureSourceWidth;
    uint32_t ExposureSourceHeight;
    uint32_t ExposurePadding;
    float ExposureAnchors[16]; // Eight float2 pairs, packed as four float4s in HLSL.

    // Direct-DLSS detail recovery. These occupy the former alignas padding; sizeof stays 256.
    uint32_t DirectDetailMode; // 0 off, 1 full lost detail, 2 NR-gated
    float DirectDetailMaskStrength; // 0..1
    uint32_t DirectResolveUpscaler; // selector used by internal Transfer=7 fused Direct NR resolve
    uint32_t DirectResolveFlags;    // reserved, keeps the shared constant buffer at 256 bytes
};
static_assert(sizeof(DlssNrConstants) == 256);
}
