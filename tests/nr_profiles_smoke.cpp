// Host test for dlssnr/DlssNr_Profiles.h: which [DlssNr] keys an NR profile keeps, profile names, and that every key
// Config::SaveIni writes is sorted into kept or left out (so a new setting cannot slip past the choice).
//
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\nr_profiles_smoke.cpp
// Run:   nr_profiles_smoke.exe OptiScaler\Config.cpp
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include "../OptiScaler/dlssnr/DlssNr_Profiles.h"

using namespace DlssNrProfiles;

static int fails = 0;
static void Check(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

static void Names()
{
    Check(SanitiseName("NBA sharp-2") == std::optional<std::string>("NBA sharp-2"), "plain name kept");
    Check(SanitiseName("  Soft v1.2  ") == std::optional<std::string>("Soft v1.2"), "outer spaces trimmed");
    Check(SanitiseName("a_b") == std::optional<std::string>("a_b"), "underscore kept");
    Check(SanitiseName(std::string(kMaxNameLength, 'x')).has_value(), "longest name kept");

    for (const char* bad : { "", "   ", "..\\x", "a/b", "a\\b", "c:x", "a*b", "a?b", "a\"b", "a<b", "a|b", ".hidden",
                             "trailing.", "CON", "con", "nul.txt", "Com1", "LPT9.ini", "AUX", "PRN", "a\tb" })
        Check(!SanitiseName(bad).has_value(), std::string("name rejected: '") + bad + "'");

    Check(!SanitiseName(std::string(kMaxNameLength + 1, 'x')).has_value(), "too long rejected");
    Check(SanitiseName("CONSOLE").has_value(), "a name that only starts with a device name is kept");
    Check(SanitiseName("com10").has_value(), "COM10 is not a device name");

    const std::filesystem::path folder = std::filesystem::path("C:\\Games\\X\\OptiScaler") / kFolderName;
    const auto path = ProfilePath(folder, "NBA sharp");
    Check(path.has_value() && path->filename() == "NBA sharp.ini" && path->parent_path() == folder,
          "profile path is <folder>\\<name>.ini");
    Check(!ProfilePath(folder, "..\\..\\evil").has_value(), "a path name gives no path");

    const auto name = NameFromPath(std::filesystem::path("C:\\x\\NRProfiles\\Soft v1.2.ini"));
    Check(name == std::optional<std::string>("Soft v1.2"), "name read back from a file name");
    Check(!NameFromPath(std::filesystem::path("C:\\x\\NRProfiles\\notes.txt")).has_value(), "non-ini file ignored");
}

static void Keys()
{
    Check(IsKept("Passes") && IsKept("passes") && IsKept("PASSES"), "keys match case-insensitively (as the ini does)");
    Check(IsKept("Pass4Style") && IsKept("Pass30AutoMask") && IsKept("Pass17LocalTone"), "passes 4-30 kept");
    Check(!IsKept("Pass31Style") && !IsKept("Pass3x") && !IsKept("Pass4") && !IsKept("Pass04Style"),
          "not a pass key");
    Check(IsKept("WhitePointSource") && IsKept("NativeMotion") && IsKept("ColourEncoding") &&
              IsKept("FinishedPicture") && IsKept("SpatialCenterX"),
          "the game's own settings kept (a profile is shared for one game)");
    Check(!IsKept("Enabled") && !IsKept("ToggleKey") && !IsKept("DisplayPaperWhiteNits") && !IsKept("HoldFrame") &&
              !IsKept("NativeDebugView") && !IsKept("Compare"),
          "the player's own and session keys left out");
    Check(!IsKept("Unknown"), "unknown key left out");

    Check(SameGame("NBA2K27.exe", "nba2k27.EXE") && SameGame("", "x.exe") && !SameGame("Cyberpunk2077.exe", "x.exe"),
          "same game by exe name, a file without one counts as this game's");
    Check(NativeChanged({ { "NativeMotion", "true" } }, {}) && !NativeChanged({ { "NativeMotion", "auto" } }, {}) &&
              !NativeChanged({ { "Passes", "3" } }, { { "Passes", "2" } }),
          "only a Native key change counts as an Optical F5Low change");

    for (auto key : kKeptKeys)
        Check(!IsLeftOut(key), "key in both lists: " + std::string(key));

    Section s { { "Passes", "3" }, { "Enabled", "true" }, { "LutFile", "a.cube" }, { "Bogus", "1" } };
    const Section f = Filter(s);
    Check(f.size() == 2 && f.count("Passes") && f.count("LutFile"), "filter keeps only kept keys");

    Check(Same({ { "Passes", "3" } }, { { "Passes", "3" }, { "Enabled", "false" } }), "left-out keys do not count");
    Check(Same({ { "Passes", "auto" } }, {}), "missing = auto");
    Check(!Same({ { "Passes", "3" } }, { { "Passes", "2" } }), "different value differs");
    Check(Same({ { "passes", "3" } }, { { "Passes", "3" } }), "key case does not count");
}

// Every key SaveIni writes into [DlssNr] is kept or left out. The Pass{}... keys written in a loop are covered by the
// pass rule.
static void Coverage(const char* configCpp)
{
    std::ifstream in(configCpp);
    Check(in.good(), std::string("open ") + configCpp);
    if (!in.good())
        return;

    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    const std::regex written(R"re(SetValue\("DlssNr",\s*"([A-Za-z0-9]+)")re");
    std::set<std::string> keys;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), written); it != std::sregex_iterator(); ++it)
        keys.insert((*it)[1].str());

    Check(keys.size() > 100, "found the [DlssNr] keys SaveIni writes (" + std::to_string(keys.size()) + ")");

    for (const auto& key : keys)
        Check(IsKept(key) != IsLeftOut(key), "SaveIni key sorted into exactly one list: " + key);

    for (auto key : kKeptKeys)
        Check(keys.count(std::string(key)) == 1, "kept key is one SaveIni writes: " + std::string(key));

    // Config.cpp clears each kept field before a profile is read (set_from_config only fills an empty one): the clear
    // table must name every kept key, and only those.
    const std::regex cleared(R"re(NR_PROFILE_FIELD\("([A-Za-z0-9]+)")re");
    std::set<std::string> clearedKeys;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), cleared); it != std::sregex_iterator(); ++it)
        clearedKeys.insert((*it)[1].str());

    for (auto key : kKeptKeys)
        Check(clearedKeys.count(std::string(key)) == 1, "kept key cleared before a load: " + std::string(key));
    for (const auto& key : clearedKeys)
        Check(IsKept(key), "cleared key is a kept key: " + key);
}

int main(int argc, char** argv)
{
    Names();
    Keys();

    if (argc > 1)
        Coverage(argv[1]);
    else
        Check(false, "pass OptiScaler\\Config.cpp as the first argument");

    if (fails == 0)
        std::printf("nr_profiles_smoke: PASS\n");
    return fails == 0 ? 0 : 1;
}
