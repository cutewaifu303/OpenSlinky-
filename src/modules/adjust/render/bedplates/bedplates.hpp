#pragma once

#include "events/module.hpp"

namespace slinky::modules {

// Adjust "Bed Plates" (com.adjustclient.module.impl.render.bedplates). Adjust visual style only.
events::Module* get_adjust_bedplates_module() noexcept;

} // namespace slinky::modules
