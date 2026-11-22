#pragma once

#include "events/module.hpp"

namespace slinky::modules {

// Adjust "Indicators" (com.adjustclient.module.impl.render.indicators). Adjust visual style only.
events::Module* get_adjust_indicators_module() noexcept;

} // namespace slinky::modules
