#pragma once

// Internal to the exposure scan: Auto's part of it (plan 2026-10-14-exposure-scan-all-apis, story 5). With the white
// point source on Auto the scan runs by itself, judges its candidates with the matcher (DlssNr_ExposureMatch.h) and lets
// go of what it does not need (the lifecycle: DlssNr_ExposureAuto.h). The candidate list and the backends are the
// scan's (DlssNr_ExposureScanCore.h); this keeps the history the matcher reads, the matcher's worker, the lifecycle and
// the found exposure the picture uses (DlssNr_ExposureScan.h AutoStatus, FoundExposure).
//
// Everything *Locked is called under Core::Mutex().

#include "DlssNr_ExposureScanCore.h"

#include <cstddef>

namespace DlssNr
{
namespace ExposureScan
{
namespace AutoScan
{
// The white point source is Auto
bool On();

// Once per NR frame, by the pass that runs NR (Core::EndFrameLocked), before the recording's row
void FrameLocked(const FrameExposure& frame);

// Core::AdoptLocked takes no candidate of this rank: Auto has finished with candidates (the game hands its exposure
// over, the picture is already exposed, or the game's upscaler is not D3D12), or, once found or given up, a bigger
// buffer (rank 1), which would be held without ever starting a search (AdoptedLocked)
bool ClosedLocked(int rank);

// Core::AdoptLocked took a candidate of this rank (DlssNrExposureScanShape::Rank)
void AdoptedLocked(int rank);

// Whether the backend copies this slot's value this frame: every candidate while watching and searching, only the match
// once found, none after giving up (those are held for the next search). Always true when Auto is not on.
bool CopiesLocked(size_t slot);
} // namespace AutoScan
} // namespace ExposureScan
} // namespace DlssNr
