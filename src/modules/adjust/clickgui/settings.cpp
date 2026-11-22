// Adjust ClickGUI port - setting rows (ui/clickgui/setting/**) mapped onto OpenSlinky++ settings, and the
// generation of a module's rows from its captured settings (ComponentFactory.build).
//
//   BooleanSetting  -> config::BoolSetting       BooleanSettingComponent ("On" / "Off", check box)
//   NumberSetting   -> config::FloatSetting      NumberSettingComponent (slider, wheel, middle click = value prompt)
//   (no equivalent) <- config::RangeSetting      RangeSettingComponent (NumberSettingComponent look, two ends)
//   ModeSetting     -> config::ModeSetting       ModeSettingComponent (left = next, right = previous)
//   ColorSetting    -> adjust::ColorOption       ColorSettingComponent + ColorModeComponent ("Type") + ColorPicker
//                   -> config::ColorSetting      ColorSettingComponent + ColorPicker (always shown)
//   ExpandSetting   -> section header            ExpandSettingComponent (+ SettingSeparatorComponent footer)
//   StringSetting   -> config::DynamicListSetting StringSettingComponent + StringEditorComponent (one line per entry)
//   Keybind/Button/Items/Hotbar/EspPreview/BedDefense settings: no OpenSlinky++ equivalent.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "modules/adjust/clickgui/internal.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "util/keycodes.hpp"

namespace slinky::modules::adjust_clickgui::detail {

namespace hud = slinky::modules::adjust_hud;

namespace {

int scaled_channel(int component, float factor) noexcept {
    return std::max(0, static_cast<int>(static_cast<float>(component) * factor));
}

Argb scaled_rgb(Argb c, float factor) noexcept {
    return adjust::argb(scaled_channel(adjust::red_of(c), factor), scaled_channel(adjust::green_of(c), factor),
                        scaled_channel(adjust::blue_of(c), factor), adjust::alpha_of(c));
}

std::string printf_value(const char* format, float value) {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), format != nullptr ? format : "%0.2f", static_cast<double>(value));
    return buf;
}

bool parse_double(const std::string& text, double& out) noexcept {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) {
        return false;
    }
    while (*end == ' ') {
        ++end;
    }
    return *end == '\0' && std::isfinite(out);
}

void invalid_number(const std::string& text) {
    notify("Invalid Value", {{"Value ", false}, {text, true}, {" is not a valid number.", false}});
}

// adjust::ColorOption that owns `mode`, if `mode` is the Plain / Rainbow / Main Color dropdown of one
adjust::ColorOption* color_option_of(config::ModeSetting* mode) noexcept {
    if (mode == nullptr || mode->options != adjust::kColorModeOptions || mode->option_count != 3) {
        return nullptr;
    }
    return reinterpret_cast<adjust::ColorOption*>(reinterpret_cast<char*>(mode) - offsetof(adjust::ColorOption, mode));
}

// =============================================================================================================
// SettingComponent
// =============================================================================================================

class SettingRow : public Component {
public:
    SettingRow() { hover_ = add_modifier(std::make_unique<HoverBackground>(this)); }

    RowKind kind() const noexcept override { return RowKind::Setting; }
    virtual std::string label() const = 0;
    bool search_text(std::string& out) const override {
        out = label();
        return true;
    }
    virtual void reset_setting() {}

protected:
    std::string fit_label(const std::string& text, float reserved_width) {
        float max_width = bounds.width - reserved_width;
        if (max_width < 0.0f) {
            max_width = 0.0f;
        }
        const std::string fitted = adjust::kTahoma10.trim_to_width(text, max_width);
        if (fitted == text) {
            return fitted;
        }
        if (!full_label_tooltip_added_) {
            full_label_tooltip_added_ = true;
            std::string full = text.empty() ? text : text.substr(0, text.size() - 1);
            const size_t pos = full.find("Dist.");
            if (pos != std::string::npos) {
                full.replace(pos, 5, "Distance");
            }
            add_modifier(std::make_unique<Tooltip>(this, 700, full, std::string(), false, nullptr));
        }
        size_t b = 0;
        size_t e = fitted.size();
        while (b < e && fitted[b] == ' ') {
            ++b;
        }
        while (e > b && fitted[e - 1] == ' ') {
            --e;
        }
        if (b == e) {
            return "...";
        }
        return fitted.substr(b, e - b) + "...";
    }

    void open_value_prompt(std::function<void(const std::string&)> on_set) {
        const std::string l = label();
        auto dialog = std::make_unique<Dialog>("Set value of " + l);
        dialog->message.append("Enter a new value for ");
        dialog->message.italic = true;
        dialog->message.append(l);
        dialog->message.append(".");
        dialog->add_text_input("Value", "", 10, 120);
        dialog->add_button("Set", [on_set](Dialog& d) { on_set(d.input_text(0)); });
        dialog->add_button("Cancel", nullptr);
        show_dialog(std::move(dialog), 150.0f);
    }

    void open_reset_prompt() {
        const std::string l = label();
        auto dialog = std::make_unique<Dialog>("Reset setting " + l + " to default?");
        dialog->message.append("The setting ");
        dialog->message.italic = true;
        dialog->message.append(l);
        dialog->message.append(" will be reset to default if you continue.");
        dialog->add_button("Cancel", nullptr);
        dialog->add_button("Reset Setting", [this](Dialog&) {
            reset_setting();
            mark_dirty();
        });
        show_dialog(std::move(dialog));
    }

    HoverBackground* hover_ = nullptr;
    bool full_label_tooltip_added_ = false;
};

// =============================================================================================================
// BooleanSettingComponent
// =============================================================================================================

class BoolRow final : public SettingRow {
public:
    explicit BoolRow(config::BoolSetting* s) : s_(s) {
        label_color_ = add_modifier(std::make_unique<HoverEffect>(
            this, [s] { return s->value; }, hovered_of(this), 0.0f, 0.6f));
    }

    std::string label() const override { return safe(s_->label); }

    void render_content(DrawQueue&, Vec2) override {
        const std::string l = label();
        const std::string text = !l.empty() ? l + ":" : ":";
        const std::string fitted = fit_label(text, 25.0f);
        const bool enabled = s_->value;
        const std::string full = fitted + " " ACG_BOLD + (enabled ? "On" : "Off");
        const Argb text_color = label_color_->color(190);
        const float animated = label_color_->k().value;
        const Argb accent = hud::hud_color();
        const int alpha = clamp_int(static_cast<int>(255.0f * animated), 0, 255);
        const Argb box = adjust::alpha_of(accent) != alpha ? adjust::with_alpha(accent, alpha) : accent;
        draw_check_box(bounds, y, full, text_color, box);
    }

