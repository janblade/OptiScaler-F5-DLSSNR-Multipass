#pragma once

// The Optical F5Low debug views the menu offers (motion/FlowDebug_Hlsl.h draws them; the index is its `view`), with
// the key shown under each. From PR #232 (TechvibeCoder07).

#include <algorithm>

namespace native::FlowDebugViews
{

inline constexpr const char* kNames[] = {
    "Motion arrows",
    "Normals / depth",
    "Final optical flow",
    "Confidence / rejection",
    "Confidence channels (texture / fit / history)",
    "History rejection",
    "Still age",
};

inline constexpr const char* kLegends[] = {
    "Arrows point with the visible motion on screen. Hue = direction, length = speed (capped to the grid), in game "
    "pixels per frame.",
    "Left: normals from the game's depth. Right: the depth, with the view's linearization and multiplier. Magenta "
    "checker = no depth. The view's settings never change the depth the motion estimate gets; the alignment does.",
    "The motion Neural Rendering gets, after uncover and the surface guide. Hue = direction, brightness = speed, "
    "black = still.",
    "Green = strong support, orange = weak support or history rejected. Support comes from the texture, a checked "
    "weak-texture match or an agreeing camera fit, limited by how well the motion fits and by the history accepted. "
    "Evidence, not a probability.",
    "Red = texture structure, green = how well the motion fits the picture, blue = history trusted. Brighter = "
    "stronger. Three separate signals, not one score.",
    "White = the last frame's history is rejected, black = kept, grey = partly. The first frame and cuts reject all of "
    "it.",
    "Frames in a row the picture stayed still. Black = 0, white = the still threshold or more. It gates the camera "
    "candidate, not all motion.",
};

inline constexpr int kCount = (int) (sizeof(kNames) / sizeof(kNames[0]));
static_assert(sizeof(kLegends) / sizeof(kLegends[0]) == kCount, "a key for every view");

inline int Clamp(int view) { return std::clamp(view, 0, kCount - 1); }
inline const char* Name(int view) { return kNames[Clamp(view)]; }
inline const char* Legend(int view) { return kLegends[Clamp(view)]; }

} // namespace native::FlowDebugViews
