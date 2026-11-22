#pragma once

// Item icons of the Adjust HUD "Armor" element (hud_armor.cpp).
//
// Adjust draws the held item and the armor pieces with Minecraft's RenderItem inside its 2D GUI pass. The
// OpenSlinky++ HUD layer is NanoVG in an offscreen buffer composited at Display.update, where RenderItem cannot be
// used safely (the GlStateManager cache would desync from the glPushAttrib/glPopAttrib around the GUI layer). The
// only existing event with a Minecraft-consistent GL state and an orthographic pixel projection is
// kEventRenderWorld2DProj (the same pass the Name Tags module renders its equipment icons in), so the icons are drawn
// there, at the positions the Armor element computed in its last HUD frame; the enchantment / durability text stays
// on the NanoVG layer, on top of the icons.
//
// The Adjust HUD module has to forward kEventRenderWorld2DProj to this function (see the porting report).

#include "events/events.hpp"

namespace slinky::modules::adjust_hud {

// Draws the item icons (RenderItem.renderItemAndEffectIntoGUI + renderItemOverlayIntoGUI). Call from a
// kEventRenderWorld2DProj handler; no-op when the Armor element is off / not visible or on 1.7.10.
void armor_render_items(events::RenderWorld2DProjEvent* ev) noexcept;

} // namespace slinky::modules::adjust_hud
