// Host check of DlssNr_LutChain.h: a stack of LUTs baked into one lattice matches applying them one after another.
// No GPU and no game. Optional argument: a folder of .cube files; every pair in it is also compared (the error is
// printed, so the bound can be judged on real looks).
// cl /std:c++20 /EHsc /W4 tests/nr_lut_chain_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_LutChain.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

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

using namespace DlssNr;

// A lattice from a per-point function, with an optional domain.
template <typename F> static Lut3D Make(int n, F f, float lo = 0.0f, float hi = 1.0f)
{
    Lut3D l;
    l.size = n;
    for (int c = 0; c < 3; ++c)
    {
        l.domainMin[c] = lo;
        l.domainMax[c] = hi;
    }
    l.rgb.resize((size_t) n * n * n * 3);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
            {
                const float in[3] = { lo + (hi - lo) * r / (n - 1), lo + (hi - lo) * g / (n - 1),
                                      lo + (hi - lo) * b / (n - 1) };
                float out[3];
                f(in, out);
                for (int c = 0; c < 3; ++c)
                    l.rgb[(((size_t) b * n + g) * n + r) * 3 + c] = out[c];
            }
    return l;
}

static Lut3D Identity(int n) { return Make(n, [](const float* i, float* o) { for (int c = 0; c < 3; ++c) o[c] = i[c]; }); }

// Deliberately non-linear: a strong gamma per channel with a cross-channel term.
static Lut3D Curvy(int n, float gamma, float mix)
{
    return Make(n, [=](const float* i, float* o)
                {
                    const float l = (i[0] + i[1] + i[2]) / 3.0f;
                    for (int c = 0; c < 3; ++c)
                        o[c] = std::pow(std::clamp(i[c] * (1 - mix) + l * mix, 0.0f, 1.0f), gamma);
                });
}

static float SequentialError(const std::vector<ChainLayer>& layers, const Lut3D& baked, double* meanOut)
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    float worst = 0.0f;
    double sum = 0.0;
    const int samples = 20000;
    for (int s = 0; s < samples; ++s)
    {
        const float in[3] = { u(rng), u(rng), u(rng) };
        float v[3] = { in[0], in[1], in[2] };
        for (const ChainLayer& layer : layers)
        {
            float next[3];
            ApplyLayer(*layer.lut, layer.strength, v, next);
            for (int c = 0; c < 3; ++c)
                v[c] = next[c];
        }
        float got[3];
        SampleLut(baked, in, got);
        for (int c = 0; c < 3; ++c)
        {
            const float e = std::fabs(got[c] - v[c]);
            worst = std::max(worst, e);
            sum += e;
        }
    }
    *meanOut = sum / (samples * 3.0);
    return worst;
}

