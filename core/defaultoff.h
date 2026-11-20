// Mods that are off unless the config asks for them ("<mod>=1"), per platform. Shared by core (modEnabled, core.cpp)
// and the patcher (patcher/patcher.cpp), so both agree on what an empty config means.
//
// A mod that has only been checked statically on a platform goes here until it has run there (autocue and genres
// were, on Windows, until 2026-11-17). Empty now: every mod is on by default.
#pragma once
#include <string.h>

namespace rbm {
inline bool offUnlessAsked(const char* mod) {
    (void)mod;
    return false;
}
}  // namespace rbm