    bool on_mouse_pressed(Vec2, int button, bool pressed) override {
        if (button == 0 && pressed) {
            if (ctrl_down()) {
                open_reset_prompt();
                return true;
            }
            s_->value = !s_->value;
            mark_dirty();
            return true;
        }
        return false;
    }

    void reset_setting() override { s_->value = s_->default_value; }
    float get_height() override { return 9.0f; }

private:
    config::BoolSetting* s_;
    HoverEffect* label_color_ = nullptr;
};

// =============================================================================================================
// NumberSettingComponent (FloatSetting) / RangeSettingComponent
// =============================================================================================================

float snap_to_step(float v, float step, float lo, float hi) noexcept {
    if (step > 0.0f) {
        v = std::round(v / step) * step;
    }
    return clampf(v, lo, hi);
}

float effective_step(float step, float lo, float hi) noexcept {
    const float s = step > 0.0f ? step : ((hi - lo) > 0.0f ? (hi - lo) / 100.0f : 1.0f);
    return s * (shift_down() ? 10.0f : 1.0f); // NumberSetting.getEffectiveStep
}

// Shared drawing of the slider rows (NumberSettingComponent.renderContent)
void draw_slider_row(Component& row, HoverEffect& label_color, float from_fill, float fill, float fraction,
                     const std::string& label, const std::string& value_text) {
    const Box& b = row.bounds;
    const float track_left = b.x + 0.0f;
    const float track_width = b.width - 2.0f;
    const float bar_y = row.y + 8.5f;
    const float bar_left = track_left + 2.0f;
    const float bar_right = track_width + track_left;
    adjust::push_even_scale();
    const math::Vec4f track = adjust::to_pixels(bar_left, bar_y, bar_right, bar_y + 1.0f);
    if (fill > 2.0f) {
        const Argb accent = label_color.accent(255) | 0xFF000000u;
        Argb second = 0;
        if (!hud::secondary_active()) {
            second = adjust::scale(accent, 0.58f);
        } else {
            second = adjust::blend(hud::secondary_for(accent), accent, fraction);
        }
        const float end = std::min(track_width, std::max(fill, 2.0f));
        const float start = std::max(bar_left, track_left + from_fill);
        const math::Vec4f upper = adjust::to_pixels(start, bar_y, end + track_left, bar_y + 0.5f);
        adjust::fill_gradient_rect(scaled_rgb(accent, 0.7f), scaled_rgb(second, 0.87f), upper.x, upper.y, upper.w,
                                   upper.h, upper.x, upper.y, upper.w, upper.y);
        const float p = pixel_line_width();
        const float ly1 = upper.y + p;
        const float ly2 = upper.y + p + p;
        adjust::fill_gradient_rect(scaled_rgb(accent, 0.585f), scaled_rgb(second, 0.598f), upper.x, ly1, upper.w, ly2,
                                   upper.x, ly1, upper.w, ly1);
    }
    adjust::fill_rect(label_color.accent(90), track.x, track.y, track.w, track.h);
    adjust::stroke_rect(true, label_color.color(5), track.x, track.y, track.w, track.h, pixel_line_width());
    adjust::pop_even_scale();
    const std::string shown = ACG_BOLD + value_text;
    const adjust::Font& font = adjust::kTahoma10;
    const float value_width = font.width(shown);
    const float label_width = font.width(label);
    const float value_x = (track_left + b.width) - 2.5f - value_width;
    const float label_x = std::min(2.0f + b.x, (value_x - label_width) - 1.0f);
    const float text_y = row.y + 4.0f;
    font.draw(label, label_x, text_y, label_color.color(190));
    font.draw(shown, value_x, text_y, label_color.color(190));
}

class FloatRow final : public SettingRow {
public:
    explicit FloatRow(config::FloatSetting* s) : s_(s) {
        label_color_ = add_modifier(std::make_unique<HoverEffect>(
            this, [] { return true; }, hovered_of(this), 0.27f, 0.75f));
    }

    std::string label() const override { return safe(s_->label); }

    std::string format_value() const {
        if (s_ == click_gui_scale_setting()) {
            // UserConfiguration scale formatter: the maximum shows "Auto"
            return s_->value >= s_->max_val ? std::string("Auto")
                                            : std::to_string(static_cast<int>(s_->value));
        }
        const float shown = s_->display_scale != 0.0f ? s_->value * s_->display_scale : s_->value;
        return printf_value(s_->format, shown);
    }

    void set_value(float v) {
        v = snap_to_step(v, s_->step, s_->min_val, s_->max_val);
        if (v != s_->value) {
            s_->value = v;
            mark_dirty();
        }
    }

    void set_from_text(const std::string& text) {
        double parsed = 0.0;
        if (!parse_double(text, parsed)) {
            invalid_number(text);
            return;
        }
        float v = static_cast<float>(parsed);
        if (s_->display_scale != 0.0f) {
            v /= s_->display_scale;
        }
        set_value(v);
    }

    bool on_scroll(Vec2 mouse, float delta) override {
        if (!bounds.contains(mouse)) {
            return false;
        }
        if (owner != nullptr && owner->screen != nullptr && owner->screen->timer48 + 700 > adjust::now_ms()) {
            return false;
        }
        const float st = effective_step(s_->step, s_->min_val, s_->max_val);
        if (delta > 0.0f) {
            set_value(std::max(s_->value - st, s_->min_val)); // decrement
        } else if (delta < 0.0f) {
            set_value(std::min(s_->value + st, s_->max_val)); // increment
        }
        return true;
    }

    void render_content(DrawQueue&, Vec2 mouse) override {
        const float track_left = bounds.x;
        const float track_width = bounds.width - 2.0f;
        if (dragging_) {
            if (mouse_button_down(0)) {
                float t = (mouse.x - track_left) / track_width;
                t = clampf(t, 0.0f, 1.0f);
                const float lo = s_->min_val;
                const float hi = s_->max_val;
                double v = static_cast<double>((hi - lo) * t + lo);
                if (s_->step <= 0.0f) {
                    v = static_cast<double>(std::round(v * 20.0)) / 20.0; // quantised to 0.05
                }
                set_value(static_cast<float>(v));
            } else {
                dragging_ = false;
            }
        }
        const float range = s_->max_val - s_->min_val;
        const float fraction = range != 0.0f ? (s_->value - s_->min_val) / range : 0.0f;
        const float fill_target = fraction * track_width;
        if (!has_fill_) {
            fill_ = adjust::Animation(adjust::Easing::EaseOutSine, 80);
            fill_.value = fill_target;
            fill_.restart();
            has_fill_ = true;
        }
        const float fill = fill_.update(fill_target);
        draw_slider_row(*this, *label_color_, 0.0f, fill, fraction, label(), format_value());
    }

