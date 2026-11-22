#pragma once
#include "events/module.hpp"

namespace slinky::modules {
events::Module* get_bridge_module() noexcept;

// True while Bridge wants vanilla's sneak ledge-clip applied to the local player (queried by
// hook_should_clip_ledge, same path Safe Walk uses). Stops you at the edge in the same move.
bool bridge_wants_ledge_clip() noexcept;
} // namespace slinky::modules
