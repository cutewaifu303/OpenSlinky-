#pragma once

#include "events/module.hpp"

namespace slinky::modules {

// Adjust "Block Overlay" (com.adjustclient.module.impl.render.blockoverlay). Adjust visual style only.
events::Module* get_adjust_block_overlay_module() noexcept;

// True while Block Overlay draws the looked-at block itself; the RenderGlobal.drawSelectionBoundingBox hook then
// skips Minecraft's own black selection outline (as Adjust's patched RenderGlobal does).
bool adjust_block_overlay_replaces_vanilla_outline() noexcept;

} // namespace slinky::modules