    bool on_mouse_pressed(Vec2, int button, bool pressed) override {
        if (button != 0) {
            if (button != 2 || dragging_ || !pressed) {
                return false;
            }
            open_value_prompt([this](const std::string& text) { set_from_text(text); });
            return true;
        }
        if (pressed && ctrl_down()) {
            dragging_ = false;
            open_reset_prompt();
            return true;
        }
        dragging_ = pressed;
        return true;
    }

    void reset_setting() override { s_->value = s_->default_value; }
    float get_height() override { return 11.0f; }

private:
    config::FloatSetting* s_;
    HoverEffect* label_color_ = nullptr;
    bool dragging_ = false;
    adjust::Animation fill_{};
    bool has_fill_ = false;
};

class RangeRow final : public SettingRow {
public:
    explicit RangeRow(config::RangeSetting* s) : s_(s) {
        label_color_ = add_modifier(std::make_unique<HoverEffect>(
            this, [] { return true; }, hovered_of(this), 0.27f, 0.75f));
    }

    std::string label() const override { return safe(s_->label); }

    std::string format_value() const {
        const float k = s_->display_scale != 0.0f ? s_->display_scale : 1.0f;
        char buf[96]{};
        if (s_->value.min == s_->value.max && s_->single_format != nullptr) {
            std::snprintf(buf, sizeof(buf), s_->single_format, static_cast<double>(s_->value.min * k));
        } else {
            std::snprintf(buf, sizeof(buf), s_->range_format != nullptr ? s_->range_format : "%0.2f - %0.2f",
                          static_cast<double>(s_->value.min * k), static_cast<double>(s_->value.max * k));
        }
        return buf;
    }

    void set_end(bool is_max, float v) {
        v = snap_to_step(v, s_->step, s_->min_val, s_->max_val);
        math::Range2f r = s_->value;
        if (is_max) {
            r.max = std::max(v, r.min);
        } else {
            r.min = std::min(v, r.max);
        }
        if (r.min != s_->value.min || r.max != s_->value.max) {
            s_->value = r;
            mark_dirty();
        }
    }

    float fraction_of(float v) const noexcept {
        const float range = s_->max_val - s_->min_val;
        return range != 0.0f ? (v - s_->min_val) / range : 0.0f;
    }

    bool nearest_is_max(float mouse_x) const noexcept {
        const float track_width = bounds.width - 2.0f;
        const float min_x = bounds.x + fraction_of(s_->value.min) * track_width;
        const float max_x = bounds.x + fraction_of(s_->value.max) * track_width;
        if (s_->value.min == s_->value.max) {
            return mouse_x >= max_x;
        }
        return std::fabs(mouse_x - max_x) <= std::fabs(mouse_x - min_x);
    }

    void set_from_text(const std::string& text) {
        // "a" sets both ends, "a - b" / "a b" / "a, b" sets the range
        std::string first = text;
        std::string second;
        for (size_t i = 1; i < text.size(); ++i) {
            if (text[i] == '-' || text[i] == ',' || text[i] == ' ') {
                first = text.substr(0, i);
                second = text.substr(i + 1);
                break;
            }
        }
        const auto trim = [](std::string v) {
            while (!v.empty() && (v.back() == ' ' || v.back() == '-' || v.back() == ',')) {
                v.pop_back();
            }
            while (!v.empty() && (v.front() == ' ' || v.front() == ',')) {
                v.erase(0, 1);
            }
            return v;
        };
        first = trim(first);
        second = trim(second);
        double a = 0.0;
        double b = 0.0;
        if (!parse_double(first, a) || (!second.empty() && !parse_double(second, b))) {
            invalid_number(text);
            return;
        }
        if (second.empty()) {
            b = a;
        }
        const double k = s_->display_scale != 0.0f ? static_cast<double>(s_->display_scale) : 1.0;
        float lo = static_cast<float>(std::min(a, b) / k);
        float hi = static_cast<float>(std::max(a, b) / k);
        lo = snap_to_step(lo, s_->step, s_->min_val, s_->max_val);
        hi = snap_to_step(hi, s_->step, s_->min_val, s_->max_val);
        if (lo != s_->value.min || hi != s_->value.max) {
            s_->value.min = lo;
            s_->value.max = hi;
            mark_dirty();
        }
    }

    bool on_scroll(Vec2 mouse, float delta) override {
        if (!bounds.contains(mouse)) {
            return false;
        }
        if (owner != nullptr && owner->screen != nullptr && owner->screen->timer48 + 700 > adjust::now_ms()) {
            return false;
        }
        const bool is_max = nearest_is_max(mouse.x);
        const float st = effective_step(s_->step, s_->min_val, s_->max_val);
        const float cur = is_max ? s_->value.max : s_->value.min;
        if (delta > 0.0f) {
            set_end(is_max, cur - st);
        } else if (delta < 0.0f) {
            set_end(is_max, cur + st);
        }
        return true;
    }

    void render_content(DrawQueue&, Vec2 mouse) override {
        const float track_left = bounds.x;
        const float track_width = bounds.width - 2.0f;
        if (dragging_) {
            if (mouse_button_down(0)) {
                const float t = clampf((mouse.x - track_left) / track_width, 0.0f, 1.0f);
                double v = static_cast<double>((s_->max_val - s_->min_val) * t + s_->min_val);
                if (s_->step <= 0.0f) {
                    v = static_cast<double>(std::round(v * 20.0)) / 20.0;
                }
                const auto fv = static_cast<float>(v);
                if (drag_max_ && fv < s_->value.min) {
                    drag_max_ = false; // crossed the other end: drag that one instead
                } else if (!drag_max_ && fv > s_->value.max) {
                    drag_max_ = true;
                }
                set_end(drag_max_, fv);
            } else {
                dragging_ = false;
            }
        }
        const float from_target = fraction_of(s_->value.min) * track_width;
        const float to_target = fraction_of(s_->value.max) * track_width;
        if (!has_fill_) {
            from_ = adjust::Animation(adjust::Easing::EaseOutSine, 80);
            to_ = adjust::Animation(adjust::Easing::EaseOutSine, 80);
            from_.value = from_target;
            to_.value = to_target;
            from_.restart();
            to_.restart();
            has_fill_ = true;
        }
        const float from_fill = from_.update(from_target);
        const float fill = to_.update(to_target);
        draw_slider_row(*this, *label_color_, from_fill, std::max(fill, from_fill + 2.01f), fraction_of(s_->value.max),
                        label(), format_value());
    }

