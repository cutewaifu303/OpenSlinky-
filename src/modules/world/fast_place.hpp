#pragma once
#include "events/module.hpp"

namespace slinky::modules {
events::Module* get_fast_place_module() noexcept;

// Fast Place's configured maximum CPS (Bridge clicks at this rate while Fast Place is on).
float fast_place_get_max_cps() noexcept;
} // namespace slinky::modules
