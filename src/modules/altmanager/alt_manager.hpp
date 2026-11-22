#pragma once

// OpenSlinky++ Alt Manager - the full-screen account manager overlay.
//
// A self-contained screen rendered on the NanoVG GUI layer (kEventGuiRender2D) whenever it is open, so it
// works both in-game and in the main menu. It is driven as a module (always enabled, no toggle) purely so its
// render / frame-poll callbacks run; the actual visibility is the internal `open` flag.
//
// Opened from the Settings popup (bottom-right of the ClickGUI) or by Right Shift in the main menu. Input is
// fed in from the Win32 WndProc path (gui_handle_window_input) before the ClickGUI sees it.

#include "events/module.hpp"
#include "util/input_win32.hpp"

namespace slinky::altmanager {

// Registry entry (always enabled; renders only while open).
events::Module* get_alt_manager_module() noexcept;

void open() noexcept;
void close() noexcept;
bool is_open() noexcept;

// True while no world is loaded (updated every frame from the GUI frame-poll). Used to decide whether Right
// Shift opens the Alt Manager (menu) or the ClickGUI (in-game).
bool is_in_menu() noexcept;

// Feeds one Win32 input event to the overlay. Returns true if it was consumed (caller must not forward it).
bool handle_input(const input::InputEvent* ev, double now) noexcept;

} // namespace slinky::altmanager
