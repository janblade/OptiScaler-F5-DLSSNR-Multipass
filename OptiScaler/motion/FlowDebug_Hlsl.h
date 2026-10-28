#pragma once

// The Optical F5Low debug views: one compute pass that draws the menu's picture of the estimate. Display only: no
// estimator pass reads what it writes, and it runs only while a view is shown. The drawing code (arrows, the legend's
// bitmap font, the confidence colours, the depth normals) is TechvibeCoder07's from PR #232, drawn from scratch with no
// third-party shader code; the normals follow the approach of DisplayDepth.fx (crosire/reshade-shaders, CC0).
//
// Bound by OpticalFlowDx12::Visualise through the flow's root signature (eight SRVs, two UAVs, 32 root constants):
//   t0 the flow shown, t1 the picture (behind the arrows and the confidence colours), t2 this frame's luma, t3 the raw
//   depth as the game wrote it, t4 the luma before, t5 the trust mask's guide motion or its mask (see `view`), t6 the
//   camera model, t7 the still age; u0 the picture written.
// Its bytecode is in native/F5LowShaderBytecode.h (group "FlowDebug"); regenerate it with
// `nr_shader_bytecode_smoke.exe --write` after changing anything here.

namespace FlowDebugHlsl
{
inline constexpr const char* kSource = R"HLSL(
// Eight rows of four values, as OpticalFlowDx12::Visualise's ViewConstants lays them out (no value crosses a row).
cbuffer V : register(b0)
{
    uint2 size;          // the picture written
    uint2 frameSize;     // the game's picture, in its pixels (the flow's units)
    uint view;           // 0 arrows, 1 normals / depth, 2 final flow, 3 confidence, 4 confidence channels,
                         // 5 history rejection, 6 still age (native/FlowDebugViews.h names them)
    uint hasPicture;     // t1 is there: arrows and confidence draw over it
    uint hasGuide;       // t5 is there (guide motion for views 0 and 2, the trust mask for 3 to 5)
    uint hasCameraModel; // t6 holds this frame's camera fit
    uint hasAge;         // t7 holds this frame's still age
    uint stillFrames;    // the age that shows as white
    uint hasDepth;       // t3 is there
    uint reversed;       // the depth is reversed (1 is near)
    float maxSpeed;      // pixels per frame that show as full colour
    float arrowSpacing;  // the arrow grid, in the game's pixels
    float arrowGain;     // arrow length per pixel of motion
    float arrowMinimum;  // shorter motion draws no arrow
    float arrowThickness;
    float arrowOpacity;
    float gridOpacity;
    uint depthLinearize;   // the depth view only: linearized against depthFarPlane
    float2 depthScale;     // the depth alignment: the depth's coordinates are uv / depthScale + depthOffset
    float2 depthOffset;
    float depthMultiplier; // the depth view only: the raw depth is shown times this
    float depthFarPlane;
    uint2 padding;
    uint4 padding2;
};

SamplerState Linear : register(s0);
Texture2D<float4> FlowIn : register(t0);
Texture2D<float4> Picture : register(t1);
Texture2D<float>  LumaNow : register(t2);
Texture2D<float>  SceneDepth : register(t3);
Texture2D<float>  LumaBefore : register(t4);
Texture2D<float4> Guide : register(t5);
Texture2D<float4> CameraModel : register(t6);
Texture2D<uint>   StillAge : register(t7);
RWTexture2D<float4> OutPicture : register(u0);

float ArrowSegmentDistance(float2 p, float2 a, float2 b)
{
    float2 ab = b - a;
    return length(p - a - ab * saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-6)));
}