    bool on_mouse_pressed(Vec2 mouse, int button, bool pressed) override {
        if (button != 0) {
            if (button != 2 || dragging_ || !pressed) {
                return false;
            }
            open_value_prompt([this](const std::string& text) { set_from_text(text); });
            return true;
        }
        if (pressed && ctrl_down()) {
            dragging_ = false;
            open_reset_prompt();
            return true;
        }
        if (pressed) {
            drag_max_ = nearest_is_max(mouse.x);
        }
        dragging_ = pressed;
        return true;
    }

    void reset_setting() override { s_->value = s_->default_value; }
    float get_height() override { return 11.0f; }

private:
    config::RangeSetting* s_;
    HoverEffect* label_color_ = nullptr;
    bool dragging_ = false;
    bool drag_max_ = true;
    adjust::Animation from_{};
    adjust::Animation to_{};
    bool has_fill_ = false;
};

// =============================================================================================================
// ModeSettingComponent / ColorModeComponent
// =============================================================================================================

class ModeRow final : public SettingRow {
public:
    // `label_override` = ColorModeComponent ("Type")
    ModeRow(config::ModeSetting* s, const char* label_override) : s_(s), label_override_(label_override) {
        label_color_ = add_modifier(std::make_unique<HoverEffect>(
            this, [] { return true; }, hovered_of(this), 0.5f, 0.8f));
    }

    std::string base_label() const { return label_override_ != nullptr ? label_override_ : safe(s_->label); }
    std::string option_name() const {
        if (s_->options == nullptr || s_->option_count == 0) {
            return {};
        }
        const auto idx = static_cast<uint32_t>(s_->value) < s_->option_count ? static_cast<uint32_t>(s_->value) : 0u;
        return safe(s_->options[idx]);
    }
    std::string label() const override { return base_label() + ": " + option_name(); }

    void render_content(DrawQueue&, Vec2) override {
        const std::string l = base_label() + ": ";
        const std::string mode = ACG_BOLD + option_name();
        const float x0 = bounds.x + 2.0f;
        const float ty = y + 4.0f;
        adjust::kTahoma10.draw(l, x0, ty, label_color_->color(170));
        adjust::kTahoma10.draw(mode, x0 + adjust::kTahoma10.width(l), ty, label_color_->color(190));
    }

    bool on_mouse_pressed(Vec2, int button, bool pressed) override {
        if (!pressed) {
            return false;
        }
        if (button == 0 && ctrl_down()) {
            open_reset_prompt();
            return true;
        }
        if (s_->option_count == 0) {
            return false;
        }
        const auto count = static_cast<int32_t>(s_->option_count);
        const int32_t index = (s_->value >= 0 && s_->value < count) ? s_->value : 0;
        if (button == 1) {
            s_->value = index != 0 ? index - 1 : count - 1;
            mark_dirty();
            return true;
        }
        if (button != 0) {
            return false;
        }
        s_->value = index == count - 1 ? 0 : index + 1;
        mark_dirty();
        return true;
    }

    void reset_setting() override { s_->value = s_->default_value; }
    float get_height() override { return 9.0f; }

private:
    config::ModeSetting* s_;
    const char* label_override_;
    HoverEffect* label_color_ = nullptr;
};

// =============================================================================================================
// ColorPicker (SaturationBrightnessSquare, HueSlider)
// =============================================================================================================

class ColorTarget {
public:
    ColorTarget(config::ColorSetting* color, adjust::ColorOption* option) : color_(color), option_(option) {}

    Argb plain() const noexcept {
        return option_ != nullptr ? option_->plain() : adjust::from_color3f(color_->value);
    }
    Argb shown() const noexcept { return option_ != nullptr ? option_->get() : adjust::from_color3f(color_->value); }
    void set(Argb c) noexcept {
        const math::Color3f v = adjust::to_color3f(c);
        if (v.r != color_->value.r || v.g != color_->value.g || v.b != color_->value.b) {
            color_->value = v;
            mark_dirty();
        }
    }
    bool plain_mode() const noexcept { return option_ == nullptr || option_->current_mode() == adjust::ColorMode::Plain; }
    config::ColorSetting* color() const noexcept { return color_; }
    adjust::ColorOption* option() const noexcept { return option_; }

private:
    config::ColorSetting* color_;
    adjust::ColorOption* option_;
};

void draw_knob(float x, float y, float width, float height, float progress) {
    // ColorPickerSlider.drawKnob: 2.5 wide, centred on progress
    const float left = clampf(progress * width + x - 1.25f, x, x + width - 2.5f);
    adjust::push_even_scale();
    const math::Vec4f px = adjust::to_pixels(left, y, left + 2.5f, y + height);
    const float x1 = px.x + 0.5f;
    const float y1 = px.y + 0.5f;
    const float x2 = px.w - 0.5f;
    const float y2 = px.h - 0.5f;
    adjust::stroke_rect(true, adjust::argb(9, 9, 9, 200), x1, y1, x2, y2, 0.5f);
    adjust::stroke_rect(false, adjust::argb(228, 228, 228, 200), x1, y1, x2, y2, 0.5f);
    adjust::pop_even_scale();
}

class ColorPickerRow final : public SettingRow {
public:
    explicit ColorPickerRow(ColorTarget target) : target_(target) {
        modifiers.clear(); // no hover background / tooltip
        hover_ = nullptr;
        update_from_color();
    }

    RowKind kind() const noexcept override { return RowKind::ColorPicker; }
    std::string label() const override { return safe(target_.color()->label); }
    bool search_text(std::string& out) const override {
        out = label();
        return true;
    }

    bool is_visible(int stamp) override {
        if (stamp != visibility_stamp) {
            visibility_stamp = stamp;
            cached_visible = parent == nullptr || parent->compute_visible(stamp);
            if (!cached_visible) {
                return false;
            }
        } else if (!cached_visible) {
            return false;
        }
        return target_.plain_mode();
    }

    void render_content(DrawQueue&, Vec2 mouse) override {
        if (!mouse_button_down(0)) {
            dragged_ = Part::None;
        }
        const Argb current = target_.plain();
        if (current != last_seen_) {
            update_from_color(); // changed elsewhere (reset, profile load ...)
        }
        // SaturationBrightnessSquare
        sq_ = Box{2.0f + x, 2.0f + y, width - 4.0f, 36.0f};
        hue_ = Box{2.0f + x, 2.0f + y + sq_.height + 2.0f, width - 4.0f, 5.0f};
        render_square();
        if (dragged_ == Part::Square) {
            set_square(mouse);
        }
        render_hue();
        if (dragged_ == Part::Hue) {
            set_hue(mouse);
        }
    }

    bool on_mouse_pressed(Vec2 mouse, int button, bool pressed) override {
        if (button == 0) {
            if (sq_.contains(mouse)) {
                dragged_ = Part::Square;
                return true;
            }
            if (hue_.contains(mouse)) {
                dragged_ = Part::Hue;
                return true;
            }
        }
        return false;
    }

