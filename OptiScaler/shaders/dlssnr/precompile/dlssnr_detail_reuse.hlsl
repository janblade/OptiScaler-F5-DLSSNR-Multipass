// Reuse detail between frames (dlssnr/DlssNrDetailReuse.h): the model runs on one frame, and on the next the saved detail
// (answer minus input, in the proxy domain at the model's working size) is moved with the motion vectors and added to
// the new input instead of running the model. The method is reverse reprojection caching (Nehab et al. 2007) with the
// history rejection TAA uses:
//   - two motion candidates per pixel: its own, and the closest surface's in the 3x3 neighbourhood so edges can move
//     with the foreground (Karis 2014); each is tested with the depth of its own surface, the better one is kept;
//   - a depth test against the saved 2x2 footprint the reprojected sample is read from;
//   - a motion test: the current vectors at the place it came from must be close to the pixel's own, or something
//     else moves there now (relative to the vector's length, so fast motion does not reject);
//   - a colour test against the current 3x3 neighbourhood's variance box (Salvi 2016), measured in standard
//     deviations, so noise does not reject and a real change does;
//   - Catmull-Rom for the moved detail, clamped to its 2x2 footprint (sharp, no ringing).
// Trust fades smoothly rather than switching, and full frames can be pulled toward the moved detail (Steady), so
// frames next to each other differ less.
//
// Separate from dlssnr.hlsl so the shared shader and every ordinary pass stay as they are. It reuses that shader's
// root signature and descriptor table (t0..t4, u0..u1, b0, s0 = linear clamp) through
// DlssNr_Dx12::DispatchDetailReuse. On Vulkan (VK_MODE) it has its own pass and descriptor set layout,
// DlssNrDetailReuse_Vk, one binding per register: 0 b0, 1-5 t0-t4, 6-7 u0-u1, 8 s0. The storage images carry no
// fixed format (what they write varies by mode), which needs shaderStorageImageWriteWithoutFormat; they are never read.
// Modes and bindings: DlssNr_DetailReuseConstants.h.
//
// Replace modes (replaceCurve != 0): there the answer IS the picture, decoded through the curve's inverse (Neutwo or the
// hybrid, dlssnr_replace_curve.hlsli), whose slope runs away near white. A saved change -- a difference of proxy values,
// as in every mode -- moved a pixel off at an edge, so that a mid grey's change landed on a highlight, decoded to over a
// hundred times the pixel, and the resolve's guard showed it at 2x: a white flash on every reused frame. So where a
// moved change lands in these modes it is landed two ways, as that difference and as the ratio of decoded light it made
// where it came from, and the one that changes the pixel less is kept: measured in stops of whichever channel moves
// most (the ratio is per channel, so a colour must not shift while its brightest channel holds still), at the trust the
// change is applied at. The difference is right except on a highlight
// (the pole); the ratio is right except where a dark pixel's change lands on something bright (grey patches on a lit
// floor); on the surface a change came from the two agree. Single forms were tried in game first (2026-09-30), and each
// broke one way: the ratio (grey patches), a difference of open-ended sRGB light (white patches beside reflections), and
// a logarithm above white (the same, milder). The Composed modes and passthrough frames land the difference, bit for bit.
//
// Raw vectors times MvScale are pixels of the motion texture's own subrect (render resolution with low-resolution
// vectors), so they are divided by its size into a uv displacement of the image; the saved vectors are kept in that
// form too, so a render-size change between two frames cannot misread them.
// Vectors point from the current pixel to where it was: previous = current + mv. Depth is kept far-is-zero on both
// conventions (reversed Z as is, standard Z as 1 - d): with reversed Z, |a - b| / max(a, b) is the relative
// difference of linear view depth, and sky compares equal to sky.

#ifdef VK_MODE
[[vk::binding(0, 0)]]
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    uint mode;
    uint workWidth;
    uint workHeight;
    uint motionWidth;
    uint motionHeight;
    uint motionBaseX;
    uint motionBaseY;
    uint depthWidth;
    uint depthHeight;
    uint depthBaseX;
    uint depthBaseY;
    uint depthInverted;
    float mvScaleX;
    float mvScaleY;
    float depthTolerance;
    float clipGamma;
    float clipFalloff;
    float sigmaFloor;
    float steady;
    uint debugView;
    float fillStrength;
    float fillRadius;
    uint replaceCurve; // DlssNrReplaceCurve: 0 none, 1 Neutwo, 2 hybrid (see the header)
    float motionReject;
    float steadyDeadZone;
};

