#pragma once
#include <cstdint>
#include "DlssNr_InterPassSchedule.h"

namespace DlssNr::InterPass
{
// Whether Superior's gain-based inter-pass correction may run: below 90% scale, not packed, more than one pass.
// The contributor's lab path catalogue (Classic/Fused/RGB16/20/Fast variants) is test-only: tests/nr_gpu/InterPassPaths.h.
inline bool Eligible(uint32_t mode, float scale, uint32_t passes, bool spatial)
{
    return mode != 0 && !spatial && scale > 0.0f && scale <= 0.90f && passes > 1u;
}
} // namespace DlssNr::InterPass
