#pragma once

// Adjust "Crosshair" (com.adjustclient.module.impl.render.CrosshairModule + CrosshairMode + CrosshairFadeCondition).

#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_crosshair_module() noexcept;

// True while the Adjust crosshair is running. Phase-2 hook: GuiIngame.renderGameOverlay must skip vanilla's
// crosshair (drawTexturedModalRect(i / 2 - 7, j / 2 - 7, 0, 0, 16, 16)) while this returns true.
bool adjust_crosshair_replaces_vanilla() noexcept;

} // namespace slinky::modules
