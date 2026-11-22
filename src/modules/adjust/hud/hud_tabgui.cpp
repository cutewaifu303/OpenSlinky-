// Port of the Adjust "Tab GUI" HUD element: com.adjustclient.ui.tabgui.TabGui + ui.hud.tabgui.TabGuiSettings, driven
// the way HUDModule / HudElements drive it (onKey, onMouse, update, render; top-left anchor).
//
// Adjust receives key presses through its KeyBindingUpdateEvent / KeyInputEvent / MouseButtonInputEvent hooks and
// cancels the ones it consumes. OpenSlinky++ HUD elements have no key callback, so the element polls the client's
// key state table (GameContext::key_states, OpenSlinky++ key codes) with edge detection, every frame and every tick,
// and only while in game with no screen open (GameContext::in_game_focus). Consumed keys still reach Minecraft and
// the module keybinds (no hook to cancel them).
//
// The node tree lives in modules/adjust/hud/tabgui/ (nodes.*, tab_gui.*, draw_list.*, setting_capture.*).

#include <cstdint>

#include "config/settings.hpp"
#include "events/events.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/adjust/hud/tabgui/draw_list.hpp"
#include "modules/adjust/hud/tabgui/nodes.hpp"
#include "modules/adjust/hud/tabgui/tab_gui.hpp"
#include "util/keycodes.hpp"

namespace slinky::modules::adjust_hud {

namespace {

using tabgui::DrawList;
using tabgui::TabGui;
using tabgui::TabGuiSettings;

// LWJGL keyboard repeat (enabled by SliderComponent.keyEvent while an arrow key is held on a slider): Windows'
// default typematic delay / rate.
constexpr int64_t kRepeatDelayMs = 500;
constexpr int64_t kRepeatIntervalMs = 33;

TabGuiSettings g_settings{};
TabGui g_gui{g_settings};
DrawList g_draw{};

struct KeyPoller {
    uint8_t previous[input::KEY_COUNT]{};
    int repeat_key = 0;       // arrow key whose repeat is armed (0 = none)
    int64_t repeat_next = 0;  // next repeat time
};
KeyPoller g_keys{};

bool is_repeat_key(int key) noexcept {
    return key == input::KEY_UP || key == input::KEY_DOWN || key == input::KEY_RIGHT;
}

void dispatch(int key) noexcept {
    if (key >= input::KEY_MOUSE1 && key <= input::KEY_MOUSE5) {
        g_gui.on_mouse(true, key); // HUDModule.onMouseButton -> TabGui.onMouse
    } else {
        g_gui.on_key(key); // HUDModule.onKey (KeyBindingUpdateEvent) -> TabGui.onKey
    }
}

// Edge-detects key presses in the key state table and feeds them to the Tab GUI.
void poll_keys(events::GameContext* ctx, bool with_repeat) noexcept {
    if (ctx == nullptr || ctx->key_states == nullptr) {
        return;
    }
    const bool active = g_settings.enabled.value && ctx->in_game_focus;
    const int64_t now = adjust::now_ms();
    for (int key = 1; key < input::KEY_COUNT; ++key) {
        const uint8_t down = ctx->key_states[key] & 1u;
        const bool pressed = down != 0 && g_keys.previous[key] == 0;
        g_keys.previous[key] = down;
        if (!pressed || !active) {
            continue;
        }
        // SliderComponent.keyEvent runs before the key is handled: repeat only when the slider already had focus
        const bool slider_focused = g_gui.selected_is_slider();
        dispatch(key);
        if (is_repeat_key(key)) {
            g_keys.repeat_key = slider_focused ? key : 0;
            g_keys.repeat_next = now + kRepeatDelayMs;
        } else if (key == input::KEY_LEFT) {
            g_keys.repeat_key = 0; // Keyboard.enableRepeatEvents(false)
        }
    }

    if (!active) {
        g_keys.repeat_key = 0;
        return;
    }
    if (g_keys.repeat_key != 0) {
        if ((ctx->key_states[g_keys.repeat_key] & 1u) == 0) {
            g_keys.repeat_key = 0; // released: Keyboard.enableRepeatEvents(false)
        } else if (with_repeat && now >= g_keys.repeat_next && g_gui.selected_is_slider()) {
            dispatch(g_keys.repeat_key);
            g_keys.repeat_next = now + kRepeatIntervalMs;
        }
    }
}

void begin_pass(events::GameContext* ctx) noexcept {
    tabgui::TreeContext& tree = tabgui::tree_context();
    tree.game = ctx;
    ++tree.generation;
}

void end_pass() noexcept {
    tabgui::tree_context().game = nullptr;
}

void tick(events::GameContext* ctx) noexcept {
    begin_pass(ctx);
    poll_keys(ctx, false);
    g_gui.update();
    end_pass();
}

void render(const HudContext& ctx) noexcept {
    if (!g_settings.enabled.value || ctx.game == nullptr || ctx.frame == nullptr) {
        return;
    }
    begin_pass(ctx.game);
    poll_keys(ctx.game, true);
    g_gui.build(g_draw);
    g_draw.flush(); // top-left anchor: offset (0, 0)
    end_pass();
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    g_settings.enabled.render(gui, renderer);
    g_settings.gradient.render(gui, renderer);
    g_settings.auto_close.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    g_settings.enabled.visit(ar, "tabgui_enabled");
    g_settings.gradient.visit(ar, "tabgui_gradient");
    g_settings.auto_close.visit(ar, "tabgui_auto_close");
}

void on_client_shutdown() noexcept {
    g_gui.reset();
    g_draw.clear();
    g_keys = KeyPoller{};
}

} // namespace

const HudElement& element_tabgui() noexcept {
    static const HudElement s_element = [] {
        HudElement e{};
        e.name = "Tab GUI";
        e.render_settings = render_settings;
        e.visit_config = visit_config;
        e.tick = tick;
        e.render = render;
        e.on_client_shutdown = on_client_shutdown;
        return e;
    }();
    return s_element;
}

} // namespace slinky::modules::adjust_hud
