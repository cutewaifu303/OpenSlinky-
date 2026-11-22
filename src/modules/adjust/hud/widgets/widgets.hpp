#pragma once

// Internal API of the Adjust HUD widgets (com.adjustclient.module.impl.render.hud.widget.*), used by
// hud_widgets.cpp. One translation unit per widget:
//
//   hud_widget.cpp                   HudWidget (panel + title frame, default positions of HudWidgets)
//   radar_widget.cpp                 RadarWidget / RadarGroup / RadarTarget
//   session_information_widget.cpp   SessionInformationWidget (+ the SessionService time tracking it reads)
//   click_speed_widget.cpp           ClickSpeedWidget / ClickTracker
//   memory_usage_widget.cpp          MemoryUsageWidget
//
// Adjust widgets are Movables that can be dragged in Adjust's HUD editor. OpenSlinky++ has no HUD editor, so every
// widget stays at the default position HudWidgets.createMovable() registers (all of them resolve to the TOP_LEFT
// anchor area, i.e. absolute HUD units).

#include "modules/adjust/hud/adjust_hud.hpp"

namespace slinky::modules::adjust_hud::widgets {

// ---- HudWidget ----------------------------------------------------------------------------------------------

// Default positions (HudWidgets.createMovable).
inline constexpr float kRadarX = 2.0f;
inline constexpr float kRadarY = 150.0f;
inline constexpr float kSessionX = 2.0f;
inline constexpr float kSessionY = 250.0f;
inline constexpr float kClickSpeedX = 350.0f;
inline constexpr float kClickSpeedY = 250.0f;
inline constexpr float kMemoryX = 350.0f;
inline constexpr float kMemoryY = 280.0f;

// Size of a widget's movable after HudWidget.drawFrame (width x content height + 9 header).
struct WidgetBox {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f; // content height + 9
};

// HudWidget.drawFrame: stores the size, draws the panel (alpha 140) and the centred Tahoma 10 title.
WidgetBox draw_frame(const char* name, float x, float y, float width, float height) noexcept;

// HudWidget.drawPanel.
void draw_panel(int alpha, float x, float y, float width, float height) noexcept;

// java.text.DecimalFormat("#.##") (NumberSetting.FORMAT).
void format_number(double value, char* out, size_t out_size) noexcept;

// ---- RadarWidget --------------------------------------------------------------------------------------------

void radar_render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void radar_visit_config(config::ConfigArchive* ar) noexcept;
void radar_tick(events::GameContext* ctx) noexcept;
void radar_render(const HudContext& ctx) noexcept;
void radar_shutdown() noexcept;

// ---- SessionInformationWidget -------------------------------------------------------------------------------

void session_render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void session_visit_config(config::ConfigArchive* ar) noexcept;
void session_tick(events::GameContext* ctx) noexcept;
void session_render(const HudContext& ctx) noexcept;
void session_shutdown() noexcept;

// ---- ClickSpeedWidget ---------------------------------------------------------------------------------------

void click_speed_render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void click_speed_visit_config(config::ConfigArchive* ar) noexcept;
void click_speed_on_attack(events::AttackOrUseEntityEvent* ev) noexcept;
void click_speed_render(const HudContext& ctx) noexcept;

// ---- MemoryUsageWidget --------------------------------------------------------------------------------------

void memory_render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void memory_visit_config(config::ConfigArchive* ar) noexcept;
void memory_render(const HudContext& ctx) noexcept;

} // namespace slinky::modules::adjust_hud::widgets
