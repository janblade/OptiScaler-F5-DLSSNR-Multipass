// Host test for the post-NR stack's look input (DlssNr_LutSpaces.h + DlssNr_LutChain.h, post-NR LUT epic Story 6):
//
//   1. Each log curve matches its vendor's published values (mid grey, and black / 90% where the vendor tables give them).
//   2. Every gamut keeps white white; Rec.709 light -> LogC4 AWG4 matches the pack's own LinRec709-to-ArriLogC4AWG4.cube
//      (its 1D shaper + 3D table, read as a file) from deep shadow to 16x light, to that file's own error (its 33^3 table
//      gives mid grey 0.2759, slightly tinted, where ARRI's value is 0.2784).
//   3. Frame decodes: Linear HDR undoes the pass's sRGB(Neutwo(light)) exactly; SDR keeps mid grey and sends white to 16.
//   4. Auto detection: names with and without "-to-", and over the pack: every ARRILogC4AWG4-to-* look reads LogC4 AWG4
//      from its header and writes the picture; LinRec709-to-ArriLogC4AWG4 reads linear Rec.709 and writes LogC4.
//   5. Chains: a look alone on Auto bakes like a conversion file + the look (LinearHDR-to-ArriLogC4AWG4 on a Linear HDR
//      frame, sRGB-Expand16-to-ArriLogC4AWG4 on SDR; files made by an independent numpy implementation of the same maths),
//      and the pack's LinRec709 file + the look on Auto does too, to the pack's own error: its shaper is read past 1, a
//      clip there would put highlights ~0.1 off.
//
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\nr_lut_spaces_smoke.cpp
// Run:   nr_lut_spaces_smoke.exe [<pack folder> <folder with LinearHDR-to-ArriLogC4AWG4.cube,
//        sRGB-Expand16-to-ArriLogC4AWG4.cube and Sports-Broadcast-sRGB.cube>]   (without them only 1, 3 and the name
//        cases of 4 run; the reference files: make_exact_refs.py, see the post-NR LUT plan's Story 6)
#include "../OptiScaler/dlssnr/DlssNr_LutChain.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace DlssNr;

static int fails = 0;
static void expect(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what.c_str());
        ++fails;
    }
}

static float Curve(LutSpace s, float x)
{
    const float light[3] = { x, x, x };
    float out[3];
    EncodeLightToSpace(s, light, out);
    return out[1];
}

static std::string Name(LutSpace s) { return LutSpaceName(s); }

// The stack's display value for Linear HDR light: what dlssnr_lut.hlsl hands it (sRGB of Neutwo on the peak channel).
static void HandedLinearHdr(const float light[3], float out[3])
{
    const float m = std::max({ light[0], light[1], light[2] });
    const float scale = m > 1e-6f ? (m / std::sqrt(m * m + 1.0f)) / m : 1.0f;
    for (int c = 0; c < 3; ++c)
    {
        const float v = std::clamp(light[c] * scale, 0.0f, 1.0f);
        out[c] = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    }
}

static char g_where[160] = "";

// Over display 0..`top` per channel.
static float WorstBakeDiff(const Lut3D& a, const Lut3D& b, float top)
{
    float worst = 0.0f;
    const int n = 23;
    for (int bi = 0; bi < n; ++bi)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
            {
                const float v[3] = { top * r / float(n - 1), top * g / float(n - 1), top * bi / float(n - 1) };
                float x[3], y[3];
                SampleLut(a, v, x);
                SampleLut(b, v, y);
                for (int c = 0; c < 3; ++c)
                    if (std::abs(x[c] - y[c]) > worst)
                    {
                        worst = std::abs(x[c] - y[c]);
                        std::snprintf(g_where, sizeof(g_where), "at display (%.2f %.2f %.2f): %.3f vs %.3f", v[0], v[1],
                                      v[2], x[c], y[c]);
                    }
            }
    return worst;
}

