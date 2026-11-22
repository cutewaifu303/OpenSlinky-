// Port of com.adjustclient.ui.hud.notification.{Notifications, Notification, NotificationBuilder, NotificationAction}
// (+ the helpers it draws with: RenderUtil.drawShadowRect, TextComponent / AccentText).
//
// Cards stack upwards from the bottom-right corner: dark translucent box, accent "shadow" line on top, bold title in
// the HUD colour, body parts in Tahoma 12, optional clickable actions in Tahoma 10 and an "N more notifications..."
// line when more cards are queued than fit on half the screen. Lifetime runs in 45 ms steps (or a wall-clock
// duration), cards slide in (175 ms), move (190 ms) and fade (190 ms).
//
// The public API used by other ported modules lives in adjust_notifications.hpp. Client events wired here:
//   - profile loaded / saved / deleted (kEventProfileAction)      -> "Configuration" card (ConfigurationTab style)
//   - error notifications (kEventErrorNotification)               -> "Error" card, 200 steps (Module.reportToggleError)
//   - module toggles (kEventModuleStateChanged)                   -> optional, off by default: Adjust itself never
//                                                                    shows toggle notifications.

#include "modules/adjust/hud/adjust_notifications.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "adjust/jni_lookup.hpp"

namespace slinky::modules {

namespace {

using adjust::Animation;
using adjust::Argb;
using adjust::Easing;
using adjust_notifications::NotificationSpec;
using adjust_notifications::TextPart;

constexpr Argb kGrey = adjust::argb(167, 166, 165); // action / "more" text

// ---- model ----------------------------------------------------------------------------------------------------

struct ActionState {
    std::string label;
    std::function<void()> on_click;
    Animation hover{Easing::EaseOutSine, 150};
    Argb color = 0xFFFFFFFFu;
    bool dismiss_on_click = false;
};

// Notification (state: 0 = shown, 2 = closing, 3 = closed)
struct Notification {
    std::string title;
    std::vector<TextPart> parts;
    int64_t step_timer = 0;   // Timer.lastMs
    Animation slide{};
    Animation position{};
    Animation fade{};
    std::vector<ActionState> actions;
    bool has_duration = false; // Cooldown
    int64_t duration_ms = 0;
    int64_t duration_started = 0;
    std::string key;           // empty = no key
    int lifetime_steps = 0;
    int step = 0;
    uint8_t state = 0;
    bool action_run = false;
    bool clearable = false;
};

using NotificationPtr = std::shared_ptr<Notification>;

struct HoveredAction {
    NotificationPtr owner;
    size_t index = 0;
};

std::mutex g_mutex;
std::vector<NotificationPtr> g_list;
HoveredAction g_hovered{};
bool g_mouse_was_down = false;

struct Settings {
    // Not an Adjust setting: Adjust never notifies module toggles. Off by default to stay faithful.
    config::BoolSetting module_toggles{{}, "Module toggles", false, false, {}};
};

Settings g_settings{};

size_t utf8_length(std::string_view s) noexcept {
    size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) {
            ++n;
        }
    }
    return n;
}

// TextComponent.getColor / AccentText.getColor
Argb part_color(const TextPart& part) noexcept {
    return part.accent ? adjust_hud::hud_color() : part.color;
}

// NotificationBuilder.build()
NotificationPtr build(const NotificationSpec& spec) {
    auto n = std::make_shared<Notification>();
    const int64_t now = adjust::now_ms();
    n->title = spec.title;
    n->parts = spec.parts;
    int steps = 0;
    if (spec.lifetime_steps.has_value()) {
        steps = *spec.lifetime_steps;
    } else {
        // reading time: 7 + 1.32 per character over all parts, at least 22 steps of 45 ms
        int sum = 7;
        for (const TextPart& part : spec.parts) {
            sum += static_cast<int>(static_cast<float>(utf8_length(part.text)) * 1.32f);
        }
        steps = std::max(22, sum);
    }
    n->step_timer = now;
    n->slide = Animation(Easing::EaseOutSine, 0, now);
    n->position = Animation(Easing::EaseOutSine, 0, now);
    n->fade = Animation(Easing::Linear, 0, now);
    n->key = spec.key;
    n->lifetime_steps = steps;
    n->state = 0;
    n->action_run = false;
    n->clearable = spec.clearable;
    if (spec.duration_ms.has_value()) {
        n->has_duration = true;
        n->duration_ms = *spec.duration_ms;
        n->duration_started = now;
    }
    n->actions.reserve(spec.actions.size());
    for (const adjust_notifications::Action& a : spec.actions) {
        ActionState s{};
        s.label = a.label;
        s.on_click = a.on_click;
        s.color = a.color;
        s.dismiss_on_click = a.dismiss_on_click;
        n->actions.push_back(std::move(s));
    }
    return n;
}

