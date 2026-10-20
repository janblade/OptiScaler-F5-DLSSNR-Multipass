# Superior Scaling port

Base: janblade `fix/menu-overlay-back-buffers`, `ae7d2c4092e5aee79bf9b1e6ea7a5bd38933a2d4` (release 0.1.33 + menu fix).
Reference: mattjaas v59, `914d8d1f50f2d52b11f9bacd7b6bf8fd7966d9e2`.

The port uses janblade's UI, feature creation, model selection, per-pass profiles, and API entry points.
Superior shaders keep the v59 256-byte ABI in a separate namespace; janblade's existing shader ABI and bytecode stay unchanged.
The descriptor/constant ring is shared, and the v59 specializations are lazy PSOs.

NR model A gain pairs (high/medium, low): Standard 0.800/0.661, Natural 0.750/0.787, Cinematic 0.950/0.784.
NR model B: Standard 0.950/0.861, Natural 0.925/0.838, Cinematic 0.955/0.861.
Each pass selects its own table before scale and Structure * Intensity adjustment. Private DLSS SR presets A/K do not select these tables.

## Validation status

Implementation in progress. Shader hashes are pinned in `superior-v59-shaders.json`.
Full DLL compilation, GPU parity, timing and integration checks have not yet completed.

## Integration audit checklist

- [ ] A/B mixed pass gains, schedule >0.15 only when worsening, low means/high products and failed-boundary recovery
- [ ] Original shader binaries and source, fallback PSO selection, 8x16 final low dispatch
- [ ] Area input, fixed carrier, fused carrier + motion, same-frame NR depth reuse
- [ ] Input encode/automatic exposure and final resolve parameters against v59
- [ ] Low/high resolution motion, offsets, padded allocations, native 100%, sub-native, odd dimensions
- [ ] Private DLSS A/K, HDR/AE flags, unit exposure, zero jitter, frame time, creation submission, reset and lifetime
- [ ] Model/pass switching, resize, hold, before-SR/native/bridge API limitations and failure cleanup
- [ ] UI hides Bilinear/SGSR1 only for Superior and retains model/resolution controls
- [ ] Legacy shader hashes and paths stay unchanged
- [ ] Build, CPU contracts, GPU parity/performance, publication and remote HEAD verification
