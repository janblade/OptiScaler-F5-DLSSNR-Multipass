#pragma once

// Optical F5Low motion profiles: named sets of the motion estimate's knobs (OpticalFlowDx12::Settings and
// TrustMaskDx12::Settings) for a kind of game, picked on the menu's Optical F5Low page and kept per game in the ini
// ([DlssNr] NativeMotionProfile, NativeMotionProfileCustom). A profile only sets existing knobs; Default is the structs'
// own defaults. Custom is the Flow tuning menu's values, saved as a key=value list. No D3D, no ImGui, no Config: the
// knob functions are templates over the two settings structs, so Config.h and a host test can include this.
//
// A genre profile ships only when it beats Default on its own scene of tests/nr_flow_genres_gpu.cpp and costs every
// other scene and flow test no more than 0.01 (plan 2026-10-16-f5low-motion-profiles); until then its knob list is
// empty and the menu leaves it out.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>

namespace native::motion
{
enum class Profile : uint32_t
{
    Default,
    Anime,
    Realistic,
    SportsWide,
    SportsPan,
    Custom
};

inline constexpr Profile kAllProfiles[] = { Profile::Default,    Profile::Anime,     Profile::Realistic,
                                            Profile::SportsWide, Profile::SportsPan, Profile::Custom };

inline const char* ProfileIniText(Profile profile)
{
    switch (profile)
    {
    case Profile::Anime:
        return "anime";
    case Profile::Realistic:
        return "realistic";
    case Profile::SportsWide:
        return "sportswide";
    case Profile::SportsPan:
        return "sportspan";
    case Profile::Custom:
        return "custom";
    default:
        return "default";
    }
}

// The ini word (lower case, as Config reads it); anything else is Default
inline Profile ProfileFromIniText(const std::string& text)
{
    for (const Profile profile : kAllProfiles)
        if (text == ProfileIniText(profile))
            return profile;

    return Profile::Default;
}

inline const char* ProfileMenuName(Profile profile)
{
    switch (profile)
    {
    case Profile::Anime:
        return "Anime / cel-shaded";
    case Profile::Realistic:
        return "Realistic";
    case Profile::SportsWide:
        return "Sports, wide camera";
    case Profile::SportsPan:
        return "Sports, side pan";
    case Profile::Custom:
        return "Custom (Flow tuning)";
    default:
        return "Default";
    }
}

// The knobs a genre profile sets over Default, as a key=value list (keys of VisitKnobs). Empty: no profile yet.
inline const char* ProfileKnobs(Profile profile)
{
    // None has passed its gate yet (S2-S5 of the plan add them here, one case per genre)
    (void) profile;
    return "";
}

// Offered in the menu: Default, Custom, and a genre profile once it has knobs
inline bool ProfileOffered(Profile profile)
{
    return profile == Profile::Default || profile == Profile::Custom || ProfileKnobs(profile)[0] != '\0';
}

// Every knob a profile or Custom may set: fn(key, value&, lo, hi) per knob, value an int, float or bool member. The keys
// are the ones tests/nr_flow_genres_gpu.cpp takes on its command line. lo/hi bound what an ini or a profile can set
// (the menu's slider ranges, or what the shaders accept).
template <class Flow, class Trust, class Fn> void VisitKnobs(Flow& f, Trust& t, Fn&& fn)
{
    fn("radius", f.radius, 1.0f, 3.0f);
    fn("coarse", f.coarseRadius, 1.0f, 8.0f);
    fn("lambda", f.lambda, 0.0f, 1.0f);
    fn("history", f.useHistory, 0.0f, 1.0f);
    fn("cells", f.coarseCells, 1.0f, 9.0f);
    fn("smooth", f.smoothRadius, 0.0f, 4.0f);
    fn("knee", f.confidenceKnee, 0.0005f, 0.05f);
    fn("dmatch", f.depthMatching, 0.0f, 1.0f);
    fn("global", f.globalCandidate, 0.0f, 1.0f);
    fn("inverse", f.inverseRefinement, 0.0f, 1.0f);
    fn("scenecut", f.sceneCutDetector, 0.0f, 1.0f);
    fn("luma", f.perceptualLuma, 0.0f, 1.0f);
    fn("zero", f.zeroMargin, 0.0f, 0.1f);
    fn("zeroreach", f.zeroReach, 0.0f, 64.0f);
    fn("still", f.stillFrames, 0.0f, 255.0f);
    fn("stilleps", f.stillEpsilon, 0.0005f, 0.05f);
    fn("stillmargin", f.stillMargin, 0.0f, 0.2f);
    fn("look", f.lookWeights, 0.0f, 1.0f);
    fn("lookrange", f.lookRange, 0.001f, 1.0f);
    fn("lookdist", f.lookDistance, 0.5f, 64.0f);
    fn("depthtol", t.depthTolerance, 0.0f, 1.0f);
    fn("flowtol", t.flowTolerance, 0.0f, 16.0f);
    fn("lumatol", t.lumaTolerance, 0.0f, 1.0f);
    fn("revealtol", t.revealTolerance, 0.0f, 1.0f);
    fn("decay", t.decay, 0.0f, 1.0f);
    fn("surface", t.guideSurface, 0.0f, 1.0f);
    fn("uncover", t.uncoverBackground, 0.0f, 1.0f);
    fn("reach", t.uncoverReach, 0.0f, 64.0f);
}

namespace detail
{
template <class V> void Store(V& member, float v, float lo, float hi)
{
    v = v < lo ? lo : (v > hi ? hi : v);

    if constexpr (std::is_same_v<V, bool>)
        member = v != 0.0f;
    else if constexpr (std::is_integral_v<V>)
        member = (V) std::lround(v);
    else
        member = v;
}

// The shortest text that reads back as the same float
inline std::string FloatText(float v)
{
    char text[32];

    for (int digits = 6; digits <= 9; ++digits)
    {
        std::snprintf(text, sizeof(text), "%.*g", digits, v);

        if (std::strtof(text, nullptr) == v)
            break;
    }

    return text;
}
} // namespace detail

// One knob by key (bounded to its range); false for an unknown key
template <class Flow, class Trust> bool SetKnob(std::string_view key, float v, Flow& f, Trust& t)
{
    bool found = false;
    VisitKnobs(f, t,
               [&](const char* name, auto& member, float lo, float hi)
               {
                   if (!found && key == name)
                   {
                       detail::Store(member, v, lo, hi);
                       found = true;
                   }
               });
    return found;
}

// A key=value list, entries split by commas, spaces or semicolons. Unknown keys and values that are not numbers are
// skipped. Returns how many knobs were set.
template <class Flow, class Trust> int ParseKnobs(std::string_view text, Flow& f, Trust& t)
{
    int set = 0;
    size_t at = 0;

    while (at < text.size())
    {
        const size_t end = text.find_first_of(", ;", at);
        const std::string_view entry = text.substr(at, end == std::string_view::npos ? text.size() - at : end - at);
        at = end == std::string_view::npos ? text.size() : end + 1;

        const size_t eq = entry.find('=');

        if (eq == std::string_view::npos || eq == 0)
            continue;

        const std::string value(entry.substr(eq + 1));
        char* stop = nullptr;
        const float v = std::strtof(value.c_str(), &stop);

        if (value.empty() || stop == nullptr || *stop != '\0' || !std::isfinite(v))
            continue;

        if (SetKnob(entry.substr(0, eq), v, f, t))
            ++set;
    }

    return set;
}

// Every knob as a key=value list, comma separated (bools as 0/1); ParseKnobs reads it back to the same values
template <class Flow, class Trust> std::string WriteKnobs(const Flow& f, const Trust& t)
{
    std::string out;
    VisitKnobs(const_cast<Flow&>(f), const_cast<Trust&>(t),
               [&](const char* name, auto& member, float, float)
               {
                   using V = std::remove_reference_t<decltype(member)>;

                   if (!out.empty())
                       out += ',';

                   out += name;
                   out += '=';

                   if constexpr (std::is_same_v<V, bool>)
                       out += member ? '1' : '0';
                   else if constexpr (std::is_integral_v<V>)
                       out += std::to_string(member);
                   else
                       out += detail::FloatText(member);
               });
    return out;
}

// The profile's settings: both structs back to their defaults (the trust mask's debug view, a menu choice, is kept),
// then the profile's knobs, or for Custom the saved list. A genre profile without knobs is Default.
template <class Flow, class Trust>
void ApplyProfile(Profile profile, std::string_view custom, Flow& f, Trust& t)
{
    const int debugView = t.debugView;
    f = Flow {};
    t = Trust {};
    t.debugView = debugView;
    ParseKnobs(profile == Profile::Custom ? custom : std::string_view(ProfileKnobs(profile)), f, t);
}
} // namespace native::motion