float3 ArrowDirectionColor(float2 forward, float speed, float speedRange)
{
    float hue = frac(atan2(forward.y, forward.x) / 6.2831853 + 1.0);
    float3 rgb = saturate(abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return lerp(0.12.xxx, rgb, 0.90) * (0.70 + 0.30 * sqrt(saturate(speed / max(speedRange, 1.0))));
}

float4 DrawMotionArrow(float2 pixel, float2 root, float2 backwardPixels, float aa)
{
    if (any(abs(pixel - root) > arrowSpacing * 0.90 + arrowThickness + aa))
        return 0.0;

    float magnitude = length(backwardPixels);

    if (!(magnitude >= arrowMinimum) || !(magnitude < 1e6))
        return 0.0;

    float2 forward = -backwardPixels / max(magnitude, 1e-6);
    float2 side = float2(-forward.y, forward.x);
    float arrowLength = min(magnitude * arrowGain, arrowSpacing * 0.90);
    float headLength = min(7.0, arrowLength * 0.42);
    float2 tip = root + forward * arrowLength;
    float2 headBase = tip - forward * headLength;
    float distance = min(ArrowSegmentDistance(pixel, root, tip),
                         min(ArrowSegmentDistance(pixel, tip, headBase + side * headLength * 0.55),
                             ArrowSegmentDistance(pixel, tip, headBase - side * headLength * 0.55)));
    float coverage = 1.0 - smoothstep(max(0.0, arrowThickness - aa), arrowThickness + aa, distance);

    if (coverage <= 0.0)
        return 0.0;

    return float4(ArrowDirectionColor(forward, magnitude, maxSpeed), coverage);
}

float3 VectorColour(float2 backward)
{
    float magnitude = length(backward);
    return magnitude > 0.0 ? ArrowDirectionColor(-backward / magnitude, magnitude, maxSpeed) : 0.0;
}

// How badly the motion fails to explain this pixel's 3x3 neighbourhood: 0 fits, 1 does not (contrast-normalised).
float PhotoError(float2 uv, float2 motion)
{
    float current[9], previous[9];
    float meanC = 0.0, meanP = 0.0;

    [unroll] for (int k = 0; k < 9; ++k)
    {
        float2 offset = float2(k % 3 - 1, k / 3 - 1) / float2(size);
        current[k] = LumaNow.SampleLevel(Linear, uv + offset, 0);
        previous[k] = LumaBefore.SampleLevel(Linear, uv + motion / float2(frameSize) + offset, 0);
        meanC += current[k];
        meanP += previous[k];
    }

    meanC /= 9.0;
    meanP /= 9.0;
    float error = 0.0, contrast = 0.0;

    [unroll] for (int j = 0; j < 9; ++j)
    {
        error += abs((current[j] - meanC) - (previous[j] - meanP));
        contrast += abs(current[j] - meanC);
    }

    return saturate(error / (contrast + 9.0 / 255.0));
}

// 0 is weak and 1 is strong: the texture's structure, or a camera fit that agrees with this motion.
float MotionSupport(float2 uv, float2 motion, float structure)
{
    if (hasCameraModel == 0)
        return saturate(structure);

    float4 x = CameraModel.Load(int3(0, 0, 0));
    float4 y = CameraModel.Load(int3(1, 0, 0));

    if (x.w <= 0.5)
        return saturate(structure);

    float3 p = float3(1.0, 2.0 * uv - 1.0);
    float2 difference = motion - float2(dot(x.xyz, p), dot(y.xyz, p));
    float geometric = saturate(y.w) / (1.0 + 4.0 * dot(difference, difference));
    return max(saturate(structure), geometric);
}

// The depth view reads the game's raw depth through the alignment, with linear sampling, as the alignment shows it.
float DepthAt(float2 uv, bool linearize)
{
    float d = SceneDepth.SampleLevel(Linear, uv / depthScale + depthOffset, 0) * depthMultiplier;

    if (linearize)
    {
        d = reversed != 0 ? 1.0 - d : d;
        d /= depthFarPlane - d * (depthFarPlane - 1.0);
    }

    return d;
}

float3 DepthNormal(float2 uv)
{
    float2 pixel = 1.0 / float2(frameSize);
    float2 north = uv - float2(0.0, pixel.y);
    float2 east = uv + float2(pixel.x, 0.0);
    float3 centre = float3(uv - 0.5, 1.0) * DepthAt(uv, true);
    float3 up = float3(north - 0.5, 1.0) * DepthAt(north, true);
    float3 right = float3(east - 0.5, 1.0) * DepthAt(east, true);
    float3 n = cross(centre - up, centre - right);
    // A flat or missing neighbourhood has no normal: black, as an 8-bit target would show the NaN.
    return dot(n, n) > 0.0 ? normalize(n) * 0.5 + 0.5 : 0.0;
}

float3 DepthDither(float2 uv, float d)
{
    float grid = frac(dot(uv, float2(frameSize) * float2(1.0 / 16.0, 10.0 / 36.0) + 0.25));
    const float shift = 0.25 / 255.0;
    float3 rgb = float3(shift, -shift, shift);
    return d.xxx + lerp(2.0 * rgb, -2.0 * rgb, grid);
}
)HLSL"
                                       R"HLSL(
