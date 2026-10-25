# PR #249 formatting check

The old workflow ran clang-format against the entire OptiScaler tree. Its PR #249 log reported
302 files: 267 had no changes in this PR, and many others were generated shader-byte arrays or
existing formatting outside the edited lines. Reformatting all those files would obscure the
rendering patch and conflict with deterministic shader-header generation.

The workflow now pins clang-format **20.1.8** and checks **added/modified C/C++ lines** against the
PR merge base. Push builds use the previous revision; initial pushes use the first parent (or an
empty tree for a root commit). Checkout fetches history so merge-base resolution works for fork
PRs. Existing vendor exclusions remain. New source files are checked in full; deleted-only hunks
have no new code to format. C++ `.inl` includes are checked along with ordinary source and headers.

Generated `*_Shader.h` byte arrays are excluded from style formatting. They remain covered by
`tests/test_nr_superior_scaling.py` (embedded bytes equal `.cso`) and the Superior workflow's
byte-for-byte rebuild check. Neither their generator nor generated headers/CSOs are changed.
HLSL is not reformatted by this C/C++ gate.

Local check/fix:

```powershell
python tools/check_changed_format.py --base upstream/main
python tools/check_changed_format.py --base upstream/main --fix
python -m unittest discover -s tests -p test_changed_format.py
```

The `--formatter` argument can point to a local clang-format 20.1.8 binary. Other versions fail
explicitly, rather than silently using different formatting rules. The existing PR's changed
C/C++ lines already pass 20.1.8, so this fix introduces **no rendering source edits**. Historical
global style debt is retained; this is an incremental gate, not a claim that the whole tree is
formatted. The gate fails if an edited line requires formatting and prints the requested diff.

Validation includes policy tests on a temporary real Git repository, the CPU/ABI suite and a
rebuild of all 51 shader blobs/headers. All rendering sources, precompiled shaders and generated
headers remain identical to `4f1e792e`; the existing DLL is still the current rendering build.
