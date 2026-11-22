// Port of com.adjustclient.ui.hud.toggled.{ToggledList, ToggledEntry, ToggledSettings, ToggledColor, ToggledBorder,
// ToggledPadding}: Adjust's right-aligned module list with sliding entries.
//
// An entry is listed while its module is running (enabled + part of the active visual style) and not hidden
// from the HUD. Text = name + " §7" + suffix (OpenSlinky++ module HUD tag), Tahoma 12.

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"

namespace slinky::modules::adjust_hud {

namespace {

using adjust::Animation;
using adjust::Argb;
using adjust::Easing;

constexpr const char* kColorOptions[4] = {"Default", "Rainbow", "Astolfo", "Random"};
constexpr const char* kBorderOptions[3] = {"Full", "Right", "None"};
constexpr const char* kPaddingOptions[2] = {"Normal", "Compact"};
constexpr float kRowHeights[2] = {8.0f, 7.5f}; // ToggledPadding.rowHeight

enum ToggledColor : int32_t { kColorDefault = 0, kColorRainbow = 1, kColorAstolfo = 2, kColorRandom = 3 };
enum ToggledBorder : int32_t { kBorderFull = 0, kBorderRight = 1, kBorderNone = 2 };

struct Settings {
    config::BoolSetting enabled{{}, "Enabled", true, true, {}};
    config::BoolSetting background{{}, "Background", true, true, {}};
    config::BoolSetting suffix{{}, "Suffix", true, true, {}};
    config::ModeSetting color{{}, "Color", 4, 0, kColorOptions, 0, 0, false, {}};
    config::ModeSetting border{{}, "Border", 3, 0, kBorderOptions, 0, 0, false, {}};
    config::ModeSetting padding{{}, "Padding", 2, 0, kPaddingOptions, 0, 0, false, {}};
};

Settings g_settings{};

struct Entry {
    events::Module* module = nullptr;
    std::string text;
    float width = 0.0f;
    float target_x = 0.0f;
    float target_y = 0.0f;
    bool active = false;
    bool removing = false;
    bool has_x = false; // xAnim != null
    bool has_y = false; // yAnim != null
    Animation x_anim{};
    Animation y_anim{};
};

std::vector<Entry> g_entries;
std::vector<Entry*> g_visible;
events::ModuleManager* g_manager = nullptr;

float row_height() noexcept {
    const int idx = std::clamp(g_settings.padding.value, 0, 1);
    return kRowHeights[idx];
}

bool module_listed(const events::Module* mod) noexcept {
    return (mod->flags & events::kModuleFlagHiddenFromHud) == 0 && events::module_is_running(mod);
}

void ensure_entries(events::ModuleManager* mgr) noexcept {
    if (mgr == nullptr || (g_manager == mgr && g_entries.size() == mgr->module_count)) {
        return;
    }
    g_manager = mgr;
    g_entries.clear();
    g_entries.reserve(mgr->module_count);
    for (size_t i = 0; i < mgr->module_count; ++i) {
        Entry e{};
        e.module = mgr->all_modules[i];
        g_entries.push_back(e);
    }
}

// ToggledList.updateEntry
void update_entry(Entry& entry) noexcept {
    events::Module* mod = entry.module;
    entry.text = mod->name != nullptr ? mod->name : "";
    if (g_settings.suffix.value && mod->format_hud_tag != nullptr) {
        char tag[32]{};
        if (mod->format_hud_tag(tag) > 0 && tag[0] != '\0') {
            entry.text += " " ADJUST_FMT "7";
            entry.text += tag;
        }
    }
    entry.width = adjust::kTahoma12.width(entry.text);

    if (module_listed(mod)) {
        entry.removing = false;
        entry.target_x = -entry.width;
        entry.active = true;
    } else if (entry.active) {
        if (entry.has_x && entry.x_anim.value == entry.target_x && entry.removing) {
            entry.has_x = false;
            entry.has_y = false;
            entry.active = false;
        } else {
            entry.removing = true;
            entry.target_x = 4.0f;
        }
    }
}

// ToggledList.update + sort: refresh every entry, order widest first (ToggledEntry.compareTo, stable so equal
// widths keep their order), then lay out the visible rows. g_visible points into g_entries, so it is rebuilt
// only after sorting.
void update() noexcept {
    for (Entry& entry : g_entries) {
        update_entry(entry);
    }
    std::stable_sort(g_entries.begin(), g_entries.end(),
                     [](const Entry& a, const Entry& b) noexcept { return a.width > b.width; });

    g_visible.clear();
    int row = 0;
    const float rh = row_height();
    for (Entry& entry : g_entries) {
        if (!entry.active) {
            continue;
        }
        entry.target_y = static_cast<float>(row) * rh;
        if (!entry.has_x) {
            entry.x_anim = Animation(Easing::EaseOutExpo, 600);
            entry.x_anim.value = 3.5f;
            entry.has_x = true;
        }
        if (!entry.has_y) {
            entry.y_anim = Animation(Easing::EaseOutExpo, 600);
            entry.y_anim.value = entry.target_y - 16.0f;
            entry.has_y = true;
        } else if (entry.removing) {
            entry.target_y -= 16.0f;
        }
        if (!entry.removing) {
            ++row;
        }
        g_visible.push_back(&entry);
    }
}

Argb entry_color(const Entry& entry, float y, Argb primary, Argb secondary) noexcept {
    int mode = g_settings.color.value;
    if (mode == kColorDefault && hud_color_rainbow()) {
        mode = kColorRainbow;
    }
    const int64_t now = adjust::now_ms();
    switch (mode) {
        case kColorRainbow: {
            const float hue = (static_cast<float>(now % 5000) + (5000.0f - y * 30.0f)) / 5000.0f;
            return adjust::hsb_to_rgb(hue, 0.45f, 0.8f) | 0xFF000000u;
        }
        case kColorAstolfo: {
            const int offset = static_cast<int>(y * 50.0f);
            const float phase = static_cast<float>((now - offset) % 8000) / 4000.0f;
            const float hue = std::fabs(phase - 1.0f) * 0.5f + 0.5f;
            return adjust::hsb_to_rgb(hue, 0.45f, 0.8f) | 0xFF000000u;
        }
        case kColorRandom: {
            const auto h = std::hash<std::string_view>{}(entry.module->name != nullptr ? entry.module->name : "");
            return static_cast<Argb>(h) | 0xFF000000u;
        }
        case kColorDefault:
        default: {
            const double angle = static_cast<double>(now) * -0.003 + static_cast<double>(y * 0.075f);
            float t = (static_cast<float>(std::cos(angle)) + 1.0f) * 0.5f;
            t = std::clamp(t, 0.0f, 1.0f);
            return adjust::lerp_argb(primary, secondary, static_cast<double>(t), 1.0 - static_cast<double>(t));
        }
    }
}

// ToggledEntry.render
void render_entry(Entry& entry, const adjust::HudFrame& frame, const Entry* previous, bool last,
                  Argb primary, Argb secondary) noexcept {
    entry.x_anim.update(entry.target_x);
    entry.y_anim.update(entry.target_y);

    const float y = entry.y_anim.value;
    const float x = frame.width + entry.x_anim.value; // right aligned: the animation runs from -width to 0

    adjust::push_even_scale();

    const float rh = row_height();
    float right = x + entry.width;
    float bottom = y + rh;
    float left = x;
    float top = y;

    float bar_width = 0.5f;
    float inset = 3.0f;
    float inset_full = 3.5f;
    float ratio = 1.0f;
    const float factor = adjust::even_scale_factor();
    if (factor != 0.0f) {
        ratio = factor;
        const auto snapped = static_cast<float>(static_cast<int>(ratio));
        const float sx = ratio * left;
        const float sy = ratio * top;
        const float w = right - left;
        const float h = bottom - top;
        const float s_right = w > 1.0f ? right * ratio : sx + w * snapped;
        const float s_bottom = h > 1.0f ? bottom * ratio : sy + h * snapped;
        left = sx;
        top = sy;
        right = s_right;
        bottom = s_bottom;
        bar_width = snapped * 0.5f;
        inset_full = ratio * 3.5f;
        inset = ratio * 3.0f;
    }

    const int border = g_settings.border.value;
    if (g_settings.background.value) {
        const float bg_left = border == kBorderRight ? left - inset_full : left - inset;
        adjust::fill_rect(0x46131313u, bg_left, top, right, bottom);
    }

    const Argb color = entry_color(entry, y, primary, secondary);

    float text_x = x - 1.5f;
    if (border == kBorderFull) {
        adjust::fill_rect(color, (left - inset) - bar_width, top, left - inset, bottom);
        if ((!entry.removing || last) && previous != nullptr) {
            const float screen_w = frame.width;
            const float prev_x = screen_w + previous->x_anim.value;
            float connect_y = rh + previous->y_anim.value; // bottom of the previous row
            float base;
            if (entry.x_anim.finished) {
                base = (prev_x - (screen_w + entry.target_x)) + x;
                if (!previous->x_anim.finished && !previous->removing) {
                    connect_y = y; // previous row still sliding: connect at this row's top
                }
            } else {
                base = prev_x;
            }
            if (prev_x != x) {
                const float c_left = (base - 3.0f) * ratio - bar_width;
                const float c_top = connect_y * ratio;
                const float c_right = c_left + (x - prev_x) * ratio;
                adjust::fill_rect(color, c_left, c_top, c_right, c_top + bar_width);
            }
        }
        if (last) {
            adjust::fill_rect(color, (left - inset) - bar_width, bottom, ratio * entry.width + left,
                              bottom + bar_width);
        }
    } else if (border == kBorderRight) {
        adjust::fill_rect(color, right - bar_width, top, right, bottom);
        text_x -= 0.5f;
    }

    adjust::pop_even_scale();

    adjust::kTahoma12.draw(entry.text, text_x, (rh * 0.5f + y) - 1.5f, color);
}

void render(const HudContext& ctx) noexcept {
    if (!g_settings.enabled.value || ctx.game == nullptr || ctx.frame == nullptr) {
        set_toggled_list_bottom(0.0f);
        return;
    }
    ensure_entries(ctx.game->module_manager);
    update();

    const Argb primary = hud_color();
    const Argb secondary = secondary_or_darker(primary);

    float max_y = 0.0f;
    const Entry* previous = nullptr;
    const size_t count = g_visible.size();
    for (size_t i = 0; i < count; ++i) {
        Entry* entry = g_visible[i];
        render_entry(*entry, *ctx.frame, previous, i == count - 1, primary, secondary);
        if (!entry->removing) {
            previous = entry;
        }
        max_y = std::max(max_y, entry->y_anim.value);
    }

    const float extra = g_settings.border.value == kBorderFull ? 0.5f : 0.0f;
    set_toggled_list_bottom(count == 0 ? 0.0f : max_y + row_height() + extra);
}

// When the "Enabled" switch flips, the slide-in starts over (ToggledList.initialize listener).
bool g_last_enabled = true;

void tick(events::GameContext* /*ctx*/) noexcept {
    if (g_settings.enabled.value != g_last_enabled) {
        g_last_enabled = g_settings.enabled.value;
        for (Entry& e : g_entries) {
            e.has_x = false;
            e.has_y = false;
        }
    }
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    g_settings.enabled.render(gui, renderer);
    g_settings.background.render(gui, renderer);
    g_settings.suffix.render(gui, renderer);
    g_settings.color.render(gui, renderer);
    g_settings.border.render(gui, renderer);
    g_settings.padding.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    g_settings.enabled.visit(ar, "toggled_enabled");
    g_settings.background.visit(ar, "toggled_background");
    g_settings.suffix.visit(ar, "toggled_suffix");
    g_settings.color.visit(ar, "toggled_color");
    g_settings.border.visit(ar, "toggled_border");
    g_settings.padding.visit(ar, "toggled_padding");
}

void on_client_shutdown() noexcept {
    g_visible.clear();
    g_entries.clear();
    g_manager = nullptr;
}

} // namespace

const HudElement& element_toggled() noexcept {
    static const HudElement s_element = [] {
        HudElement e{};
        e.name = "Toggled";
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