// Notifications.find: same key and not closed (caller holds the lock)
Notification* find_by_key(const std::string& key) noexcept {
    for (const NotificationPtr& candidate : g_list) {
        if (!candidate->key.empty() && candidate->key == key && candidate->state != 3) {
            return candidate.get();
        }
    }
    return nullptr;
}

// Notifications.add: only while the HUD module is running; an equal key replaces the card in place.
bool add(NotificationPtr notification) {
    if (!events::module_is_running(get_adjust_hud_module())) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!notification->key.empty()) {
        if (Notification* existing = find_by_key(notification->key); existing != nullptr) {
            notification->position = existing->position;
            notification->slide = existing->slide;
            notification->fade = existing->fade;
            for (NotificationPtr& slot : g_list) {
                if (slot.get() == existing) {
                    slot = std::move(notification);
                    break;
                }
            }
            return true;
        }
    }
    g_list.push_back(std::move(notification));
    return true;
}

// Notifications.advance: one 45 ms step; true = remove the card.
bool advance(Notification& n, bool window_active, int64_t now) noexcept {
    if (n.step_timer + 45 > now) {
        return false;
    }
    if (!n.has_duration && !window_active) {
        return false;
    }
    if (n.state != 0) {
        if (n.state == 3) {
            return true;
        }
    } else {
        const int next = n.step + 1;
        bool expired = false;
        if (n.has_duration && n.duration_ms >= 0) {
            expired = n.duration_started + n.duration_ms <= now;
        } else {
            expired = next >= n.lifetime_steps;
        }
        if (expired) {
            n.state = 2;
        } else {
            n.step = next;
        }
    }
    n.step_timer = now;
    return false;
}

// Notifications.animate
bool animate(Notification& n, bool instant, float width, float height, float offset) noexcept {
    const uint8_t state = n.state;
    if (instant) {
        n.slide.duration = 0;
        n.position.duration = 0;
        n.fade.duration = 0;
    } else {
        n.slide.duration = 175;
        n.position.duration = 190;
        n.fade.duration = 190;
    }
    if (state == 2) {
        n.slide.update(0.0f);
        n.position.update(0.0f);
        n.fade.update(0.0f);
        if (n.fade.finished) {
            n.state = 3;
            return false;
        }
        return true;
    }
    if (state == 0) {
        n.slide.update(width + 6.0f);
        n.position.update(offset + (height + 6.0f));
        n.fade.update(1.15f);
    }
    return true;
}

// RenderUtil.drawShadowRect: half-pixel line fading in from both ends (no even scale is pushed here, so
// NvgContext.toPixels is the identity).
void draw_shadow_rect(Argb color, float x, float y, float width) noexcept {
    const Argb c = adjust::with_alpha(color, std::min(adjust::alpha_of(color), 230));
    const float middle = x + width / 2.0f;
    adjust::fill_gradient_rect(0u, c, x, y, middle, y + 0.5f, x, y, middle, y);
    adjust::fill_gradient_rect(c, 0u, middle, y, x + width, y + 0.5f, middle, y, x + width, y);
}

// ---- input / game state ------------------------------------------------------------------------------------------

