#pragma once
#include "Nvngx_DllProxy.h"

#include <string>

// A DLSS-G replacement that ships as its own nvngx_dlssg.dll (the plain NVSDK_NGX_* exports, as NVIDIA's file has), in
// OptiScaler's custom folder: [FrameGen] FGNvngxReplacement=<name> loads custom\<name>\dlssg\nvngx_dlssg.dll, or
// custom\<name>\nvngx_dlssg.dll. The DLL is loaded by its full path, so its own files next to it are found the same way
// they would be in a game folder.
class Nvngx_Custom : public Nvngx_DllProxy
{
    std::wstring _path; // the file loaded, empty when none was found

  protected:
    void LoadLibraries() override final;

  public:
    Nvngx_Custom() { LoadLibraries(); }

    int getMaxFakeFramesCount() override { return 5; }
    FGNvngxReplacement getType() override { return FGNvngxReplacement::Custom; }

    const std::wstring& path() const { return _path; }
};
