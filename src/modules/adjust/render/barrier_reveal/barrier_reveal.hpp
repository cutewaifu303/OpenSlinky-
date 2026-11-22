#pragma once

// Adjust "Barrier Reveal" (com.adjustclient.module.impl.render.BarrierRevealModule).

#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_barrier_reveal_module() noexcept;

} // namespace slinky::modules