    float get_height() override { return 47.0f; } // no alpha slider (OpenSlinky++ colours are opaque)

private:
    enum class Part : uint8_t { None, Square, Hue };

    void update_from_color() {
        float hsb[3]{};
        adjust::rgb_to_hsb(target_.plain(), hsb);
        hue_value_ = hsb[0];
        saturation_ = hsb[1];
        brightness_ = hsb[2];
        last_seen_ = target_.plain();
    }

    void apply(float h, float s, float b) {
        const Argb rgb = adjust::hsb_to_rgb(h, s, b);
        target_.set(rgb);
        last_seen_ = target_.plain();
    }

    void render_square() {
        const Argb pure = adjust::hsb_to_rgb(hue_value_, 1.0f, 1.0f);
        const float x1 = sq_.x;
        const float x2 = x1 + sq_.width;
        const float y1 = sq_.y;
        const float y2 = y1 + sq_.height;
        adjust::fill_gradient_rect(0xFFFFFFFFu, pure, x1, y1, x2, y2, x1, y1, x2, y1);
        adjust::fill_gradient_rect(0x19000000u, 0xFF000000u, x1, y1, x2, y2, x1, y1, x1, y2);
        const float knob_x = clampf(sq_.width * saturation_ + sq_.x - 1.5f, sq_.x, sq_.x + sq_.width - 3.0f);
        const float knob_y = clampf((1.0f - brightness_) * sq_.height + sq_.y - 1.5f, sq_.y, sq_.y + sq_.height - 3.0f);
        adjust::push_even_scale();
        const math::Vec4f px = adjust::to_pixels(knob_x, knob_y, knob_x + 3.0f, knob_y + 3.0f);
        adjust::stroke_rect(true, adjust::argb(9, 9, 9, 200), px.x + 0.5f, px.y + 0.5f, px.w - 0.5f, px.h - 0.5f, 0.5f);
        adjust::stroke_rect(false, adjust::argb(228, 228, 228, 200), px.x + 0.5f, px.y + 0.5f, px.w - 0.5f, px.h - 0.5f,
                            0.5f);
        adjust::pop_even_scale();
    }

    void render_hue() {
        static constexpr Argb kRainbow[7] = {0xFFFF0000u, 0xFFFFFF00u, 0xFF00FF00u, 0xFF00FFFFu,
                                             0xFF0000FFu, 0xFFFF00FFu, 0xFFFF0000u};
        const float segment = hue_.width / 6.0f;
        for (int i = 0; i < 6; ++i) {
            const float x1 = static_cast<float>(i) * segment + hue_.x;
            const float x2 = x1 + segment;
            adjust::fill_gradient_rect(kRainbow[i], kRainbow[i + 1], x1, hue_.y, x2, hue_.y + hue_.height, x1, hue_.y,
                                       x2, hue_.y);
        }
        draw_knob(hue_.x, hue_.y, hue_.width, hue_.height, hue_value_);
    }

    void set_square(Vec2 mouse) {
        const float mx = std::min(std::max(mouse.x, sq_.x), sq_.x + sq_.width);
        const float my = std::min(std::max(mouse.y, sq_.y), sq_.y + sq_.height);
        brightness_ = 1.0f - (my - sq_.y) / sq_.height;
        saturation_ = (mx - sq_.x) / sq_.width;
        apply(hue_value_, saturation_, brightness_);
    }

    void set_hue(Vec2 mouse) {
        const float mx = std::min(std::max(mouse.x, hue_.x), hue_.x + hue_.width);
        hue_value_ = (mx - hue_.x) / hue_.width;
        apply(hue_value_, saturation_, brightness_);
    }

    ColorTarget target_;
    Box sq_{};
    Box hue_{};
    Part dragged_ = Part::None;
    float hue_value_ = 0.0f;
    float saturation_ = 0.0f;
    float brightness_ = 0.0f;
    Argb last_seen_ = 0;
};

// =============================================================================================================
// ColorSettingComponent
// =============================================================================================================

class ColorRow final : public SettingRow {
public:
    explicit ColorRow(ColorTarget target) : target_(target) {
        swatch_ = add_modifier(std::make_unique<HoverEffect>(
            this, [] { return true; }, hovered_of(this), 0.0f, 0.79f));
        hover_color_ = add_modifier(std::make_unique<HoverEffect>(
            this, [this] { return is_fully_expanded(); }, hovered_of(this), 0.2f, 1.0f));
        add_expand_button_bar(this);
        if (target.option() != nullptr) {
            mode_row_ = std::make_unique<ModeRow>(&target.option()->mode, "Type");
            mode_row_->parent = this;
            children.push_back(mode_row_.get());
        }
        picker_ = std::make_unique<ColorPickerRow>(target);
        picker_->parent = this;
        children.push_back(picker_.get());
    }

    RowKind kind() const noexcept override { return RowKind::Color; }
    std::string label() const override { return safe(target_.color()->label); }

    void render_content(DrawQueue&, Vec2) override {
        draw_check_box(bounds, y, label(), hover_color_->color(190), swatch_->with_alpha(target_.shown()));
    }

    bool on_mouse_pressed(Vec2, int button, bool pressed) override {
        if (!pressed) {
            if (button != 2) {
                return false;
            }
            open_value_prompt([this](const std::string& text) { apply_text(text); });
            return false;
        }
        if (button == 0) {
            if (ctrl_down()) {
                open_reset_prompt();
            } else {
                toggle_expanded();
            }
            return true;
        }
        return false;
    }

    void reset_setting() override {
        target_.color()->value = target_.color()->default_value;
        if (target_.option() != nullptr) {
            target_.option()->mode.value = target_.option()->mode.default_value;
        }
    }

    float get_height() override { return 9.0f; }

private:
    void apply_text(const std::string& text) {
        // Integer.decode: "0x..", "#..", decimal
        std::string t = text;
        for (char& c : t) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        int base = 10;
        size_t start = 0;
        if (t.rfind("0x", 0) == 0) {
            base = 16;
            start = 2;
        } else if (!t.empty() && t[0] == '#') {
            base = 16;
            start = 1;
        }
        char* end = nullptr;
        const std::string digits = t.substr(start);
        const unsigned long long v = std::strtoull(digits.c_str(), &end, base);
        if (digits.empty() || end == digits.c_str() || *end != '\0' || v > 0xFFFFFFFFull) {
            notify("Invalid Value",
                   {{"Value ", false}, {text, true}, {" could not be converted into a color.", false}});
            return;
        }
        target_.set(static_cast<Argb>(v) | 0xFF000000u); // no alpha
    }

    ColorTarget target_;
    HoverEffect* swatch_ = nullptr;
    HoverEffect* hover_color_ = nullptr;
    std::unique_ptr<ModeRow> mode_row_;
    std::unique_ptr<ColorPickerRow> picker_;
};

