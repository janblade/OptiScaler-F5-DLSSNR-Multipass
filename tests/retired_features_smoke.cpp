// Host check of RetiredFeatures (OptiScaler/inputs/RetiredFeatures.h): a feature the game released must outlive every
// present that could still have read State::currentFeature, and must be gone before an NGX shutdown.
//
// A simulated present thread copies the current feature at the start of a present and reads it at the end (as
// RenderMenu and the FPS overlay do), while the game thread releases the feature at a random point in between and makes
// its next NGX call some milliseconds later. A read of a destroyed feature is counted as unsafe. The old release path
// (destroy at once) is run the same way for contrast. No GPU, no game, no threads: the interleavings are stepped.
// cl /std:c++20 /EHsc /W4 tests/retired_features_smoke.cpp
#include "../OptiScaler/inputs/RetiredFeatures.h"

#include <cstdio>
#include <random>
#include <set>

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

static std::set<int> alive;

struct Feature
{
    int id;
    explicit Feature(int i) : id(i) { alive.insert(i); }
    ~Feature() { alive.erase(id); }
};

// One run: `presentMs` per present, the present reads the copied pointer `readLagMs` after copying it. Returns how many
// reads hit a destroyed feature.
static int Run(bool parked, double presentMs, double readLagMs, unsigned seed)
{
    std::mt19937 rng(seed);
    RetiredFeatures<Feature> retired;
    std::unique_ptr<Feature> current = std::make_unique<Feature>(0);
    int nextId = 1, unsafe = 0;
    uint64_t presents = 0;
    double now = 0.0;

    for (int frame = 0; frame < 400; ++frame)
    {
        // The present copies the pointer...
        Feature* copied = current.get();
        const int copiedId = copied != nullptr ? copied->id : -1;

        // ...the game thread may release the feature in between (photo mode, a quality change)...
        if (rng() % 7 == 0 && current != nullptr)
        {
            if (parked)
                retired.Park(std::move(current), presents, now + readLagMs * 0.5);
            else
                current.reset();
        }

        // ...and the present reads it.
        if (copied != nullptr && alive.count(copiedId) == 0)
            ++unsafe;

        ++presents;
        now += presentMs;

        // The game's next NGX call: collect, and make a new feature after a release.
        retired.Collect(presents, now);
        if (current == nullptr && rng() % 2 == 0)
            current = std::make_unique<Feature>(nextId++);
    }

    // An NGX shutdown takes everything parked
    retired.Collect(presents, now, true);
    CHECK(retired.Size() == 0);
    current.reset();
    CHECK(alive.empty());
    return unsafe;
}

int main()
{
    printf("present ms | read lag ms | destroyed at once: unsafe reads | parked: unsafe reads\n");
    for (double presentMs : { 4.0, 8.3, 16.7, 33.3 })
        for (double lag : { 0.1, 2.0, 8.0 })
        {
            int oldUnsafe = 0, newUnsafe = 0;
            for (unsigned seed = 1; seed <= 20; ++seed)
            {
                oldUnsafe += Run(false, presentMs, lag, seed);
                newUnsafe += Run(true, presentMs, lag, seed);
            }
            printf("%10.1f | %11.1f | %31d | %19d\n", presentMs, lag, oldUnsafe, newUnsafe);
            CHECK(oldUnsafe > 0); // the simulation does reach the race
            CHECK(newUnsafe == 0);
        }

    // The waits: not before kPresents presents, not before kMs, and at once with `all`
    {
        RetiredFeatures<Feature> r;
        r.Park(std::make_unique<Feature>(100), 10, 1000.0);
        r.Collect(12, 5000.0);
        CHECK(r.Size() == 1); // 2 presents only
        r.Collect(13, 1200.0);
        CHECK(r.Size() == 1); // 200 ms only
        r.Collect(13, 1500.0);
        CHECK(r.Size() == 0);
        r.Park(std::make_unique<Feature>(101), 20, 0.0);
        r.Collect(20, 0.0, true);
        CHECK(r.Size() == 0);
        r.Park(nullptr, 0, 0.0);
        CHECK(r.Size() == 0);
    }

    printf(fails == 0 ? "PASS\n" : "%d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
