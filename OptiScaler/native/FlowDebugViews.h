#pragma once
#include <algorithm>
namespace native::FlowDebugViews {
inline constexpr const char* names[]={"Motion arrows","Normals / depth","Final optical flow","Confidence / rejection","Confidence channels (texture / fit / history)","History rejection","Still age"};
inline constexpr const char* legends[]={"Arrows point with visible screen motion. Hue = direction; length = speed, capped to the grid. Units: game pixels/frame.","Left: DisplayDepth normals reconstructed from the original depth with linear sampling and ReShade UV calibration. Right: depth, using the selected preview linearization and multiplier. The split preserves scene coordinates. Magenta checker means no depth. Preview controls never transform the raw depth delivered to DLSS/NR.","Final consumer motion, after uncover and surface guide. Hue = visible motion direction; brightness emphasizes speed. Black = zero motion.","Green = strong support; orange = weak support or rejection. Support grows from 0 (weak) to 1 (strong). Texture structure, validated weak-texture refinement, or an agreeing region/camera proposal supplies motion support, limited by photometric fit and accepted history. Evidence display, not a probability. History rejection is available separately.","Red = texture structure; green = local photometric fit; blue = trusted history. Brighter = stronger. These are separate signals, not a combined probability.","White = reject previous history; black = accept; grey = partial rejection, including fading remembered rejection. First frame and cuts reject all history.","Consecutive image frames with change below the still threshold. Black = 0; white = configured still-frame threshold or older. This gates the global candidate, not all motion."};
inline constexpr int modes[]={7,6,0,18,5,17,12};
inline constexpr int Count=7;
inline int Mode(int selection){return modes[std::clamp(selection,0,Count-1)];}
inline const char* Legend(int selection){return legends[std::clamp(selection,0,Count-1)];}
}
