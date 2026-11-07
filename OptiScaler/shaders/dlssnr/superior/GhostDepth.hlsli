#ifndef DLSSNR_GHOST_DEPTH
#define DLSSNR_GHOST_DEPTH
// Zero in history means missing data. Valid far-plane sky needs a distinct positive token.
// Its exponent survives compact FP32 depth packing (depth is never stored as binary16).
float GhostNearDepth(float z, uint reversed)
{
    return isfinite(z) && z >= 0 && z <= 1 ? max(reversed != 0 ? z : 1 - z, 1e-20) : -1;
}
#endif