int main(int argc, char** argv)
{
    std::string err;
    Lut3D out;

    // Nothing to do: empty, null, zero strength.
    CHECK(!BakeChain({}, &out, &err));
    const Lut3D id17 = Identity(17);
    CHECK(!BakeChain({ { nullptr, 1.0f } }, &out, &err));
    CHECK(!BakeChain({ { &id17, 0.0f } }, &out, &err));
    CHECK(out.size == 0);

    // One layer at full strength is the file itself, bit for bit, whatever its size and domain.
    const Lut3D curvy33 = Curvy(33, 2.2f, 0.3f);
    CHECK(BakeChain({ { &curvy33, 1.0f } }, &out, &err));
    CHECK(out.size == 33 && out.rgb == curvy33.rgb);
    const Lut3D dom = Make(9, [](const float* i, float* o) { for (int c = 0; c < 3; ++c) o[c] = i[c] * 0.5f; }, -0.25f, 1.25f);
    CHECK(BakeChain({ { &dom, 1.0f } }, &out, &err));
    CHECK(out.size == 9 && out.domainMin[0] == -0.25f && out.domainMax[2] == 1.25f && out.rgb == dom.rgb);

    // One layer at half strength equals the shader's lerp(display, graded, s), anywhere inside the domain.
    for (const Lut3D* lut : { &curvy33, &dom })
    {
        CHECK(BakeChain({ { lut, 0.5f } }, &out, &err));
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> u(lut->domainMin[0], lut->domainMax[0]);
        float worst = 0.0f;
        for (int s = 0; s < 5000; ++s)
        {
            const float in[3] = { u(rng), u(rng), u(rng) };
            float want[3], got[3];
            ApplyLayer(*lut, 0.5f, in, want);
            SampleLut(out, in, got);
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::fabs(got[c] - want[c]));
        }
        CHECK(worst < 1e-5f);
    }

    // An identity layer anywhere in a stack changes nothing: the result is the other layer.
    {
        const Lut3D a = Curvy(33, 1.8f, 0.0f);
        CHECK(BakeChain({ { &id17, 1.0f }, { &a, 1.0f }, { &id17, 1.0f } }, &out, &err));
        CHECK(out.size == 33);
        double mean = 0.0;
        const float worst = SequentialError({ { &a, 1.0f } }, out, &mean);
        printf("identity-wrapped single: worst %.5f mean %.6f\n", worst, mean);
        CHECK(worst < 2e-3f);
    }

    // Two non-linear layers: the baked lattice follows sequential application. The bound is what a 33 or 65 lattice
    // can do for a gamma pair; printed so it can be judged.
    {
        const Lut3D a = Curvy(33, 2.2f, 0.3f);
        const Lut3D b = Curvy(33, 0.6f, 0.0f);
        CHECK(BakeChain({ { &a, 1.0f }, { &b, 0.7f } }, &out, &err));
        CHECK(out.size == 33);
        double mean = 0.0;
        const float worst = SequentialError({ { &a, 1.0f }, { &b, 0.7f } }, out, &mean);
        printf("2 layers @33: worst %.5f mean %.6f\n", worst, mean);
        CHECK(worst < 0.02f);
        CHECK(mean < 0.002);

        const Lut3D a65 = Curvy(65, 2.2f, 0.3f);
        CHECK(BakeChain({ { &a65, 1.0f }, { &b, 0.7f } }, &out, &err));
        CHECK(out.size == 65);
        const float worst65 = SequentialError({ { &a65, 1.0f }, { &b, 0.7f } }, out, &mean);
        printf("2 layers @65: worst %.5f mean %.6f\n", worst65, mean);
        CHECK(worst65 < worst);
    }

    // Mixed sizes and a domain: result is clamped into 17..65, 4 layers allowed, a fifth is not.
    {
        const Lut3D tiny = Curvy(5, 1.4f, 0.1f);
        CHECK(BakeChain({ { &tiny, 1.0f }, { &dom, 1.0f } }, &out, &err));
        CHECK(out.size == kChainMinSize);
        const Lut3D big = Identity(128);
        CHECK(BakeChain({ { &big, 1.0f }, { &tiny, 1.0f } }, &out, &err));
        CHECK(out.size == kChainMaxSize);
        CHECK(BakeChain({ { &tiny, 1.0f }, { &tiny, 1.0f }, { &tiny, 1.0f }, { &tiny, 1.0f } }, &out, &err));
        CHECK(!BakeChain({ { &tiny, 1.0f }, { &tiny, 1.0f }, { &tiny, 1.0f }, { &tiny, 1.0f }, { &tiny, 1.0f } }, &out, &err));
        CHECK(out.size == 0 && !err.empty());
    }

    // The state: parse, bake once per change, keep the last good chain when a layer fails.
    {
        const auto write = [](const char* name, const Lut3D& l)
        {
            const std::string path = std::string("nr_lut_chain_smoke_") + name + ".cube";
            std::ofstream f(path);
            f << "LUT_3D_SIZE " << l.size << "\n";
            for (size_t i = 0; i < l.rgb.size(); i += 3)
                f << l.rgb[i] << " " << l.rgb[i + 1] << " " << l.rgb[i + 2] << "\n";
            return path;
        };
        const std::string pa = write("a", Curvy(17, 2.0f, 0.2f));
        const std::string pb = write("b", Curvy(17, 0.7f, 0.0f));
        const std::string bad = "nr_lut_chain_smoke_missing.cube";

        DlssNr_LutChainState st;
        CHECK(!DlssNr_LutChainEnsure(&st, {}));
        CHECK(!st.wanted && !st.Loaded());

        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f } }));
        CHECK(st.wanted && st.baked.size == 17 && st.failedLayer < 0);

        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f }, { pb, 0.5f } }));
        const std::vector<float> two = st.baked.rgb;
        const std::string key2 = st.key;
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f }, { pb, 0.5f } })); // unchanged: not baked again
        CHECK(st.key == key2 && st.baked.rgb == two);

        // A layer that cannot be read: the chain that worked stays, the failure is named.
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f }, { bad, 0.5f } }));
        CHECK(st.failedLayer == 1 && st.failedPath == bad && !st.error.empty() && st.baked.rgb == two);

        // Order matters: swapping the layers changes the bake.
        CHECK(DlssNr_LutChainEnsure(&st, { { pb, 0.5f }, { pa, 1.0f } }));
        CHECK(st.failedLayer < 0 && st.baked.rgb != two);

        // Emptied: everything released.
        CHECK(!DlssNr_LutChainEnsure(&st, {}));
        CHECK(!st.wanted && st.baked.size == 0 && st.key.empty());

        std::filesystem::remove(pa);
        std::filesystem::remove(pb);
    }

    // Optional: every pair of real LUTs in a folder.
    if (argc > 1)
    {
        std::vector<Lut3D> luts;
        std::vector<std::string> names;
        for (const auto& e : std::filesystem::directory_iterator(argv[1]))
        {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
            if (ext == ".cube")
            {
                Lut3D l;
                std::string perr;
                if (ParseCube(e.path().string(), &l, &perr))
                {
                    luts.push_back(std::move(l));
                    names.push_back(e.path().filename().string());
                }
            }
        }
        float worstAll = 0.0f;
        double meanSum = 0.0;
        int pairs = 0;
        std::string worstName;
        for (size_t i = 0; i < luts.size(); ++i)
            for (size_t j = 0; j < luts.size(); j += std::max<size_t>(1, luts.size() / 6))
            {
                std::vector<ChainLayer> layers = { { &luts[i], 1.0f }, { &luts[j], 0.8f } };
                Lut3D baked;
                if (!BakeChain(layers, &baked, &err))
                    continue;
                double mean = 0.0;
                const float worst = SequentialError(layers, baked, &mean);
                meanSum += mean;
                ++pairs;
                if (worst > worstAll)
                {
                    worstAll = worst;
                    worstName = names[i] + " + " + names[j];
                }
            }
        printf("real LUTs: %zu files, %d pairs, worst %.5f (%s), mean of means %.6f\n", luts.size(), pairs, worstAll,
               worstName.c_str(), pairs ? meanSum / pairs : 0.0);
    }

    printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
