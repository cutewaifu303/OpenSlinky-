// Adjust ClickGUI port - shared widgets: input state, GuiTextField / TextInput / TextInputBox, AdjustButton,
// DialogBuilder + DialogScreen, notifications and small drawing helpers (DimTransition, RenderUtil).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "config/profile_manager.hpp"
#include "modules/adjust/clickgui/internal.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/adjust/hud/adjust_notifications.hpp"
#include "util/keycodes.hpp"

namespace slinky::modules::adjust_clickgui::detail {

namespace hud = slinky::modules::adjust_hud;

// =============================================================================================================
// Environment
// =============================================================================================================

Env& env() noexcept {
    static Env e{};
    return e;
}

bool key_down(int key_code) noexcept {
    if (key_code <= 0 || key_code >= input::KEY_COUNT) {
        return false;
    }
    const Env& e = env();
    if (e.key_states != nullptr) {
        return (e.key_states[key_code] & 1u) != 0;
    }
    return e.keys[key_code];
}

bool ctrl_down() noexcept {
    return key_down(input::KEY_LCONTROL) || key_down(input::KEY_RCONTROL);
}

bool shift_down() noexcept {
    return key_down(input::KEY_LSHIFT) || key_down(input::KEY_RSHIFT);
}

bool lcontrol_down() noexcept {
    return key_down(input::KEY_LCONTROL);
}

bool mouse_button_down(int java_button) noexcept {
    return java_button >= 0 && java_button < 5 && key_down(input::KEY_MOUSE1 + java_button);
}

Vec2 mouse_hud() noexcept {
    const Env& e = env();
    const float s = e.scale > 0.0f ? e.scale : 2.0f;
    return Vec2{e.cursor_px.x / s, e.cursor_px.y / s};
}

void mark_dirty() noexcept {
    config::mark_profile_dirty(true);
}

int clamp_int(int v, int lo, int hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

float clampf(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

float pixel_line_width(bool rounded) noexcept {
    const float factor = adjust::even_scale_factor();
    int unit = 1;
    if (factor != 0.0f) {
        unit = rounded ? static_cast<int>(std::lround(factor)) : static_cast<int>(factor);
    }
    return static_cast<float>(unit) * 0.5f;
}

const char* safe(const char* s) noexcept {
    return s != nullptr ? s : "";
}

void DrawQueue::run() {
    // tasks may queue more tasks; run by index
    for (size_t i = 0; i < post_tasks.size(); ++i) {
        auto task = post_tasks[i];
        if (task) {
            task();
        }
    }
    post_tasks.clear();
}

// ---- clipboard (ClipboardUtil / GuiScreen.getClipboardString) --------------------------------------------------

std::string get_clipboard() {
    std::string out;
    if (OpenClipboard(nullptr) == 0) {
        return out;
    }
    HANDLE data = GetClipboardData(CF_TEXT);
    if (data != nullptr) {
        const auto* text = static_cast<const char*>(GlobalLock(data));
        if (text != nullptr) {
            out = text;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return out;
}

void set_clipboard(const std::string& text) noexcept {
    if (OpenClipboard(nullptr) == 0) {
        return;
    }
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (mem != nullptr) {
        void* dst = GlobalLock(mem);
        if (dst != nullptr) {
            std::memcpy(dst, text.c_str(), text.size() + 1);
            GlobalUnlock(mem);
            if (SetClipboardData(CF_TEXT, mem) == nullptr) {
                GlobalFree(mem);
            }
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

std::string key_name(int key_code) {
    static constexpr const char* kButtons[5] = {"LMB", "RMB", "MMB", "BMB", "FMB"}; // MouseKey.BUTTON_NAMES
    if (key_code >= input::KEY_MOUSE1 && key_code <= input::KEY_MOUSE5) {
        return kButtons[key_code - input::KEY_MOUSE1];
    }
    return safe(input::get_key_name(key_code));
}

// =============================================================================================================
// GuiTextField
// =============================================================================================================

bool TextField::allowed(int ch) noexcept {
    // ChatAllowedCharacters.isAllowedCharacter: != '§', >= ' ', != DEL (ASCII only in this port)
    return ch >= 0x20 && ch < 0x7F;
}

void TextField::set_text(std::string_view value) {
    text = std::string(value.substr(0, std::min<size_t>(value.size(), static_cast<size_t>(std::max(0, max_length)))));
    set_cursor(static_cast<int>(text.size()));
}

void TextField::set_focused(bool value) noexcept {
    if (value && !focused) {
        focus_ms = adjust::now_ms(); // cursorCounter = 0
    }
    focused = value;
}

int TextField::cursor_counter() const noexcept {
    return static_cast<int>((adjust::now_ms() - focus_ms) / 50);
}

void TextField::set_cursor(int pos) noexcept {
    cursor = clamp_int(pos, 0, static_cast<int>(text.size()));
    set_selection(cursor);
}

void TextField::set_selection(int pos) noexcept {
    selection_end = clamp_int(pos, 0, static_cast<int>(text.size()));
}

std::string TextField::selected_text() const {
    const int a = std::min(cursor, selection_end);
    const int b = std::max(cursor, selection_end);
    return text.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
}

void TextField::write_text(std::string_view value) {
    std::string filtered;
    for (char c : value) {
        if (allowed(static_cast<unsigned char>(c))) {
            filtered.push_back(c);
        }
    }
    const int i = std::min(cursor, selection_end);
    const int j = std::max(cursor, selection_end);
    const int k = max_length - static_cast<int>(text.size()) - (i - j);
    std::string s;
    if (!text.empty()) {
        s += text.substr(0, static_cast<size_t>(i));
    }
    int l = 0;
    if (k < static_cast<int>(filtered.size())) {
        s += filtered.substr(0, static_cast<size_t>(std::max(0, k)));
        l = std::max(0, k);
    } else {
        s += filtered;
        l = static_cast<int>(filtered.size());
    }
    if (!text.empty() && j < static_cast<int>(text.size())) {
        s += text.substr(static_cast<size_t>(j));
    }
    text = std::move(s);
    move_cursor_by(i - selection_end + l);
}

void TextField::delete_from_cursor(int amount) {
    if (text.empty()) {
        return;
    }
    if (selection_end != cursor) {
        write_text("");
        return;
    }
    const bool back = amount < 0;
    const int i = back ? cursor + amount : cursor;
    const int j = back ? cursor : cursor + amount;
    std::string s;
    if (i >= 0) {
        s = text.substr(0, static_cast<size_t>(i));
    }
    if (j < static_cast<int>(text.size())) {
        s += text.substr(static_cast<size_t>(std::max(0, j)));
    }
    text = std::move(s);
    if (back) {
        move_cursor_by(amount);
    } else {
        set_cursor(cursor);
    }
}

int TextField::nth_word(int n, int pos) const noexcept {
    int i = pos;
    const bool back = n < 0;
    const int count = std::abs(n);
    const int len = static_cast<int>(text.size());
    for (int k = 0; k < count; ++k) {
        if (!back) {
            const size_t found = text.find(' ', static_cast<size_t>(std::max(0, i)));
            if (found == std::string::npos) {
                i = len;
            } else {
                i = static_cast<int>(found);
                while (i < len && text[static_cast<size_t>(i)] == ' ') {
                    ++i;
                }
            }
        } else {
            while (i > 0 && text[static_cast<size_t>(i - 1)] == ' ') {
                --i;
            }
            while (i > 0 && text[static_cast<size_t>(i - 1)] != ' ') {
                --i;
            }
        }
    }
    return i;
}

void TextField::delete_words(int count) {
    if (text.empty()) {
        return;
    }
    if (selection_end != cursor) {
        write_text("");
    } else {
        delete_from_cursor(nth_word(count, cursor) - cursor);
    }
}

bool TextField::key_typed(int ch, int key) {
    if (!focused) {
        return false;
    }
    const bool ctrl = ctrl_down();
    const bool shift = shift_down();
    if (key == input::KEY_A && ctrl && !shift) {
        set_cursor(static_cast<int>(text.size()));
        set_selection(0);
        return true;
    }
    if (key == input::KEY_C && ctrl && !shift) {
        set_clipboard(selected_text());
        return true;
    }
    if (key == input::KEY_V && ctrl && !shift) {
        write_text(get_clipboard());
        return true;
    }
    if (key == input::KEY_X && ctrl && !shift) {
        set_clipboard(selected_text());
        write_text("");
        return true;
    }
    switch (key) {
        case input::KEY_BACKSPACE:
            if (ctrl) {
                delete_words(-1);
            } else {
                delete_from_cursor(-1);
            }
            return true;
        case input::KEY_HOME:
            if (shift) {
                set_selection(0);
            } else {
                set_cursor(0);
            }
            return true;
        case input::KEY_LEFT:
            if (shift) {
                set_selection(ctrl ? nth_word(-1, selection_end) : selection_end - 1);
            } else if (ctrl) {
                set_cursor(nth_word(-1, cursor));
            } else {
                move_cursor_by(-1);
            }
            return true;
        case input::KEY_RIGHT:
            if (shift) {
                set_selection(ctrl ? nth_word(1, selection_end) : selection_end + 1);
            } else if (ctrl) {
                set_cursor(nth_word(1, cursor));
            } else {
                move_cursor_by(1);
            }
            return true;
        case input::KEY_END:
            if (shift) {
                set_selection(static_cast<int>(text.size()));
            } else {
                set_cursor(static_cast<int>(text.size()));
            }
            return true;
        case input::KEY_DEL:
            if (ctrl) {
                delete_words(1);
            } else {
                delete_from_cursor(1);
            }
            return true;
        default:
            break;
    }
    if (key == kNoKey && allowed(ch)) {
        const char c = static_cast<char>(ch);
        write_text(std::string_view(&c, 1));
        return true;
    }
    return false;
}

// =============================================================================================================
// TextInput / TextInputBox
// =============================================================================================================

TextInput::TextInput(std::string placeholder_, adjust::Font font_, int max_length)
    : placeholder(std::move(placeholder_)), font(font_) {
    field.max_length = max_length;
    set_width(1000);
}

void TextInput::set_width(int width) {
    // GuiUtilRenderComponents.splitText(text, max(width - 8, 5)): the lines partition the text exactly
    lines.clear();
    const std::string& raw = field.text;
    const float limit = static_cast<float>(std::max(width - 8, 5));
    size_t start = 0;
    while (start < raw.size()) {
        size_t end = start;
        size_t last_space = std::string::npos;
        while (end < raw.size()) {
            const float w = font.width(std::string_view(raw).substr(start, end - start + 1));
            if (w > limit && end > start) {
                break;
            }
            if (raw[end] == ' ') {
                last_space = end;
            }
            ++end;
        }
        if (end < raw.size() && last_space != std::string::npos && last_space + 1 > start) {
            end = last_space + 1;
        }
        lines.push_back(raw.substr(start, end - start));
        start = end;
    }
    if (lines.empty()) {
        lines.emplace_back();
    }
}

void TextInput::draw(float x, float y) {
    animation.update(focused ? (hovered ? 1.0f : 0.9f) : (hovered ? 0.4f : 0.0f));
    if (lines.empty()) {
        set_width(1000);
    }
    const std::string& text = field.text;
    if (!focused && text.empty()) {
        if (has_placeholder) {
            const int grey = static_cast<int>(30.0f * animation.value + 130.0f);
            font.draw(placeholder, x, y, adjust::argb(grey, grey, grey));
        }
        return;
    }
    const Argb color = adjust::blend(adjust::argb(199, 199, 199), adjust::argb(149, 149, 149), animation.value);
    int offset = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        const int cur = field.cursor;
        bool bar = false;
        std::string shown = line;
        if (focused && field.cursor_counter() / 6 % 2 == 0) {
            if (cur == static_cast<int>(text.size())) {
                if (i == lines.size() - 1) {
                    shown = line + ADJUST_FMT "f_";
                }
            } else {
                bar = (offset == 0 || offset <= cur) && offset + static_cast<int>(line.size()) > cur;
            }
        }
        const float line_y = y + static_cast<float>(i) * 7.5f;
        font.draw(shown, x, line_y, color);
        const int end = offset + static_cast<int>(line.size());
        if (focused) {
            const int sel = field.selection_end;
            const int from = std::min(cur, sel);
            const int to = std::max(cur, sel);
            if (cur != sel && end > from && to > offset) {
                const float sx = offset > from
                                     ? x
                                     : x + font.width(std::string_view(text).substr(
                                               static_cast<size_t>(offset), static_cast<size_t>(from - offset)));
                const int a = std::max(offset, from);
                const int b = std::min(end, to);
                const float w = font.width(
                    std::string_view(text).substr(static_cast<size_t>(a), static_cast<size_t>(std::max(0, b - a))));
                adjust::fill_rect(adjust::argb(228, 228, 228, 60), sx, line_y - 1.5f, sx + w, line_y - 1.5f + font.size());
            }
        }
        if (bar) {
            const float cx = x + font.width(std::string_view(text).substr(static_cast<size_t>(offset),
                                                                          static_cast<size_t>(cur - offset)));
            adjust::fill_rect(adjust::argb(166, 166, 166), cx, line_y - 1.5f, cx + 0.5f, line_y - 1.5f + font.size());
        }
        offset = end;
    }
}

bool TextInputBox::key_typed(int ch, int key) {
    if (key == input::KEY_ESC && input.focused) {
        input.focused = false;
        return true;
    }
    if (!input.focused) {
        return false;
    }
    input.field.set_focused(true);
    return input.field.key_typed(ch, key);
}

void TextInputBox::draw(Vec2 mouse, float at_x, float at_y) {
    const float h = 9.0f + static_cast<float>(std::max(0, static_cast<int>(input.lines.size()) - 1)) * 7.5f;
    adjust::push_even_scale();
    const math::Vec4f r = adjust::to_pixels(at_x, at_y, at_x + width, at_y + h);
    adjust::fill_rect(0x5A2D2D2Du, r.x, r.y, r.w, r.h);
    adjust::stroke_rect(true, 0x822D2D2Du, r.x, r.y, r.w, r.h, pixel_line_width());
    adjust::pop_even_scale();
    input.hovered = mouse.x >= at_x && mouse.y >= at_y && at_x + width > mouse.x && at_y + h > mouse.y;
    if (input.focused) {
        input.field.set_focused(true);
    } else {
        input.field.focused = false;
    }
    input.draw(at_x + 2.0f, at_y + 3.5f);
    x = at_x;
    y = at_y;
    height = h;
}

// =============================================================================================================
// AdjustButton
// =============================================================================================================

void Button::draw(float mx, float my) {
    if (!visible) {
        return;
    }
    hovered = mx >= x && my >= y && x + static_cast<float>(width) > mx && y + static_cast<float>(height) > my;
    hover.update(enabled && hovered ? 1.0f : 0.0f);
    const float h = hover.value;
    int grey = static_cast<int>(h * 25.0f + 201.0f);
    if (!enabled) {
        grey -= 40;
    }
    const int opacity = static_cast<int>(h * 30.0f);
    const auto am = [this](int a) { return alpha75 ? static_cast<int>(static_cast<float>(a) * 0.75f) : a; };
    adjust::push_even_scale();
    const math::Vec4f r = adjust::to_pixels(x, y, x + static_cast<float>(width), y + static_cast<float>(height));
    adjust::fill_rect(adjust::argb(base, base, base, clamp_int(am(opacity + 82), 0, 255)), r.x, r.y, r.w, r.h);
    adjust::stroke_rect(true, adjust::argb(base, base, base, clamp_int(am(opacity + 122), 0, 255)), r.x, r.y, r.w, r.h,
                        pixel_line_width());
    adjust::pop_even_scale();
    float center = static_cast<float>(width) / 2.0f + x;
    if (label.size() == 1 && font.family() == adjust::FontFamily::Tahoma) {
        center += 0.25f;
    }
    grey = clamp_int(grey, 0, 255);
    font.draw_centered(label, center + label_offset_x, static_cast<float>(height) / 2.0f + y - 1.5f + label_offset_y,
                       adjust::argb(grey, grey, grey, clamp_int(am(2 * opacity + 190), 0, 255)));
}

bool Button::is_hovered(float mx, float my) const noexcept {
    return enabled && visible && mx >= x && my >= y && x + static_cast<float>(width) > mx &&
           y + static_cast<float>(height) > my;
}

// =============================================================================================================
// Dialog (DialogBuilder / DialogScreen)
// =============================================================================================================

Dialog& Dialog::add_button(std::string_view text, Action action, Validator enabled) {
    ButtonRow row;
    row.button = Button(static_cast<int>(buttons_.size()), std::string(text),
                        static_cast<int>(adjust::kTahoma12.width(text) + 6.0f), 10, 62, false, adjust::kTahoma12);
    row.action = std::move(action);
    row.enabled = std::move(enabled);
    buttons_.push_back(std::move(row));
    return *this;
}

Dialog& Dialog::add_text_input(std::string_view label, std::string_view text, int max_length, int width) {
    TextInput input(std::string(label), adjust::kTahoma12, max_length);
    input.field.set_text(text);
    inputs_.push_back(InputRow{TextInputBox(std::move(input)), static_cast<int>(inputs_.size()), width});
    if (inputs_.size() == 1) {
        inputs_.front().box.input.focused = true; // build(): the first input is focused
    }
    return *this;
}

std::string Dialog::input_text(int index) const {
    for (const InputRow& row : inputs_) {
        if (row.index == index) {
            return row.box.input.field.text;
        }
    }
    return {};
}

bool Dialog::has_focused_input() const noexcept {
    for (const InputRow& row : inputs_) {
        if (row.box.input.focused) {
            return true;
        }
    }
    return false;
}

void Dialog::close() noexcept {
    transition_.duration = 220;
    closing_ = true;
}

float Dialog::message_width() const {
    float w = 0.0f;
    for (const Message::Part& p : message.parts) {
        w += adjust::kTahoma12.width(p.text);
    }
    return w;
}

bool Dialog::compute_width(float width) const {
    if (lines_.size() < 2) {
        return false;
    }
    float end = 2.5f;
    for (const Segment& s : lines_.back()) {
        end += adjust::kTahoma12.width(s.text);
    }
    for (const ButtonRow& b : buttons_) {
        width -= static_cast<float>(b.button.width) + 5.0f;
        if (width - end < 15.0f) {
            return false;
        }
    }
    return true;
}

void Dialog::update_size() {
    if (!laid_out_) {
        return;
    }
    const float width = std::max(std::min(std::max(message_width(), adjust::kTahoma12.width(title) + 4.5f), 170.0f),
                                 minimum_width);
    const bool inline_buttons = compute_width(width);
    float input_height = 0.0f;
    for (InputRow& row : inputs_) {
        row.box.input.set_width(row.width);
        const int count = static_cast<int>(row.box.input.lines.size());
        input_height += count >= 2 ? static_cast<float>(count - 1) * 7.5f + 10.0f : 10.0f;
    }
    if (input_height > 0.0f) {
        input_height += 10.0f;
    }
    size_ = Vec2{width, input_height + (30.0f - static_cast<float>(bottom_inset) - (inline_buttons ? 11.0f : 4.0f) +
                                        static_cast<float>(lines_.size()) * 10.0f)};
}

void Dialog::layout() {
    if (laid_out_) {
        return;
    }
    laid_out_ = true;
    const float width = std::max(std::min(message_width(), 170.0f), minimum_width);
    const float wrap = static_cast<float>(static_cast<int>(width - 2.0f));
    // GuiUtilRenderComponents.splitText: word wrap over the styled parts
    lines_.clear();
    lines_.emplace_back();
    float line_w = 0.0f;
    for (const Message::Part& part : message.parts) {
        const Argb color = part.italic ? adjust::argb(211, 211, 207, 255) : adjust::argb(199, 199, 199, 200);
        size_t i = 0;
        while (i < part.text.size()) {
            size_t j = i;
            while (j < part.text.size() && part.text[j] != ' ') {
                ++j;
            }
            while (j < part.text.size() && part.text[j] == ' ') {
                ++j;
            }
            std::string word = part.text.substr(i, j - i);
            const float w = adjust::kTahoma12.width(word);
            if (line_w + w > wrap && line_w > 0.0f) {
                lines_.emplace_back();
                line_w = 0.0f;
                size_t lead = 0;
                while (lead < word.size() && word[lead] == ' ') {
                    ++lead;
                }
                word.erase(0, lead);
            }
            if (!word.empty()) {
                if (!lines_.back().empty() && lines_.back().back().color == color) {
                    lines_.back().back().text += word;
                } else {
                    lines_.back().push_back(Segment{word, color});
                }
                line_w += adjust::kTahoma12.width(word);
            }
            i = j;
        }
    }
    update_size();
}

void Dialog::draw_inputs(Vec2 mouse, float x, float y) {
    float row_y = y + 12.0f + static_cast<float>(lines_.size()) * 10.0f;
    for (InputRow& row : inputs_) {
        row.box.input.set_width(row.width);
        row.box.width = static_cast<float>(row.width);
        row.box.draw(mouse, x + 2.5f, row_y);
        const int count = static_cast<int>(row.box.input.lines.size());
        row_y += count >= 2 ? 10.5f + static_cast<float>(count - 1) * 7.5f : 10.5f;
    }
}

void Dialog::draw_buttons(Vec2 mouse, float x, float y) {
    float right = x + size_.x - 3.0f;
    for (ButtonRow& row : buttons_) {
        row.button.enabled = !row.enabled || row.enabled(*this);
        row.button.x = right - static_cast<float>(row.button.width);
        row.button.y = y + size_.y - 13.0f;
        row.button.draw(mouse.x, mouse.y);
        right = row.button.x - 5.0f;
    }
}

void Dialog::render(const Window& window, Vec2 mouse) {
    transition_.update(closing_ ? 0.0f : 1.0f);
    if (closing_ && transition_.value == 0.0f) {
        finished_ = true;
        return;
    }
    layout();
    const float sw = static_cast<float>(window.scaled_width);
    const float sh = static_cast<float>(window.scaled_height);
    const float x = sw / 2.0f - size_.x / 2.0f;
    const float y = sh / 2.0f - size_.y / 2.0f;
    const float previous = adjust::global_alpha();
    adjust::set_global_alpha(transition_.value);
    dim_.update(115.0f);
    adjust::fill_rect(adjust::argb(0, 0, 0, clamp_int(static_cast<int>(dim_.value), 0, 255)), 0.0f, 0.0f, sw, sh);
    adjust::fill_rect(0x9619191Cu, x, y, x + size_.x, y + size_.y);
    adjust::stroke_rect(true, 0x96181818u, x, y, x + size_.x, y + size_.y, 0.5f);
    draw_accent_gradient(x, y, size_.x, 10.0f, 0.0f, 0.0f);
    adjust::kTahoma12.draw(title, x + 2.5f, y + 3.5f, 0xF5C8C8C8u);
    for (size_t i = 0; i < lines_.size(); ++i) {
        float lx = x + 2.5f;
        for (const Segment& s : lines_[i]) {
            adjust::kTahoma12.draw(s.text, lx, y + 15.0f + static_cast<float>(i) * 10.0f, s.color);
            lx += adjust::kTahoma12.width(s.text);
        }
    }
    draw_inputs(mouse, x, y);
    draw_buttons(mouse, x, y);
    adjust::set_global_alpha(previous);
}

bool Dialog::key_typed(int ch, int key) {
    for (size_t i = 0; i < inputs_.size(); ++i) {
        TextInput& input = inputs_[i].box.input;
        if (!input.focused) {
            continue;
        }
        if (key == input::KEY_TAB && inputs_.size() != 1) {
            input.focused = false;
            inputs_[i == inputs_.size() - 1 ? 0 : i + 1].box.input.focused = true;
            return true;
        }
        if (key == input::KEY_ESC || key == input::KEY_ENTER) {
            input.focused = false;
            return true;
        }
        input.field.set_focused(true);
        input.field.key_typed(ch, key);
        update_size();
        return true;
    }
    if (key != input::KEY_ESC) {
        return false;
    }
    close();
    return true;
}

bool Dialog::mouse_clicked(Vec2 mouse, int button, bool pressed, const Window& window) {
    if (!pressed || button != 0) {
        return true;
    }
    if (closing_) {
        return true;
    }
    layout();
    const auto mx = static_cast<float>(static_cast<int>(mouse.x));
    const auto my = static_cast<float>(static_cast<int>(mouse.y));
    for (ButtonRow& row : buttons_) {
        if (row.button.enabled && row.button.is_hovered(mx, my) && (!row.enabled || row.enabled(*this))) {
            if (row.action) {
                row.action(*this);
            }
            close();
            return true;
        }
    }
    const float x = static_cast<float>(window.scaled_width) / 2.0f - size_.x / 2.0f + 2.5f;
    float row_y = static_cast<float>(window.scaled_height) / 2.0f - size_.y / 2.0f + 12.0f +
                  static_cast<float>(lines_.size()) * 10.0f;
    TextInput* focused = nullptr;
    for (InputRow& row : inputs_) {
        const int count = static_cast<int>(row.box.input.lines.size());
        const float h = 10.5f + (count >= 2 ? static_cast<float>(count - 1) * 7.5f : 0.0f);
        if (mouse.x >= x && mouse.x <= x + static_cast<float>(row.width) && mouse.y >= row_y && mouse.y <= row_y + h) {
            focused = &row.box.input;
            break;
        }
        row_y += h;
    }
    for (InputRow& row : inputs_) {
        row.box.input.focused = false;
    }
    if (focused != nullptr) {
        focused->focused = true;
    }
    return true;
}

// =============================================================================================================
// Notifications / drawing helpers
// =============================================================================================================

void notify(std::string_view title, std::initializer_list<std::pair<std::string_view, bool>> parts) {
    adjust_notifications::NotificationBuilder builder(title);
    for (const auto& p : parts) {
        if (p.second) {
            builder.append(p.first);
        } else {
            builder.add_text(p.first);
        }
    }
    builder.show();
}

void draw_dim(adjust::Animation& anim, const Window& window, float target) {
    const float alpha = anim.update(target);
    const int opacity = static_cast<int>(alpha);
    adjust::fill_rect(adjust::argb(0, 0, 0, clamp_int(opacity + 20, 0, 255)), 0.0f, 0.0f,
                      static_cast<float>(window.scaled_width), static_cast<float>(window.scaled_height));
}

void draw_accent_gradient(float x, float y, float width, float height, float a, float b) {
    Argb base = hud::hud_color();
    const int alpha = clamp_int(static_cast<int>((b - a * 0.3f) * 50.0f + 180.0f), 0, 255);
    base = adjust::with_alpha(base, alpha);
    const Argb left = adjust::brighter(base, 0.5f - (a * 0.08f + 0.08f) * b);
    const Argb right = adjust::scale(hud::secondary_for(base), 1.0f - (a * 0.3f + 0.2f) * b);
    const math::Vec4f p = adjust::to_pixels(x, y, x + width, y + height);
    adjust::fill_gradient_rect(left, right, p.x, p.y, p.w, p.h, p.x, p.y, p.w, p.y);
}

void draw_check_box(const Box& bounds, float row_y, const std::string& text, Argb text_color, Argb box_fill) {
    const float x1 = bounds.x + 2.0f;
    const float y1 = row_y + 2.0f;
    const float x2 = x1 + 5.5f;
    const float y2 = y1 + 5.5f;
    adjust::fill_rect(0x620F0F13u, x1, y1, x2, y2);
    adjust::fill_rect(box_fill, x1 + 0.5f, y1 + 0.5f, x2 - 0.5f, y2 - 0.5f);
    adjust::kTahoma10.draw(text, x1 + 7.0f, row_y + 4.0f, text_color);
}

} // namespace slinky::modules::adjust_clickgui::detail
