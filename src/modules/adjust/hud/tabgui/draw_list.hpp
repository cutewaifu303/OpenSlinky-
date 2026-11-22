#pragma once

// Port of com.adjustclient.render.NvgRenderer (+ RectCommand, TextCommand, GradientRectCommand, PushScissorCommand,
// PopScissorCommand): a recorded list of draw commands whose bounds can be measured before it is flushed.
// The Tab GUI records its whole frame into one list, measures it and wraps it into a clip rectangle.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "adjust/kit.hpp"

namespace slinky::modules::adjust_hud::tabgui {

struct Rect2f {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

class DrawList {
public:
    enum class Kind : uint8_t {
        Rect,     // RectCommand
        Text,     // TextCommand (with shadow)
        Gradient, // GradientRectCommand (horizontal: start colour left, end colour right)
        PushClip, // PushScissorCommand (state command: ignored by the bounds)
        PopClip,  // PopScissorCommand (state command)
    };

    struct Command {
        Kind kind = Kind::Rect;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        adjust::Argb color = 0xFFFFFFFFu;
        adjust::Argb end_color = 0xFFFFFFFFu;
        const adjust::Font* font = nullptr;
        std::string text;

        bool is_state() const noexcept { return kind == Kind::PushClip || kind == Kind::PopClip; }
        Rect2f bounds() const noexcept;
    };

    // NvgRenderer.color / insertionIndex (-1 = append; otherwise the next command is inserted there, once).
    adjust::Argb color = 0xFFFFFFFFu;
    int insertion_index = -1;

    void clear() noexcept;
    DrawList& set_color(adjust::Argb c) noexcept {
        color = c;
        return *this;
    }

    void rect(float x, float y, float width, float height) noexcept;
    void text(const adjust::Font& font, std::string_view text, float x, float y) noexcept;
    void gradient_rect(float x, float y, float width, float height, adjust::Argb start, adjust::Argb end) noexcept;
    void push_clip(float x, float y, float width, float height) noexcept;
    void pop_clip() noexcept;

    Rect2f bounds() const noexcept; // getBounds (empty list -> all zero)
    float width() const noexcept;   // getWidth
    bool empty() const noexcept { return commands_.empty(); }

    // Draws every command with the Adjust kit (inside an open begin_hud frame).
    void flush() const noexcept;

private:
    void add(Command&& command) noexcept;
    size_t take_insert_index() noexcept;

    std::vector<Command> commands_;
    std::vector<size_t> clip_stack_; // indices of the open PushClip commands (NvgRenderer.transforms)
};

} // namespace slinky::modules::adjust_hud::tabgui
