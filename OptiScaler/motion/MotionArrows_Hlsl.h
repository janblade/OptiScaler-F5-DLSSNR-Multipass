#pragma once

// Original drawing code. The visual reference is Lumenite Kernel's colored arrows
// on a dotted grid; no Lumenite source is included or required by this renderer.
#define F5LOW_MOTION_ARROWS_HLSL R"HLSL(
float ArrowSegmentDistance(float2 p, float2 a, float2 b)
{
    float2 ab = b - a;
    return length(p - a - ab * saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-6)));
}

float3 ArrowDirectionColor(float2 forward, float speed, float speedRange)
{
    float hue = frac(atan2(forward.y, forward.x) / 6.2831853 + 1.0);
    float3 rgb = saturate(abs(frac(hue + float3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0) - 1.0);
    return lerp(0.12.xxx, rgb, 0.90) * (0.70 + 0.30 * sqrt(saturate(speed / max(speedRange, 1.0))));
}

float4 DrawMotionArrow(float2 pixel, float2 root, float2 backwardPixels, float spacing,
                       float gain, float thickness, float aa, float minimumMotion, float speedRange)
{
    if (any(abs(pixel-root)>spacing*0.90+thickness+aa)) return 0.0;
    float magnitude = length(backwardPixels);
    if (!(magnitude >= minimumMotion) || !(magnitude < 1e6)) return 0.0;
    float2 forward = -backwardPixels / max(magnitude, 1e-6);
    float2 side = float2(-forward.y, forward.x);
    float arrowLength = min(magnitude * gain, spacing * 0.90);
    float headLength = min(7.0, arrowLength * 0.42);
    float2 tip = root + forward * arrowLength;
    float2 headBase = tip - forward * headLength;
    float distance = min(ArrowSegmentDistance(pixel, root, tip),
        min(ArrowSegmentDistance(pixel, tip, headBase + side * headLength * 0.55),
            ArrowSegmentDistance(pixel, tip, headBase - side * headLength * 0.55)));
    float coverage = 1.0 - smoothstep(max(0.0, thickness - aa), thickness + aa, distance);
    if (coverage <= 0.0) return 0.0;
    return float4(ArrowDirectionColor(forward, magnitude, speedRange), coverage);
}
)HLSL"