#ifdef VK_MODE
#define DR_BINDING(n) [[vk::binding(n, 0)]]
// Without it dxc declares float4 storage images as rgba32f, which must then match the image's own format.
#define DR_ANY_FORMAT [[vk::image_format("unknown")]]
#else
#define DR_BINDING(n)
#define DR_ANY_FORMAT
#endif
DR_BINDING(1) Texture2D<float4> t0 : register(t0);
DR_BINDING(2) Texture2D<float4> t1 : register(t1);
DR_BINDING(3) Texture2D<float4> t2 : register(t2);
DR_BINDING(4) Texture2D<float4> t3 : register(t3);
DR_BINDING(5) Texture2D<float4> t4 : register(t4);
DR_BINDING(6) DR_ANY_FORMAT RWTexture2D<float4> u0 : register(u0);
DR_BINDING(7) DR_ANY_FORMAT RWTexture2D<float4> u1 : register(u1);
DR_BINDING(8) SamplerState gLinear : register(s0);

bool Finite3(float3 v) { return all(isfinite(v)); }

#include "dlssnr_replace_curve.hlsli"

// A proxy value -> the light the resolve decodes it to, and back (Replace modes only).
float3 ReplaceDecode(float3 proxy)
{
    const float3 y = SrgbToLinear(proxy);
    return replaceCurve == 1u ? NeutwoDecode(y) : HybridDecode(y);
}

float3 ReplaceEncode(float3 light) { return LinearToSrgb(replaceCurve == 1u ? NeutwoEncode(light) : HybridEncode(light)); }

// How far a candidate moves the pixel whose decoded light is `from`: in stops of whichever channel moves most.
float StopsFrom(float3 from, float3 candidate)
{
    const float3 stops = abs(log2((ReplaceDecode(candidate) + kRatioFloor) / (from + kRatioFloor)));
    return max(stops.r, max(stops.g, stops.b));
}

// Of two ways to land a change on `input` (decoded: `light`), as the values they take it to, the one that moves it less
// when added at `strength` (the trust it is applied at). Returned as the difference to add to input.
float3 SmallerLanding(float3 input, float3 light, float3 asRatio, float3 asDifference, float strength)
{
    const float3 byRatio = asRatio - input, byDifference = asDifference - input;
    const bool ratioMovesLess =
        StopsFrom(light, input + byRatio * strength) < StopsFrom(light, input + byDifference * strength);
    return ratioMovesLess ? byRatio : byDifference;
}

// Replace modes: a change `detail` saved where the picture was `source`, landing on `input` at `strength` (see the
// header). Returns the difference to add to input: the moved one, or the ratio it made at its source applied here.
float3 LandReplaceDetail(float3 input, float3 detail, float3 source, float strength)
{
    if (all(detail == 0.0))
        return 0.0;
    const float3 light = ReplaceDecode(input);
    const float3 ratio = (ReplaceDecode(source + detail) + kRatioFloor) / (ReplaceDecode(source) + kRatioFloor);
    const float3 asRatio = ReplaceEncode(max((light + kRatioFloor) * ratio - kRatioFloor, 0.0));
    return SmallerLanding(input, light, asRatio, input + detail, strength);
}

float2 WorkSize() { return float2(workWidth, workHeight); }
float2 WorkUv(uint2 p) { return (float2(p) + 0.5) / WorkSize(); }

// depthWidth/depthHeight are 0 on a frame the game (or the native producer) supplied no depth for -- the same
// signal DlssNr_Dx12::Dispatch's guide resolution already produces for "no depth" (DlssNr_Guides.h). t2/t4 are
// not valid depth data in that case (DispatchDetailReuse stands t0 in for any null guide so every descriptor
// slot stays bound), so nothing below may read them for trust.
bool HasDepth() { return depthWidth != 0 && depthHeight != 0; }

float FarIsZero(float d) { return depthInverted != 0 ? d : 1.0 - d; }

int2 GuideTexel(float2 uv, uint2 size)
{
    return int2(min(uint2(saturate(uv) * float2(size)), size - 1));
}

float GuideDepth(Texture2D<float4> depth, int2 texel)
{
    return FarIsZero(depth.Load(int3(uint2(texel) + uint2(depthBaseX, depthBaseY), 0)).r);
}

float3 ToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

