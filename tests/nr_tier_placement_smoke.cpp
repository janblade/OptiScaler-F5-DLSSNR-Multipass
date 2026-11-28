// Host check of dlssnr/DlssNr_TierPlacement.h: the resolution tiers (High to Potato) set the model's size and method, and
// leave "NR Pass at: Finished Picture" alone, so NR Only mode keeps running when a tier is picked.
// cl /std:c++20 /EHsc /W4 tests/nr_tier_placement_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_TierPlacement.h"

#include <cstdio>
#include <initializer_list>
#include <optional>

using namespace DlssNrTierPlacement;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main()
{
    // After SR / before SR: a tier row moves NR to its own placement, as it always did
    {
        const Placement afterSr { false, false, false };
        const auto post = AfterTier(afterSr, false);
        CHECK(!post.finishedPicture && !post.runBeforeSr && !post.deferred);

        const auto pre = AfterTier(afterSr, true);
        CHECK(!pre.finishedPicture && pre.runBeforeSr && !pre.deferred);

        const Placement deferred { false, true, true };
        const auto fromDeferred = AfterTier(deferred, false);
        CHECK(!fromDeferred.finishedPicture && !fromDeferred.runBeforeSr && !fromDeferred.deferred);
    }

    // Finished Picture (NR Only, and NR + frame generation's picture): any tier row keeps the placement as it is
    for (bool beforeRow : { false, true })
        for (bool runBeforeSr : { false, true })
            for (bool deferred : { false, true })
            {
                const Placement finished { true, runBeforeSr, deferred };
                const auto after = AfterTier(finished, beforeRow);
                CHECK(after.finishedPicture);
                CHECK(after.runBeforeSr == runBeforeSr);
                CHECK(after.deferred == deferred);
            }

    // The highlight: after/before SR needs the row's placement; Finished Picture matches both rows (placement does not
    // apply there)
    CHECK(RowMatchesPlacement({ false, false, false }, false));
    CHECK(!RowMatchesPlacement({ false, false, false }, true));
    CHECK(RowMatchesPlacement({ false, true, false }, true));
    CHECK(!RowMatchesPlacement({ false, true, false }, false));
    CHECK(RowMatchesPlacement({ true, false, false }, false));
    CHECK(RowMatchesPlacement({ true, true, true }, true));

    // Picking a tier and then asking which row is lit is consistent, for every start
    for (bool finished : { false, true })
        for (bool beforeRow : { false, true })
        {
            const auto after = AfterTier({ finished, false, false }, beforeRow);
            CHECK(RowMatchesPlacement(after, beforeRow));
        }

    // The "Runs NR:" choice: hidden with Finished Picture, shown otherwise
    CHECK(!PickerShown({ true, false, false }));
    CHECK(!PickerShown({ true, true, true }));
    CHECK(PickerShown({ false, false, false }));
    CHECK(PickerShown({ false, true, false }));

    // It shows where NR runs while a tier is in effect, whatever was picked before
    CHECK(PickedBeforeSr({ false, true, false }, true, false));
    CHECK(!PickedBeforeSr({ false, false, false }, true, true));
    // and before any pick this session
    CHECK(PickedBeforeSr({ false, true, false }, false, std::nullopt));
    CHECK(!PickedBeforeSr({ false, false, false }, false, std::nullopt));
    // With no tier in effect, the player's pick stands (it only sets the next press)
    CHECK(PickedBeforeSr({ false, false, false }, false, true));
    CHECK(!PickedBeforeSr({ false, true, false }, false, false));

    // A tier pressed on the picked side lands there and shows as in effect on that side
    for (bool picked : { false, true })
    {
        const auto after = AfterTier({ false, !picked, false }, picked);
        CHECK(RowMatchesPlacement(after, picked));
        CHECK(PickedBeforeSr(after, true, picked) == picked);
    }

    if (fails == 0)
        printf("PASS\n");

    return fails == 0 ? 0 : 1;
}