bool chat_open(events::GameContext* game) noexcept {
    if (game == nullptr || game->jni == nullptr || game->minecraft == nullptr) {
        return false;
    }
    events::JniContext* jni = game->jni;
    static const jclass s_gui_chat = adjust::jni::find_class(jni, "awv", "net/minecraft/client/gui/GuiChat");
    jfieldID fid = jni->mappings->fid_mc_current_screen;
    if (s_gui_chat == nullptr || fid == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    jobject screen = env->GetObjectField(game->minecraft, fid);
    const bool open = screen != nullptr && env->IsInstanceOf(screen, s_gui_chat);
    if (screen != nullptr) {
        env->DeleteLocalRef(screen);
    }
    return open;
}

struct MouseState {
    float x = -1.0f;
    float y = -1.0f;
    bool clicked = false; // left button went down this frame while the cursor is free
};

// EntityRenderer: hudMouseX / hudMouseY from LWJGL Mouse in HUD units; click edge from Mouse.isButtonDown(0).
MouseState poll_mouse(events::GameContext* game, const adjust::HudFrame& frame) noexcept {
    MouseState m{};
    if (game == nullptr || game->jni == nullptr || game->minecraft == nullptr) {
        return m;
    }
    events::JniContext* jni = game->jni;
    JNIEnv* env = jni->env;
    const jclass mouse_cls = jni->mappings->cls_lwjgl_mouse;
    if (mouse_cls == nullptr) {
        return m;
    }
    static const jmethodID s_get_x = [&]() noexcept {
        jmethodID id = env->GetStaticMethodID(mouse_cls, "getX", "()I");
        if (id == nullptr) env->ExceptionClear();
        return id;
    }();
    static const jmethodID s_get_y = [&]() noexcept {
        jmethodID id = env->GetStaticMethodID(mouse_cls, "getY", "()I");
        if (id == nullptr) env->ExceptionClear();
        return id;
    }();
    const int display_w = minecraft::mc_get_display_width(jni, game->minecraft);
    const int display_h = minecraft::mc_get_display_height(jni, game->minecraft);
    if (s_get_x != nullptr && s_get_y != nullptr && display_w > 0 && display_h > 0) {
        const int raw_x = env->CallStaticIntMethod(mouse_cls, s_get_x);
        const int raw_y = env->CallStaticIntMethod(mouse_cls, s_get_y);
        if (!adjust::jni::clear_exception(jni)) {
            m.x = static_cast<float>(raw_x * frame.scaled_width) / static_cast<float>(display_w);
            m.y = static_cast<float>(frame.scaled_height) -
                  static_cast<float>(raw_y * frame.scaled_height) / static_cast<float>(display_h) - 1.0f;
        }
    }
    const bool down = minecraft::lwjgl_mouse_is_button_down(jni, 0);
    const bool grabbed = minecraft::lwjgl_mouse_is_grabbed(jni);
    m.clicked = down && !g_mouse_was_down && !grabbed;
    g_mouse_was_down = down;
    return m;
}

// Notifications.runClick: runs the action hovered in the previous frame. The callback is invoked outside the
// lock so it may push notifications itself.
void run_click() noexcept {
    std::function<void()> callback;
    std::string title;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hovered.owner == nullptr || g_list.empty() || g_hovered.owner->action_run ||
            g_hovered.index >= g_hovered.owner->actions.size()) {
            return;
        }
        ActionState& action = g_hovered.owner->actions[g_hovered.index];
        if (action.dismiss_on_click) {
            g_hovered.owner->state = 2;
        }
        callback = action.on_click;
        title = g_hovered.owner->title;
        g_hovered.owner->action_run = true;
    }
    if (callback) {
        try {
            callback();
        } catch (...) {
            std::fprintf(stderr, "Failed to run notification '%s' ClickEvent\n", title.c_str());
        }
    }
}

// ---- rendering (Notifications.draw) ----------------------------------------------------------------------------

