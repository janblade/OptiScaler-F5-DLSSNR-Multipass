// Host check of misc/RelativePathRule.h: relative ini paths resolve from OptiScaler's folder first, then the exe
// folder; absolute paths stay as they are.
// cl /std:c++20 /EHsc /W4 tests/relative_path_rule_smoke.cpp
#include "../OptiScaler/misc/RelativePathRule.h"

#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;
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
    const fs::path root = fs::temp_directory_path() / L"relpath_rule_smoke";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path opti = root / L"game" / L"OptiScaler";
    const fs::path exe = root / L"game";
    fs::create_directories(opti / L"inopti", ec);
    fs::create_directories(exe / L"OptiScaler" / L"dlss31016", ec); // Forza style: written against the exe folder
    fs::create_directories(exe / L"onlyexe", ec);

    using RelativePathRule::Resolve;

    // Opti's folder first
    CHECK(Resolve(L"inopti", opti, exe) == opti / L"inopti");
    // Nothing in Opti's folder, the exe folder has it: fallback
    CHECK(Resolve(L"OptiScaler/dlss31016", opti, exe) == exe / L"OptiScaler/dlss31016");
    CHECK(Resolve(L"onlyexe", opti, exe) == exe / L"onlyexe");
    // Neither: the documented base
    CHECK(Resolve(L"nothing", opti, exe) == opti / L"nothing");
    // Both: Opti's folder wins
    fs::create_directories(opti / L"onlyexe", ec);
    CHECK(Resolve(L"onlyexe", opti, exe) == opti / L"onlyexe");
    // Absolute and empty unchanged
    CHECK(Resolve(exe / L"onlyexe", opti, exe) == exe / L"onlyexe");
    CHECK(Resolve(L"", opti, exe).empty());
    // Proxy dll in the exe folder: one base
    CHECK(Resolve(L"nothing", exe, exe) == exe / L"nothing");

    fs::remove_all(root, ec);
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
