// Rescue only an existing timer on a smoothly moving, locally planar surface.
// Comparing two NDC frames directly can mistake forward motion over the road for a new closer object.
// Never seeds or renews a window, and never reads previous edits/changes NGX history.
bool ContinueGhostSurface(float4 current, float4 old, float2 chosen, float tolerance, float precision, float gate)
{
    float2 endPosition = chosen + current.zw;
    float2 midPosition = chosen + current.zw * .5;
    int2 endPixel = int2(round(endPosition)), midPixel = int2(round(midPosition));
    if (any(endPixel < 0) || any(midPixel < 0) || endPixel.x >= int(fullWidth) || endPixel.y >= int(fullHeight) ||
        midPixel.x >= int(fullWidth) || midPixel.y >= int(fullHeight))
        return false;
    float endZ = Depth.Load(int3(endPixel, 0)), midZ = Depth.Load(int3(midPixel, 0));
    if (!isfinite(endZ) || !isfinite(midZ) || endZ < 0 || endZ > 1 || midZ < 0 || midZ > 1)
        return false;
    endZ = GhostNearDepth(endZ, inverted);
    midZ = GhostNearDepth(midZ, inverted);
    if (endZ <= 0 || midZ <= 0)
        return false;
    // The old same-screen-depth shortcut is valid for a constant camera orientation.
    // Defer its failure: camera pitch changes the NDC depth plane even for the SAME road.
    bool oldDepthMismatch = abs(endZ - old.x) > max(endZ, old.x) * tolerance + precision;
    // An intervening silhouette/depth step is not a continuous road. Near depth is affine on a plane.
    if (abs(midZ - (current.x + endZ) * .5) > max(current.x, endZ) * min(tolerance * .25, .0125) + precision)
        return false;
    float2 endMv = Motion.Load(int3(endPixel, 0)), midMv = Motion.Load(int3(midPixel, 0));
    if (!all(isfinite(endMv)) || !all(isfinite(midMv)) || any(abs(endMv) >= 65504) || any(abs(midMv) >= 65504))
        return false;
    float2 curvature = midMv - (current.zw + endMv) * .5;
    if (dot(curvature, curvature) > gate * gate * 4)
        return false;
    [branch] if (oldDepthMismatch)
    {
        // This is a small camera-orientation correction, not permission to inherit across a new depth layer.
        if (current.x > old.x * 1.25 + precision || old.x > current.x * 1.25 + precision)
            return false;
        float matchTolerance = min(tolerance * .5, .025);
        // A second correspondence checks that BOTH frames describe the same local depth plane,
        // allowing its projected slope/intercept to change under camera pitch. This only carries
        // an existing timer; it cannot seed or renew disocclusion.
#ifdef DLSSNR_GHOST_STEP
        const float stride = DLSSNR_GHOST_STEP;
#else
        const float stride = 2;
#endif
        bool matched = false;
        [loop] for (uint witness = 0; witness < 2; ++witness)
        {
            if (matched)
                break;
            int2 at = witness == 0 ? endPixel : midPixel;
            float2 mv = witness == 0 ? endMv : midMv;
            float target = witness == 0 ? endZ : midZ;
            int2 q = int2(round((float2(at) + .5 + mv) / stride - .5));
            if (any(q < 0) || q.x >= int(width) || q.y >= int(height))
                continue;
            float4 oldGuide = PreviousGhostGuide(q);
            if (!all(isfinite(oldGuide)) || oldGuide.x <= 0)
                continue;
            float predicted = current.x * (oldGuide.x / old.x);
            if (isfinite(predicted) && abs(target - predicted) <= max(target, predicted) * matchTolerance + precision)
                matched = true;
        }
        [branch] if (!matched)
        {
            // Near a silhouette the forward witnesses may reproject INTO the old occluder.
            // Look half a motion step the other way, but only on the same current depth/flow plane.
            int2 reversePixel = int2(round(chosen - current.zw * .5));
            if (any(reversePixel < 0) || reversePixel.x >= int(fullWidth) || reversePixel.y >= int(fullHeight))
                return false;
            float reverseZ = GhostNearDepth(Depth.Load(int3(reversePixel, 0)), inverted);
            float planeZ = current.x * 1.5 - endZ * .5;
            if (reverseZ <= 0 || planeZ <= 0 ||
                abs(reverseZ - planeZ) > max(reverseZ, planeZ) * min(tolerance * .5, .025) + precision)
                return false;
            float2 reverseMv = Motion.Load(int3(reversePixel, 0));
            if (!all(isfinite(reverseMv)) || any(abs(reverseMv) >= 65504))
                return false;
            float2 curve = reverseMv - (current.zw * 1.5 - endMv * .5);
            if (dot(curve, curve) > gate * gate * 4)
                return false;
            int2 q = int2(round((float2(reversePixel) + .5 + reverseMv) / stride - .5));
            if (any(q < 0) || q.x >= int(width) || q.y >= int(height))
                return false;
            float4 oldGuide = PreviousGhostGuide(q);
            if (!all(isfinite(oldGuide)) || oldGuide.x <= 0)
                return false;
            float predicted = current.x * (oldGuide.x / old.x);
            if (!isfinite(predicted) ||
                abs(reverseZ - predicted) > max(reverseZ, predicted) * matchTolerance + precision)
                return false;
        }
    }
    return true;
}
