#pragma once

#include <cstdint>

// Which frame generation output OptiScaler makes (State::activeFgOutput). Its own header, so the pure HDR output rules
// (output/SdrToHdr_Create.h) can map it on the host.
enum class FGOutput : uint32_t
{
    NoFG,
    FSRFG,
    DLSSG,
    XeFG,
};
