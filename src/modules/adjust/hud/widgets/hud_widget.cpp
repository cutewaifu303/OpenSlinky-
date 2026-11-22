// Port of com.adjustclient.module.impl.render.hud.widget.HudWidget (frame + panel of every HUD widget).

#include "modules/adjust/hud/widgets/widgets.hpp"

#include <cstdio>
#include <cstring>

namespace slinky::modules::adjust_hud::widgets {

WidgetBox draw_frame(const char* name, float x, float y, float width, float height) noexcept {
    // movable.width = width, movable.height = height + 9, headerHeight = 9. The movable position resolves to the
    // default TOP_LEFT position (no HUD editor: drawOverlay never draws, MovablePosition.hudResolution is never set
    // in Adjust either, so there is no clamping).
    WidgetBox box{};
    box.x = x;
    box.y = y;
    box.width = width;
    box.height = height + 9.0f;

    draw_panel(140, x, y, width, height);
    const float center_x = x + width * 0.5f;
    const float title_y = y + 4.0f;
    const adjust::Argb title_color = adjust::argb(240, 240, 240, 222);
    adjust::kTahoma10.draw(name, center_x - adjust::kTahoma10.width(name) * 0.5f, title_y, title_color);
    return box;
}

void draw_panel(int alpha, float x, float y, float width, float height) noexcept {
    adjust::push_even_scale();
    const float header_bottom = y + 9.0f;
    const float bottom = header_bottom + height + 1.0f;
    const adjust::Argb body_color = (static_cast<adjust::Argb>(alpha & 0xFF) << 24) | 0x131313u;

    // to_pixels returns (x1, y1, x2, y2) in Vec4f{x, y, w, h}.
    const math::Vec4f body = adjust::to_pixels(x, y, x + width, bottom);
    adjust::fill_rect(body_color, body.x, body.y, body.w, body.h);
    // Two more fills with the colour still set on the NanoVG state (the body colour): a 0.5 px separator at the
    // bottom of the header (negative height rectangle, as in Adjust) and the header strip, which darkens the header
    // because the colour is translucent.
    const math::Vec4f separator = adjust::to_pixels(x, header_bottom, x + width, header_bottom - 0.5f);
    adjust::fill_rect(body_color, separator.x, separator.y, separator.w, separator.h);
    const math::Vec4f header = adjust::to_pixels(x, y, x + width, header_bottom);
    adjust::fill_rect(body_color, header.x, header.y, header.w, header.h);

    // Outline: 0.5 wide without an even-scale factor, else (int) factor * 0.5.
    const math::Vec4f outline = adjust::to_pixels(x, y, x + width, bottom);
    const float factor = adjust::even_scale_factor();
    const float line_width = factor == 0.0f ? 0.5f : static_cast<float>(static_cast<int>(factor)) * 0.5f;
    adjust::stroke_rect(false, (static_cast<adjust::Argb>(alpha & 0xFF) << 24) | 0x1B1B1Bu, outline.x, outline.y,
                        outline.w, outline.h, line_width);
    adjust::pop_even_scale();
}

void format_number(double value, char* out, size_t out_size) noexcept {
    if (out == nullptr || out_size == 0) {
        return;
    }
    std::snprintf(out, out_size, "%.2f", value);
    char* dot = std::strchr(out, '.');
    if (dot == nullptr) {
        return;
    }
    char* end = out + std::strlen(out) - 1;
    while (end > dot && *end == '0') {
        *end-- = '\0';
    }
    if (end == dot) {
        *dot = '\0';
    }
    if (std::strcmp(out, "-0") == 0) {
        out[0] = '0';
        out[1] = '\0';
    }
}

} // namespace slinky::modules::adjust_hud::widgets
