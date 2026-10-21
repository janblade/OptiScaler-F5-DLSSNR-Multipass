#include "pch.h"
#include "SdrToHdr_Display.h"

#include <Logger.h>

#include <vector>

namespace SdrToHdr
{
namespace
{
// The SDR white level of the monitor whose source (desktop) has this GDI name, in Windows' units; false when none is found.
bool ReadSdrWhiteLevel(const wchar_t* gdiName, unsigned long& level)
{
    UINT32 pathCount = 0, modeCount = 0;

    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0)
        return false;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return false;

    for (UINT32 i = 0; i < pathCount; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;

        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, gdiName) != 0)
            continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white {};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;

        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS || white.SDRWhiteLevel == 0)
            return false;

        level = white.SDRWhiteLevel;
        return true;
    }

    return false;
}
} // namespace

DisplayInfo QueryDisplay(IDXGIOutput* output)
{
    DisplayInfo info;

    if (output == nullptr)
        return info;

    IDXGIOutput6* output6 = nullptr;

    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output6))) || output6 == nullptr)
    {
        LOG_INFO("HDR output: the output reports no colour space (not IDXGIOutput6), taking the display as SDR");
        return info;
    }

    DXGI_OUTPUT_DESC1 desc {};

    if (SUCCEEDED(output6->GetDesc1(&desc)))
    {
        info.hdrOn = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        info.maxLuminance = desc.MaxLuminance;

        unsigned long level = 0;

        if (ReadSdrWhiteLevel(desc.DeviceName, level))
        {
            info.sdrWhiteNits = SdrWhiteLevelToNits(level);
            info.sdrWhiteFromWindows = true;
        }

        LOG_INFO("HDR output: display HDR {}, max {:.0f} nits, SDR white {:.0f} nits ({})", info.hdrOn,
                 info.maxLuminance, info.sdrWhiteNits,
                 info.sdrWhiteFromWindows ? "from Windows" : "Windows gave none, using the 80 nit reference");
    }
    else
    {
        LOG_INFO("HDR output: GetDesc1 failed, taking the display as SDR");
    }

    output6->Release();
    return info;
}
} // namespace SdrToHdr