// Catmull-Rom from five bilinear taps (the four corners of the 4x4 are dropped).
float4 SampleCatmullRom(Texture2D<float4> tex, float2 uv, float2 size)
{
    const float2 position = uv * size;
    const float2 centre = floor(position - 0.5) + 0.5;
    const float2 f = position - centre;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 texel = 1.0 / size;
    const float2 uv0 = (centre - 1.0) * texel;
    const float2 uv3 = (centre + 2.0) * texel;
    const float2 uv12 = (centre + w2 / w12) * texel;

    float4 result = tex.SampleLevel(gLinear, float2(uv12.x, uv0.y), 0) * (w12.x * w0.y);
    result += tex.SampleLevel(gLinear, float2(uv0.x, uv12.y), 0) * (w0.x * w12.y);
    result += tex.SampleLevel(gLinear, uv12, 0) * (w12.x * w12.y);
    result += tex.SampleLevel(gLinear, float2(uv3.x, uv12.y), 0) * (w3.x * w12.y);
    result += tex.SampleLevel(gLinear, float2(uv12.x, uv3.y), 0) * (w12.x * w3.y);
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return result / max(weight, 1e-4);
}

float2 MotionSize() { return float2(motionWidth, motionHeight); }

// Raw vector at image position uv, read from the motion subrect.
float2 RawMotion(float2 uv)
{
    return t3.Load(int3(uint2(motionBaseX, motionBaseY) + uint2(GuideTexel(uv, uint2(motionWidth, motionHeight))),
                        0)).rg;
}

// Raw vector -> displacement in image uv.
float2 UvDisplacement(float2 raw)
{
    return raw * float2(mvScaleX, mvScaleY) / MotionSize();
}

// The 3x3 colour neighbourhood of the current input, in YCoCg: mean and standard deviation (floored).
void ColourBox(uint2 p, out float3 mean, out float3 sigma)
{
    const int2 last = int2(WorkSize()) - 1;
    float3 sum = 0.0, sumSquares = 0.0;
    [unroll] for (int cy = -1; cy <= 1; ++cy)
    {
        [unroll] for (int cx = -1; cx <= 1; ++cx)
        {
            const float3 c = ToYCoCg(t0.Load(int3(clamp(int2(p) + int2(cx, cy), int2(0, 0), last), 0)).rgb);
            sum += c;
            sumSquares += c * c;
        }
    }
    mean = sum / 9.0;
    sigma = max(sqrt(max(sumSquares / 9.0 - mean * mean, 0.0)), max(sigmaFloor, 1e-5));
}

// How far the vector `raw` a pixel moved by is trusted, given the current vectors where it points (0..1). A pixel and
// the place it came from should move alike; where they do not, something else moves there now (an edge crossing it, a
// surface that was behind) and what was saved there is not this pixel's. The difference is measured in working-size
// pixels against the vector's length plus one, so fast motion and its small errors do not reject, and fades out
// between half and the whole of motionReject times that.
float MotionTrust(float2 previousUv, float2 raw)
{
    if (motionReject <= 0.0)
        return 1.0;
    const float2 here = UvDisplacement(raw) * WorkSize();
    const float2 there = UvDisplacement(RawMotion(previousUv)) * WorkSize();
    const float difference = length(here - there);
    if (!isfinite(difference))
        return 1.0; // a vector that is not a number says nothing about the source
    return 1.0 - smoothstep(0.5, 1.0, difference / (motionReject * (length(here) + 1.0)));
}

