#pragma once

// Adjust ClickGUI for OpenSlinky++ (port of com.adjustclient.ui.clickgui.** and module/impl/render/ClickGui).
//
// While the "Adjust" visual style is selected, the GUI module (src/modules/visual/gui.cpp) shows this menu instead
// of the OpenSlinky++ one: draggable category frames (events::ModuleCategory, modules of the active style only) with
// module rows, expandable setting rows (the modules' real config:: settings, enumerated with the Tab GUI setting
// capture), the search bar, bind overlay, the "Configuration" frame (OpenSlinky++ profiles) and the tiny
// "Slinky++ | Adjust" visual-style switch in the bottom-right corner.
//
// The menu's own settings (Adjust HUD > Click GUI: Scale, Separators) live in the module "Click GUI"
// (get_adjust_click_gui_module(), Adjust style only, config key "Adjust Click GUI").
//
// Integration (gui.cpp), all calls on the render / window thread:
//   - GUI opened  (kEventModuleStateChanged, enabled)  -> on_open(time::now_seconds())
//   - GUI closed  (kEventModuleStateChanged, disabled) -> on_close()
//   - kEventGuiRender2D                               -> render(ev); afterwards, if consume_close_request() returns
//                                                        true, disable the GUI module (the fade-out has finished)
//   - gui_handle_window_input                         -> handle_input(event, now) first (true = consumed); the GUI
//                                                        keybind while open -> request_close() instead of disabling
//                                                        the module directly (plays Adjust's fade-out)

#include "events/events.hpp"
#include "events/module.hpp"
#include "util/input_win32.hpp"

namespace slinky::modules {

// "Click GUI" settings module (Adjust style only, no on/off state).
events::Module* get_adjust_click_gui_module() noexcept;

namespace adjust_clickgui {

void on_open(double now) noexcept;
void on_close() noexcept;

// Draws the whole menu on the OpenSlinky++ GUI layer (opens its own adjust::begin_hud frame).
void render(events::GuiRender2DEvent* ev) noexcept;

// Feeds one window input event. Returns true if the event was consumed by the menu (do not toggle / pass it on).
// ESC is always consumed: it closes an overlay / dialog / the search, or starts the closing fade.
bool handle_input(const input::InputEvent* event, double now) noexcept;

// True while a text field or the bind overlay owns the keyboard (keys must not reach the game).
bool wants_keyboard() noexcept;

// Starts the closing fade (Adjust PopupTransition.close); consume_close_request() turns true when it has finished.
void request_close() noexcept;

// True once after the closing fade has finished: the caller must now disable the GUI module.
bool consume_close_request() noexcept;

} // namespace adjust_clickgui
} // namespace slinky::modules
