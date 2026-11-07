// Mixed-precision visibility state: depth keeps 16 mantissa bits (FP32 exponent),
// age is an exact 7-bit integer, MV is binary16 per component. 8 bytes/half-raster pixel.
uint2 PackGhost(float4 state)
{
    uint z = asuint(state.x) & 0xffffff80u;
    uint age = (uint) clamp(state.y, 0.0, 120.0);
    return uint2(z | age, f32tof16(state.z) | (f32tof16(state.w) << 16));
}
float4 UnpackGhost(uint2 bits)
{
    return float4(asfloat(bits.x & 0xffffff80u), float(bits.x & 127u), f16tof32(bits.y & 65535u),
                  f16tof32(bits.y >> 16));
}