void draw(const adjust::HudFrame& frame, bool in_chat, float mouse_x, float mouse_y) noexcept {
    const adjust::Font& font = adjust::kTahoma12;
    const adjust::Font& small = adjust::kTahoma10;
    const float scaled_w = static_cast<float>(frame.scaled_width);
    const float scaled_h = static_cast<float>(frame.scaled_height);

    float offset = in_chat ? 13.0f / std::max(1.0f, frame.scale - 1.0f) : 0.0f;
    const int limit = static_cast<int>((scaled_h / 2.0f) / 30.0f);
    if (limit < 1) {
        return;
    }
    const int size = static_cast<int>(g_list.size());
    for (int index = 0; index < size && index < limit; ++index) {
        const int more = (index == 0 && size > limit) ? size - limit : -1;
        const bool instant = size > limit;
        Notification& card = *g_list[static_cast<size_t>(index)];
        float height = more == -1 ? 20.0f : 27.0f;
        if (!card.actions.empty()) {
            height += 7.0f;
        }
        float width = 5.0f;
        for (const TextPart& part : card.parts) {
            width = font.width(part.text) + width;
        }
        width = std::max(width, font.width(card.title) + 12.0f);
        const float left = scaled_w - card.slide.value;
        if (!animate(card, instant, width, height, offset)) {
            continue;
        }
        const float alpha = std::max(card.fade.value - 0.15f, 0.0f);
        Argb accent = adjust_hud::hud_color();
        accent = adjust::with_alpha(accent, static_cast<int>(adjust::alpha_of(accent) * static_cast<double>(alpha)));
        const float top = scaled_h - card.position.value;
        const float right = width + left;
        const float bottom = height + top;
        adjust::fill_rect(adjust::argb(25, 25, 28, static_cast<int>(alpha * 130.0f)), left, top, right, bottom);
        adjust::stroke_rect(true, adjust::argb(25, 25, 28, static_cast<int>(alpha * 190.0f)), left, top, right,
                            bottom, 0.5f);
        draw_shadow_rect(accent, left, top, width);
        const Argb dark_accent = adjust::argb(static_cast<int>(adjust::red_of(accent) * 0.7),
                                              static_cast<int>(adjust::green_of(accent) * 0.7),
                                              static_cast<int>(adjust::blue_of(accent) * 0.7),
                                              adjust::alpha_of(accent));
        draw_shadow_rect(dark_accent, left, top + 0.5f, width);

        float text_x = left + 2.5f;
        font.draw(std::string(ADJUST_FMT "l") + card.title, text_x, top + 5.0f, accent);

        const float action_y = top + 21.5f;
        if (!card.actions.empty()) {
            adjust::push_scissor(left, top, width, height);
            float action_offset = 0.0f;
            for (size_t i = 0; i < card.actions.size(); ++i) {
                ActionState& action = card.actions[i];
                const float label_width = small.width(action.label);
                const float action_x = text_x + action_offset;
                if (g_hovered.owner == nullptr && mouse_x >= action_x && action_x + label_width >= mouse_x &&
                    mouse_y >= action_y && action_y + 5.0f >= mouse_y) {
                    g_hovered.owner = g_list[static_cast<size_t>(index)];
                    g_hovered.index = i;
                }
                const bool hovered = g_hovered.owner.get() == &card && g_hovered.index == i;
                action.hover.update(hovered ? 1.0f : 0.0f);
                const Argb color = adjust::blend(action.color | 0xFF000000u, kGrey, action.hover.value);
                small.draw(action.label, action_x, action_y, color);
                action_offset = (label_width + 5.0f) + action_offset;
            }
            adjust::pop_scissor();
        }
        if (more != -1) {
            adjust::push_scissor(left, top, width, height);
            char text[64];
            std::snprintf(text, sizeof(text), "%d more %s...", more, more == 1 ? "notification" : "notifications");
            const float more_y = (card.actions.empty() ? 0.0f : 7.0f) + action_y;
            small.draw(text, text_x, more_y, kGrey);
            adjust::pop_scissor();
        }
        const float body_y = top + 13.0f;
        const int body_alpha = std::clamp(static_cast<int>(alpha * 255.0f), 0, 255);
        for (const TextPart& part : card.parts) {
            Argb color = part_color(part) | 0xFF000000u;
            if (body_alpha != 255) {
                color = (color & 0x00FFFFFFu) | (static_cast<Argb>(body_alpha) << 24);
            }
            font.draw(part.text, text_x, body_y, color);
            text_x = font.width(part.text) + text_x;
        }
        offset = (height + 4.0f) + offset;
    }
}

// Notifications.render (HudMouseRenderEvent)
void render(const adjust_hud::HudContext& ctx) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_list.empty()) {
            g_hovered = {};
            return;
        }
    }
    if (ctx.frame == nullptr) {
        return;
    }
    const MouseState mouse = poll_mouse(ctx.game, *ctx.frame);
    if (mouse.clicked) {
        run_click(); // HUDModule.onMouse: left press runs the hovered action
    }
    const bool window_active = ctx.game != nullptr && ctx.game->jni != nullptr &&
                               minecraft::lwjgl_display_is_active(ctx.game->jni); // AdjustMainMenu.windowActive
    const bool in_chat = chat_open(ctx.game);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_hovered = {};
    const int64_t now = adjust::now_ms();
    g_list.erase(std::remove_if(g_list.begin(), g_list.end(),
                                [&](const NotificationPtr& n) noexcept { return advance(*n, window_active, now); }),
                 g_list.end());
    draw(*ctx.frame, in_chat, mouse.x, mouse.y);
}