// Two lines of the view's name and key in the top left corner, in a 5x7 bitmap font (a-z, space, ',' '/' '=').
float4 Legend(uint2 pixel, float4 image)
{
    if (pixel.x < 5u || pixel.y < 5u || pixel.y >= 25u || view > 6u)
        return image;

    uint2 at = pixel - uint2(7u, 7u);
    uint textRow = min(at.y / 9u, 1u), letter = at.x / 6u;
    uint ch = 26u, count = 0u;
    if(view==0u && textRow==0u){static const uint text[36]={0u,17u,17u,14u,22u,18u,27u,26u,7u,20u,4u,26u,3u,8u,17u,4u,2u,19u,8u,14u,13u,26u,28u,26u,11u,4u,13u,6u,19u,7u,26u,18u,15u,4u,4u,3u};count=36u;ch=text[uint(min(letter,count-1u))];}
    if(view==0u && textRow==1u){static const uint text[33]={21u,8u,18u,8u,1u,11u,4u,26u,12u,14u,19u,8u,14u,13u,26u,28u,26u,15u,8u,23u,4u,11u,18u,26u,15u,4u,17u,26u,5u,17u,0u,12u,4u};count=33u;ch=text[uint(min(letter,count-1u))];}
    if(view==1u && textRow==0u){static const uint text[24]={11u,4u,5u,19u,26u,13u,14u,17u,12u,0u,11u,18u,26u,17u,8u,6u,7u,19u,26u,3u,4u,15u,19u,7u};count=24u;ch=text[uint(min(letter,count-1u))];}
    if(view==1u && textRow==1u){static const uint text[38]={15u,17u,4u,21u,8u,4u,22u,26u,14u,13u,11u,24u,26u,17u,0u,22u,26u,12u,14u,3u,4u,11u,26u,3u,4u,15u,19u,7u,26u,20u,13u,2u,7u,0u,13u,6u,4u,3u};count=38u;ch=text[uint(min(letter,count-1u))];}
    if(view==2u && textRow==0u){static const uint text[25]={5u,8u,13u,0u,11u,26u,5u,11u,14u,22u,27u,26u,7u,20u,4u,26u,3u,8u,17u,4u,2u,19u,8u,14u,13u};count=25u;ch=text[uint(min(letter,count-1u))];}
    if(view==2u && textRow==1u){static const uint text[29]={1u,17u,8u,6u,7u,19u,13u,4u,18u,18u,26u,18u,15u,4u,4u,3u,26u,28u,26u,1u,11u,0u,2u,10u,26u,25u,4u,17u,14u};count=29u;ch=text[uint(min(letter,count-1u))];}
    if(view==3u && textRow==0u){static const uint text[38]={6u,17u,4u,4u,13u,26u,18u,19u,17u,14u,13u,6u,26u,28u,26u,14u,17u,0u,13u,6u,4u,26u,22u,4u,0u,10u,26u,14u,17u,26u,17u,4u,9u,4u,2u,19u,4u,3u};count=38u;ch=text[uint(min(letter,count-1u))];}
    if(view==3u && textRow==1u){static const uint text[39]={21u,4u,2u,19u,14u,17u,26u,18u,20u,15u,15u,14u,17u,19u,26u,28u,26u,5u,8u,19u,26u,28u,26u,0u,2u,2u,4u,15u,19u,4u,3u,26u,7u,8u,18u,19u,14u,17u,24u};count=39u;ch=text[uint(min(letter,count-1u))];}
    if(view==4u && textRow==0u){static const uint text[35]={17u,4u,3u,26u,19u,4u,23u,19u,20u,17u,4u,26u,28u,26u,6u,17u,4u,4u,13u,26u,15u,7u,14u,19u,14u,12u,4u,19u,17u,8u,2u,26u,5u,8u,19u};count=35u;ch=text[uint(min(letter,count-1u))];}
    if(view==4u && textRow==1u){static const uint text[36]={1u,11u,20u,4u,26u,19u,17u,20u,18u,19u,4u,3u,26u,7u,8u,18u,19u,14u,17u,24u,26u,28u,26u,1u,17u,8u,6u,7u,19u,26u,18u,19u,17u,14u,13u,6u};count=36u;ch=text[uint(min(letter,count-1u))];}
    if(view==5u && textRow==0u){static const uint text[35]={22u,7u,8u,19u,4u,26u,17u,4u,9u,4u,2u,19u,26u,7u,8u,18u,19u,14u,17u,24u,26u,28u,26u,1u,11u,0u,2u,10u,26u,0u,2u,2u,4u,15u,19u};count=35u;ch=text[uint(min(letter,count-1u))];}
    if(view==5u && textRow==1u){static const uint text[30]={6u,17u,4u,24u,26u,15u,0u,17u,19u,8u,0u,11u,26u,28u,26u,2u,20u,19u,18u,26u,17u,4u,9u,4u,2u,19u,26u,0u,11u,11u};count=30u;ch=text[uint(min(letter,count-1u))];}
    if(view==6u && textRow==0u){static const uint text[39]={18u,19u,8u,11u,11u,26u,0u,6u,4u,27u,26u,1u,11u,0u,2u,10u,26u,25u,4u,17u,14u,26u,28u,26u,22u,7u,8u,19u,4u,26u,19u,7u,17u,4u,18u,7u,14u,11u,3u};count=39u;ch=text[uint(min(letter,count-1u))];}
    if(view==6u && textRow==1u){static const uint text[29]={2u,14u,20u,13u,19u,18u,26u,20u,13u,2u,7u,0u,13u,6u,4u,3u,26u,8u,12u,0u,6u,4u,26u,5u,17u,0u,12u,4u,18u};count=29u;ch=text[uint(min(letter,count-1u))];}

    if (pixel.x >= 7u + count * 6u)
        return image;

    static const uint2 glyphs[29]={uint2(1663026734u,4u),uint2(3809986095u,3u),uint2(2182120510u,7u),uint2(3810051631u,3u),uint2(3256321087u,7u),uint2(1108837439u,0u),uint2(2736686142u,7u),uint2(1663026737u,4u),uint2(3359772831u,7u),uint2(2459181340u,1u),uint2(1381078321u,4u),uint2(3255862305u,7u),uint2(1662703473u,4u),uint2(1662834289u,4u),uint2(2736309806u,3u),uint2(1108854319u,0u),uint2(2472068654u,5u),uint2(1381484079u,4u),uint2(3775333438u,3u),uint2(138547359u,1u),uint2(2736309809u,3u),uint2(353945137u,1u),uint2(2874852913u,2u),uint2(1654794801u,4u),uint2(138553905u,1u),uint2(3257016863u,7u),uint2(0u,0u),uint2(138416256u,0u),uint2(1109533200u,0u)};
    float3 colour = float3(0.035, 0.045, 0.055);

    if (pixel.x >= 7u && pixel.y >= 7u && at.y < 16u && at.x % 6u < 5u && at.y % 9u < 7u)
    {
        uint bit = (at.y % 9u) * 5u + at.x % 6u;
        uint2 glyph = glyphs[ch];
        uint word = bit < 32u ? glyph.x : glyph.y;

        if (((word >> (bit % 32u)) & 1u) != 0u)
            colour = 0.95.xxx;
    }

    return float4(colour, 1.0);
}
)HLSL"
                                       R"HLSL(