// =============================================================================================================
// SettingSeparatorComponent / ExpandSettingComponent (section header)
// =============================================================================================================

class SeparatorRow final : public Component {
public:
    RowKind kind() const noexcept override { return RowKind::Separator; }

    void render_content(DrawQueue&, Vec2 mouse) override {
        if (bounds.height == 0.0f) {
            return;
        }
        const float dx = bounds.x - mouse.x;
        const float dy = bounds.y - mouse.y;
        const float a = clampf(-(std::sqrt(dx * dx + dy * dy) / 30.0f * 0.5f), 0.0f, 1.0f);
        const float sx = mouse.x - bounds.y;
        const float sy = mouse.y - bounds.x;
        const float strength = clampf(std::sqrt(sx * sx + sy * sy) / 31.0f, 0.15f, 0.3f);
        adjust::push_even_scale();
        draw_accent_gradient(bounds.x, bounds.y, bounds.width, bounds.height, a, strength);
        adjust::pop_even_scale();
    }

    float get_height() override { return click_gui_separators() ? 0.5f : 0.0f; }
};

class ExpandRow final : public SettingRow {
public:
    explicit ExpandRow(const char* title) : title_(safe(title)) {
        label_color_ = add_modifier(std::make_unique<HoverEffect>(
            this, [] { return true; }, hovered_of(this), 0.3f, 0.89f));
        add_expand_button_bar(this);
        footer = std::make_unique<SeparatorRow>();
    }

    RowKind kind() const noexcept override { return RowKind::Expand; }
    std::string label() const override { return title_; }

    void render_content(DrawQueue&, Vec2) override {
        expand_animation_.update(is_fully_expanded() ? 1.0f : 0.0f);
        const std::string l = label();
        const float centre_x = width / 2.0f + x;
        const float text_y = y + 4.0f;
        const Argb accent = hud::hud_color();
        const Argb grey = label_color_->color(static_cast<int>(55.0f * expand_animation_.value + 145.0f));
        const double t = std::max(0.0f, std::min(1.0f, 0.13f * expand_animation_.value));
        const double s = 1.0 - t;
        const int r = static_cast<int>(adjust::red_of(accent) * t + adjust::red_of(grey | 0xFF000000u) * s);
        const int g = static_cast<int>(adjust::green_of(accent) * t + adjust::green_of(grey) * s);
        const int b = static_cast<int>(adjust::blue_of(accent) * t + adjust::blue_of(grey) * s);
        const int a = static_cast<int>(adjust::alpha_of(accent) * t + s * 255.0);
        adjust::kTahoma10.draw(l, centre_x - adjust::kTahoma10.width(l) * 0.5f, text_y,
                               adjust::argb(clamp_int(r, 0, 255), clamp_int(g, 0, 255), clamp_int(b, 0, 255),
                                            clamp_int(a, 0, 255)));
    }

    bool on_mouse_pressed(Vec2, int button, bool pressed) override {
        if (!pressed || button != 0) {
            return false;
        }
        if (ctrl_down()) {
            const std::string l = label();
            auto dialog = std::make_unique<Dialog>("Reset setting group " + l + " to default?");
            dialog->message.append("All settings under ");
            dialog->message.italic = true;
            dialog->message.append(l);
            dialog->message.append(" will be reset to their defaults if you continue.");
            dialog->add_button("Cancel", nullptr);
            dialog->add_button("Reset Settings", [this](Dialog&) {
                for (Component* c : children) {
                    if (auto* row = dynamic_cast<SettingRow*>(c)) {
                        row->reset_setting();
                    }
                }
                mark_dirty();
            });
            show_dialog(std::move(dialog));
            return true;
        }
        toggle_expanded();
        return true;
    }

    void on_collapsed_hidden() override {
        expand_animation_.value = 0.0f;
        expand_animation_.restart();
    }

    float get_height() override { return 9.0f; }

    std::unique_ptr<SeparatorRow> footer;

private:
    std::string title_;
    HoverEffect* label_color_ = nullptr;
    adjust::Animation expand_animation_{adjust::Easing::Linear, 100};
};

// =============================================================================================================
// StringSettingComponent / StringEditorComponent (DynamicListSetting)
// =============================================================================================================

size_t list_count(const config::DynamicListSetting* s) noexcept {
    return s->get_count != nullptr ? s->get_count(s->ctx) : 0;
}

std::string list_name(const config::DynamicListSetting* s, size_t i) {
    return s->get_name != nullptr ? safe(s->get_name(s->ctx, i)) : "";
}

std::vector<std::string> list_entries(const config::DynamicListSetting* s) {
    std::vector<std::string> out;
    const size_t n = list_count(s);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(list_name(s, i));
    }
    return out;
}

class StringEditorRow;

class StringRow final : public SettingRow {
public:
    explicit StringRow(config::DynamicListSetting* s);

    RowKind kind() const noexcept override { return RowKind::StringSetting; }
    std::string label() const override { return safe(s_->label); }

    void render_content(DrawQueue&, Vec2) override {
        const std::string l = label();
        const std::string counter = std::to_string(list_count(s_));
        const adjust::Font& font = adjust::kTahoma10;
        const float centre = width / 2.0f + x;
        const float w = font.width(l + counter);
        const float lx = centre - w * 0.5f;
        font.draw(l, lx, y + 4.0f, hover_color_->color(170));
        font.draw(counter, lx + font.width(l) + 1.0f, y + 4.0f, hover_color_->color(130));
    }

    void set_expanded(bool open) override;

    bool on_mouse_pressed(Vec2, int button, bool pressed) override {
        if (!pressed || button != 0) {
            if (!pressed || button != 2) {
                return false;
            }
            // middle click: "Remove all text" confirmation
            auto dialog = std::make_unique<Dialog>("Remove all text");
            dialog->message.append("Are you sure you want to clear ");
            dialog->message.italic = true;
            dialog->message.append(label());
            dialog->message.append(" and its content?");
            config::DynamicListSetting* s = s_;
            dialog->add_button("Clear", [s](Dialog&) {
                if (s->clear_all != nullptr) {
                    s->clear_all(s->ctx);
                    s->modified = true;
                    mark_dirty();
                }
            });
            dialog->add_button("Cancel", nullptr);
            show_dialog(std::move(dialog));
            return true;
        }
        toggle_expanded(); // (Ctrl + click = reset in Adjust: lists have no default in OpenSlinky++)
        return true;
    }

    float get_height() override { return 9.0f; }

private:
    config::DynamicListSetting* s_;
    HoverEffect* hover_color_ = nullptr;
    std::unique_ptr<StringEditorRow> editor_;
};

