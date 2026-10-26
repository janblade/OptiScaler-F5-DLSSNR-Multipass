# Private residual DLSS IsHDR test

Base: PR #249 head `34a1504e`, on separate personal test branch `codex/superior-private-dlss-ishdr-test`. Do not advance the PR branch until the author completes the in-game comparison. This branch excludes the later Finished Picture/native-FG experiments.

Below the CNN/Transformer private DLSS choice, the unchecked-by-default checkbox reads:

> Disable DLSS IsHDR flag (may reduce severe ghosting at the cost of less stable NR)

`[DlssNr] SuperiorScalingDisableIsHDR=false` preserves the original private DLSS creation flags exactly. Setting it to true removes only `NVSDK_NGX_DLSS_Feature_Flags_IsHDR`; `AutoExposure`, MVLowRes and the applicable DepthInverted flag remain unchanged. This applies to CNN A/E, Transformer K and the existing runtime-default preset fallback. It does not change the game's DLSS/HDR settings, carrier encoding/range, NR input or composition.

The requested flag is part of the private temporal generation identity. Changing it retires the old instance through the existing GPU lifetime tracker and creates a fresh instance/history using the existing submission gate. An inactive, non-temporal Superior path does not recreate resources merely because this checkbox changes. The preference is saved/read in the INI and NR profiles.

Validation: the exact production flag expressions from the PR base and this branch were compiled against the repository's NGX SDK. With the checkbox off the masks match the base for normal and reversed depth; with it on, their XOR is exactly IsHDR and AutoExposure remains set. Existing Superior identity/ABI and CPU policy/guide tests pass. No HLSL or precompiled shader was changed. Image quality and any GPU-time difference with IsHDR disabled require the author's in-game testing; the checkbox describes a hypothesis rather than a measured improvement.
