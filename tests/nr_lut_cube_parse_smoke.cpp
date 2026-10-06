// Host check of DlssNr_LutFile.h: parsing a .cube LUT file into an in-memory 3D lattice. No GPU and no
// game -- writes small temp .cube files beside the binary and parses them back.
// cl /std:c++20 /EHsc /W4 tests/nr_lut_cube_parse_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_LutFile.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace DlssNr;

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

// Writes `contents` to a temp file and returns its path. Each call uses its own name so tests never
// collide; the file is removed by the caller once it is done with it.
static std::string WriteTemp(const char* name, const std::string& contents)
{
    const std::string path = std::string("nr_lut_cube_parse_smoke_") + name + ".cube";
    std::ofstream out(path);
    out << contents;
    out.close();
    return path;
}

int main()
{
    // A valid 2x2x2 file round-trips: size, domain, and every RGB triple in file order.
    {
        const std::string path = WriteTemp("valid", "TITLE \"test\"\n"
                                                    "# a comment\n"
                                                    "LUT_3D_SIZE 2\n"
                                                    "\n"
                                                    "0.0 0.0 0.0\n"
                                                    "1.0 0.0 0.0\n"
                                                    "0.0 1.0 0.0\n"
                                                    "1.0 1.0 0.0\n"
                                                    "0.0 0.0 1.0\n"
                                                    "1.0 0.0 1.0\n"
                                                    "0.0 1.0 1.0\n"
                                                    "1.0 1.0 1.0\n");

        Lut3D lut;
        std::string error;
        CHECK(ParseCube(path, &lut, &error));
        CHECK(lut.size == 2);
        CHECK(lut.domainMin[0] == 0.0f && lut.domainMin[1] == 0.0f && lut.domainMin[2] == 0.0f);
        CHECK(lut.domainMax[0] == 1.0f && lut.domainMax[1] == 1.0f && lut.domainMax[2] == 1.0f);
        CHECK(lut.rgb.size() == 8 * 3);
        // Fourth row: 1.0 1.0 0.0
        CHECK(lut.rgb[3 * 3 + 0] == 1.0f && lut.rgb[3 * 3 + 1] == 1.0f && lut.rgb[3 * 3 + 2] == 0.0f);
        // Last row: 1.0 1.0 1.0
        CHECK(lut.rgb[7 * 3 + 0] == 1.0f && lut.rgb[7 * 3 + 1] == 1.0f && lut.rgb[7 * 3 + 2] == 1.0f);

        std::filesystem::remove(path);
    }

    // DOMAIN_MIN / DOMAIN_MAX are read when present.
    {
        const std::string path = WriteTemp("domain", "LUT_3D_SIZE 2\n"
                                                     "DOMAIN_MIN 0.1 0.2 0.3\n"
                                                     "DOMAIN_MAX 0.9 0.8 0.7\n"
                                                     "0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n");

        Lut3D lut;
        std::string error;
        CHECK(ParseCube(path, &lut, &error));
        CHECK(lut.domainMin[0] == 0.1f && lut.domainMin[1] == 0.2f && lut.domainMin[2] == 0.3f);
        CHECK(lut.domainMax[0] == 0.9f && lut.domainMax[1] == 0.8f && lut.domainMax[2] == 0.7f);

        std::filesystem::remove(path);
    }

    // A 1D-only LUT is rejected with its own message, not a generic failure.
    {
        const std::string path = WriteTemp("1d", "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n");

        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));
        CHECK(error.find("1D") != std::string::npos);

        std::filesystem::remove(path);
    }

    // A row count that doesn't match LUT_3D_SIZE is rejected (too few here).
    {
        const std::string path = WriteTemp("short", "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n");

        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));
        CHECK(!error.empty());

        std::filesystem::remove(path);
    }

    // Too many rows for the declared size is also rejected, not silently truncated.
    {
        std::string body = "LUT_3D_SIZE 2\n";
        for (int i = 0; i < 9; ++i)
            body += "0 0 0\n"; // one more than 2^3 = 8
        const std::string path = WriteTemp("long", body);

        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));

        std::filesystem::remove(path);
    }

    // No LUT_3D_SIZE at all.
    {
        const std::string path = WriteTemp("nosize", "0 0 0\n1 1 1\n");

        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));
        CHECK(!error.empty());

        std::filesystem::remove(path);
    }

    // LUT_3D_SIZE outside the supported range.
    {
        const std::string path = WriteTemp("toobig", "LUT_3D_SIZE 999\n");

        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));

        std::filesystem::remove(path);
    }

    // LUT_3D_SIZE with a non-numeric value, not just a missing or out-of-range one.
    {
        const std::string path = WriteTemp("notanumber", "LUT_3D_SIZE banana\n");

        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));

        std::filesystem::remove(path);
    }

    // A missing file is its own clear error, not a crash.
    {
        Lut3D lut;
        std::string error;
        CHECK(!ParseCube("nr_lut_cube_parse_smoke_does_not_exist.cube", &lut, &error));
        CHECK(!error.empty());
    }

    printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
