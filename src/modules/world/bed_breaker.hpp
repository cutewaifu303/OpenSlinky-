#pragma once
#include "events/module.hpp"

namespace slinky::modules {
events::Module* get_bed_breaker_module() noexcept;

// True while Bed Breaker drives the mining with its built-in Fast Mine; the Fast Mine module then stays out
// of the way so the same block is never boosted twice.
bool bed_breaker_owns_fast_mine() noexcept;
} // namespace slinky::modules
