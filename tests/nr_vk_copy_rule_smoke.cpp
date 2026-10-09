// Host check of native/VkPresentCopyRule.h: what the Vulkan bridge's D3D12 present does when the picture did not reach the
// back buffer. No GPU, no game. cl /std:c++20 /EHsc /W4 tests/nr_vk_copy_rule_smoke.cpp
#include "../OptiScaler/native/VkPresentCopyRule.h"

#include <cstdio>

using namespace native;

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
    // A game that has not produced a picture yet: presents, says nothing, counts nothing
    {
        VkCopyFailureRule rule;

        for (int i = 0; i < 50; ++i)
        {
            const auto d = rule.OnFrame(VkCopyOutcome::NoPicture);
            CHECK(d.present && !d.warn && !d.recovered && d.failures == 0);
        }

        // A copy that fails before any success is a real failure: counted and said, but there is no last frame to keep
        const auto d = rule.OnFrame(VkCopyOutcome::CopyFailed);
        CHECK(d.present && d.warn && d.failures == 1);
    }

    // After a good frame: the first three failures in a row skip the present, the fourth presents anyway
    {
        VkCopyFailureRule rule;
        CHECK(rule.OnFrame(VkCopyOutcome::Copied).present);

        auto d = rule.OnFrame(VkCopyOutcome::NoPicture);
        CHECK(!d.present && d.warn && d.failures == 1); // said at 1
        d = rule.OnFrame(VkCopyOutcome::CopyFailed);
        CHECK(!d.present && !d.warn && d.failures == 2);
        d = rule.OnFrame(VkCopyOutcome::NoPicture);
        CHECK(!d.present && d.failures == 3);
        d = rule.OnFrame(VkCopyOutcome::NoPicture);
        CHECK(d.present && d.failures == 4); // a persistent failure still presents: the window never freezes

        for (int i = 5; i <= 9; ++i)
            CHECK(rule.OnFrame(VkCopyOutcome::NoPicture).present);

        d = rule.OnFrame(VkCopyOutcome::NoPicture);
        CHECK(d.present && d.warn && d.failures == 10); // the 1/10/100 cadence

        // Pictures come back: said once with the count, and the count restarts
        d = rule.OnFrame(VkCopyOutcome::Copied);
        CHECK(d.present && d.recovered && d.failures == 10 && !d.warn);
        d = rule.OnFrame(VkCopyOutcome::Copied);
        CHECK(d.present && !d.recovered);

        // ... so the next dropout skips again
        d = rule.OnFrame(VkCopyOutcome::CopyFailed);
        CHECK(!d.present && d.warn && d.failures == 1);
    }

    printf(fails == 0 ? "PASS\n" : "FAILED %d\n", fails);
    return fails == 0 ? 0 : 1;
}
