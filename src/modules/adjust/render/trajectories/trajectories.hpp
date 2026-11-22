#pragma once

#include "events/module.hpp"

namespace slinky::modules {

// Adjust "Trajectories" (com.adjustclient.module.impl.render.trajectories). Adjust visual style only.
events::Module* get_adjust_trajectories_module() noexcept;

} // namespace slinky::modules