[numthreads(8, 8, 1)]
void Visualise(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(size);
    float4 flow = view == 1u ? 0.0 : FlowIn.Load(int3(id.xy, 0));
    float4 output = float4(VectorColour(flow.xy), 1.0);

    if (view == 0u)
    {
        // Arrows on a dotted grid, in the game's pixels; each pixel takes the strongest of the nine nearest arrows.
        float2 pixel = (float2(id.xy) + 0.5) * float2(frameSize) / float2(size);
        float aa = max(0.8, max(float(frameSize.x) / size.x, float(frameSize.y) / size.y) * 0.65);
        float2 nearest = round(pixel / arrowSpacing);
        float grid = (1.0 - smoothstep(1.25, 1.25 + aa, length(pixel - nearest * arrowSpacing))) * gridOpacity;
        float4 best = 0.0;

        [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                float2 root = (nearest + float2(x, y)) * arrowSpacing;

                if (any(root < 0.0) || any(root >= float2(frameSize)))
                    continue;

                int2 at = clamp(int2(root * float2(size) / float2(frameSize)), 0, int2(size) - 1);
                float2 backward = hasGuide != 0 ? Guide.SampleLevel(Linear, root / float2(frameSize), 0).xy
                                                : FlowIn.Load(int3(at, 0)).xy;

                if (!all(abs(backward) < 1e6))
                    continue;

                float4 arrow = DrawMotionArrow(pixel, root, backward, aa);

                if (arrow.a > best.a)
                    best = arrow;
            }

        float alpha = best.a + grid * (1.0 - best.a);
        float3 premultiplied = best.rgb * best.a + float3(0.58, 0.60, 0.64) * grid * (1.0 - best.a);
        output = float4(alpha > 1e-6 ? premultiplied / alpha : 0.0, alpha * arrowOpacity);

        if (hasPicture != 0)
            output = float4(lerp(Picture.SampleLevel(Linear, uv, 0).rgb, output.rgb, output.a), 1.0);
    }
    else if (view == 1u)
    {
        // Left half the normals, right half the depth; a magenta checker where there is no depth.
        if (hasDepth == 0)
        {
            float checker = float(((id.x / 16u) + (id.y / 16u)) & 1u);
            output = float4(lerp(float3(0.06, 0.0, 0.06), float3(0.3, 0.0, 0.3), checker), 1.0);
        }
        else if (uv.x < 0.5)
            output = float4(DepthNormal(uv), 1.0);
        else
            output = float4(DepthDither(uv, DepthAt(uv, depthLinearize != 0)), 1.0);
    }
    else if (view == 2u)
    {
        // The motion the consumer gets: the guide motion (after uncover and the surface guide) when there is one.
        if (hasGuide != 0)
            output = float4(VectorColour(Guide.SampleLevel(Linear, uv, 0).xy), 1.0);
    }
    else if (view == 3u || view == 5u)
    {
        float rejected = hasGuide != 0 ? saturate(Guide.SampleLevel(Linear, uv, 0).x) : 1.0;

        if (view == 5u)
            output = float4(rejected.xxx, 1.0);
        else
        {
            // Evidence, not a probability: support limited by the photometric fit and by the history accepted.
            float support = min(max(saturate(flow.w), MotionSupport(uv, flow.xy, flow.z)),
                                min(1.0 - PhotoError(uv, flow.xy), 1.0 - rejected));
            float3 colour = support < 0.5 ? lerp(float3(1.0, 0.25, 0.02), float3(1.0, 0.90, 0.02), support * 2.0)
                                          : lerp(float3(1.0, 0.90, 0.02), float3(0.05, 0.95, 0.15), (support - 0.5) * 2.0);

            if (hasPicture != 0)
                colour = lerp(Picture.SampleLevel(Linear, uv, 0).rgb, colour, 0.90);

            output = float4(colour, 1.0);
        }
    }
    else if (view == 4u)
    {
        // Three separate signals: red the texture's structure, green the photometric fit, blue the history trusted.
        float history = hasGuide != 0 ? 1.0 - Guide.SampleLevel(Linear, uv, 0).x : 0.0;
        output = float4(saturate(flow.z), 1.0 - PhotoError(uv, flow.xy), saturate(history), 1.0);
    }
    else if (view == 6u)
    {
        float age = hasAge != 0 ? float(StillAge.Load(int3(id.xy, 0))) : 0.0;
        output = float4(saturate(age / max(float(stillFrames), 1.0)).xxx, 1.0);
    }

    OutPicture[id.xy] = Legend(id.xy, output);
}
)HLSL";
} // namespace FlowDebugHlsl
