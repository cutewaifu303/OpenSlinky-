#pragma once

// Adjust "Block Places" (com.adjustclient.module.impl.render.BlockPlacesModule + PlacedBlock).

#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_block_places_module() noexcept;

} // namespace slinky::modules
