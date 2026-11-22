// Port of com.adjustclient.module.impl.render.hud.widget.ClickSpeedWidget and ClickTracker.
//
// Adjust counts its InputEvent(LEFT / RIGHT), posted from Minecraft.clickMouse() / rightClickMouse(). OpenSlinky++
// already hooks exactly those two calls: they raise kEventPreAttackOrUseEntity (is_attack = true for clickMouse,
// false for rightClickMouse), which the HUD module forwards to HudElement::on_attack. So every click Minecraft
// processes is counted, including the synthetic clicks of the Auto Clicker.

#include "modules/adjust/hud/widgets/widgets.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>

#include "config/settings.hpp"
#include "minecraft/mc_wrappers.hpp"

namespace slinky::modules::adjust_hud::widgets {

namespace {

config::BoolSetting g_enabled{{}, "Click Speed", false, false, {}};

// ClickTracker: click timestamps of the last second + the 220 ms flash animation.
struct ClickTracker {
    std::deque<int64_t> clicks;
    adjust::Animation animation{adjust::Easing::Linear, 220};

    // Removal predicate of the click list: the click happened at least one second ago (Timer.hasElapsed(1000)).
    void drop_old() noexcept {
        const int64_t now = adjust::now_ms();
        while (!clicks.empty() && now - clicks.front() >= 1000) {
            clicks.pop_front();
        }
    }

    // Clicks per second: drops clicks older than one second and returns the count (at most 20).
    int cps() noexcept {
        drop_old();
        return static_cast<int>(std::min<size_t>(20, clicks.size()));
    }

    // Grey whose brightness (205 + 5 t) and alpha (185 + 9 t) rise with the flash and the click count,
    // t = animation + cps / 20 (just the animation without clicks).
    adjust::Argb color() noexcept {
        animation.update(0.0f);
        const float flash = animation.value;
        drop_old();
        const int count = static_cast<int>(std::min<size_t>(20, clicks.size()));
        int gray = 0;
        int alpha = 0;
        if (count == 0) {
            gray = static_cast<int>(flash * 5.0f + 205.0f);
            alpha = static_cast<int>(flash * 9.0f + 185.0f);
        } else {
            const float t = static_cast<float>(count) / 20.0f + flash;
            gray = static_cast<int>(t * 5.0f + 205.0f);
            alpha = static_cast<int>(t * 9.0f + 185.0f);
        }
        return adjust::argb(gray, gray, gray, alpha);
    }

    // ClickSpeedWidget.onClick: flash the tracker and record the click time.
    void click() noexcept {
        animation.value = 1.0f;
        animation.started_at = adjust::now_ms();
        animation.from = animation.value;
        animation.finished = false;
        clicks.push_back(adjust::now_ms());
        if (clicks.size() > 256) {
            clicks.pop_front(); // bound memory if the widget is not rendered for a while
        }
    }
};

ClickTracker g_trackers[2]{}; // [0] = left (InputAction.LEFT), [1] = right

} // namespace

void click_speed_render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    g_enabled.render(gui, renderer);
}

void click_speed_visit_config(config::ConfigArchive* ar) noexcept {
    g_enabled.visit(ar, "widgets_click_speed");
}

void click_speed_on_attack(events::AttackOrUseEntityEvent* ev) noexcept {
    if (!g_enabled.value || ev == nullptr || ev->ctx == nullptr) {
        return;
    }
    events::GameContext* ctx = ev->ctx;
    if (ctx->local_player == nullptr) {
        return; // clickMouse only posts its InputEvent with a player
    }
    if (!ev->is_attack) {
        // rightClickMouse posts its InputEvent only when the controller is not hitting a block.
        if (ctx->jni != nullptr && ctx->minecraft != nullptr) {
            jobject controller = minecraft::mc_get_player_controller(ctx->jni, ctx->minecraft);
            if (controller != nullptr) {
                const bool hitting = minecraft::controller_is_hitting_block(ctx->jni, controller);
                ctx->jni->env->DeleteLocalRef(controller);
                if (hitting) {
                    return;
                }
            }
        }
    }
    g_trackers[ev->is_attack ? 0 : 1].click();
}

void click_speed_render(const HudContext& /*ctx*/) noexcept {
    if (!g_enabled.value) {
        return;
    }
    const WidgetBox box = draw_frame("Click Speed", kClickSpeedX, kClickSpeedY, 50.0f, 17.0f);
    const float x = box.x;
    const float y = box.y;
    const adjust::Argb label_color = adjust::argb(215, 219, 213, 215);
    ClickTracker& left = g_trackers[0];
    ClickTracker& right = g_trackers[1];

    char text[32]{};
    adjust::kTahoma12.draw(ADJUST_FMT "lLeft", x + 4.5f, y + 13.0f, label_color);
    std::snprintf(text, sizeof(text), "%d CPS", left.cps());
    adjust::kTahoma10.draw(text, x + 11.0f - adjust::kTahoma10.width(text) * 0.5f, y + 21.0f, left.color());

    adjust::kTahoma12.draw(ADJUST_FMT "lRight", x + 28.5f, y + 13.0f, label_color);
    std::snprintf(text, sizeof(text), "%d CPS", right.cps());
    adjust::kTahoma10.draw(text, x + 37.0f - adjust::kTahoma10.width(text) * 0.5f, y + 21.0f, right.color());
}

} // namespace slinky::modules::adjust_hud::widgets