// ---- client events -------------------------------------------------------------------------------------------

void on_module_state_changed(events::ModuleStateChangedEvent* ev) noexcept {
    if (!g_settings.module_toggles.value || ev == nullptr || ev->module == nullptr) {
        return;
    }
    events::Module* mod = ev->module;
    constexpr uint32_t kSkip = events::kModuleFlagGlobalSettings | events::kModuleFlagNoToggleSwitch;
    if ((mod->flags & kSkip) != 0 || mod == get_adjust_hud_module() ||
        !events::module_matches_style(mod, events::get_active_visual_style())) {
        return;
    }
    try {
        adjust_notifications::NotificationBuilder("Module")
            .append(mod->name != nullptr ? mod->name : "")
            .add_text(ev->enabled ? " has been enabled." : " has been disabled.")
            .key(std::string("module.toggle.") + events::module_config_key(mod))
            .show();
    } catch (...) {
    }
}

// Profile actions: 0 = loaded, 1 = saved, 2 = opened folder (no notification), 3 = deleted.
void on_profile_action(events::ProfileActionEvent* ev) noexcept {
    if (ev == nullptr || ev->action < 0 || ev->action > 3 || ev->action == 2) {
        return;
    }
    static constexpr const char* kVerbs[4] = {"Loaded configuration ", "Saved configuration ", nullptr,
                                              "Deleted configuration "};
    try {
        adjust_notifications::NotificationBuilder("Configuration")
            .add_text(kVerbs[ev->action])
            .append(ev->profile_name != nullptr ? ev->profile_name : "Default")
            .add_text(".")
            .show();
    } catch (...) {
    }
}

// Errors: ErrorReport.notification("Error") with 200 lifetime steps (Module.reportToggleError).
void on_error_notification(events::ErrorNotificationEvent* ev) noexcept {
    if (ev == nullptr) {
        return;
    }
    try {
        adjust_notifications::NotificationBuilder builder("Error");
        if (ev->custom_message != nullptr) {
            builder.add_text(ev->custom_message);
        } else {
            builder.format("Error code {}.", {std::to_string(ev->error_code)});
        }
        builder.lifetime_steps(200).show();
    } catch (...) {
    }
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    g_settings.module_toggles.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    g_settings.module_toggles.visit(ar, "notifications_module_toggles");
}

void on_client_shutdown() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hovered = {};
    g_list.clear();
}

} // namespace

// ---- public API (adjust_notifications.hpp) -----------------------------------------------------------------------

namespace adjust_notifications {

bool NotificationBuilder::show() const noexcept {
    return adjust_notifications::show(spec_);
}

bool show(const NotificationSpec& spec) noexcept {
    try {
        return add(build(spec));
    } catch (...) {
        return false;
    }
}

bool push(std::string_view title, std::string_view text, Argb color, int64_t duration_ms) noexcept {
    try {
        NotificationBuilder builder(title);
        builder.add_text(text, color);
        if (duration_ms > 0) {
            builder.duration_ms(duration_ms);
        }
        return builder.show();
    } catch (...) {
        return false;
    }
}

void clear_clearable() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_list.erase(std::remove_if(g_list.begin(), g_list.end(),
                                [](const NotificationPtr& n) noexcept { return n->clearable; }),
                 g_list.end());
}

size_t count() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_list.size();
}

} // namespace adjust_notifications

namespace adjust_hud {

const HudElement& element_notifications() noexcept {
    static const HudElement s_element = [] {
        HudElement e{};
        e.name = "Notifications";
        e.render_settings = render_settings;
        e.visit_config = visit_config;
        e.render = render;
        e.on_module_state_changed = on_module_state_changed;
        e.on_profile_action = on_profile_action;
        e.on_error_notification = on_error_notification;
        e.on_client_shutdown = on_client_shutdown;
        return e;
    }();
    return s_element;
}

} // namespace adjust_hud
} // namespace slinky::modules
