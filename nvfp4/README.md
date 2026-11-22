# NVFP4 (4-bit) NR model path

C++ sources of the CUDA-on-D3D12 kernel layers, ported from wilsjo2's
`OptiScaler-DLSSNR-PreSR-Multipass` fork. They sit between the NR runtime and NvAPI's CUDA entry points,
outermost over `DlssNrNative`'s hybrid, and run the 4-bit and FP8 models that the runtime's own shaders do not.

The model files are **not in this repository** (856 MB, four of them 140 MB, above GitHub's per-file push limit).
Deploy them from the `OptiScaler-NR-v0.9.5` package: copy its `OptiScaler\nvfp4` folder next to the DLL, so the
game has `Win64r\OptiScaler\nvfp4\`. A package from this fork's releases has the same folder.

```
OptiScaler/nvfp4/
  ffn_v2.cubin                       feed-forward pair kernels
  net4_set12.txt, net4_set13.txt     set descriptions for the network layers
  modelA/   ffn_v2.txt, ffn_v2_set12.bin, ffn_v2_set13.bin, ffn_v2_trained.bin, ffn08_k4peek.cubin
  swin4/    swin4.txt, 40 fused-block .cubin, swin4_set12/13.bin, swin4_set12/13_fp8.bin, swin4_trained.*
  split4/   split4.txt, .cubin, split4_set12/13.bin, split4_trained.bin
  vit4/     vit4.txt, .cubin, vit4_set12/13.bin, vit4_trained.bin
  prepost4/ prepost4.bin, prepost4.txt, prepost4_pre.cubin, prepost4_post.cubin
  fp8/      fp8_set1..4.bin, fp8_set1..4.txt
```

Everything the models need comes from this folder, nothing from `nvngx_dlssnr.dll`. A pass that asks for a
model whose files are missing is not applied and says why in the menu and `OptiScaler.log`; there is no
fallback to the runtime's own model. `modelB/` (Model B's 4-bit weights) is in no package; choosing
`Preset B (small model)` runs the runtime's small model, not a 4-bit one.

In the menu the model is the **`Preset` combo of each pass card** (`Pass 1`, `Pass 2`, `Pass 3`):

| Value | Menu name | Files it needs |
| --- | --- | --- |
| 0, 1, 2 | Automatic, Preset A, Preset B | runtime's own models |
| 4 | Original - NVFP4 (4-bit kernels) | `modelA/`, `swin4/`, `split4/`, `vit4/`, `prepost4/` |
| 16 | NVFP4 2xPass-in-one | same, plus `*_set13` files |
| 15 | NVFP4 3xPass-in-one | same, plus `*_set12` files |
| 17, 18, 19, 20 | FP8 2x/3xPass-in-one, FP8 2x/3xPass-in-one v2 | `fp8/`, `swin4/*_fp8.bin` |

The layers load kernels, weights and buffers per device on first use, keep one model ready, and set aside
what the passes stop asking for; turning Neural Rendering off releases all of it once the GPU has finished
with the recorded command lists. The Vulkan path refuses the 4-bit models and reports the reason.

Tested on RTX 5070 Ti, DirectX 12, NR runtime 310.9.16 (266 MB `nvngx_dlssnr.dll`).