static bool Bake(const std::vector<ChainEntry>& entries, FrameDecode decode, DlssNr_LutChainState* state)
{
    return DlssNr_LutChainEnsure(state, entries, decode);
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // 1. Published values (colour-science's doctests quote the same vendor numbers).
    struct Ref
    {
        LutSpace space;
        float x, want, tolerance;
    };
    const Ref refs[] = {
        { LutSpace::ArriLogC3, 0.18f, 0.3910068f, 2e-6f },    { LutSpace::ArriLogC3, 0.0f, 0.092809f, 1e-6f },
        { LutSpace::ArriLogC4, 0.18f, 0.2783958f, 2e-6f },    { LutSpace::ArriLogC4, 0.0f, 95.0f / 1023.0f, 1e-6f },
        { LutSpace::AcesCct, 0.18f, 0.4135884f, 2e-6f },      { LutSpace::AcesCct, 0.0f, 0.0729055f, 1e-6f },
        { LutSpace::SLog3, 0.18f, 0.4105571f, 2e-6f },        { LutSpace::SLog3, 0.0f, 95.0f / 1023.0f, 1e-6f },
        { LutSpace::SLog3, 0.9f, 598.0f / 1023.0f, 0.6f / 1023.0f },
        { LutSpace::VLog, 0.18f, 0.4233114f, 2e-6f },         { LutSpace::VLog, 0.0f, 128.0f / 1023.0f, 0.6f / 1023.0f },
        { LutSpace::VLog, 0.9f, 602.0f / 1023.0f, 0.6f / 1023.0f },
        { LutSpace::DaVinciIntermediate, 0.18f, 0.3360432f, 2e-6f }, { LutSpace::DaVinciIntermediate, 0.0f, 0.0f, 1e-7f },
    };
    for (const Ref& r : refs)
    {
        const float got = Curve(r.space, r.x);
        expect(std::abs(got - r.want) <= r.tolerance,
               Name(r.space) + " at " + std::to_string(r.x) + ": " + std::to_string(got) + ", want " + std::to_string(r.want));
    }
    std::puts("1. curves match the published values");

    // 2. White stays white (equal channels) in every gamut; LogC4 vs the pack's file below, with the folder.
    for (int s = (int) LutSpace::LinearRec709; s < (int) LutSpace::Count; ++s)
    {
        const float white[3] = { 1.0f, 1.0f, 1.0f };
        float out[3];
        EncodeLightToSpace((LutSpace) s, white, out);
        expect(std::abs(out[0] - out[1]) < 2e-5f && std::abs(out[2] - out[1]) < 2e-5f,
               Name((LutSpace) s) + " does not keep white neutral");
    }

    // 3. Frame decodes.
    {
        float worst = 0.0f;
        const float lights[][3] = { { 0.18f, 0.18f, 0.18f }, { 1, 1, 1 }, { 4, 4, 4 }, { 0.6f, 0.2f, 0.03f },
                                    { 12, 3, 0.5f },         { 0.01f, 0.02f, 0.05f } };
        for (const auto& light : lights)
        {
            float handed[3], back[3];
            HandedLinearHdr(light, handed);
            DecodeFrameToLight(FrameDecode::LinearHdr, handed, back);
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(back[c] - light[c]) / std::max(light[c], 0.01f));
        }
        std::printf("3. Linear HDR decode: worst relative error %.2e\n", worst);
        expect(worst < 2e-3f, "Linear HDR decode does not undo the pass's sRGB(Neutwo)");

        const float grey[3] = { 0.4613561f, 0.4613561f, 0.4613561f }; // sRGB of 0.18
        const float white[3] = { 1, 1, 1 };
        float g[3], w[3];
        DecodeFrameToLight(FrameDecode::Srgb, grey, g);
        DecodeFrameToLight(FrameDecode::Srgb, white, w);
        std::printf("   SDR: mid grey -> %.4f, white -> %.2f\n", g[1], w[1]);
        expect(std::abs(g[1] - 0.18f) < 0.005f, "SDR decode moves mid grey by more than 0.04 EV");
        expect(std::abs(w[1] - kSdrWhiteLight) < 1e-3f, "SDR decode does not send white to kSdrWhiteLight");
    }

    // 4. Names.
    {
        struct NameCase
        {
            const char* stem;
            LutSpace in, out;
            const char* from;
        };
        const NameCase names[] = {
            { "ARRILogC4AWG4-to-AgX-Blender-sRGB", LutSpace::ArriLogC4, LutSpace::Display, "name" },
            { "LinRec709-to-ArriLogC4AWG4", LutSpace::LinearRec709, LutSpace::ArriLogC4, "name" },
            { "SLog3_to_Rec709_Kodak2383", LutSpace::SLog3, LutSpace::Display, "name" },
            { "VLog Film Look", LutSpace::VLog, LutSpace::Display, "name" },
            { "Lakers_Broadcast_33", LutSpace::Display, LutSpace::Display, "" },
            { "LinearHDR-to-ArriLogC4AWG4", LutSpace::Display, LutSpace::ArriLogC4, "" },
            { "sRGB-Expand16-to-ArriLogC4AWG4", LutSpace::Display, LutSpace::ArriLogC4, "" },
            { "Sports-Broadcast-sRGB", LutSpace::Display, LutSpace::Display, "" },
        };
        for (const NameCase& c : names)
        {
            const LookSpaces got = DetectLookSpaces(c.stem, "", "", "", "");
            expect(got.in == c.in && got.out == c.out && std::string(got.inFrom) == c.from,
                   std::string("name ") + c.stem + ": in " + Name(got.in) + " (" + got.inFrom + "), out " + Name(got.out));
        }
        std::puts("4. names resolve");
    }

    if (argc >= 3)
    {
        const std::filesystem::path pack = argv[1], ours = argv[2];
        const std::string toLog = (pack / "LinRec709-to-ArriLogC4AWG4.cube").string();

        // 2b. LogC4 AWG4 vs the pack's file.
        {
            Lut3D file;
            std::string err;
            const bool parsed = ParseCube(toLog, &file, &err);
            expect(parsed, "pack LinRec709 file: " + err);
            float worst = parsed ? 0.0f : 1.0f;
            const float lights[][3] = { { 0.001f, 0.001f, 0.001f }, { 0.18f, 0.18f, 0.18f }, { 1, 1, 1 },  { 16, 16, 16 },
                                        { 0.6f, 0.2f, 0.03f },      { 0.02f, 0.03f, 0.3f },   { 3, 1, 0.2f } };
            for (const auto& light : lights)
            {
                if (!parsed)
                    break;
                float mine[3], theirs[3];
                EncodeLightToSpace(LutSpace::ArriLogC4, light, mine);
                SampleLutFile(file, light, theirs);
                for (int c = 0; c < 3; ++c)
                    worst = std::max(worst, std::abs(mine[c] - theirs[c]));
                std::printf("   light (%g %g %g): mine (%.4f %.4f %.4f) pack (%.4f %.4f %.4f)\n", light[0], light[1],
                            light[2], mine[0], mine[1], mine[2], theirs[0], theirs[1], theirs[2]);
            }
            std::printf("2. LogC4 AWG4 vs the pack's LinRec709-to-ArriLogC4AWG4.cube: worst %.2e\n", worst);
            expect(worst < 4e-3f, "LogC4 AWG4 conversion differs from the pack's file by more than its own error");
        }

        // 4b. Detection over the pack.
        int looks = 0;
        for (const auto& entry : std::filesystem::directory_iterator(pack))
        {
            if (entry.path().extension() != ".cube")
                continue;
            Lut3D lut;
            std::string err;
            if (!ParseCube(entry.path().string(), &lut, &err))
                continue;
            const std::string stem = entry.path().stem().string();
            const LookSpaces got = DetectLookSpaces(stem, lut.inGamma, lut.inGamut, lut.outGamma, lut.outGamut);
            if (stem.rfind("ARRILogC4AWG4-to-", 0) == 0)
            {
                ++looks;
                expect(got.in == LutSpace::ArriLogC4 && std::string(got.inFrom) == "header" && got.out == LutSpace::Display,
                       stem + ": in " + Name(got.in) + " (" + got.inFrom + "), out " + Name(got.out));
            }
            else if (stem == "LinRec709-to-ArriLogC4AWG4")
                expect(got.in == LutSpace::LinearRec709 && std::string(got.inFrom) == "header" &&
                           got.out == LutSpace::ArriLogC4,
                       stem + ": in " + Name(got.in) + ", out " + Name(got.out));
        }
        std::printf("4. pack: %d looks read LogC4 AWG4 from their headers\n", looks);
        expect(looks == 22, "expected the pack's 22 ARRILogC4AWG4-to-* looks");

        // 5. Chains.
        const std::string look = (pack / "ARRILogC4AWG4-to-AgX-Resolve-Bombastic-sRGB.cube").string();
        const auto compare = [&](const char* what, std::vector<ChainEntry> autoStack, std::vector<ChainEntry> fileStack,
                                 FrameDecode decode, float bound)
        {
            DlssNr_LutChainState a, b;
            expect(Bake(autoStack, decode, &a), std::string(what) + ": Auto stack did not bake: " + a.error);
            expect(Bake(fileStack, decode, &b), std::string(what) + ": file stack did not bake: " + b.error);
            // Linear HDR never reaches display 1.0 (Neutwo < 1); the top lattice row is where the two clamp the
            // inverse differently, so it is left out there.
            const float worst = WorstBakeDiff(a.baked, b.baked, decode == FrameDecode::LinearHdr ? 0.98f : 1.0f);
            std::printf("5. %s: worst %.2e %s\n   auto: %s\n   file: %s\n", what, worst, g_where, a.description.c_str(),
                        b.description.c_str());
            expect(worst < bound, std::string(what) + ": Auto differs from the conversion file");
        };
        compare("Linear HDR, look on Auto vs LinearHDR-to-ArriLogC4AWG4 + look", { { look, 1.0f } },
                { { (ours / "LinearHDR-to-ArriLogC4AWG4.cube").string(), 1.0f }, { look, 1.0f } }, FrameDecode::LinearHdr,
                2e-3f);
        compare("SDR, look on Auto vs sRGB-Expand16-to-ArriLogC4AWG4 + look", { { look, 1.0f } },
                { { (ours / "sRGB-Expand16-to-ArriLogC4AWG4.cube").string(), 1.0f }, { look, 1.0f } }, FrameDecode::Srgb,
                2e-3f);
        compare("Linear HDR, pack LinRec709 + look on Auto vs LinearHDR-to-ArriLogC4AWG4 + look",
                { { toLog, 1.0f }, { look, 1.0f } },
                { { (ours / "LinearHDR-to-ArriLogC4AWG4.cube").string(), 1.0f }, { look, 1.0f } }, FrameDecode::LinearHdr,
                3e-2f);

        // An ordinary creative stack is not converted: no "as" in the key, the bake is the file's own.
        {
            DlssNr_LutChainState s;
            const std::string creative = (ours / "Sports-Broadcast-sRGB.cube").string();
            if (std::filesystem::exists(creative))
            {
                expect(Bake({ { creative, 0.7f } }, FrameDecode::LinearHdr, &s), "creative LUT did not load");
                expect(!s.inputs[0].converted && s.key.find("|as ") == std::string::npos && s.strength == 0.7f,
                       "a creative LUT got a conversion or lost the single-layer shortcut");
            }
        }

        // One converted look: baked at full strength, its strength left to the pass, so its slider bakes nothing.
        {
            DlssNr_LutChainState s, full;
            expect(Bake({ { look, 0.5f } }, FrameDecode::LinearHdr, &s), "converted look at 0.5 did not bake");
            expect(Bake({ { look, 1.0f } }, FrameDecode::LinearHdr, &full), "converted look at 1 did not bake");
            const std::string keyBefore = s.key;
            expect(s.strength == 0.5f && s.inputs[0].converted, "a single converted look does not leave its strength to the pass");
            expect(WorstBakeDiff(s.baked, full.baked, 1.0f) == 0.0f, "a single converted look baked its strength in");
            expect(Bake({ { look, 0.8f } }, FrameDecode::LinearHdr, &s) && s.key == keyBefore && s.strength == 0.8f,
                   "moving a single converted look's slider re-baked");
            expect(Bake({ { look, 0.8f } }, FrameDecode::Srgb, &s) && s.key != keyBefore,
                   "a frame decode change did not re-bake a converted look");
            std::puts("5. a single converted look keeps its strength in the pass");
        }
    }
    else
        std::puts("(no pack folder given: file checks skipped)");

    std::puts(fails == 0 ? "PASS: look input" : "FAILED");
    return fails == 0 ? 0 : 1;
}
