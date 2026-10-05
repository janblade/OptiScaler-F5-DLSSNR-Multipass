// Host test for menu/MenuPages.h: the main menu's page ids, their stable ini names, and the fallback for a missing or
// unknown saved page.
//
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\menu_pages_smoke.cpp
#include <cstdio>
#include <set>
#include <string>
#include "../OptiScaler/menu/MenuPages.h"

using namespace MenuPages;

static int fails = 0;
static void Check(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

int main()
{
    std::set<std::string> names;
    std::set<std::string> labels;

    for (size_t i = 0; i < kPageCount; ++i)
    {
        const Page page = static_cast<Page>(i);
        const std::string name = Name(page);
        const std::string label = Label(page);

        Check(!name.empty() && !label.empty(), "page " + std::to_string(i) + " has a name and a label");
        Check(PageFromName(name) == page, "page '" + name + "' round-trips through its ini name");
        Check(names.insert(name).second, "ini name '" + name + "' is unique");
        Check(labels.insert(label).second, "label '" + label + "' is unique");
        Check(IsNeuralRendering(page) == (name.rfind("nr.", 0) == 0),
              "NR pages are exactly the nr.* names ('" + name + "')");
    }

    Check(kPageCount == 13, "7 categories plus 6 Neural Rendering sub-pages");
    Check(std::string(Name(Page::NrOutput)) == "nr.output", "the NR Output page keeps its documented ini name");
    Check(std::string(Name(Page::Upscaler)) == "upscaler", "the Upscaler page keeps its documented ini name");

    // Missing or unknown saved pages fall back to Upscaler.
    Check(PageFromName("") == Page::Upscaler, "empty name falls back to Upscaler");
    Check(PageFromName("auto") == Page::Upscaler, "'auto' (nothing saved) falls back to Upscaler");
    Check(PageFromName("nr.nonsense") == Page::Upscaler, "unknown NR name falls back to Upscaler");
    Check(PageFromName("Upscaler") == Page::Upscaler, "names are lower case; other spellings fall back");
    Check(PageFromName("nr.OUTPUT") == Page::Upscaler, "a wrong-case NR name falls back too");

    // Out-of-range ids are safe to name.
    Check(std::string(Name(static_cast<Page>(kPageCount))) == "upscaler", "out-of-range page names as Upscaler");

    if (fails == 0)
        std::printf("menu_pages_smoke: all checks passed (%zu pages)\n", kPageCount);

    return fails == 0 ? 0 : 1;
}