// The saved detail moved from previousUv (rgb) and how far it is trusted (a, 0..1), for a pixel whose surface has
// far-is-zero depth depthNow and which moved by the raw vector `raw`. Reads t1 saved detail, t2 saved colour + depth.
// byMotion is the share of trust the vectors' disagreement took away (only the debug view reads it).
float4 MovedFrom(float2 previousUv, float2 raw, float depthNow, float3 mean, float3 sigma, out float3 source,
                 out float byMotion)
{
    source = 0.0;
    byMotion = 0.0;
    const float2 work = WorkSize();
    const bool hasDepth = HasDepth();
    // depthNow is only meaningful (and only finite by construction) when hasDepth; a depth-less frame passes a
    // placeholder that must not reject the sample.
    if (!all(isfinite(previousUv)) || any(previousUv < 0.0) || any(previousUv > 1.0) || (hasDepth && !isfinite(depthNow)))
        return 0.0;

    // The 2x2 footprint the moved sample is built from: detail range, validity, depth range. Only texels with a
    // bilinear weight count: on a texel centre that is one texel, and its zero-weight neighbour (maybe another
    // surface) must not widen the depth range.
    const float2 position = previousUv * work - 0.5;
    const int2 base = int2(floor(position));
    const float2 f = position - float2(base);
    float3 detailLo = 65504.0, detailHi = -65504.0;
    float valid = 1.0, depthLo = 65504.0, depthHi = -65504.0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        const int2 corner = int2(i & 1, i >> 1);
        const float2 axis = float2(corner.x != 0 ? f.x : 1.0 - f.x, corner.y != 0 ? f.y : 1.0 - f.y);
        if (axis.x * axis.y < 1e-3)
            continue;
        const int2 texel = clamp(base + corner, int2(0, 0), int2(work) - 1);
        const float4 d = t1.Load(int3(texel, 0));
        const float savedDepth = t2.Load(int3(texel, 0)).a;
        const bool ok = Finite3(d.rgb) && isfinite(d.a) && isfinite(savedDepth);
        valid = ok ? min(valid, d.a) : 0.0;
        detailLo = ok ? min(detailLo, d.rgb) : detailLo;
        detailHi = ok ? max(detailHi, d.rgb) : detailHi;
        depthLo = ok ? min(depthLo, savedDepth) : depthLo;
        depthHi = ok ? max(depthHi, savedDepth) : depthHi;
    }
    if (valid < 0.999)
        return 0.0;

    const float3 moved = clamp(SampleCatmullRom(t1, previousUv, work).rgb, detailLo, detailHi);

    // Depth: how far this surface lies outside the saved range, relative to the nearer of the two. With no depth
    // this frame (hasDepth false) there is nothing to compare depthNow against, so trust falls back to colour alone.
    float depthTrust = 1.0;
    if (hasDepth)
    {
        const float outside = max(max(depthLo - depthNow, depthNow - depthHi), 0.0);
        const float relativeDepth = outside / max(max(depthNow, depthHi), 1e-6);
        depthTrust = 1.0 - smoothstep(depthTolerance, 2.0 * depthTolerance, relativeDepth);
    }

    // Colour: the saved input colour against the current input's variance box, in standard deviations.
    source = t2.SampleLevel(gLinear, previousUv, 0).rgb;
    const float3 saved = ToYCoCg(source);
    const float3 excess = max(abs(saved - mean) - clipGamma * sigma, 0.0) / sigma;
    const float colourTrust = 1.0 - saturate(max(excess.x, max(excess.y, excess.z)) / max(clipFalloff, 1e-3));

    if (!Finite3(moved) || !Finite3(saved))
        return 0.0;
    const float motionTrust = MotionTrust(previousUv, raw);
    byMotion = 1.0 - motionTrust;
    return float4(moved, saturate(depthTrust * colourTrust * motionTrust));
}

// The moved detail for work pixel p (rgb), how far it is trusted (a, 0..1), and the saved picture it came from. Reads
// t0 input, t1 saved detail, t2 saved colour + depth, t3 motion guide, t4 depth guide.
float4 MovedDetailFrom(uint2 p, out float3 source, out float byMotion)
{
    source = 0.0; // matches MovedFrom's own convention; keeps every exit, including the one below, defined
    byMotion = 0.0;
    const float2 uv = WorkUv(p);

    float3 mean, sigma;
    ColourBox(p, mean, sigma);

    // No depth this frame: t4 is not a real depth guide (DispatchDetailReuse stood a null In4 in with t0, the
    // colour), so there is no "closest surface in the 3x3" to find -- one motion candidate, trusted by colour alone
    // (MovedFrom's hasDepth check ignores the placeholder depth below).
    if (!HasDepth())
    {
        const float2 raw = RawMotion(uv);
        return MovedFrom(uv + UvDisplacement(raw), raw, 0.0, mean, sigma, source, byMotion);
    }

    const uint2 depthSize = uint2(depthWidth, depthHeight);
    const int2 centre = GuideTexel(uv, depthSize);

    // The pixel's own surface, and the closest surface in its 3x3.
    const float ownDepth = GuideDepth(t4, centre);
    float closestDepth = isfinite(ownDepth) ? ownDepth : -1.0;
    int2 closest = centre;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 texel = clamp(centre + int2(dx, dy), int2(0, 0), int2(depthSize) - 1);
            const float z = GuideDepth(t4, texel);
            if (isfinite(z) && z > closestDepth)
            {
                closestDepth = z;
                closest = texel;
            }
        }
    }

    // Own motion with own depth. The closest surface's motion is read at this pixel's position shifted by the
    // depth-texel offset (motion may be finer than depth), and tested with that surface's depth.
    float3 ownSource, nearSource;
    float ownByMotion, nearByMotion;
    const float2 ownRaw = RawMotion(uv);
    const float4 own = MovedFrom(uv + UvDisplacement(ownRaw), ownRaw, ownDepth, mean, sigma, ownSource, ownByMotion);
    source = ownSource;
    byMotion = ownByMotion;
    if (all(closest == centre))
        return own;
    const float2 closestUv = uv + float2(closest - centre) / float2(depthSize);
    const float2 nearRaw = RawMotion(closestUv);
    const float4 near =
        MovedFrom(uv + UvDisplacement(nearRaw), nearRaw, closestDepth, mean, sigma, nearSource, nearByMotion);
    // On a tie the closer surface's motion wins, so edges move with the foreground.
    if (near.a >= own.a)
    {
        source = nearSource;
        byMotion = nearByMotion;
        return near;
    }
    return own;
}

