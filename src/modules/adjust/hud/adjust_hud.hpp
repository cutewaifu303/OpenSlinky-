#pragma once

// Adjust "HUD" module (com.adjustclient.module.impl.render.hud.HUDModule) for OpenSlinky++.
//
// It owns the Adjust theme (Color / Secondary / Radiance / Scale) and drives the HUD elements in Adjust's order:
//   watermark, toggled list, information, tab GUI, potion effects, armor, target HUD, widgets, notifications.
// Each element lives in its own hud_<name>.cpp and exposes a HudElement; the module renders them inside one
// adjust::begin_hud()/end_hud() frame and forwards ticks/events to them.
//
// Only available while the "Adjust" visual style is selected (kModuleFlagStyleAdjust).

#include <cstdint>

#include "adjust/kit.hpp"
#include "adjust/settings.hpp"
#include "events/events.hpp"
#include "events/module.hpp"

namespace slinky::gui {
struct ClickGui;
struct GuiRenderer;
}

namespace slinky::config {
struct ConfigArchive;
}

namespace slinky::modules {

events::Module* get_adjust_hud_module() noexcept;

namespace adjust_hud {

// ---- theme (HUDModule statics) ------------------------------------------------------------------------------

adjust::Argb hud_color() noexcept;                   // HUDModule.getHudColor() (incl. 185 ms colour transition)
adjust::Argb hud_color(int alpha) noexcept;          // HUDModule.getColor(alpha)
bool hud_color_rainbow() noexcept;                   // HUD > Color mode == Rainbow
bool secondary_active() noexcept;                    // HUDModule.isSecondaryActive()
adjust::Argb secondary_color() noexcept;             // HUD > Secondary
adjust::Argb secondary_for(adjust::Argb primary) noexcept; // ColorUtil.secondary(primary)
adjust::Argb secondary_or_darker(adjust::Argb primary) noexcept; // ToggledList: secondary, else primary * 0.8
float radiance() noexcept;                           // HUD > Radiance (0..345)
adjust::Argb theme_color_or(adjust::Argb fallback) noexcept; // ColorUtil.themeColor() (null -> fallback)
float requested_hud_scale() noexcept;                // HUDModule.getHudScale()
bool watermark_enabled() noexcept;                   // HUD > Other > Watermark

// Height (HUD units) occupied above the hotbar by the armor element (HUDModule.armorOffset), for elements that
// stack above it.
int armor_offset() noexcept;
void set_armor_offset(int offset) noexcept;

// Bottom edge (HUD units) of the toggled list (ToggledList.totalHeight/bounds), for elements placed below it.
float toggled_list_bottom() noexcept;
void set_toggled_list_bottom(float bottom) noexcept;

// ---- elements -----------------------------------------------------------------------------------------------

struct HudContext {
    events::GameContext* game = nullptr;   // may be nullptr in render if no world (elements must check)
    gui::GuiRenderer* renderer = nullptr;
    const adjust::HudFrame* frame = nullptr; // scaled resolution of the open frame (HUD units)
    float delta_time = 0.0f;               // seconds since the previous HUD frame
};

// All callbacks are optional (nullptr = not used). Config keys must be unique within the HUD module, so prefix
// them with the element name (e.g. "toggled_color").
struct HudElement {
    const char* name = nullptr;                                        // settings section header
    void (*render_settings)(gui::ClickGui*, gui::GuiRenderer*) = nullptr;
    void (*visit_config)(config::ConfigArchive*) = nullptr;
    void (*tick)(events::GameContext*) = nullptr;                      // kEventGameTick
    void (*render)(const HudContext&) = nullptr;                       // inside begin_hud / end_hud
    void (*on_module_state_changed)(events::ModuleStateChangedEvent*) = nullptr;
    void (*on_attack)(events::AttackOrUseEntityEvent*) = nullptr;      // kEventPreAttackOrUseEntity
    void (*on_main_thread_packet)(events::MainThreadPacketEvent*) = nullptr; // kEventMainThreadPacket
    void (*on_profile_action)(events::ProfileActionEvent*) = nullptr;
    void (*on_error_notification)(events::ErrorNotificationEvent*) = nullptr;
    void (*on_client_shutdown)() = nullptr;
    // kEventIncomingPacket - runs on the NETTY thread: only cheap checks (IsInstanceOf, atomics), no game state.
    void (*on_incoming_packet)(events::NettyPacketEvent*) = nullptr;
    // kEventRenderWorld2DProj (GlStateManager-synced GL pass) - for things that must be drawn by Minecraft's own
    // renderers (e.g. RenderItem icons), outside the NanoVG HUD frame. Runs late (priority -100).
    void (*on_render_world_2d)(events::RenderWorld2DProjEvent*) = nullptr;
};

// One accessor per element (hud_<name>.cpp).
const HudElement& element_watermark() noexcept;     // ui/hud/watermark
const HudElement& element_toggled() noexcept;       // ui/hud/toggled
const HudElement& element_information() noexcept;   // ui/hud/information
const HudElement& element_tabgui() noexcept;        // ui/tabgui + ui/hud/tabgui
const HudElement& element_potions() noexcept;       // ui/hud/potion
const HudElement& element_armor() noexcept;         // HUDModule armor (renderArmor / EnchantInfo)
const HudElement& element_target() noexcept;        // module/impl/render/hud/target
const HudElement& element_widgets() noexcept;       // module/impl/render/hud/widget
const HudElement& element_notifications() noexcept; // ui/hud/notification

} // namespace adjust_hud
} // namespace slinky::modules
