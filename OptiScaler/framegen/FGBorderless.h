#pragma once

#include <atomic>

// The pure parts of keeping a frame generation game borderless (hooks/FG_Hooks.h), so a host test can build them.

// A library chain asked for as exclusive full screen may be made windowed (and kept borderless) unless the player
// switched FGXeFGForceBorderless off on purpose
inline bool FgWindowable(bool playerOptionSet, bool playerOptionValue) { return !playerOptionSet || playerOptionValue; }

// The game is kept borderless when the player asked for it (FGXeFGForceBorderless) or HDR output keeps it for its
// windowed chain
inline bool BorderlessForced(bool playerOption, bool hdrKeeps) { return playerOption || hdrKeeps; }

// "HDR output keeps this window borderless": set with the frame generation library's windowed HDR chain, cleared when
// that chain stands down or is released. Owned by the chain, so a library that makes its new chain before the old one
// is released does not lose the state. The player's own option is never touched.
class HdrBorderlessKeeper
{
  public:
    void Keep(const void* owner) { _owner.store(owner); }

    // True when `owner` was the one keeping it (and it is not any more)
    bool Release(const void* owner)
    {
        const void* expected = owner;
        return owner != nullptr && _owner.compare_exchange_strong(expected, nullptr);
    }

    bool Active() const { return _owner.load() != nullptr; }

  private:
    std::atomic<const void*> _owner { nullptr };
};