// MovedDetailFrom, landed on this pixel's input in Replace modes (see the header), so everything that adds it --
// Reproject, Steady, Fill -- adds a change that is safe here.
float4 MovedDetail(uint2 p, out float byMotion)
{
    float3 source;
    float4 moved = MovedDetailFrom(p, source, byMotion);
    if (replaceCurve != 0u && moved.a > 0.0)
    {
        const float3 landed = LandReplaceDetail(t0.Load(int3(p, 0)).rgb, moved.rgb, source, moved.a);
        moved.rgb = Finite3(landed) ? landed : 0.0;
    }
    return moved;
}

void Capture(uint2 p)
{
    const float4 input = t0.Load(int3(p, 0));
    const float3 answer = t1.Load(int3(p, 0)).rgb;
    const float3 detail = answer - input.rgb;
    const bool valid = Finite3(detail) && Finite3(input.rgb) && all(abs(detail) <= 65504.0);
    // No depth this frame: t2 stands in with the colour, so save far (0) instead. A next frame with depth then finds
    // its surface nearer than the saved range and distrusts the moved detail for that one frame.
    const float depth = HasDepth() ? GuideDepth(t2, GuideTexel(WorkUv(p), uint2(depthWidth, depthHeight))) : 0.0;
    u0[p] = float4(valid ? detail : 0.0, valid ? 1.0 : 0.0);
    u1[p] = float4(Finite3(input.rgb) ? input.rgb : 0.0, isfinite(depth) ? depth : 0.0);
}

void Reproject(uint2 p)
{
    const float4 input = t0.Load(int3(p, 0));
    float byMotion;
    const float4 moved = MovedDetail(p, byMotion);
    float4 result = input;
    const float3 reconstructed = input.rgb + moved.rgb * moved.a;
    if (Finite3(reconstructed) && all(abs(reconstructed) <= 65504.0))
        result.rgb = reconstructed;

    if (debugView != 0)
        result.rgb = lerp(lerp(float3(1.0, 0.0, 1.0), float3(1.0, 1.0, 0.0), byMotion), result.rgb, moved.a);

    u0[p] = result;
}

void Steady(uint2 p)
{
    const float4 input = t0.Load(int3(p, 0));
    const float4 answer = t1.Load(int3(p, 0));
    const float4 estimate = t2.Load(int3(p, 0));
    const float amount = saturate(steady) * saturate(estimate.a);
    const float3 fresh = answer.rgb - input.rgb;
    // A difference below the dead zone (about one 8-bit step) is no difference: the full frame takes the moved detail there
    // (as far as it is trusted), so rounding does not show the two frames apart. Fades out between one and two dead zones,
    // and only where steadiness is on.
    float3 pull = amount;
    if (steadyDeadZone > 0.0 && amount > 0.0)
    {
        const float3 apart = smoothstep(steadyDeadZone, 2.0 * steadyDeadZone, abs(estimate.rgb - fresh));
        pull = lerp(saturate(estimate.a), amount, apart);
    }
    const float3 steadied = input.rgb + lerp(fresh, estimate.rgb, pull);
    u0[p] = float4(Finite3(steadied) ? steadied : answer.rgb, answer.a);
}

