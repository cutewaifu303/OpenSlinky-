#include "modules/visual/watermark.hpp"

#include "config/settings.hpp"
#include "core/branding.hpp"
#include "gui/gui_renderer.hpp"
#include "gui/widgets.hpp"
#include "modules/module_interop.hpp"
#include "nanovg.h"
#include "util/math.hpp"

// Small "OpenSlinky++" watermark in the top-left HUD corner ("++" in the GUI accent color).
//
// The module is always running (no toggle switch, no keybind) and stores its single setting in the
// global settings file, so loading any profile - including ones made with the original Slinky client,
// which have no "Watermark" entry - never hides it. Missing keys fall back to the defaults.

namespace slinky::modules {

namespace {

constexpr float kMargin   = 6.0f;
constexpr float kPadX     = 6.0f;
constexpr float kPadY     = 3.0f;
constexpr float kFontSize = 13.0f;
constexpr float kRounding = 4.0f;

config::BoolSetting s_show{{}, "Show watermark", true, true, {}};

bool s_drawn_visible = false;    // visibility baked into the cached HUD buffer
math::Color3f s_drawn_accent{};  // accent color baked into the cached HUD buffer
float s_bottom = kMargin + kFontSize + kPadY * 2.0f; // refined after the first draw

bool same_color(const math::Color3f& a, const math::Color3f& b) noexcept {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

// Keeps the HUD layer visible while shown and requests one redraw whenever what we drew is stale.
void on_gui_frame_poll(void* raw_event) noexcept {
    auto* ev = static_cast<events::GuiFramePollEvent*>(raw_event);
    const bool show = s_show.value;
    if (show) {
        ev->render_flags |= 1u;
    }
    if (show != s_drawn_visible || (show && !same_color(gui_get_accent_color(), s_drawn_accent))) {
        ev->render_flags |= 3u;
    }
}

void on_gui_render_2d(void* raw_event) noexcept {
    const auto* ev = static_cast<const events::GuiRender2DEvent*>(raw_event);
    const math::Color3f accent = gui_get_accent_color();
    s_drawn_visible = s_show.value;
    s_drawn_accent = accent;
    if (!s_show.value) {
        return;
    }

    gui::GuiRenderer* renderer = ev->renderer;
    NVGcontext* vg = renderer->vg;

    nvgFontSize(vg, kFontSize);
    renderer->select_font(gui::kFontBold);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

    float ascender = 0.0f;
    float descender = 0.0f;
    float line_h = 0.0f;
    nvgTextMetrics(vg, &ascender, &descender, &line_h);
    const float base_w = nvgTextBounds(vg, 0.0f, 0.0f, branding::kWatermarkBase, nullptr, nullptr);
    const float suffix_w = nvgTextBounds(vg, 0.0f, 0.0f, branding::kWatermarkSuffix, nullptr, nullptr);

    const float box_w = base_w + suffix_w + kPadX * 2.0f;
    const float box_h = line_h + kPadY * 2.0f;
    s_bottom = kMargin + box_h;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, kMargin, kMargin, box_w, box_h, kRounding);
    nvgFillColor(vg, nvgRGBAf(0.0f, 0.0f, 0.0f, 0.6f));
    nvgFill(vg);

    const float text_x = kMargin + kPadX;
    const float text_y = kMargin + kPadY;
    nvgFillColor(vg, nvgRGBAf(0.0f, 0.0f, 0.0f, 0.5f)); // soft shadow
    nvgText(vg, text_x + 0.75f, text_y + 0.75f, branding::kClientName, nullptr);

    nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
    const float adv_x = nvgText(vg, text_x, text_y, branding::kWatermarkBase, nullptr);
    nvgFillColor(vg, nvgRGBf(accent.r, accent.g, accent.b));
    nvgText(vg, adv_x, text_y, branding::kWatermarkSuffix, nullptr);
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    s_show.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    s_show.visit(ar, "show");
}

events::Module create_module() noexcept {
    events::Module mod{};
    mod.name = "Watermark";
    mod.description = "Shows the OpenSlinky++ watermark.";
    mod.category = events::ModuleCategory::Visual;
    mod.enabled = true; // always running; visibility is the "Show watermark" setting
    mod.flags = events::kModuleFlagNoToggleSwitch |
                events::kModuleFlagNoKeybind |
                events::kModuleFlagHiddenFromHud |
                events::kModuleFlagNoHoldMode |
                events::kModuleFlagGlobalSettings;
    mod.events[events::kEventGuiFramePoll] = {&on_gui_frame_poll, 0};
    mod.events[events::kEventGuiRender2D] = {&on_gui_render_2d, 100}; // under the ClickGUI
    mod.render_settings = &render_settings;
    mod.visit_config = &visit_config;
    mod.flags |= events::kModuleFlagStyleSlinky; // OpenSlinky++: Slinky++ visual style
    return mod;
}

} // namespace

events::Module* get_watermark_module() noexcept {
    static events::Module s_module = create_module();
    return &s_module;
}

float watermark_hud_bottom() noexcept {
    return s_show.value ? s_bottom : 0.0f;
}

} // namespace slinky::modules
