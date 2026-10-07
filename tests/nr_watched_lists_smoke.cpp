// Host check of shaders/dlssnr/DlssNr_WatchedLists.h: the submit and reset hooks take NR's lock only for a command
// list NR recorded on. No GPU. cl /std:c++20 /EHsc /W4 tests/nr_watched_lists_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_WatchedLists.h"

#include <cstdio>

using namespace DlssNrWatchedLists;

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
    int a = 0, b = 0, c = 0;
    const void* game[] = { &b, &c };
    const void* withA[] = { &b, &a, &c };

    // Nothing watched: no submit takes the lock
    CHECK(!Any(2, game));
    CHECK(!Any(0, nullptr));

    // NR recorded on a: a submit that carries it takes the lock, one without it does not
    Add(&a);
    CHECK(Any(3, withA));
    CHECK(!Any(2, game));

    // Added twice, removed once: gone (one entry per list)
    Add(&a);
    Remove(3, withA);
    CHECK(!Any(3, withA));

    // A reset of a watched list
    Add(&b);
    const void* reset[] = { &b };
    CHECK(Any(1, reset));
    Remove(1, reset);
    CHECK(!Any(1, reset));

    // A null list is never watched
    Add(nullptr);
    const void* none[] = { nullptr };
    CHECK(!Any(1, none));

    // The table full: every hook takes the lock, as before the table existed
    static int many[kCount + 1];
    for (auto& list : many)
        Add(&list);
    CHECK(g_full.load());
    CHECK(Any(2, game));

    printf(fails == 0 ? "nr_watched_lists_smoke: PASS\n" : "nr_watched_lists_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