class StringEditorRow final : public SettingRow {
public:
    explicit StringEditorRow(config::DynamicListSetting* s) : s_(s) {}

    RowKind kind() const noexcept override { return RowKind::StringEditor; }
    std::string label() const override { return safe(s_->label); }

    void request_rebuild() noexcept { rebuild_requested_ = true; }

    bool key_typed(int ch, int key) override {
        int index = -1;
        for (size_t i = 0; i < inputs_.size(); ++i) {
            if (inputs_[i].focused) {
                index = static_cast<int>(i);
                break;
            }
        }
        if (index < 0) {
            return false;
        }
        TextInput& focused = inputs_[static_cast<size_t>(index)];
        if (key == input::KEY_ESC) {
            commit();
            return true;
        }
        const std::string text = focused.field.text;
        if (key == input::KEY_ENTER) {
            if (text.empty()) {
                return true; // Enter on an empty line does nothing
            }
            if (index == static_cast<int>(inputs_.size()) - 1) {
                add_input("");
                focus_last();
                return true;
            }
            commit();
            return true;
        }
        if (key == input::KEY_DEL || key == input::KEY_BACKSPACE) {
            if (text.empty() && inputs_.size() >= 2) {
                inputs_.erase(inputs_.begin() + index);
                relayout();
                focus_last();
                return true;
            }
        } else if (key == input::KEY_V && ctrl_down()) {
            std::vector<std::string> pasted;
            std::string clip = get_clipboard();
            size_t start = 0;
            while (start <= clip.size()) {
                size_t nl = clip.find('\n', start);
                if (nl == std::string::npos) {
                    nl = clip.size();
                }
                std::string line = clip.substr(start, nl - start);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.find_first_not_of(" \t") != std::string::npos) {
                    pasted.push_back(line);
                }
                start = nl + 1;
            }
            if (pasted.size() >= 2) {
                if (text.empty()) {
                    inputs_.erase(inputs_.begin() + index); // the empty line is replaced by the pasted lines
                }
                for (const std::string& line : pasted) {
                    add_input(line);
                }
                relayout();
                return true;
            }
        }
        focused.field.set_focused(true);
        focused.field.key_typed(ch, key);
        focused.set_width(static_cast<int>(bounds.width - 2.0f));
        relayout();
        return true;
    }

    void render_content(DrawQueue&, Vec2) override {
        sync_from_setting();
        float px = bounds.x + 1.5f;
        float py = y + 4.0f;
        for (TextInput& input : inputs_) {
            input.field.focused = input.focused;
            input.draw(px, py);
            const int lines = static_cast<int>(input.lines.size());
            py += lines >= 2 ? static_cast<float>(lines - 1) * 7.5f + 8.5f : 8.5f;
        }
    }

    bool on_mouse_pressed(Vec2 mouse, int button, bool pressed) override {
        if (!pressed || button != 0 || screen().search_bar.active) {
            return false;
        }
        float top = y + 2.0f;
        const float left = x + 1.5f;
        bool hit = false;
        for (TextInput& input : inputs_) {
            const int lines = static_cast<int>(input.lines.size());
            const float h = lines >= 2 ? static_cast<float>(lines - 1) * 7.5f : 0.0f;
            if (!hit && mouse.x >= left - 2.0f && left + bounds.width >= mouse.x && mouse.y >= top &&
                top + h + 8.5f >= mouse.y) {
                input.focused = true;
                input.field.set_focused(true);
                hit = true;
            } else {
                input.focused = false;
                top += h + 8.5f;
            }
        }
        return hit;
    }

    void on_click_outside(Vec2, int button, bool pressed) override {
        if (pressed && button == 0) {
            commit();
        }
    }

    void expand_self() override {
        if (!expanded) {
            screen().layout_dirty = true;
            expanded = true;
            refresh_pending = true;
        }
    }

    float get_height() override {
        sync_from_setting();
        return total_height_;
    }

    bool wants_keyboard() const override { return editing(); }

    bool editing() const noexcept {
        for (const TextInput& input : inputs_) {
            if (input.focused) {
                return true;
            }
        }
        return false;
    }

private:
    void add_input(const std::string& text) {
        TextInput input("...", adjust::kTahoma10, 64);
        input.field.set_text(text);
        input.set_width(static_cast<int>(bounds.width > 4.0f ? bounds.width - 2.0f : 64.0f));
        inputs_.push_back(std::move(input));
        relayout();
    }

    void focus_last() {
        for (TextInput& input : inputs_) {
            input.focused = false;
        }
        if (!inputs_.empty()) {
            inputs_.back().focused = true;
            inputs_.back().field.set_focused(true);
        }
    }

    void relayout() {
        float h = 1.0f;
        const int wrap = static_cast<int>(bounds.width > 4.0f ? bounds.width - 2.0f : 64.0f);
        for (TextInput& input : inputs_) {
            input.set_width(wrap);
            const int lines = static_cast<int>(input.lines.size());
            h += lines >= 2 ? static_cast<float>(lines - 1) * 7.5f + 8.5f : 8.5f;
        }
        if (h != total_height_) {
            total_height_ = h;
            screen().layout_dirty = true;
        }
        last_width_ = bounds.width;
    }

    // rebuildInputs: one input per entry (one empty input for an empty list), unless an edit is in progress
    void sync_from_setting() {
        if (editing()) {
            if (bounds.width != last_width_) {
                relayout();
            }
            return;
        }
        std::vector<std::string> entries = list_entries(s_);
        if (!rebuild_requested_ && entries == shown_entries_) {
            if (bounds.width != last_width_) {
                relayout();
            }
            return;
        }
        rebuild_requested_ = false;
        shown_entries_ = entries;
        inputs_.clear();
        if (entries.empty()) {
            entries.emplace_back();
        }
        for (const std::string& e : entries) {
            TextInput input("...", adjust::kTahoma10, 64);
            input.field.set_text(e);
            inputs_.push_back(std::move(input));
        }
        relayout();
    }

    // commit: unfocus every input and write the lines back into the list (clear + add, colours kept by name)
    void commit() {
        const bool was_editing = editing();
        for (TextInput& input : inputs_) {
            input.focused = false;
            input.field.focused = false;
        }
        if (!was_editing) {
            return;
        }
        std::vector<std::string> wanted;
        for (const TextInput& input : inputs_) {
            const std::string& t = input.field.text;
            if (t.find_first_not_of(' ') != std::string::npos) {
                wanted.push_back(t);
            }
        }
        const std::vector<std::string> current = list_entries(s_);
        if (wanted == current) {
            rebuild_requested_ = true;
            return;
        }
        if (s_->clear_all == nullptr || s_->add_item == nullptr) {
            rebuild_requested_ = true;
            return;
        }
        std::vector<std::pair<std::string, math::Color3f>> colors;
        if (s_->item_kind == config::DynamicListItemKind::ColoredEntry && s_->get_color != nullptr) {
            for (size_t i = 0; i < current.size(); ++i) {
                const math::Color3f* c = s_->get_color(s_->ctx, i);
                if (c != nullptr) {
                    colors.emplace_back(current[i], *c);
                }
            }
        }
        s_->clear_all(s_->ctx);
        for (const std::string& name : wanted) {
            math::Color3f color{1.0f, 1.0f, 1.0f};
            for (const auto& kv : colors) {
                if (kv.first == name) {
                    color = kv.second;
                    break;
                }
            }
            s_->add_item(s_->ctx, name.c_str(), &color);
        }
        s_->modified = true;
        mark_dirty();
        rebuild_requested_ = true;
    }

    config::DynamicListSetting* s_;
    std::vector<TextInput> inputs_;
    std::vector<std::string> shown_entries_;
    bool rebuild_requested_ = true;
    float total_height_ = 1.0f;
    float last_width_ = 0.0f;
};

