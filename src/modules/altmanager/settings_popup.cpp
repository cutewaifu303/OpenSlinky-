#include "modules/altmanager/settings_popup.hpp"

#include <cmath>

#include "events/module_manager.hpp"
#include "gui/gui_internal.hpp"
#include "modules/altmanager/alt_manager.hpp"
#include "util/math.hpp"

namespace slinky::altmanager {

namespace {

namespace g = slinky::gui;

struct PopupState {
    bool open = false;
    // Hit rects (logical screen coords), refreshed every render.
    math::Vec4f gear{};
    math::Vec4f panel{};
    math::Vec4f style_seg[2]{};
    math::Vec4f open_button{};
    bool valid = false;
};

PopupState& st() {
    static PopupState s;
    return s;
}

bool rect_hit(const math::Vec4f& r, float x, float y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

} // namespace

void settings_popup_render(gui::GuiRenderer* renderer, float cursor_x, float cursor_y) noexcept {
    PopupState& s = st();
    NVGcontext* vg = renderer->vg;
    const NVGcolor accent = nvgRGBf(0.953f, 0.365f, 0.071f);

    constexpr float kMargin = 8.0f;
    constexpr float kHeight = 16.0f;
    constexpr float kPadX = 8.0f;

    renderer->select_font(g::kFontRegular);
    nvgFontSize(vg, 11.0f);
    const float gear_w = nvgTextBounds(vg, 0.0f, 0.0f, "Settings", nullptr, nullptr) + kPadX * 2.0f;
    s.gear = math::make_rect(renderer->logical_width - kMargin - gear_w,
                             renderer->logical_height - kMargin - kHeight, gear_w, kHeight);

    const bool gear_hover = rect_hit(s.gear, cursor_x, cursor_y);
    nvgFillColor(vg, nvgTransRGBAf(g::detail::color_card_bg(), 0.9f));
    renderer->fill_rounded_rect(s.gear, kHeight * 0.5f);
    nvgFillColor(vg, (gear_hover || s.open) ? g::detail::color_text_hover() : g::detail::color_text_muted());
    renderer->draw_text(s.gear, g::kTextAlignCenterMiddle, g::kFontRegular, "Settings");

    if (!s.open) {
        s.valid = true;
        return;
    }

    // Panel above the gear.
    const float panel_w = 180.0f;
    const float panel_h = 86.0f;
    s.panel = math::make_rect(renderer->logical_width - kMargin - panel_w,
                              s.gear.y - 6.0f - panel_h, panel_w, panel_h);
    nvgFillColor(vg, nvgTransRGBAf(g::detail::color_card_bg(), 0.97f));
    renderer->fill_rounded_rect(s.panel, 6.0f);
    nvgStrokeColor(vg, g::detail::color_border());
    renderer->stroke_rounded_rect(s.panel, 6.0f);

    const float pad = 10.0f;
    nvgFontSize(vg, 11.0f);
    nvgFillColor(vg, g::detail::color_text_muted());
    renderer->draw_text(math::make_rect(s.panel.x + pad, s.panel.y + 6.0f, s.panel.w - pad * 2.0f, 12.0f),
                        g::kTextAlignLeftMiddle, g::kFontBold, "Theme");

    // Style segmented control.
    static const char* kLabels[2] = {"Slinky++", "Adjust"};
    const int active = static_cast<int>(events::get_requested_visual_style());
    const float seg_y = s.panel.y + 20.0f;
    const float seg_w = (s.panel.w - pad * 2.0f) / 2.0f;
    for (int i = 0; i < 2; ++i) {
        s.style_seg[i] = math::make_rect(s.panel.x + pad + seg_w * i, seg_y, seg_w - 2.0f, 18.0f);
        const bool hov = rect_hit(s.style_seg[i], cursor_x, cursor_y);
        nvgFillColor(vg, (i == active) ? accent : g::detail::color_widget_bg());
        renderer->fill_rounded_rect(s.style_seg[i], 3.0f);
        nvgFontSize(vg, 10.0f);
        nvgFillColor(vg, (i == active)   ? g::detail::color_text_hover()
                         : hov           ? g::detail::color_text_muted_hover()
                                         : g::detail::color_text_muted());
        renderer->draw_text(s.style_seg[i], g::kTextAlignCenterMiddle, g::kFontRegular, kLabels[i]);
    }

    // Alt Manager row.
    nvgFontSize(vg, 11.0f);
    nvgFillColor(vg, g::detail::color_text_primary());
    renderer->draw_text(math::make_rect(s.panel.x + pad, s.panel.y + 46.0f, s.panel.w - pad * 2.0f, 24.0f),
                        g::kTextAlignLeftMiddle, g::kFontRegular, "Alt Manager");

    const float btn_w = 54.0f;
    s.open_button = math::make_rect(s.panel.x + s.panel.w - pad - btn_w, s.panel.y + 48.0f, btn_w, 20.0f);
    const bool obtn_hover = rect_hit(s.open_button, cursor_x, cursor_y);
    nvgFillColor(vg, obtn_hover ? g::detail::color_danger_hover() : accent);
    renderer->fill_rounded_rect(s.open_button, 4.0f);
    nvgFontSize(vg, 11.0f);
    nvgFillColor(vg, g::detail::color_text_hover());
    renderer->draw_text(s.open_button, g::kTextAlignCenterMiddle, g::kFontBold, "Open");

    s.valid = true;
}

bool settings_popup_click(float cursor_x, float cursor_y) noexcept {
    PopupState& s = st();
    if (!s.valid) {
        return false;
    }
    if (rect_hit(s.gear, cursor_x, cursor_y)) {
        s.open = !s.open;
        return true;
    }
    if (!s.open) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        if (rect_hit(s.style_seg[i], cursor_x, cursor_y)) {
            if (i != static_cast<int>(events::get_requested_visual_style())) {
                events::request_visual_style(static_cast<events::VisualStyle>(i));
            }
            return true;
        }
    }
    if (rect_hit(s.open_button, cursor_x, cursor_y)) {
        open();
        s.open = false;
        return true;
    }
    if (rect_hit(s.panel, cursor_x, cursor_y)) {
        return true; // click inside the panel, consume
    }
    s.open = false; // click outside closes the popup
    return false;
}

} // namespace slinky::altmanager
