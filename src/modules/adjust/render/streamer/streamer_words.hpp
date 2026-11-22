#pragma once

// Word lists used by the Adjust Streamer name generators (com.adjustclient.auth.RandomNames).

#include <cstddef>

namespace slinky::modules::adjust_streamer {

extern const char* const kAdjectives[]; // RandomNames.adjectives
extern const size_t kAdjectivesCount;
extern const char* const kNouns[];      // RandomNames.nouns
extern const size_t kNounsCount;
extern const char* const kNames[];      // RandomNames.names
extern const size_t kNamesCount;

} // namespace slinky::modules::adjust_streamer
