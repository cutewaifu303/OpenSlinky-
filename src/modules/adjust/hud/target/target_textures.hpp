#pragma once

// Minecraft textures for the Adjust Target HUD, drawn through NanoVG.
//
// Adjust draws the player head (RenderUtil.drawHead: face + hat layer of the skin) and the held item / armor
// (RenderItem.renderItemIntoGUI) with plain GL after its NanoVG frame. OpenSlinky++ draws the HUD into an
// offscreen NanoVG layer, so both are reproduced with NanoVG image patterns that wrap Minecraft's own GL
// textures (nvglCreateImageFromHandleGL3, NVG_IMAGE_NODELETE):
//   - head: the skin texture of the player's NetworkPlayerInfo (default skin as fallback);
//   - items: the particle sprite of the item's baked model on the blocks/items atlas, tinted with
//     Item.getColorFromItemStack(stack, 0). Flat items (swords, armor, tools...) look identical; block items
//     show their particle face instead of the 3D block, and the enchantment glint is not drawn.
//
// resolve_* must run on the client thread outside a NanoVG frame (they may load the skin texture);
// draw_* run inside the HUD frame. 1.8.9 only - on other versions resolve_* return "not available".

#include <cstdint>
#include <jni.h>

#include "adjust/kit.hpp"
#include "events/events.hpp"

namespace slinky::modules::adjust_hud::target {

struct Sprite {
    uint32_t texture = 0;                 // GL texture name (0 = nothing to draw)
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    adjust::Argb tint = 0xFFFFFFFFu;      // Item.getColorFromItemStack(stack, 0), opaque
};

// GL texture of the player's skin (0 if unavailable).
uint32_t resolve_skin_texture(events::JniContext* jni, jobject minecraft, jobject player) noexcept;

// Particle sprite of `stack` (texture 0 if unavailable or stack is null).
Sprite resolve_item_sprite(events::JniContext* jni, jobject minecraft, jobject stack) noexcept;

// RenderUtil.drawHead at half scale: 8x8 face + hat layer of a 64x64 skin in a 16x16 HUD-unit square.
void draw_head(uint32_t skin_texture, float x, float y, adjust::Argb tint) noexcept;

// Item sprite in a size x size square, colour = sprite tint with `alpha`.
void draw_item(const Sprite& sprite, float x, float y, float size, int alpha) noexcept;

// Drops cached NanoVG image handles once the cache grows large. Call outside a NanoVG frame.
void trim_image_cache() noexcept;

// Forgets every cached handle (shutdown).
void reset_image_cache() noexcept;

} // namespace slinky::modules::adjust_hud::target
