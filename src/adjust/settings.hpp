#pragma once

// Adjust-style settings on top of OpenSlinky++'s config/settings types.
//
// Adjust's ColorSetting is a colour plus a mode (Plain / Rainbow / Main Color = the HUD colour). OpenSlinky++'s
// ColorSetting is RGB only, so ColorOption pairs it with a ModeSetting. Use it for every colour you port.

#include "adjust/kit.hpp"
#include "config/settings.hpp"

namespace slinky::adjust {

inline constexpr const char* kColorModeOptions[3] = {
    "Plain",
    "Rainbow",
    "Main Color",
};

enum class ColorMode : int32_t {
    Plain = 0,
    Rainbow = 1,
    Main = 2,
};

struct ColorOption {
    config::ColorSetting color;
    config::ModeSetting mode;
    int alpha = 255;

    // `label` = colour label, `mode_label` = label of its mode dropdown (e.g. "Color" / "Color mode").
    ColorOption(const char* label, const char* mode_label, Argb default_color,
                ColorMode default_mode = ColorMode::Plain) noexcept
        : color{label, to_color3f(default_color), to_color3f(default_color)},
          mode{{}, mode_label, 3, 0, kColorModeOptions, static_cast<int32_t>(default_mode),
               static_cast<int32_t>(default_mode), false, {}},
          alpha(alpha_of(default_color)) {}

    ColorMode current_mode() const noexcept { return static_cast<ColorMode>(mode.value); }

    // ColorSetting.getColor(): Rainbow -> ColorUtil.rainbow(0, 0.45, 0.8); Main -> HUD colour; Plain -> value.
    Argb get() const noexcept;

    // Plain value with its alpha (ignores the mode).
    Argb plain() const noexcept { return from_color3f(color.value, alpha); }

    void render(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
        mode.render(gui, renderer);
        if (current_mode() == ColorMode::Plain) {
            color.render(gui, renderer);
        }
    }

    // Two JSON keys: the colour and its mode.
    void visit(config::ConfigArchive* ar, const char* color_key, const char* mode_key) noexcept {
        color.visit(ar, color_key);
        mode.visit(ar, mode_key);
    }
};

} // namespace slinky::adjust