// Where the moved detail was dropped (another surface was there, or it came from off-screen), take the trusted moved
// detail of nearby pixels on the same surface instead of none. Uncovered background gets the background's detail from
// around it, not the body that uncovered it. The average is smooth -- mostly NR's tone rather than its fine detail --
// which is what stops the dropped areas flashing to the un-NR'd image with several passes. 16 taps on a golden-angle
// spiral out to fillRadius, each weighted by its trust and by depth agreement (within three times the reuse tolerance).
void Fill(uint2 p)
{
    const float4 input = t0.Load(int3(p, 0));
    const float4 estimate = t1.Load(int3(p, 0));
    const bool estimateOk = Finite3(estimate.rgb) && isfinite(estimate.a);
    const float trust = estimateOk ? saturate(estimate.a) : 0.0;
    const float3 own = estimateOk ? estimate.rgb * trust : 0.0;

    float filled = 0.0;
    float3 fillDetail = 0.0;
    if (trust < 0.999 && fillStrength > 0.0)
    {
        const uint2 depthSize = uint2(depthWidth, depthHeight);
        const float2 work = WorkSize();
        const float depthHere = GuideDepth(t4, GuideTexel(WorkUv(p), depthSize));
        const float tolerance = 3.0 * depthTolerance;
        float3 sum = 0.0, sumLogRatio = 0.0;
        float weightSum = 0.0;
        [unroll] for (int k = 0; k < 16; ++k)
        {
            const float radius = lerp(2.0, max(fillRadius, 2.0), sqrt((k + 0.5) / 16.0));
            const float angle = 2.39996323 * k;
            const int2 texel = clamp(int2(p) + int2(round(radius * float2(cos(angle), sin(angle)))), int2(0, 0),
                                     int2(work) - 1);
            const float4 tap = t1.Load(int3(texel, 0));
            if (!Finite3(tap.rgb) || !isfinite(tap.a) || tap.a <= 0.0)
                continue;
            const float depthThere = GuideDepth(t4, GuideTexel((float2(texel) + 0.5) / work, depthSize));
            const float relative = abs(depthHere - depthThere) / max(max(depthHere, depthThere), 1e-6);
            const float weight = saturate(tap.a) * (1.0 - smoothstep(tolerance, 2.0 * tolerance, relative));
            sum += tap.rgb * weight;
            weightSum += weight;
            if (replaceCurve != 0u)
            {
                // The ratio the tap's (already landed) change makes on the tap's own input.
                const float3 tapInput = t0.Load(int3(texel, 0)).rgb;
                const float3 tapRatio =
                    (ReplaceDecode(tapInput + tap.rgb) + kRatioFloor) / (ReplaceDecode(tapInput) + kRatioFloor);
                sumLogRatio += log(tapRatio) * weight;
            }
        }
        if (isfinite(depthHere) && weightSum > 1e-3)
        {
            fillDetail = sum / weightSum;
            // Full once two trusted neighbours agree; fewer fade it in.
            filled = (1.0 - trust) * saturate(fillStrength) * saturate(weightSum / 2.0);
            if (replaceCurve != 0u)
            {
                // Landed like a moved change, on the pixel as its own change left it and at the strength it is filled
                // at: the average difference, or the average ratio, whichever changes the pixel less.
                const float3 base = input.rgb + own;
                const float3 light = ReplaceDecode(base);
                const float3 asRatio = ReplaceEncode(
                    max((light + kRatioFloor) * exp(sumLogRatio / weightSum) - kRatioFloor, 0.0));
                const float3 landed = SmallerLanding(base, light, asRatio, base + fillDetail, filled);
                fillDetail = Finite3(landed) ? landed : 0.0;
            }
        }
    }

    float3 result = input.rgb + own + fillDetail * filled;
    if (!Finite3(result))
        result = input.rgb;

    // Debug: cyan where detail was filled, magenta where it is still missing.
    if (debugView != 0)
        result = result * trust + float3(0.0, 1.0, 1.0) * filled + float3(1.0, 0.0, 1.0) * max(1.0 - trust - filled, 0.0);

    u0[p] = float4(result, input.a);
}

