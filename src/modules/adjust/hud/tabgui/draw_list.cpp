#include "modules/adjust/hud/tabgui/draw_list.hpp"

#include <algorithm>
#include <climits>

namespace slinky::modules::adjust_hud::tabgui {

Rect2f DrawList::Command::bounds() const noexcept {
    if (kind == Kind::Text) {
        // TextCommand.getBounds: (x, y, stringWidth, font.size)
        Rect2f r{};
        r.x = x;
        r.y = y;
        r.width = font != nullptr ? font->width(text) : 0.0f;
        r.height = font != nullptr ? font->size() : 0.0f;
        return r;
    }
    return Rect2f{x, y, width, height};
}

void DrawList::clear() noexcept {
    commands_.clear();
    clip_stack_.clear();
    color = 0xFFFFFFFFu;
    insertion_index = -1;
}

size_t DrawList::take_insert_index() noexcept {
    const int index = insertion_index;
    insertion_index = -1;
    const size_t size = commands_.size();
    if (size == 0) {
        return 0;
    }
    if (index == -1 || size <= static_cast<size_t>(index)) {
        return size - 1;
    }
    return static_cast<size_t>(index);
}

void DrawList::add(Command&& command) noexcept {
    try {
        if (insertion_index == -1) {
            commands_.push_back(std::move(command));
            return;
        }
        const size_t at = take_insert_index();
        commands_.insert(commands_.begin() + static_cast<std::ptrdiff_t>(at), std::move(command));
        for (size_t& open : clip_stack_) {
            if (open >= at) {
                ++open;
            }
        }
    } catch (...) {
        // out of memory: drop the command
    }
}

void DrawList::rect(float x, float y, float width, float height) noexcept {
    Command c{};
    c.kind = Kind::Rect;
    c.x = x;
    c.y = y;
    c.width = width;
    c.height = height;
    c.color = color;
    add(std::move(c));
}

void DrawList::text(const adjust::Font& font, std::string_view text, float x, float y) noexcept {
    Command c{};
    c.kind = Kind::Text;
    c.x = x;
    c.y = y;
    c.color = color;
    c.font = &font;
    try {
        c.text.assign(text);
    } catch (...) {
        return;
    }
    add(std::move(c));
}

void DrawList::gradient_rect(float x, float y, float width, float height, adjust::Argb start,
                             adjust::Argb end) noexcept {
    Command c{};
    c.kind = Kind::Gradient;
    c.x = x;
    c.y = y;
    c.width = width;
    c.height = height;
    c.color = start;
    c.end_color = end;
    add(std::move(c));
}

void DrawList::push_clip(float x, float y, float width, float height) noexcept {
    if (!clip_stack_.empty() && clip_stack_.back() < commands_.size()) {
        Command& previous = commands_[clip_stack_.back()];
        if (previous.x + previous.width > x + width) {
            previous.width = x + width - previous.x;
        }
    }
    // index the command will land at (same rule as add / take_insert_index, without consuming it)
    size_t at = commands_.size();
    if (insertion_index != -1 && !commands_.empty()) {
        at = static_cast<size_t>(insertion_index) >= commands_.size() ? commands_.size() - 1
                                                                       : static_cast<size_t>(insertion_index);
    } else if (insertion_index != -1) {
        at = 0;
    }
    Command c{};
    c.kind = Kind::PushClip;
    c.x = x;
    c.y = y;
    c.width = width;
    c.height = height;
    add(std::move(c));
    try {
        clip_stack_.push_back(at);
    } catch (...) {
    }
}

void DrawList::pop_clip() noexcept {
    Command c{};
    c.kind = Kind::PopClip;
    add(std::move(c));
}

Rect2f DrawList::bounds() const noexcept {
    if (commands_.empty()) {
        return Rect2f{};
    }
    auto min_x = static_cast<float>(INT_MAX);
    auto min_y = static_cast<float>(INT_MAX);
    auto max_x = static_cast<float>(INT_MIN);
    auto max_y = static_cast<float>(INT_MIN);
    for (const Command& command : commands_) {
        if (command.is_state()) {
            continue;
        }
        const Rect2f r = command.bounds();
        min_x = std::min(min_x, r.x);
        min_y = std::min(min_y, r.y);
        max_x = std::max(max_x, r.x + r.width);
        max_y = std::max(max_y, r.y + r.height);
    }
    return Rect2f{min_x, min_y, max_x - min_x, max_y - min_y};
}

float DrawList::width() const noexcept {
    if (commands_.empty()) {
        return 0.0f;
    }
    auto min_x = static_cast<float>(INT_MAX);
    auto max_x = static_cast<float>(INT_MIN);
    for (const Command& command : commands_) {
        if (command.is_state()) {
            continue;
        }
        const Rect2f r = command.bounds();
        min_x = std::min(min_x, r.x);
        max_x = std::max(max_x, r.x + r.width);
    }
    return max_x - min_x;
}

void DrawList::flush() const noexcept {
    for (const Command& c : commands_) {
        switch (c.kind) {
            case Kind::Rect: {
                // NvgContext.toPixels returns (x1, y1, x2, y2) packed into Vec4f{x, y, w, h}
                const math::Vec4f p = adjust::to_pixels(c.x, c.y, c.x + c.width, c.y + c.height);
                adjust::fill_rect(c.color, p.x, p.y, p.w, p.h);
                break;
            }
            case Kind::Text:
                if (c.font != nullptr) {
                    c.font->draw(c.text, c.x, c.y, c.color);
                }
                break;
            case Kind::Gradient:
                adjust::fill_gradient_rect(c.color, c.end_color, c.x, c.y, c.x + c.width, c.y + c.height, c.x, c.y,
                                           c.x + c.width, c.y);
                break;
            case Kind::PushClip:
                adjust::push_scissor(c.x, c.y, c.width, c.height);
                break;
            case Kind::PopClip:
                adjust::pop_scissor();
                break;
        }
    }
}

} // namespace slinky::modules::adjust_hud::tabgui
