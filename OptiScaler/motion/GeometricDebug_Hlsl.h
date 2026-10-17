#pragma once

// Shared verbatim with the ReShade diagnostic shader; display only, no flow writes.
#define F5LOW_GEOMETRIC_DEBUG_HLSL R"HLSL(
float GeometryRejectionMask(float source)
{
    // Invalid or local-only means geometry was not used. A blend accepts geometry.
    return source < 1.5 ? 1.0 : 0.0;
}
// Display only; derive reasons from existing fusion/model diagnostics. No readback.
float4 GeometryRejectionReason(float4 info, float4 fit)
{
    if (fit.z < 0.0) return float4(0.2, 0.2, 0.2, 1); // camera geometry disabled
    if (info.x >= 1.5) return float4(0, 0, 0, 1); // geometry accepted
    if (!(fit.z > 0.5)) return float4(1, 0, 1, 1); // global model invalid
    if (!(info.z < 1e5)) return float4(1, 0.35, 0, 1); // invalid projection/outside image
    if (!(info.z < 0.20)) return float4(1, 1, 0, 1); // photometric rejection
    if (info.x >= 0.5) return float4(0, 0.8, 1, 1); // local flow retained by fusion policy
    return float4(1, 0, 0, 1); // no selected hypothesis
}
float GeometryGrid(float2 uv, float2 screenSize)
{
    float2 cells = float2(16.0, 9.0);
    float2 phase = frac(uv * cells);
    float2 distancePx = min(phase, 1.0 - phase) * screenSize / cells;
    return 1.0 - smoothstep(0.7, 2.2, min(distancePx.x, distancePx.y));
}
float4 GeometryModelDebug(float2 uv, float2 screenSize, float4 h0, float4 h1, float4 fit)
{
    if (!(fit.z > 0.5)) return float4(0.0, 0.0, 0.0, 1.0);
    float2 p = uv * 2.0 - 1.0;
    float denominator = dot(h1.zw, p) + 1.0;
    if (!(denominator > 0.1 && denominator < 10.0)) return float4(0.0, 0.0, 0.0, 1.0);
    float2 previous = float2(dot(h0.xy, p) + h0.z, h0.w * p.x + h1.x * p.y + h1.y);
    previous = previous / denominator * 0.5 + 0.5;
    if (!all(abs(previous) < 1e6)) return float4(0.0, 0.0, 0.0, 1.0);
    float reference = GeometryGrid(uv, screenSize) * 0.14;
    float model = all(previous >= 0.0) && all(previous <= 1.0) ? GeometryGrid(previous, screenSize) : 0.0;
    return float4(max(reference.xxx, float3(0.08, 0.85, 1.0) * model), 1.0);
}
)HLSL"
