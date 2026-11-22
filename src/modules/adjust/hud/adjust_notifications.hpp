#pragma once

// Public API of the Adjust notification stack (port of com.adjustclient.ui.hud.notification.{NotificationBuilder,
// NotificationAction, Notifications.add}) for other ported Adjust modules.
//
// Notifications are drawn by the Adjust HUD module (hud_notifications.cpp) in the bottom-right corner, stacked
// upwards. Like Adjust, a notification is only accepted while the HUD module is running (enabled + Adjust visual
// style selected). Every function is thread-safe.
//
// Quick use:
//     adjust_notifications::push("Builder", "You do not have enough materials in your hotbar.");
//
// Builder (mirrors NotificationBuilder):
//     adjust_notifications::NotificationBuilder("Bed Defense")
//         .add_text("Saved ")                // grey body text (0xFFBEBCBD)
//         .append(name)                      // text in the HUD accent colour (AccentText)
//         .add_text(".")
//         .show();
//     adjust_notifications::NotificationBuilder("Error").format("Failed to disable {}.", {"movement checks"}).show();
//
// Lifetime: by default 45 ms "steps" derived from the text length (max(22, 7 + 1.32 per character)); a fixed
// step count via lifetime_steps(), or a wall-clock duration via duration_ms(). Without a duration the countdown
// pauses while the game window is not focused (Adjust behaviour). A notification with a key replaces a visible
// notification that has the same key in place (keeping its position).

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "adjust/kit.hpp"

namespace slinky::modules::adjust_notifications {

using adjust::Argb;

// Default body text colour of NotificationBuilder.addText/format (0xFFBEBCBD).
inline constexpr Argb kTextColor = 0xFFBEBCBDu;
// Red used by Adjust for "failure" body parts (VirtualFileSystem serialization error: 4289219403L).
inline constexpr Argb kErrorTextColor = 0xFFA84B4Bu;

struct TextPart {
    std::string text;
    Argb color = kTextColor;
    bool accent = false; // AccentText: drawn in the current HUD colour, `color` ignored
};

// Clickable label under the body text (NotificationAction). Clicks work while a screen is open (mouse not
// grabbed), e.g. the chat or the ClickGUI.
struct Action {
    std::string label;
    std::function<void()> on_click;  // runs on the render thread, outside the notification lock
    Argb color = 0xFFFFFFFFu;        // label colour while hovered (blends to (167, 166, 165) when not hovered)
    bool dismiss_on_click = false;   // close the notification when clicked
};

struct NotificationSpec {
    std::string title;
    std::vector<TextPart> parts;
    std::string key;                       // empty = no key
    std::optional<int64_t> duration_ms;    // Cooldown duration (>= 0 = time based)
    std::optional<int> lifetime_steps;     // 45 ms steps; computed from the text when unset
    std::vector<Action> actions;
    bool clearable = false;
};

class NotificationBuilder {
public:
    NotificationBuilder() = default;
    explicit NotificationBuilder(std::string_view title) { spec_.title = std::string(title); }

    NotificationBuilder& title(std::string_view title) {
        spec_.title = std::string(title);
        return *this;
    }
    // addText(text, color)
    NotificationBuilder& add_text(std::string_view text, Argb color = kTextColor) {
        spec_.parts.push_back(TextPart{std::string(text), color, false});
        return *this;
    }
    // append(object): accent coloured part
    NotificationBuilder& append(std::string_view text) {
        spec_.parts.push_back(TextPart{std::string(text), kTextColor, true});
        return *this;
    }
    // format(pattern, args...): every "{}" becomes an accent part with the next argument, literal text is grey.
    NotificationBuilder& format(std::string_view pattern, std::initializer_list<std::string_view> args = {}) {
        std::string literal;
        auto arg = args.begin();
        for (size_t i = 0; i < pattern.size();) {
            if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
                if (!literal.empty()) {
                    add_text(literal);
                    literal.clear();
                }
                append(arg != args.end() ? *arg++ : std::string_view("{}"));
                i += 2;
            } else {
                literal.push_back(pattern[i]);
                ++i;
            }
        }
        if (!literal.empty()) {
            add_text(literal);
        }
        return *this;
    }
    NotificationBuilder& key(std::string_view key) {
        spec_.key = std::string(key);
        return *this;
    }
    NotificationBuilder& duration_ms(int64_t ms) {
        spec_.duration_ms = ms;
        return *this;
    }
    NotificationBuilder& lifetime_steps(int steps) {
        spec_.lifetime_steps = steps;
        return *this;
    }
    NotificationBuilder& clearable(bool value = true) {
        spec_.clearable = value;
        return *this;
    }
    NotificationBuilder& action(Action action) {
        spec_.actions.push_back(std::move(action));
        return *this;
    }

    const NotificationSpec& spec() const noexcept { return spec_; }

    // NotificationBuilder.show(): builds the notification and adds it to the HUD. False if it was not accepted
    // (HUD module not running).
    bool show() const noexcept;

private:
    NotificationSpec spec_{};
};

// Notifications.add(builder.build())
bool show(const NotificationSpec& spec) noexcept;

// Shortcut: title + one body part. duration_ms <= 0 = lifetime from the text length.
bool push(std::string_view title, std::string_view text, Argb color = kTextColor, int64_t duration_ms = 0) noexcept;

// Notifications.clearClearable(): removes every notification built with clearable(true).
void clear_clearable() noexcept;

// Number of queued notifications (including ones fading out).
size_t count() noexcept;

} // namespace slinky::modules::adjust_notifications
