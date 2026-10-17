// Host check of DlssNrVitReuse.h: which launches of one NR evaluation are dropped when the ViT bottleneck is reused. No GPU and no game needed.
// cl /std:c++20 /EHsc tests/nr_vit_reuse_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNrVitReuse.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace DlssNrVitReuse;

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

using Seq = std::vector<Role>;
// what one evaluation launches: other work, the ViT range, other work
static const Seq kEval = { Role::None, Role::Start, Role::Inner, Role::Inner, Role::Inner, Role::End, Role::None };

struct Result
{
    std::vector<bool> dropped;
    bool wellFormed = true;
};

static Result Eval(Filter& f, const void* feature, bool reset, unsigned every, const Seq& seq = kEval, long long slot = -1,
                   KernelSet set = KernelSet::Fp8, unsigned everyPlain = 0)
{
    Result r;
    f.Begin(feature, reset, every, slot, everyPlain);
    for (Role role : seq)
        r.dropped.push_back(f.Drop(role, set));
    r.wellFormed = f.End();
    return r;
}

static bool Dropped(const Result& r) // the ViT range was dropped, everything else kept
{
    return r.dropped == std::vector<bool> { false, true, true, true, true, true, false };
}

static bool Rebuilt(const Result& r) // the plain set: the ViT range dropped except its last launch, which rebuilds the 2-D output
{
    return r.dropped == std::vector<bool> { false, true, true, true, true, false, false };
}

// Copy mode (plain set, D3D12): the launches dropped and the copy asked for after each Drop, with `sig` on the run's first launch.
struct CopyResult
{
    std::vector<bool> dropped;
    std::vector<Copy> copies;
    bool wellFormed = true;
};

static CopyResult EvalCopy(Filter& f, const void* feature, bool reset, unsigned everyPlain, long long slot = -1, uint64_t sig = 7,
                           KernelSet set = KernelSet::Plain, const Seq& seq = kEval)
{
    CopyResult r;
    f.Begin(feature, reset, 2, slot, everyPlain, true);
    for (Role role : seq)
    {
        r.dropped.push_back(f.Drop(role, set, role == Role::Start ? sig : 0));
        f.TakeGap();
        r.copies.push_back(f.TakeCopy());
    }
    r.wellFormed = f.End();
    return r;
}

static bool CopyAt(const CopyResult& r, Copy copy, size_t at) // exactly one copy asked for, after launch `at`
{
    for (size_t i = 0; i < r.copies.size(); ++i)
        if (r.copies[i] != (i == at ? copy : Copy::None))
            return false;
    return true;
}

static bool NoCopy(const CopyResult& r) { return CopyAt(r, Copy::None, 0); }

static bool Kept(const Result& r)
{
    for (bool d : r.dropped)
        if (d)
            return false;
    return true;
}