StringRow::StringRow(config::DynamicListSetting* s) : s_(s) {
    hover_color_ = add_modifier(std::make_unique<HoverEffect>(
        this, [] { return true; }, hovered_of(this), 0.5f, 0.8f));
    add_expand_button_bar(this);
    editor_ = std::make_unique<StringEditorRow>(s);
    editor_->parent = this;
    children.push_back(editor_.get());
}

void StringRow::set_expanded(bool open) {
    if (!expanded && open && editor_) {
        editor_->request_rebuild();
    }
    SettingRow::set_expanded(open);
}

// =============================================================================================================
// ComponentFactory.build
// =============================================================================================================

enum : int {
    kKeyBool = 1,
    kKeyFloat,
    kKeyRange,
    kKeyMode,
    kKeyColorOption,
    kKeyColor,
    kKeyList,
    kKeySection = 1000, // + occurrence of the title within the module
};

bool same_items(const std::vector<capture::CapturedItem>& a, const std::vector<capture::CapturedItem>& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind || a[i].item != b[i].item) {
            return false;
        }
    }
    return true;
}

void append_tree(const std::vector<Component*>& list, std::vector<Component*>& out) {
    for (Component* c : list) {
        out.push_back(c);
        append_tree(c->children, out);
    }
}

} // namespace

bool ModuleEntry::sync() {
    std::vector<capture::CapturedItem> items;
    capture::capture_module_settings(module, items);
    if (captured_once_ && same_items(items, captured_)) {
        return false;
    }
    captured_once_ = true;
    captured_ = items;

    const auto get_or_create = [this](int kind_key, const void* ptr, auto make) -> Component* {
        auto& slot = cache_[std::make_pair(kind_key, ptr)];
        if (!slot) {
            slot = make();
        }
        return slot.get();
    };

    // colours of adjust::ColorOption are shown inside the ColorOption's own row
    std::set<const void*> merged_colors;
    size_t section_count = 0;
    for (const capture::CapturedItem& item : items) {
        if (item.kind == capture::CaptureKind::Mode) {
            auto* mode = static_cast<config::ModeSetting*>(const_cast<void*>(item.item));
            if (adjust::ColorOption* option = color_option_of(mode)) {
                merged_colors.insert(&option->color);
            }
        } else if (item.kind == capture::CaptureKind::Section) {
            ++section_count;
        }
    }
    // A single leading section adds nothing but a click: its settings are shown directly (no group row).
    const bool flatten_first = section_count == 1 && !items.empty() && items[0].kind == capture::CaptureKind::Section;

    children.clear();
    Component* container = this;
    ExpandRow* section = nullptr;
    std::map<const void*, int> occurrences;
    const auto close_section = [&section] {
        if (section != nullptr) {
            section->footer->parent = section;
            section->children.push_back(section->footer.get());
            section = nullptr;
        }
    };

    for (size_t i = 0; i < items.size(); ++i) {
        const capture::CapturedItem& item = items[i];
        void* ptr = const_cast<void*>(item.item);
        Component* row = nullptr;
        switch (item.kind) {
            case capture::CaptureKind::Section: {
                if (flatten_first && i == 0) {
                    continue;
                }
                close_section();
                const int occurrence = occurrences[item.item]++;
                auto* title = static_cast<const char*>(item.item);
                Component* c = get_or_create(kKeySection + occurrence, item.item,
                                             [title] { return std::make_unique<ExpandRow>(title); });
                section = static_cast<ExpandRow*>(c);
                section->children.clear();
                section->parent = this;
                children.push_back(section);
                container = section;
                continue;
            }
            case capture::CaptureKind::Bool: {
                auto* s = static_cast<config::BoolSetting*>(ptr);
                row = get_or_create(kKeyBool, s, [s] { return std::make_unique<BoolRow>(s); });
                break;
            }
            case capture::CaptureKind::Float: {
                auto* s = static_cast<config::FloatSetting*>(ptr);
                row = get_or_create(kKeyFloat, s, [s] { return std::make_unique<FloatRow>(s); });
                break;
            }
            case capture::CaptureKind::Range: {
                auto* s = static_cast<config::RangeSetting*>(ptr);
                row = get_or_create(kKeyRange, s, [s] { return std::make_unique<RangeRow>(s); });
                break;
            }
            case capture::CaptureKind::Mode: {
                auto* s = static_cast<config::ModeSetting*>(ptr);
                if (adjust::ColorOption* option = color_option_of(s)) {
                    row = get_or_create(kKeyColorOption, &option->color, [option] {
                        return std::make_unique<ColorRow>(ColorTarget(&option->color, option));
                    });
                } else {
                    row = get_or_create(kKeyMode, s, [s] { return std::make_unique<ModeRow>(s, nullptr); });
                }
                break;
            }
            case capture::CaptureKind::Color: {
                if (merged_colors.count(item.item) != 0) {
                    continue;
                }
                auto* s = static_cast<config::ColorSetting*>(ptr);
                row = get_or_create(kKeyColor, s,
                                    [s] { return std::make_unique<ColorRow>(ColorTarget(s, nullptr)); });
                break;
            }
            case capture::CaptureKind::DynamicList: {
                auto* s = static_cast<config::DynamicListSetting*>(ptr);
                row = get_or_create(kKeyList, s, [s] { return std::make_unique<StringRow>(s); });
                break;
            }
        }
        if (row != nullptr) {
            row->parent = container;
            container->children.push_back(row);
        }
    }
    close_section();
    return true;
}

void ModuleEntry::append_rows(std::vector<Component*>& out) const {
    append_tree(children, out);
}

} // namespace slinky::modules::adjust_clickgui::detail