// Work size: this frame's vectors as a uv displacement, so the next frame reads them right after a render-size change.
// How much of this frame arrived with no detail to move (DlssNrDetailReuse::MotionGuard, dlssnr/DlssNrDetailReuse.h):
// the moved detail's trust summed over a tile of the frame, one 8x8 thread group per tile. Every second pixel in each
// direction is measured, which is ample for a share. Written as sums rather than means, so the host can add tiles of
// unequal size exactly. Reads the estimate the same way Fill does: anything not finite is no detail at all.
static const uint kCoverageTiles = 32; // = kDlssNrDetailReuseCoverageTiles (DlssNr_DetailReuseConstants.h)

groupshared float2 gCoverage[64];

void Coverage(uint2 tile, uint2 lanePos)
{
    // The tiles partition the frame exactly: no pixel is measured twice and none is missed. A frame narrower than the
    // grid leaves the last tiles empty (x0 == x1) rather than letting neighbours overlap, which would weight some
    // columns double and bias the share.
    const uint lane = lanePos.y * 8u + lanePos.x;
    const uint x0 = min((tile.x * workWidth) / kCoverageTiles, workWidth);
    const uint x1 = min(((tile.x + 1u) * workWidth) / kCoverageTiles, workWidth);
    const uint y0 = min((tile.y * workHeight) / kCoverageTiles, workHeight);
    const uint y1 = min(((tile.y + 1u) * workHeight) / kCoverageTiles, workHeight);

    float2 sum = 0.0; // dropped (1 - trust), pixels measured
    [loop] for (uint y = y0 + lanePos.y * 2u; y < y1; y += 16u)
    {
        [loop] for (uint x = x0 + lanePos.x * 2u; x < x1; x += 16u)
        {
            const float4 estimate = t1.Load(int3(x, y, 0));
            const float trust = Finite3(estimate.rgb) && isfinite(estimate.a) ? saturate(estimate.a) : 0.0;
            sum += float2(1.0 - trust, 1.0);
        }
    }

    gCoverage[lane] = sum;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
            gCoverage[lane] += gCoverage[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
        u0[tile] = float4(gCoverage[0].x, gCoverage[0].y, 0.0, 0.0);
}

void SaveMotion(uint2 p)
{
    const float2 displacement = UvDisplacement(RawMotion(WorkUv(p)));
    u0[p] = float4(all(isfinite(displacement)) ? displacement : 0.0, 0.0, 0.0);
}

// Motion size: this frame's raw vector plus the saved displacement at the moved position, back in this frame's raw
// units. Vectors are point-sampled: blending two surfaces' vectors across an edge gives one that belongs to neither.
void Compose(uint2 q)
{
    const float2 raw = t3.Load(int3(uint2(motionBaseX, motionBaseY) + q, 0)).rg;
    const float2 scale = float2(mvScaleX, mvScaleY);
    float2 composed = all(isfinite(raw)) ? raw : 0.0;

    const float2 delta = UvDisplacement(composed);
    const float2 previousUv = (float2(q) + 0.5) / MotionSize() + delta;
    // The saved displacement exists only on screen; where the point came from off-screen, the model gets the one frame
    // of motion we know instead.
    if (all(abs(scale) > 1e-20) && all(isfinite(delta)) && all(previousUv >= 0.0) && all(previousUv <= 1.0))
    {
        const float2 prior = t4.Load(int3(GuideTexel(previousUv, uint2(workWidth, workHeight)), 0)).rg;
        const float2 sum = delta + prior;
        // A sanity bound, not an on-screen test: two frames of motion across more than the whole screen is not motion.
        if (all(isfinite(sum)) && all(abs(sum) <= 1.0))
            composed = sum * MotionSize() / scale;
    }

    u0[q] = float4(composed, 0.0, 0.0);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    const uint2 p = id.xy;
    // Before the bounds test below: a whole group reduces one tile together, so every one of its threads has to reach
    // the barriers in Coverage.
    if (mode == 7)
    {
        if (groupId.x < kCoverageTiles && groupId.y < kCoverageTiles)
            Coverage(groupId.xy, groupThreadId.xy);
        return;
    }

    if (mode == 3)
    {
        if (p.x < motionWidth && p.y < motionHeight)
            Compose(p);
        return;
    }

    if (p.x >= workWidth || p.y >= workHeight)
        return;
    if (mode == 0)
        Capture(p);
    else if (mode == 2)
        SaveMotion(p);
    else if (mode == 1)
        Reproject(p);
    else if (mode == 4)
        Steady(p);
    else if (mode == 6)
        Fill(p);
    else
    {
        float byMotion;
        u0[p] = MovedDetail(p, byMotion);
    }
}