int main()
{
    int a = 0, b = 0;

    // kernel names of both variants (fp8 chained and the plain fp16 ones) map to the range roles
    CHECK(RoleOf("cc_vit_1d_repack_2d_to_1d_fp8") == Role::Start);
    CHECK(RoleOf("cc_vit_1d_repack_2d_to_1d") == Role::Start);
    CHECK(RoleOf("cc_vit_1d_repack_1d_to_2d_fp8") == Role::End);
    CHECK(RoleOf("cc_vit_1d_repack_1d_to_2d") == Role::End);
    CHECK(RoleOf("cc_vit_1d_ffn_expand_publish_fp8") == Role::Inner);
    CHECK(RoleOf("cc_vit_1d_projection_wait_fp8") == Role::Inner);
    CHECK(RoleOf("cc_cb_clear") == Role::None);
    CHECK(RoleOf("cc_vit_ffn_expand") == Role::None); // the 2D ViT kernels are not the bottleneck range
    CHECK(RoleOf(nullptr) == Role::None);

    // the kernel set comes from the name: the fp8 kernels carry the _fp8 suffix
    CHECK(SetOf("cc_vit_1d_repack_2d_to_1d_fp8") == KernelSet::Fp8);
    CHECK(SetOf("cc_vit_1d_repack_2d_to_1d") == KernelSet::Plain);
    CHECK(SetOf("cc_vit_1d_fp8_projection") == KernelSet::Plain); // only a suffix counts
    CHECK(SetOf(nullptr) == KernelSet::Unknown);

    { // each kernel set follows its own rate: fp8 at 2, plain at 1
        Filter f;
        CHECK(f.LastSet() == KernelSet::Unknown);
        for (int i = 0; i < 4; ++i)
            CHECK(Kept(Eval(f, &a, false, 2, kEval, -1, KernelSet::Plain, 1)));
        CHECK(f.LastSet() == KernelSet::Plain);
        CHECK(Kept(Eval(f, &b, false, 2, kEval, -1, KernelSet::Fp8, 1)));
        CHECK(Dropped(Eval(f, &b, false, 2, kEval, -1, KernelSet::Fp8, 1)));
        CHECK(f.LastSet() == KernelSet::Fp8);
    }

    { // and the other way round: plain at 2 reuses, fp8 at 1 never does
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 1, kEval, -1, KernelSet::Plain, 2)));
        CHECK(Rebuilt(Eval(f, &a, false, 1, kEval, -1, KernelSet::Plain, 2)));
        CHECK(Kept(Eval(f, &b, false, 1, kEval, -1, KernelSet::Fp8, 2)));
        CHECK(Kept(Eval(f, &b, false, 1, kEval, -1, KernelSet::Fp8, 2)));
    }

    { // everyPlain 0 = the same rate as fp8 (callers that do not tell the sets apart)
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 2, kEval, -1, KernelSet::Plain)));
        CHECK(Rebuilt(Eval(f, &a, false, 2, kEval, -1, KernelSet::Plain)));
    }

    { // the gap is reported once, at the run's last launch, on both sets, whether that launch is dropped or kept; never on a computed run
        for (KernelSet set : { KernelSet::Fp8, KernelSet::Plain })
        {
            Filter f;
            for (int round = 0; round < 2; ++round) // computed, then skipped
            {
                f.Begin(&a, false, 2);
                std::vector<bool> gaps;
                for (Role role : kEval)
                {
                    f.Drop(role, set);
                    gaps.push_back(f.TakeGap());
                }
                CHECK(f.End());
                CHECK(gaps == (round == 0 ? std::vector<bool>(kEval.size(), false)
                                          : std::vector<bool> { false, false, false, false, false, true, false }));
                CHECK(!f.TakeGap());
            }
            // a skipped run counts as reused on both sets, even when its last launch still runs
            CHECK(f.Computed() == 1 && f.Reused() == 1);
        }
    }

    { // Split: one launch call cut around the barrier. Launches are ints: 0 other work, 1 start, 2 inner, 3 end.
        auto roleOf = [](const KernelSet set) {
            return [set](const int& l) {
                const Role r = l == 1 ? Role::Start : l == 2 ? Role::Inner : l == 3 ? Role::End : Role::None;
                return std::pair { r, r == Role::None ? KernelSet::Unknown : set };
            };
        };
        struct Out
        {
            bool any;
            std::vector<int> kept;
            std::ptrdiff_t gap;
        };
        // one evaluation, computed once and then skipped, launched as the given calls; returns the skipped evaluation's calls
        auto run = [&](KernelSet set, const std::vector<std::vector<int>>& calls) {
            Filter f;
            std::vector<Out> outs;
            for (int round = 0; round < 2; ++round)
            {
                f.Begin(&a, false, 2);
                outs.clear();
                for (const auto& call : calls)
                {
                    Out o { false, {}, -1 };
                    o.any = f.Split(call.data(), call.size(), roleOf(set), o.kept, o.gap);
                    outs.push_back(o);
                }
                CHECK(f.End());
                if (round == 0) // computed: nothing dropped, no barrier
                    for (const auto& o : outs)
                        CHECK(!o.any && o.gap == -1);
            }
            return outs;
        };
        using V = std::vector<int>;

        // all in one call: fp8 drops the run, the barrier goes where it was; plain keeps the end, barrier right before it
        auto o = run(KernelSet::Fp8, { { 0, 1, 2, 3, 0 } });
        CHECK(o[0].any && o[0].kept == V({ 0, 0 }) && o[0].gap == 1);
        o = run(KernelSet::Plain, { { 0, 1, 2, 3, 0 } });
        CHECK(o[0].any && o[0].kept == V({ 0, 3, 0 }) && o[0].gap == 1);

        // the end alone in its call: plain still asks for the barrier (before it), fp8 drops it
        o = run(KernelSet::Plain, { { 0, 1, 2 }, { 3 }, { 0 } });
        CHECK(o[0].any && o[0].kept == V({ 0 }) && o[0].gap == -1);
        CHECK(o[1].any && o[1].kept == V({ 3 }) && o[1].gap == 0);
        CHECK(!o[2].any && o[2].gap == -1);
        o = run(KernelSet::Fp8, { { 0, 1, 2 }, { 3 }, { 0 } });
        CHECK(o[1].any && o[1].kept.empty() && o[1].gap == 0);

        // the end first or last in a call
        o = run(KernelSet::Plain, { { 1, 2 }, { 3, 0, 0 } });
        CHECK(o[0].any && o[0].kept.empty() && o[0].gap == -1);
        CHECK(o[1].any && o[1].kept == V({ 3, 0, 0 }) && o[1].gap == 0);
        o = run(KernelSet::Plain, { { 0, 1, 2, 3 } });
        CHECK(o[0].any && o[0].kept == V({ 0, 3 }) && o[0].gap == 1);
    }

    { // One: one launch per call, as Vulkan's vkCmdCuLaunchKernelNVX does. The same decisions as a chain: over a
      // computed and a skipped evaluation, the launches that go out and where the barrier goes match Split on the whole
      // evaluation in one call.
        const auto roleOfInt = [](int l, KernelSet set)
        {
            const Role r = l == 1 ? Role::Start : l == 2 ? Role::Inner : l == 3 ? Role::End : Role::None;
            return std::pair { r, r == Role::None ? KernelSet::Unknown : set };
        };
        for (const KernelSet set : { KernelSet::Fp8, KernelSet::Plain })
        {
            const std::vector<int> evaluation { 0, 0, 1, 2, 2, 2, 3, 0, 0 };
            Filter single, chain;
            for (int round = 0; round < 2; ++round)
            {
                // one call per launch: "B" marks a barrier recorded before the next launch (or where a dropped one was)
                single.Begin(&a, false, 2);
                std::vector<int> out;
                for (int l : evaluation)
                {
                    const auto rs = roleOfInt(l, set);
                    const Filter::Single one = single.One(rs.first, rs.second);
                    if (one.barrier)
                        out.push_back(-1);
                    if (one.launch)
                        out.push_back(l);
                }
                CHECK(single.End());

                chain.Begin(&a, false, 2);
                std::vector<int> kept;
                std::ptrdiff_t gap = -1;
                chain.Split(
                    evaluation.data(), evaluation.size(), [&](const int& l) { return roleOfInt(l, set); }, kept, gap);
                CHECK(chain.End());
                std::vector<int> expected = kept;
                if (gap >= 0)
                    expected.insert(expected.begin() + gap, -1);

                CHECK(out == expected);
                if (round == 0) // computed: everything goes out, no barrier
                    CHECK(out == evaluation);
                else if (set == KernelSet::Fp8) // skipped: the run is gone, the barrier where it was
                    CHECK(out == std::vector<int>({ 0, 0, -1, 0, 0 }));
                else // plain: the run's last kernel stays, the barrier right before it
                    CHECK(out == std::vector<int>({ 0, 0, -1, 3, 0, 0 }));
            }
            CHECK(single.LastSet() == set);
        }

        // outside an evaluation (the game's own DLSS SR on the same functions): every launch goes out, no barrier. The
        // evaluation context is per thread, not per Filter, so this also relies on the End above having closed it.
        Filter idle;
        const Filter::Single one = idle.One(Role::Start, KernelSet::Fp8);
        CHECK(one.launch && !one.barrier);
    }

    { // plain set, another kernel inside a skipped run: the end still runs and the barrier is still asked for, the feature goes off
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 2, kEval, -1, KernelSet::Plain)));
        f.Begin(&a, false, 2);
        CHECK(f.Drop(Role::Start, KernelSet::Plain));
        CHECK(!f.Drop(Role::None, KernelSet::Unknown));
        CHECK(!f.Drop(Role::End, KernelSet::Plain));
        CHECK(f.TakeGap());
        CHECK(!f.End());
        CHECK(f.Disabled());
    }

    { // outside an evaluation nothing is touched, whatever the setting
        Filter f;
        CHECK(!f.Evaluating());
        CHECK(!f.Drop(Role::Start) && !f.Drop(Role::Inner) && !f.Drop(Role::End));
    }

    { // N = 1 never drops but keeps the cache warm
        Filter f;
        for (int i = 0; i < 4; ++i)
            CHECK(Kept(Eval(f, &a, false, 1)));
        CHECK(f.Computed() == 4 && f.Reused() == 0);
        CHECK(Dropped(Eval(f, &a, false, 2))); // raising N takes effect on the next evaluation, no warm-up
    }

    { // N = 2: full, reuse, full, reuse
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
        CHECK(f.Computed() == 2 && f.Reused() == 2 && !f.Disabled());
    }

    { // N = 3: full, reuse, reuse, full
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
        CHECK(Kept(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
    }

    { // lowering N mid-cycle computes at once
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
        CHECK(Kept(Eval(f, &a, false, 1)));
        CHECK(Kept(Eval(f, &a, false, 1)));
    }

    { // a reset always computes in full, and restarts the cycle
        Filter f;
        Eval(f, &a, false, 2);
        CHECK(Kept(Eval(f, &a, true, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
    }

    { // each feature (each pass) has its own cache and cycle
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Kept(Eval(f, &b, false, 2))); // b has never been computed
        CHECK(Dropped(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &b, false, 2)));
        CHECK(Kept(Eval(f, &a, false, 2)));
    }

    { // passes share the frame's slot, N = 2, 3 passes: all compute on even frames, all reuse on odd ones
        Filter f;
        int p[3];
        for (int frame = 0; frame < 9; ++frame)
        {
            int computed = 0;
            for (int pass = 0; pass < 3; ++pass)
                computed += Kept(Eval(f, &p[pass], false, 2, kEval, frame));
            CHECK(computed == (frame % 2 ? 0 : 3));
        }
    }

    { // a pass that starts over on an off frame (reset of that pass alone) is back in the frame's phase on the next frame
        Filter f;
        int p[3];
        std::vector<std::string> seen;
        for (int frame = 0; frame < 8; ++frame)
        {
            std::string s;
            for (int pass = 0; pass < 3; ++pass)
                s += Kept(Eval(f, &p[pass], pass == 1 && frame == 5, 2, kEval, frame)) ? 'V' : '-';
            seen.push_back(s);
        }
        CHECK(seen[4] == "VVV" && seen[5] == "-V-" && seen[6] == "VVV" && seen[7] == "---");
    }

    { // anchored or not, a pass never waits longer than N - 1 reused evaluations, for N = 2 and 3, any phase, with a reset
        for (unsigned every = 2; every <= 3; ++every)
            for (int pass = -1; pass < 4; ++pass)
            {
                Filter f;
                int run = 0, worst = 0, computed = 0;
                for (int frame = 0; frame < 12; ++frame)
                {
                    if (Kept(Eval(f, &a, frame == 5, every, kEval, pass < 0 ? -1 : frame + pass)))
                        run = 0, ++computed;
                    else
                        worst = std::max(worst, ++run);
                }
                CHECK(worst <= (int) every - 1);
                CHECK(computed <= 12 / (int) every + 2); // still reuses: at most the first frame and the reset extra
            }
    }

    { // destroying the modules invalidates every cache
        Filter f;
        Eval(f, &a, false, 2);
        f.Clear();
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
    }

    { // a range that is never closed: the feature turns itself off
        Filter f;
        Eval(f, &a, false, 2);
        auto r = Eval(f, &a, false, 2, { Role::None, Role::Start, Role::Inner });
        CHECK(!r.wellFormed && f.Disabled());
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Kept(Eval(f, &a, false, 2)));
    }

    { // an unclosed full range is not a valid cache
        Filter f;
        f.Begin(&a, false, 2);
        f.Drop(Role::Start);
        CHECK(!f.End());
        f.Clear();
    }

    { // a foreign launch inside a skipped range is kept, the rest of the range is still dropped, then the feature is off
        Filter f;
        Eval(f, &a, false, 2);
        auto r = Eval(f, &a, false, 2, { Role::Start, Role::Inner, Role::None, Role::Inner, Role::End });
        CHECK((r.dropped == std::vector<bool> { true, true, false, true, true }));
        CHECK(!r.wellFormed && f.Disabled());
        CHECK(Kept(Eval(f, &a, false, 2)));
    }

    { // End without Start
        Filter f;
        auto r = Eval(f, &a, false, 2, { Role::None, Role::End });
        CHECK((r.dropped == std::vector<bool> { false, false }) && !r.wellFormed && f.Disabled());
    }

    { // a second Start inside a range
        Filter f;
        auto r = Eval(f, &a, false, 2, { Role::Start, Role::Start, Role::End });
        CHECK(!r.wellFormed && f.Disabled());
    }

    { // the evaluation context belongs to its thread: a launch from another thread is not part of it
        Filter f;
        Eval(f, &a, false, 2);
        f.Begin(&a, false, 2);
        bool other = true;
        std::thread([&] { other = f.Evaluating() || f.Drop(Role::Start); }).join();
        CHECK(!other);
        f.End();
    }

    { // copy mode, plain set: a computed run is kept and asks for a save after its last kernel; a skipped run drops its last kernel
      // too and asks for a restore in its place
        Filter f;
        const std::vector<bool> all = { false, false, false, false, false, false, false };
        const std::vector<bool> run = { false, true, true, true, true, true, false };
        auto c = EvalCopy(f, &a, false, 2);
        CHECK(c.dropped == all && CopyAt(c, Copy::Save, 5) && c.wellFormed);
        c = EvalCopy(f, &a, false, 2);
        CHECK(c.dropped == run && CopyAt(c, Copy::Restore, 5) && c.wellFormed);
        c = EvalCopy(f, &a, false, 2);
        CHECK(c.dropped == all && CopyAt(c, Copy::Save, 5));
        CHECK(f.Restored() == 1 && f.Reused() == 1 && f.Computed() == 2);
    }

    { // copy mode: a different first launch (moved or resized network) is computed again, never restored
        Filter f;
        EvalCopy(f, &a, false, 2, -1, 7);
        auto c = EvalCopy(f, &a, false, 2, -1, 8);
        CHECK(!c.dropped[1] && CopyAt(c, Copy::Save, 5));
        c = EvalCopy(f, &a, false, 2, -1, 8);
        CHECK(c.dropped[1] && c.dropped[5] && CopyAt(c, Copy::Restore, 5));
    }

    { // copy mode: a reset computes and saves again; each feature (pass) has its own saved copy
        Filter f;
        EvalCopy(f, &a, false, 2);
        auto c = EvalCopy(f, &a, true, 2);
        CHECK(!c.dropped[1] && CopyAt(c, Copy::Save, 5));
        c = EvalCopy(f, &b, false, 2);
        CHECK(!c.dropped[1] && CopyAt(c, Copy::Save, 5)); // b never saved
        CHECK(CopyAt(EvalCopy(f, &a, false, 2), Copy::Restore, 5));
        CHECK(CopyAt(EvalCopy(f, &b, false, 2), Copy::Restore, 5));
    }

    { // copy mode costs nothing when the plain set's Reuse is off, and never touches the fp8 set
        Filter f;
        for (int i = 0; i < 3; ++i)
        {
            auto c = EvalCopy(f, &a, false, 1);
            CHECK(NoCopy(c) && !c.dropped[1]);
        }
        Filter g;
        auto c = EvalCopy(g, &a, false, 2, -1, 7, KernelSet::Fp8);
        CHECK(NoCopy(c) && !c.dropped[1]);
        g.Begin(&a, false, 2, -1, 2, true); // fp8 rate 2: the whole run dropped, no restore (its 2-D output keeps its memory)
        std::vector<bool> d;
        std::vector<Copy> copies;
        for (Role role : kEval)
        {
            d.push_back(g.Drop(role, KernelSet::Fp8, 7));
            g.TakeGap();
            copies.push_back(g.TakeCopy());
        }
        CHECK(g.End());
        CHECK((d == std::vector<bool> { false, true, true, true, true, true, false }));
        CHECK(std::all_of(copies.begin(), copies.end(), [](Copy x) { return x == Copy::None; }));
    }

    { // without copy mode the plain set still keeps the run's last kernel (the 310.8 planner-patched DLLs, and Vulkan)
        Filter f;
        Eval(f, &a, false, 2, kEval, -1, KernelSet::Plain, 2);
        CHECK(Rebuilt(Eval(f, &a, false, 2, kEval, -1, KernelSet::Plain, 2)));
    }

    { // copy mode, Split: where the copy goes in one launch call, and which launch names the 2-D output
        auto roleOf = [](const int& l) {
            const Role r = l == 1 ? Role::Start : l == 2 ? Role::Inner : l == 3 ? Role::End : Role::None;
            return std::pair { r, r == Role::None ? KernelSet::Unknown : KernelSet::Plain };
        };
        auto sigOf = [](const int&) { return uint64_t(7); };
        struct Out
        {
            bool any;
            std::vector<int> kept;
            std::ptrdiff_t gap, copyAt, copyFrom;
            Copy copy;
        };
        auto run = [&](const std::vector<std::vector<int>>& calls) {
            Filter f;
            std::vector<std::vector<Out>> rounds;
            for (int round = 0; round < 2; ++round)
            {
                f.Begin(&a, false, 2, -1, 2, true);
                std::vector<Out> outs;
                for (const auto& call : calls)
                {
                    Out o { false, {}, -1, -1, -1, Copy::None };
                    o.any = f.Split(call.data(), call.size(), roleOf, sigOf, o.kept, o.gap, o.copy, o.copyAt, o.copyFrom);
                    outs.push_back(o);
                }
                CHECK(f.End());
                rounds.push_back(outs);
            }
            return rounds;
        };
        using V = std::vector<int>;

        auto r = run({ { 0, 1, 2, 3, 0 } });
        const Out& saved = r[0][0];
        CHECK(saved.any && saved.kept == V({ 0, 1, 2, 3, 0 }) && saved.gap == -1 && saved.copy == Copy::Save && saved.copyAt == 4 &&
              saved.copyFrom == 3);
        const Out& restored = r[1][0];
        CHECK(restored.any && restored.kept == V({ 0, 0 }) && restored.gap == 1 && restored.copy == Copy::Restore &&
              restored.copyAt == 1 && restored.copyFrom == 3);

        // the last kernel alone in its call: the copy is asked for in that call only
        r = run({ { 0, 1, 2 }, { 3 }, { 0 } });
        CHECK(!r[0][0].any && r[0][1].any && r[0][1].copy == Copy::Save && r[0][1].copyAt == 1 && r[0][1].copyFrom == 0 && !r[0][2].any);
        CHECK(r[1][0].any && r[1][0].kept == V({ 0 }) && r[1][0].copy == Copy::None);
        CHECK(r[1][1].any && r[1][1].kept.empty() && r[1][1].gap == 0 && r[1][1].copy == Copy::Restore && r[1][1].copyAt == 0 &&
              r[1][1].copyFrom == 0);
        CHECK(!r[1][2].any && r[1][2].kept == V({ 0 }));
    }

    { // a copy the caller could not record: the feature goes off with the reason, and nothing is reused afterwards
        Filter f;
        EvalCopy(f, &a, false, 2);
        f.Disable("no buffer for the copy");
        CHECK(f.Disabled() && f.Status().find("no buffer for the copy") != std::string::npos);
        auto c = EvalCopy(f, &a, false, 2);
        CHECK(!c.dropped[1] && !c.dropped[5]);
    }

    { // copy mode once the feature is off (Disable or a launch-order anomaly): no more saves, nothing dropped
        Filter f;
        EvalCopy(f, &a, false, 2);
        f.Disable("test");
        for (int i = 0; i < 3; ++i)
        {
            auto c = EvalCopy(f, &a, false, 2);
            CHECK(NoCopy(c) && !c.dropped[1] && !c.dropped[5]);
        }
        Filter g;
        EvalCopy(g, &a, false, 2, -1, 7, KernelSet::Plain, { Role::Start, Role::Inner }); // never closed: off
        CHECK(g.Disabled());
        CHECK(NoCopy(EvalCopy(g, &a, false, 2)));
    }

    { // copy mode: a second ViT run in one evaluation turns the feature off (one saved copy per feature)
        Filter f;
        auto c = EvalCopy(f, &a, false, 2, -1, 7, KernelSet::Plain,
                          { Role::Start, Role::Inner, Role::End, Role::Start, Role::Inner, Role::End });
        CHECK(!c.wellFormed && f.Disabled());
        CHECK(CopyAt(c, Copy::Save, 2)); // the first run's save; the second run is not in copy mode
        Filter g; // without copy mode two runs stay allowed, as before
        auto r = Eval(g, &a, false, 2, { Role::Start, Role::End, Role::Start, Role::End }, -1, KernelSet::Plain, 2);
        CHECK(r.wellFormed && !g.Disabled());
    }

    { // copy mode anchored to the frame: passes save on even frames and restore on odd ones; destroyed modules start over
        Filter f;
        int p[2];
        for (int frame = 0; frame < 6; ++frame)
            for (int pass = 0; pass < 2; ++pass)
            {
                auto c = EvalCopy(f, &p[pass], false, 2, frame);
                CHECK(CopyAt(c, frame % 2 ? Copy::Restore : Copy::Save, 5));
            }
        f.Clear();
        CHECK(CopyAt(EvalCopy(f, &p[0], false, 2, 7), Copy::Save, 5)); // odd frame, but nothing saved any more: computed
        CHECK(CopyAt(EvalCopy(f, &p[0], false, 2, 9), Copy::Restore, 5));
    }

    { // One (Vulkan) has no copy mode: a copy asked for there turns the feature off
        Filter f;
        f.Begin(&a, false, 2, -1, 2, true);
        for (Role role : kEval)
            f.One(role, KernelSet::Plain);
        CHECK(!f.End() && f.Disabled());
    }

    { // Kernels: which kernel handles are the ViT run's, from their names at creation (Vulkan)
        Kernels<uint64_t> k;
        const auto none = std::pair { Role::None, KernelSet::Unknown };
        k.Created(1, "cc_vit_1d_repack_2d_to_1d_fp8");
        k.Created(2, "cc_vit_1d_qkv_chained");
        k.Created(3, "cc_dec_input_upsample_1024_512_tilesync_fp8");
        CHECK((k.Find(1) == std::pair { Role::Start, KernelSet::Fp8 }));
        CHECK((k.Find(2) == std::pair { Role::Inner, KernelSet::Plain }));
        CHECK(k.Find(3) == none);
        CHECK(k.Find(99) == none);

        // a handle reused by another kernel (after a destroy that was never seen, such as a lost device): the new
        // kernel wins
        k.Created(1, "cc_split_swin_16h_qkv_512_chained_fp8");
        CHECK(k.Find(1) == none);
        k.Created(3, "cc_vit_1d_repack_1d_to_2d_fp8");
        CHECK((k.Find(3) == std::pair { Role::End, KernelSet::Fp8 }));

        k.Destroyed(2);
        CHECK(k.Find(2) == none);
        k.Created(4, nullptr);
        CHECK(k.Find(4) == none);
        k.Clear();
        CHECK(k.Find(3) == none);
    }

    printf(fails ? "nr_vit_reuse_smoke: %d FAILED\n" : "nr_vit_reuse_smoke: all passed\n", fails);
    return fails != 0;
}
