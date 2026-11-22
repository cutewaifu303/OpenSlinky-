#pragma once

// Setting capture for the Adjust Tab GUI.
//
// OpenSlinky++ modules expose their settings only through an immediate-mode `render_settings(gui, renderer)`
// callback (a sequence of `config::*Setting::render` and `gui::widgets::render_section_header` calls, with the
// visibility conditions written as plain `if`s). Adjust's Tab GUI needs the list of a module's settings, so it calls
// `render_settings` in a "capture" mode in which every setting only reports itself instead of drawing.
//
// This needs a two-line hook at the top of each `config::*Setting::render` (config/settings.cpp) and of
// `gui::widgets::render_section_header` (gui/widgets.cpp):
//
//     if (slinky::modules::adjust_hud::tabgui::capture_active()) {
//         slinky::modules::adjust_hud::tabgui::capture_item(slinky::modules::adjust_hud::tabgui::CaptureKind::Bool, this);
//         return;
//     }
//
// plus one call of `mark_capture_hook_installed()` (e.g. a namespace-scope static initializer in settings.cpp).
// Until that call happened, capture_module_settings() never invokes a module's render_settings (it would draw with a
// null ClickGui), so the Tab GUI then lists only the modules and their keybind rows.

#include <cstdint>
#include <vector>

namespace slinky::events {
struct Module;
}

namespace slinky::modules::adjust_hud::tabgui {

enum class CaptureKind : int32_t {
    Section = 0,     // gui::widgets::render_section_header - item = const char* title
    Bool = 1,        // config::BoolSetting*
    Float = 2,       // config::FloatSetting*
    Range = 3,       // config::RangeSetting*
    Mode = 4,        // config::ModeSetting*
    Color = 5,       // config::ColorSetting*
    DynamicList = 6, // config::DynamicListSetting*
};

struct CapturedItem {
    CaptureKind kind = CaptureKind::Section;
    const void* item = nullptr;
};

// ---- hook side (called from config/settings.cpp and gui/widgets.cpp) ------------------------------------------

// True while a capture is running: the caller must report itself with capture_item() and return without drawing.
bool capture_active() noexcept;
void capture_item(CaptureKind kind, const void* item) noexcept;

// Declares that the hooks above are compiled in. Must be called once (any time before the Tab GUI is used).
void mark_capture_hook_installed() noexcept;
bool capture_hook_installed() noexcept;

// ---- Tab GUI side ---------------------------------------------------------------------------------------------

// Runs `mod->render_settings(nullptr, nullptr)` in capture mode and returns the visible settings in display order.
// Returns false (and leaves `out` empty) if the hook is not installed, the module has no settings or a capture is
// already running.
bool capture_module_settings(events::Module* mod, std::vector<CapturedItem>& out) noexcept;

} // namespace slinky::modules::adjust_hud::tabgui
