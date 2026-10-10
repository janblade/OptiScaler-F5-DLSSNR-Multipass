#pragma once

// Which of NR's surfaces are stale when a format changes (DlssNr_Dx12.cpp, ReleaseSurfacesIfFormatChanged).
//
// Two formats are in play. The model surfaces (output, the ping-pong and clamp scratches) follow the model: the game's
// colour format, or RGBA16F while Compress screen edges packs the picture. The colour surfaces (the kept proxy colorCopy,
// the shrunk copy, the crop, the LUT scratch, the native pair) are copied from and into the game's colour, so they always
// follow the game's format. With compression on the model format no longer moves when the game's does, and checking only
// that one left the colour surfaces in the old format: the next copy from the game's new colour was an invalid call
// and the device was removed (NBA 2K27, native finished picture RGBA8 -> the game's DLSS after SR, RGBA16F).

namespace DlssNr
{
// Formats are DXGI_FORMAT values as ints; 0 (UNKNOWN) means "no such surface yet".
constexpr bool SurfacesStale(int haveModel, int wantModel, int haveColour, int wantColour)
{
    return (haveModel != 0 && haveModel != wantModel) || (haveColour != 0 && haveColour != wantColour);
}
} // namespace DlssNr
