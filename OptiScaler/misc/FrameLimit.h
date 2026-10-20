#pragma once
#include "SysUtils.h"

class FrameLimit
{
    static uint64_t get_timestamp();
    static int timer_sleep(int64_t hundred_ns);
    static int busywait_sleep(int64_t ns);
    static int combined_sleep(int64_t ns);

  public:
    // The cap counts shown frames: `shownPerRendered` is how many frame generation shows per frame the game renders (1 when
    // it generates none)
    static void sleep(uint32_t shownPerRendered = 1);

    // The shown frame rate to cap at, 0: none. The FPS limit, or XeFG's Dynamic MFG target while it runs with "Use
    // target as cap" when that is lower. Every cap reader takes it from here (this limiter, PlanCap's inputs, the
    // game's XeLL); who keeps the cap stays PlanCap's decision.
    static float CapFps();

    // The time this limiter slept since the last call (XeFG's Dynamic MFG: the frame time the game doesn't need)
    static double TakeSleptMs();
};
