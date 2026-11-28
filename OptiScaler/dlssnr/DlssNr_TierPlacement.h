#pragma once

// Where a resolution tier (High, Medium, Low, Potato; the "Quality tiers" card of the NR menu, one row of buttons and a
// "Runs NR:" before / after Super Resolution choice under it) puts NR.
// Pure, so a host test can run it (tests/nr_tier_placement_smoke.cpp).
//
// A tier sets the model's size, method and composition. It also used to set the row's placement (before or after Super
// Resolution) and clear Finished Picture, which switched NR Only mode off the moment a tier was picked (it needs
// Finished Picture). With Finished Picture on, the placement of the rows does not apply (NR runs on the finished
// picture), so a tier leaves the placement as it is and either row lights up.

#include <optional>

namespace DlssNrTierPlacement
{
struct Placement
{
    bool finishedPicture = false;
    bool runBeforeSr = false;
    bool deferred = false; // private DLSS before SR (DlssNrDeferredDlss)
};

// The placement after picking a tier on the pre-SR (`beforeSuperResolution`) or post-SR row.
inline Placement AfterTier(Placement now, bool beforeSuperResolution)
{
    if (now.finishedPicture)
        return now;

    return { false, beforeSuperResolution, false };
}

// Whether the row's button can show as in effect, as far as the placement goes.
inline bool RowMatchesPlacement(Placement now, bool beforeSuperResolution)
{
    return now.finishedPicture || now.runBeforeSr == beforeSuperResolution;
}

// The "Runs NR:" choice under the tier buttons, the same pattern as the Look card's "Built on:". Not shown with
// Finished Picture on: a tier keeps the placement there.
inline bool PickerShown(Placement now) { return !now.finishedPicture; }

// Which side the choice shows: the placement NR has while a tier is in effect (the tier is there) or before the player
// picked one this session; otherwise the player's pick, which only sets where the next tier press puts NR.
inline bool PickedBeforeSr(Placement now, bool tierInEffect, std::optional<bool> sessionPick)
{
    if (tierInEffect || !sessionPick.has_value())
        return now.runBeforeSr;

    return *sessionPick;
}
} // namespace DlssNrTierPlacement
