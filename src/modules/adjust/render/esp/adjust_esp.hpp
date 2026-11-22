#pragma once

// Adjust "ESP" (com.adjustclient.module.impl.render.esp.ESPModule) for OpenSlinky++.
//
// Screen-space boxes (border, health / armor / durability / countdown bars), name tags (name, health, absorption,
// distance), held item names, equipment and potion icons, player flags and pooled item labels for players,
// teammates/friends, hostile and passive mobs, dropped items and primed TNT, in the three Adjust themes
// (Minecraft / Pixel / Adjust). Drawn in kEventRenderWorld2DProj: rectangles and Tahoma text through NanoVG (the
// Adjust NvgDraw/NvgFontRenderer port in adjust/kit.hpp), Minecraft-font text, item and potion icons through
// Minecraft's own renderers, in Adjust's order (items, NanoVG flush, then the deferred Minecraft-font text).
//
// Only available while the "Adjust" visual style is selected (kModuleFlagStyleAdjust). 1.8.9 only.

#include <jni.h>

#include "events/events.hpp"
#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_esp_module() noexcept;

// ESPModule.acceptsPlayer(EntityNameRenderEvent): true when Adjust ESP draws its own name tag for `entity` and the
// vanilla one must be suppressed (any enabled player group with Nametags enabled, bots excluded).
bool adjust_esp_hides_vanilla_name(events::JniContext* jni, jobject entity) noexcept;

} // namespace slinky::modules
