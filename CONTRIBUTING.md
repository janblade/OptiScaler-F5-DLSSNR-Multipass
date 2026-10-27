# Contributing to OptiScaler

To maintain build efficiency and project structure, please follow these requirements for all code contributions.

### Precompiled Header (PCH) Usage

We use Precompiled Headers to significantly reduce compile times. To prevent build degradation, please follow these rules:

* **Source Files (`.cpp`):** Every source file must include `"pch.h"` as the **very first** non-comment line.
* **Header Files (`.h`):** Never include `"pch.h"` inside a header file.
* **Utility Code:** Do not add general-purpose utilities, macros, or global declarations to `pch.h`. Use `SysUtils.h` or create a new functional header instead.
* **Adding Dependencies:** Only add large, stable, third-party or system headers (e.g., Windows/SDK headers) to `pch.h`.

### Why this matters

Correct PCH usage keeps our build times fast (more than 2x faster than a standard build). Including `pch.h` in other headers or using it as a global utility bucket breaks the compiler's ability to cache the precompiled state and forces unnecessary full rebuilds.

### Formatting

The CI formatting check looks only at the lines your PR changes, with clang-format 20.1.8 and the repo's `.clang-format`. Older code that isn't formatted yet never fails it. Third-party code and generated shader blobs (listed in `.clang-format-ignore`) are skipped.

Run the same check locally before you push:

```
python -m pip install -r tools/requirements-format.txt
python tools/format_check.py --base origin/<the branch your PR targets>
python tools/format_check.py --fix --base origin/<the branch your PR targets>
```

The first line is needed once. The check prints the exact changes it wants; `--fix` applies them to your working copy, uncommitted edits included. Use this clang-format, not the one bundled with Visual Studio, which is an older version and formats some lines differently.
