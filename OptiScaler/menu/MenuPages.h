#pragma once

#include <cstddef>
#include <string_view>

// Pages of the main menu's navigation pane. Pure data, no ImGui, so it can be unit tested on the host.
namespace MenuPages
{
// Neural Rendering is a nav group, not a page: clicking it opens NrStatus.
enum class Page : unsigned char
{
    Upscaler,
    FrameGeneration,
    Image,
    Framerate,
    OverlayLook,
    Input,
    Misc,
    NrStatus,
    NrOptions,
    NrInput,
    NrOutput,
    NrPasses,
    NrDebug,
};

struct PageInfo
{
    const char* name;  // stable ini name, stored in [Menu] MenuPage
    const char* label; // shown in the nav pane
};

// Same order as Page.
inline constexpr PageInfo kPages[] = {
    { "upscaler", "Upscaler" },
    { "framegen", "Frame Generation" },
    { "image", "Image" },
    { "framerate", "Framerate" },
    { "overlay", "Overlay & Look" },
    { "input", "Input" },
    { "misc", "Misc" },
    { "nr.status", "Status & Presets" },
    { "nr.options", "NR Options" },
    { "nr.input", "NR Input" },
    { "nr.output", "NR Output" },
    { "nr.passes", "Passes & Colour" },
    { "nr.debug", "Compare & Debug" },
};

inline constexpr size_t kPageCount = sizeof(kPages) / sizeof(kPages[0]);

inline constexpr bool IsNeuralRendering(Page page) { return page >= Page::NrStatus; }

inline constexpr const PageInfo& Info(Page page)
{
    const size_t index = static_cast<size_t>(page);
    return kPages[index < kPageCount ? index : 0];
}

inline constexpr const char* Name(Page page) { return Info(page).name; }
inline constexpr const char* Label(Page page) { return Info(page).label; }

// Missing or unknown names fall back to Upscaler.
inline constexpr Page PageFromName(std::string_view name)
{
    for (size_t i = 0; i < kPageCount; ++i)
    {
        if (name == kPages[i].name)
            return static_cast<Page>(i);
    }

    return Page::Upscaler;
}
} // namespace MenuPages
