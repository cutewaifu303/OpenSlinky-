#pragma once
#include "events/module.hpp"

namespace slinky::modules {
events::Module* get_bridge_assist_module() noexcept;

// True while GodBridge has taken over: vanilla sneak ledge-clip keeps you on the edge until it places.
bool bridge_assist_wants_ledge_clip() noexcept;
} // namespace slinky::modules
