#pragma once

// Which of NR's surfaces are stale when a format changes (DlssNr_Dx12.cpp, ReleaseSurfacesIfFormatChanged).
//
// Two formats are in play. The model surfaces (output, the ping-pong and clamp scratches) follow the model: the game's
// colour format, or RGBA16F while Compress screen edges packs the picture. The colour surfaces (the kept proxy
// colorCopy, the shrunk copy, the crop, the LUT scratch, the native pair) are made from and into the game's colour, so
// they follow the game's format. With compression on the model format no longer moves when the game's does, and
// checking only that one left the colour surfaces in the old format: the next copy from the game's new colour was an
// invalid call and the device was removed (NBA 2K27, native finished picture RGBA8 -> the game's DLSS after SR,
// RGBA16F).
//
// Our own surfaces are made in the canonical colour format, not the game's exact one: BGRA8 becomes its RGBA8 twin.
// Every shader reads and writes them through views of their own format, so the channel order sorts itself out, and a
// game that swaps its output between RGBA8 and BGRA8 from frame to frame (Watch Dogs: Legion) no longer looks like a
// format change. That swap used to throw away the model feature and its history every time. The few surfaces copied
// straight to or from the game's texture (CopyResource / CopyTextureRegion need the same format family) keep the exact
// format and check it themselves.

namespace DlssNr
{
// DXGI_FORMAT values, kept as ints so this header needs no Windows headers.
constexpr int kR8G8B8A8Typeless = 27, kR8G8B8A8Unorm = 28, kR8G8B8A8UnormSrgb = 29;
constexpr int kB8G8R8A8Unorm = 87, kB8G8R8A8Typeless = 90, kB8G8R8A8UnormSrgb = 91;

// The format our surfaces are made in for a game colour format. Only BGRA8 moves (to RGBA8, keeping
// typeless/UNORM/sRGB); everything else, RGBA8 itself included, stays as it is, so games that never swap see the
// surfaces they always had.
constexpr int CanonicalColourFormat(int format)
{
    switch (format)
    {
    case kB8G8R8A8Typeless:
        return kR8G8B8A8Typeless;
    case kB8G8R8A8Unorm:
        return kR8G8B8A8Unorm;
    case kB8G8R8A8UnormSrgb:
        return kR8G8B8A8UnormSrgb;
    default:
        return format;
    }
}

// Two game colour formats our surfaces treat as one: only the channel order differs.
constexpr bool SameColourFamily(int a, int b) { return CanonicalColourFormat(a) == CanonicalColourFormat(b); }

// Formats are DXGI_FORMAT values as ints; 0 (UNKNOWN) means "no such surface yet". The wanted formats are canonical.
constexpr bool SurfacesStale(int haveModel, int wantModel, int haveColour, int wantColour)
{
    return (haveModel != 0 && haveModel != wantModel) || (haveColour != 0 && haveColour != wantColour);
}

// A surface made at the ordinary (uncompressed) size: stale when that size is not the one wanted now. Under Compress
// screen edges the change check sees only the packed size, which can stay put while the ordinary size moves by a
// 16-pixel step (1920x1080, Balanced, scale 0.50 to 0.504: 960x544 -> 976x544, packed 864x496 either way), so the
// surface has to be asked for itself.
constexpr bool SizeStale(unsigned haveW, unsigned haveH, unsigned wantW, unsigned wantH)
{
    return haveW != 0 && (haveW != wantW || haveH != wantH);
}
} // namespace DlssNr
