#pragma once

// The CPU-side state of a loaded 3D LUT (Story 2 of the LUT-apply epic,
// memory/plans/2026-10-04-dlssnr-lut-apply.md): which file is loaded and its parsed lattice
// (DlssNr_LutFile.h, Story 1). Backend-agnostic on purpose -- DX12's NrState (shaders/dlssnr/DlssNr_Dx12.cpp)
// and a future Vulkan equivalent (Story 3) each keep their own GPU texture handle next to one of these, so
// this struct never becomes a dumping ground for resource lifetimes only a graphics backend can free safely.

#include "DlssNr_LutFile.h"

#include <string>
#include <utility>

struct DlssNr_LutState
{
    std::string loadedPath;    // the path currently parsed into `lut`; empty when nothing is loaded
    std::string attemptedPath; // the path last tried, success or not -- a broken file is not re-parsed every frame
    DlssNr::Lut3D lut;
    bool failed = false;
    std::string error;

    // True once `lut` is a successfully parsed 3D LUT a backend can upload/sample. Does not say the GPU
    // texture exists yet -- building and uploading it is the caller's job, driven off this turning true.
    bool Loaded() const { return !failed && !loadedPath.empty(); }
};

// Reparses `path` into `state` only when it differs from whatever was last tried (loaded or not), so a
// steady LutFile setting costs nothing beyond a string compare every frame. Leaves the previous lut/
// loadedPath untouched on a failed reparse -- a typo in the ini must not blank out a LUT that was working.
// `path` empty clears everything and reports no LUT wanted (distinct from "wanted but failed").
inline bool DlssNr_LutEnsureParsed(DlssNr_LutState* state, const std::string& path)
{
    if (state == nullptr)
        return false;

    if (path.empty())
    {
        state->loadedPath.clear();
        state->attemptedPath.clear();
        state->lut = DlssNr::Lut3D();
        state->failed = false;
        state->error.clear();
        return false;
    }

    if (path == state->attemptedPath)
        return state->Loaded();

    state->attemptedPath = path;

    DlssNr::Lut3D parsed;
    std::string err;
    if (!DlssNr::ParseCube(path, &parsed, &err))
    {
        state->failed = true;
        state->error = err;
        return false;
    }

    state->lut = std::move(parsed);
    state->loadedPath = path;
    state->failed = false;
    state->error.clear();
    return true;
}
