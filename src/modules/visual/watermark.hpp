#pragma once
#include "events/module.hpp"

namespace slinky::modules {
events::Module* get_watermark_module() noexcept;

// Bottom edge (logical px) of the top-left watermark, or 0 when it is hidden.
// Notifications use it to stack their toasts below the watermark.
float watermark_hud_bottom() noexcept;
} // namespace slinky::modules
