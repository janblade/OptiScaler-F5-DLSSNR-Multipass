#pragma once
#include "Nvngx_DllProxy.h"

#include <misc/CustomProviders.h>

#include <string>

// A DLSS-G replacement that ships as its own nvngx_dlssg.dll (the plain NVSDK_NGX_* exports, as NVIDIA's file has), in
// OptiScaler's custom\<name>\dlssg folder (or custom\<name>), or registered by a plugin. [FrameGen]
// FGNvngxReplacement=<name> picks it. What it reports before init (DLSSG.MultiFrameCountMax, FrameGeneration.Available)
// is read once: a provider that says it has no frame generation is not used, so the FG falls back.
// Adapted from upstream OptiScaler 941ce6a1 + 91ab9fe3.
class Nvngx_Custom : public Nvngx_DllProxy
{
    std::string _name;
    CustomProviders::Exports _exports;
    int _reportedMaxFakeFrames = 0;
    int _reportedAvailable = 1;

    void QueryReportedParameters();

  protected:
    void LoadLibraries() override final;

  public:
    explicit Nvngx_Custom(std::string name) : _name(std::move(name)) { LoadLibraries(); }

    // DLSS-G reports its count only after init; providers that report it before init say so here
    int getMaxFakeFramesCount() override { return _reportedMaxFakeFrames > 0 ? _reportedMaxFakeFrames : 5; }
    FGNvngxReplacement getType() override { return FGNvngxReplacement::Custom; }

    const std::string& name() const { return _name; }
    const std::filesystem::path& path() const { return _exports.path; }
};
