# Superior Scaling: validation and timing data

The Superior Scaling port (PR #230) was submitted with about 5.5 MB of raw GPU timing samples (CSV) and their
statistics (JSON), all measured by the contributor on an **NVIDIA GeForce RTX 4090, driver 616.56**, with synthetic
resident 3840x2160 images. The raw files are not kept in this repository. They remain in the history of the PR's head
branch (`codex/superior-scaling-port`, head `06b8c53d`, directory `docs/validation/`). The conclusions are below and in
[Superior-Scaling-port.md](../Superior-Scaling-port.md) and [Superior-four-optimizations.md](../Superior-four-optimizations.md);
every dataset can be regenerated locally (it is written to the ignored `artifacts/validation/`).

All timings use GPU timestamps around the measured dispatches only, in alternating ABBA/BAAB blocks with separate
warm-up (640 samples per variant unless stated). They **exclude NR and private DLSS inference** and say nothing about
total in-game performance. Compare paired block medians, not medians of unrelated runs.

| Former dataset | What it measured | Conclusion | Regenerate with |
| --- | --- | --- | --- |
| `superior-port-v59-timing` | Low64 + guided resolve, original v59 vs port, P50/P59/P65, SDR and HDR 0/2/4 | No resolved difference: paired medians -0.000128 to +0.000384 ms against a per-variant stddev of about 0.04-0.06 ms | `tools/test_superior_port.ps1 -GpuTiming -ReferenceShaderDirectory <v59 precompile>` |
| `superior-spatial-timing` | Compress screen edges: guide pack and final resolve before/after the spatial correction, 65% / 100% | No cost at 65%; about +0.07 ms at 100% from the real Low64 work the corrected path adds (a correctness cost) | `tools/test_superior_port.ps1 -GpuSpatialTiming` |
| `superior-native-temporal-timing` | 100% + Balanced: raw-answer vs signed-residual guided final stage | Small, noisy differences (SDR +0.013 ms, HDR 0 -0.022 ms); no total gain established | `tools/test_superior_port.ps1 -GpuNativeTemporalTiming` |
| `superior-packed-route-timing` | Previous unpack-before-DLSS route vs packed route (all post-NR stages except inference) | Packed route about 0.30 ms faster at 65% and 0.60 ms faster at 100% (SDR and HDR 0) | `tools/test_superior_port.ps1 -GpuPackedTiming` |
| `superior-host-opt-timing` | Packed host resolve before/after skipping unused initial samples | 0.006-0.022 ms faster for packed SDR/HDR 0; ordinary controls unchanged (identical bytecode) | `tools/test_superior_port.ps1 -GpuHostOptTiming` |
| `superior-four-*` (final, specialization, lookup after specialization, LDS, fusion, SDR recheck, results) | The four-candidate study | Keep normal-render specialisation and the exact warp lookup (final combination 0.028-0.111 ms faster in all 25 configurations); reject the shared 3x3 LDS tile (0.022-0.107 ms slower) and fused packing (no stable gain, not bit-exact) | `tools/test_superior_port.ps1 -GpuCandidates` / `-GpuCandidateTiming` |

The timing modes that compare against an earlier state of the port (`-GpuSpatialTiming`, `-GpuNativeTemporalTiming`,
`-GpuHostOptTiming`, `-GpuHost`, `-GpuCandidates`, `-GpuCandidateTiming`) extract their baselines from the contributor's
commits (`24a2d7bf`, `67318a5d`, `0fa378f4`) with `git show`. They need those commits in the local clone (fetch the PR
head: `git fetch origin pull/230/head`); without them the script skips the mode with a message. `-GpuQuality` and
`-GpuTiming` need `-ReferenceShaderDirectory` pointing at the original v59 `precompile` directory (mattjaas v59,
`914d8d1f`), which is not in this repository.

## Bit-exactness checks rerun for this repository

After the review fixes the host resolve blobs were rebuilt with the repository's dxc (`tools/compile_nr_interpass.ps1`;
CI checks they rebuild byte for byte) and compared with the previous blobs on an RTX 5070 Ti: `superior_host_quality`
(2304 cases, 5,974,272 words), `packed_host_quality` (3072 cases, 666,267,648 components) and all 15
`host_normal_*` / `host_cached_*` specialisations (480 cases) matched bit for bit on finite inputs.
