#pragma once

#include "SdrToHdr_Common.h"

#include <dxgi1_6.h>

namespace SdrToHdr
{
// What the display behind `output` reports: whether Windows HDR is on (IDXGIOutput6::GetDesc1 ColorSpace is PQ), its
// MaxLuminance, and the SDR white level Windows maps an SDR app's white to (DisplayConfigGetDeviceInfo
// GET_SDR_WHITE_LEVEL on the output's monitor, matched by the output's GDI device name). 80 nits when that cannot be
// read. Logs which value it used. `output` may be null (everything then at its default, HDR off).
DisplayInfo QueryDisplay(IDXGIOutput* output);
} // namespace SdrToHdr
