#pragma once

// OpenSlinky++ Settings popup - the bottom-right gear button shared by both ClickGUIs.
//
// Replaces the old "Slinky++ | Adjust" pill with a small "Settings" button. Clicking it opens a popup panel
// that holds the visual-style (theme) toggle and an "Alt Manager -> Open" row. Both ClickGUIs (the Slinky++
// one in src/gui/click_gui.cpp and the Adjust one in src/modules/adjust/clickgui/screen.cpp) drive it: they
// call render() during their draw and handle_click() when a left press happens. Hit rects and the passed
// cursor are both in the renderer's logical screen space.

#include "gui/gui_renderer.hpp"

namespace slinky::altmanager {

// Draws the gear button and, if the popup is open, the panel. `cursor_x/cursor_y` are logical screen coords
// (used only for hover highlight). Must be called every frame the switch would be drawn.
void settings_popup_render(gui::GuiRenderer* renderer, float cursor_x, float cursor_y) noexcept;

// Handles a left press at the given logical screen coords. Returns true if the popup/gear consumed it.
bool settings_popup_click(float cursor_x, float cursor_y) noexcept;

} // namespace slinky::altmanager
