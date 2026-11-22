// Port of com.adjustclient.ui.mainmenu.AdjustMainMenu (+ VoronoiBackground, VoronoiCell, MenuParticle) and the
// pieces of ui.dialog (AdjustButton, DialogBuilder / DialogScreen / PopupTransition) the menu uses for its quit
// confirmation. See main_menu.hpp for how the menu is wired into Minecraft.
//
// Rebranding / scope:
//   * The "Adjust" title is drawn as SLINKY_BRAND_NAME ("OpenSlinky++"): first letter bold in the accent colour
//     (Tahoma 22), the rest in Tahoma 20, exactly like Adjust's "A" + "djust".
//   * Account Manager (alt manager) is replaced by "Mods" on Forge (GuiModList) / "Language" (GuiLanguage)
//     elsewhere; the friends icon button opens the OpenSlinky++ click GUI instead (plug icon of AdjustIcons2).
//   * Skipped (Adjust backend / services): changelog preview and update dialog, weekly poll, detection status,
//     subscription countdown, Hydra authentication / recovery screens. The Christmas Santa is Adjust artwork and
//     is not shipped (the Christmas snow and the Halloween webs are code-drawn and ported).
//   * Offline "Tip:" splash (first menu after start): Adjust's tips describe Adjust commands, so OpenSlinky++ tips
//     are shown in the same format.

#include "modules/adjust/mainmenu/main_menu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "nanovg.h"

#include "adjust/jni_lookup.hpp"
#include "adjust/kit.hpp"
#include "core/branding.hpp"
#include "events/module_manager.hpp"
#include "gui/gui_renderer.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/adjust/mainmenu/voronoi.hpp"
#include "modules/visual/gui.hpp"

namespace slinky::modules {

namespace {

using adjust::Argb;
using adjust_main_menu::JavaRandom;
using adjust_main_menu::Point;

constexpr float kPi = 3.14159265358979323846f;

// AdjustIcons2.ttf: 'a' = plug (used for the click GUI button that replaces Adjust's friends button).
constexpr adjust::Font kIcons2_13 = adjust::icons2(13.0f);

enum class Holiday { None, Halloween, Christmas };

// ServiceUtil.resolveHolidayTheme(): October -> Halloween, December -> Christmas.
Holiday resolve_holiday_theme() noexcept {
    const std::time_t t = std::time(nullptr);
    const std::tm* local = std::localtime(&t);
    if (local == nullptr) {
        return Holiday::None;
    }
    if (local->tm_mon == 9) {
        return Holiday::Halloween;
    }
    if (local->tm_mon == 11) {
        return Holiday::Christmas;
    }
    return Holiday::None;
}

// ---- small helpers --------------------------------------------------------------------------------------------

// net.minecraft.util.StringUtils.stripControlCodes
std::string strip_control_codes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) == 0xC2 && i + 1 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            i += 2; // skip "§" and the code character
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string java_string(JNIEnv* env, jstring str) {
    if (env == nullptr || str == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(str, nullptr);
    if (chars == nullptr) {
        env->ExceptionClear();
        return {};
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(str, chars);
    return out;
}

// Length in bytes of the first UTF-8 character.
size_t first_char_length(std::string_view text) noexcept {
    if (text.empty()) {
        return 0;
    }
    const auto c = static_cast<unsigned char>(text[0]);
    size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    return std::min(len, text.size());
}

JavaRandom& ui_random() noexcept {
    static JavaRandom s_random(static_cast<int64_t>(adjust::now_ms()) * 31 + 7);
    return s_random;
}

// org.apache.commons.lang3.RandomUtils.nextFloat(start, end)
float random_float(float start, float end) noexcept {
    if (start == end) {
        return start;
    }
    return start + (end - start) * ui_random().next_float();
}

// ---- AdjustButton ---------------------------------------------------------------------------------------------

struct MenuButton {
    int id = 0;
    float x = 0.0f;
    float y = 0.0f;
    int width = 0;
    int height = 0;
    std::string label;
    adjust::Font font = adjust::kTahoma12;
    bool symbol_font = false;
    int base = 26;
    bool enabled = true;
    bool visible = true;
    bool hovered = false;
    adjust::Animation hover{adjust::Easing::Linear, 111};

    bool hit(float mx, float my) const noexcept {
        return enabled && visible && mx >= x && my >= y && x + static_cast<float>(width) > mx &&
               y + static_cast<float>(height) > my;
    }
};

MenuButton make_button(int id, float x, float y, int width, int height, std::string label) {
    MenuButton b{};
    b.id = id;
    b.x = x;
    b.y = y;
    b.width = width;
    b.height = height;
    b.label = std::move(label);
    return b;
}

// AdjustButton.draw (alphaModifier = identity for the menu and the dialog)
void draw_button(MenuButton& b, float mouse_x, float mouse_y, bool hover_allowed) noexcept {
    if (!b.visible) {
        return;
    }
    b.hovered = hover_allowed && mouse_x >= b.x && mouse_y >= b.y && b.x + static_cast<float>(b.width) > mouse_x &&
                b.y + static_cast<float>(b.height) > mouse_y;
    b.hover.update(b.enabled && b.hovered ? 1.0f : 0.0f);
    const float hover = b.hover.value;
    int text_grey = static_cast<int>(hover * 25.0f + 201.0f);
    if (b.label == "Quit Playing") {
        text_grey -= 40;
    }
    if (!b.enabled) {
        text_grey -= 40;
    }
    const int opacity = static_cast<int>(hover * 30.0f);

    adjust::push_even_scale();
    const math::Vec4f r = adjust::to_pixels(b.x, b.y, b.x + static_cast<float>(b.width),
                                            b.y + static_cast<float>(b.height));
    adjust::fill_rect(adjust::argb(b.base, b.base, b.base, opacity + 82), r.x, r.y, r.w, r.h);
    const float factor = adjust::even_scale_factor();
    const float scale = factor == 0.0f ? 1.0f : static_cast<float>(static_cast<int>(factor));
    adjust::stroke_rect(true, adjust::argb(b.base, b.base, b.base, opacity + 122), r.x, r.y, r.w, r.h, scale * 0.5f);
    adjust::pop_even_scale();

    float center = static_cast<float>(b.width) / 2.0f + b.x;
    if (b.label.size() == 1 && !b.symbol_font) {
        center += 0.25f;
    }
    b.font.draw_centered(b.label, center, static_cast<float>(b.height) / 2.0f + b.y - 1.5f,
                         adjust::argb(text_grey, text_grey, text_grey, 2 * opacity + 190));
}

// ---- MenuParticle ---------------------------------------------------------------------------------------------

struct Particle {
    bool has_update = false;
    int64_t last_update = 0;
    float x = 0.0f;
    float y = 0.0f;
};

void draw_particle(Particle& p, float mouse_x, float mouse_y) noexcept {
    const int64_t now = adjust::now_ms();
    if (!p.has_update || now - p.last_update >= 31) {
        const float elapsed = !p.has_update ? 1.0f : static_cast<float>(now - p.last_update) / 30.0f;
        p.y += 0.5f * elapsed;
        p.last_update = now;
        p.has_update = true;
    }
    const float dx = mouse_x - (p.x + 0.375f);
    const float dy = mouse_y - (p.y + 0.375f);
    const float distance = std::sqrt(dx * dx + dy * dy);
    const int alpha = std::min(255, std::max(0, static_cast<int>(40.0f - distance * 0.2f)) + 40);
    adjust::fill_circle(adjust::argb(240, 240, 240, alpha), p.x, p.y, 0.75f);
}

// ---- VoronoiBackground / VoronoiCell --------------------------------------------------------------------------

struct VoronoiCell {
    adjust_main_menu::VoronoiPolygon polygon;
    bool has_animation = false;
    adjust::Animation animation{};
    float brightness = 0.0f;
    bool fading = false;

    void update() noexcept {
        if (fading || animation.finished) {
            if (!fading) {
                fading = true;
                animation.duration = 100;
                animation.started_at = adjust::now_ms();
                animation.from = animation.value;
                animation.finished = false;
            }
            animation.update(0.0f);
        } else {
            animation.update(brightness);
        }
    }

    void highlight(float target) noexcept {
        if (!has_animation) {
            animation = adjust::Animation(adjust::Easing::Linear, 20);
            has_animation = true;
        }
        fading = false;
        brightness = target;
        animation.started_at = adjust::now_ms();
        animation.from = animation.value;
        animation.finished = false;
    }
};

struct Voronoi {
    std::vector<VoronoiCell> cells;
    int32_t seed = 0;
    int width = 0;
    int height = 0;
    bool has_previous_color = false;
    Argb previous_color = 0;
    bool has_transition = false;
    adjust::Animation transition{};
    int64_t reset_timer = 0;
};

Voronoi g_voronoi{};

// VoronoiBackground.update() - without the changelog close/open transition (afterClose is never set here).
float voronoi_update() noexcept {
    if (!g_voronoi.has_transition) {
        return 1.0f;
    }
    return std::clamp(g_voronoi.transition.value, 0.0f, 1.0f);
}

// VoronoiBackground.reset()
void voronoi_reset() noexcept {
    if (!g_voronoi.has_transition) {
        return;
    }
    const int64_t now = adjust::now_ms();
    if (g_voronoi.reset_timer + 500 <= now) {
        g_voronoi.reset_timer = now;
        adjust::Animation& t = g_voronoi.transition;
        t.value = -1.3f;
        t.duration = 280;
        t.started_at = now;
        t.from = t.value;
        t.finished = false;
    }
}

// VoronoiBackground.blendBackground(Color)
Argb voronoi_blend_background(Argb color) noexcept {
    if (!g_voronoi.has_previous_color) {
        return color;
    }
    if (!g_voronoi.has_transition) {
        g_voronoi.transition = adjust::Animation(adjust::Easing::Linear, 220);
        g_voronoi.has_transition = true;
    }
    g_voronoi.transition.update(1.0f);
    if (g_voronoi.transition.finished) {
        g_voronoi.has_previous_color = false;
        g_voronoi.has_transition = false;
        return color;
    }
    return adjust::blend(color, g_voronoi.previous_color, g_voronoi.transition.value);
}

// VoronoiBackground.resize(width, height) + createSites
void voronoi_resize(int width, int height, const std::string& username) {
    if (width == g_voronoi.width && height == g_voronoi.height) {
        return;
    }
    // Adjust: HashUtil.hash(Changelog.latest.id + Adjust.username) - the brand stands in for the changelog id.
    const std::string key = std::string(SLINKY_BRAND_NAME) + username;
    g_voronoi.seed = adjust_main_menu::adjust_string_hash(key.c_str());

    JavaRandom random(g_voronoi.seed);
    const int count = random.next_int(18) + 114;
    std::vector<Point> sites;
    sites.reserve(static_cast<size_t>(count));
    double min_x = 1e30;
    double min_y = 1e30;
    double max_x = -1e30;
    double max_y = -1e30;
    for (int i = 0; i < count; ++i) {
        // Java evaluates the Coordinate arguments left to right: x first.
        const double x = random.next_double() * width;
        const double y = random.next_double() * height;
        sites.push_back(Point{x, y});
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }
    // JTS VoronoiDiagramBuilder clips to the site envelope expanded by max(envelope width, envelope height).
    const double expand = std::max(max_x - min_x, max_y - min_y);
    std::vector<adjust_main_menu::VoronoiPolygon> polygons =
        adjust_main_menu::build_voronoi(sites, min_x - expand, min_y - expand, max_x + expand, max_y + expand);

    g_voronoi.cells.clear();
    g_voronoi.cells.resize(polygons.size());
    for (size_t i = 0; i < polygons.size(); ++i) {
        g_voronoi.cells[i].polygon = std::move(polygons[i]);
    }
    g_voronoi.width = width;
    g_voronoi.height = height;
}

// VoronoiBackground.draw(screen, mouseX, mouseY, baseColor). Drawn with plain GL in Adjust, so the NanoVG global
// alpha does not apply (the caller sets it to 1).
void voronoi_draw(NVGcontext* vg, int screen_width, int screen_height, int mouse_x, int mouse_y,
                  Argb base_color) noexcept {
    if (vg == nullptr || g_voronoi.cells.empty()) {
        return;
    }
    nvgSave(vg);
    nvgTranslate(vg, -(static_cast<float>(mouse_x) - static_cast<float>(screen_width) * 0.5f) / 60.0f,
                 -(static_cast<float>(mouse_y) - static_cast<float>(screen_height) * 0.5f) / 60.0f);

    JavaRandom random(static_cast<int32_t>(static_cast<uint32_t>(g_voronoi.seed) + 1u));
    for (size_t i = 0; i < g_voronoi.cells.size(); ++i) {
        VoronoiCell& cell = g_voronoi.cells[i];
        const std::vector<Point>& points = cell.polygon.points;
        const float variation = random.next_float();
        float brightness = random.next_float() * 0.038f + 0.8825f;
        if (adjust_main_menu::polygon_contains(points, mouse_x, mouse_y)) {
            cell.highlight(1.0f);
            for (int neighbour : cell.polygon.neighbours) {
                if (neighbour >= 0 && static_cast<size_t>(neighbour) < g_voronoi.cells.size() &&
                    static_cast<size_t>(neighbour) != i) {
                    g_voronoi.cells[static_cast<size_t>(neighbour)].highlight(0.78f);
                }
            }
        }
        if (cell.has_animation) {
            cell.update();
            brightness += (variation * 0.00015f + 0.02475f) * cell.animation.value;
            if (cell.animation.value == 0.0f && cell.animation.finished) {
                cell.has_animation = false;
            }
        }
        const Argb color = voronoi_blend_background(base_color);
        float hsb[3];
        adjust::rgb_to_hsb(color, hsb);
        hsb[1] = std::max(0.01f, std::min(0.7f, hsb[1]));
        hsb[2] = std::max(0.61f, std::min(0.78f, hsb[2]));
        hsb[1] *= random.next_float() * 0.001105f + 0.5f;
        hsb[2] *= random.next_float() * 0.00102f + 0.4f;
        const Argb shaded = adjust::scale(adjust::hsb_to_rgb(hsb[0], hsb[1], hsb[2]), brightness);
        if (points.size() < 3) {
            continue;
        }
        nvgBeginPath(vg);
        nvgMoveTo(vg, static_cast<float>(points[0].x), static_cast<float>(points[0].y));
        for (size_t k = 1; k < points.size(); ++k) {
            nvgLineTo(vg, static_cast<float>(points[k].x), static_cast<float>(points[k].y));
        }
        nvgClosePath(vg);
        nvgFillColor(vg, adjust::to_nvg(adjust::with_alpha(shaded, 255)));
        nvgFill(vg);
    }
    nvgRestore(vg);
}

// AdjustMainMenu.drawCircle (GL lines with GL_LINE_SMOOTH, 1 px wide)
void draw_web_circle(NVGcontext* vg, int segments, Argb color, float x, float y, float radius) noexcept {
    if (vg == nullptr || segments <= 0) {
        return;
    }
    nvgSave(vg);
    nvgTranslate(vg, x, y);
    adjust::enable_anti_alias();
    nvgBeginPath(vg);
    for (int i = 0; i < segments; ++i) {
        const float angle = 2.0f * static_cast<float>(i) * kPi / static_cast<float>(segments);
        nvgMoveTo(vg, 0.0f, 0.0f);
        nvgLineTo(vg, std::cos(angle) * radius, std::sin(angle) * radius);
    }
    for (int ring = 1; ring <= segments; ++ring) {
        const float ring_radius = static_cast<float>(ring) * radius / static_cast<float>(segments);
        for (int i = 0; i < segments; ++i) {
            const float angle = 2.0f * static_cast<float>(i) * kPi / static_cast<float>(segments);
            const float px = std::cos(angle) * ring_radius;
            const float py = std::sin(angle) * ring_radius;
            if (i == 0) {
                nvgMoveTo(vg, px, py);
            } else {
                nvgLineTo(vg, px, py);
            }
        }
        nvgClosePath(vg);
    }
    const float scale = adjust::current_scale();
    nvgStrokeColor(vg, adjust::to_nvg(color));
    nvgStrokeWidth(vg, scale > 0.0f ? 1.0f / scale : 0.5f);
    nvgStroke(vg);
    adjust::disable_anti_alias();
    nvgRestore(vg);
}

// ---- quit confirmation (DialogBuilder("Are you sure?") / DialogScreen / PopupTransition) ---------------------

constexpr const char* kDialogTitle = "Are you sure?";
constexpr const char* kDialogMessage = "Exit Minecraft?";

struct QuitDialog {
    bool open = false;
    bool closing = false;
    adjust::Animation dim{adjust::Easing::Linear, 150};
    adjust::Animation popup{adjust::Easing::EaseOutExpo, 220};
    std::vector<MenuButton> buttons; // [0] Exit, [1] Cancel (laid out right to left)

    void show() {
        open = true;
        closing = false;
        dim = adjust::Animation(adjust::Easing::Linear, 150);
        popup = adjust::Animation(adjust::Easing::EaseOutExpo, 220);
        buttons.clear();
        for (const char* text : {"Exit", "Cancel"}) {
            MenuButton b = make_button(static_cast<int>(buttons.size()), 0.0f, 0.0f,
                                       static_cast<int>(adjust::kTahoma12.width(text) + 6.0f), 10, text);
            b.base = 62;
            buttons.push_back(std::move(b));
        }
    }

    // PopupTransition.close()
    void close() noexcept {
        popup.duration = 220;
        closing = true;
    }

    float width() const noexcept {
        const float message = adjust::kTahoma12.width(kDialogMessage);
        const float title = adjust::kTahoma12.width(kDialogTitle) + 4.5f;
        return std::max(std::min(std::max(message, title), 170.0f), 70.0f);
    }

    // One message line, no inputs / dropdowns: 30 - bottomInset - 4 + lines * 10.
    static constexpr float height() noexcept { return 36.0f; }
};

// RenderUtil.drawAccentGradient(x, y, width, height, 0, 0)
void draw_accent_gradient(float x, float y, float width, float height) noexcept {
    const Argb base = adjust::with_alpha(adjust_hud::hud_color(), 180);
    const Argb left = adjust::brighter(base, 0.5f);
    const Argb right = adjust::scale(adjust_hud::secondary_for(base), 1.0f);
    const math::Vec4f p = adjust::to_pixels(x, y, x + width, y + height);
    adjust::fill_gradient_rect(left, right, p.x, p.y, p.w, p.h, p.x, p.y, p.w, p.y);
}

// ---- module state ---------------------------------------------------------------------------------------------

constexpr const char* kOfflineTips[] = {
    "Press *Right Shift* to open the click GUI",
    "Switch between the *Slinky++* and *Adjust* visual styles in the bottom-right corner of the click GUI",
    "The plug button below opens the click GUI",
    "Turn this menu off with the *Main Menu* module in the click GUI",
    "Middle-click a module in the click GUI to bind it",
    "Don't forget to smile once in a while",
};

void on_gui_frame_poll(void* event) noexcept;
void on_gui_render_2d(void* event) noexcept;

struct MainMenuState {
    events::Module base{};

    // Hook bookkeeping
    bool draw_hook_seen = false;   // the drawScreen hook is installed (ran at least once)
    bool input_hook_seen = false;  // the handleInput hook is installed (ran at least once)
    bool drawn_since_poll = false; // the menu took over a drawScreen call since the last GUI poll
    bool visible = false;          // drawn on the GUI layer this poll
    bool active = false;           // the menu is currently displayed (initGui ran, not closed yet)
    int64_t last_poll_ms = 0;      // last kEventGuiFramePoll (the GUI layer that draws the menu is alive)
    jweak screen = nullptr;        // the GuiMainMenu instance being restyled

    // GuiScreen state captured from drawScreen
    int width = 0;
    int height = 0;
    int mouse_x = 0;
    int mouse_y = 0;

    // AdjustMainMenu fields
    std::string splash = "missingno"; // constructor (vanilla's GuiMainMenu.splashText)
    std::string original_splash;
    bool opening_splash = false;
    bool interactive = false;
    std::vector<MenuButton> buttons;
    std::vector<Particle> particles;
    int particle_ticks = 0;
    int64_t last_tick_ms = 0;
    QuitDialog dialog{};
    bool open_click_gui_requested = false;

    // Cached client info
    bool info_loaded = false;
    std::string username;
    std::string label_singleplayer = "Singleplayer";
    std::string label_multiplayer = "Multiplayer";
    bool forge = false;

    MainMenuState() noexcept {
        base.name = "Main Menu";
        base.description = "Replaces the title screen with Adjust's animated main menu.";
        base.category = events::ModuleCategory::Visual;
        base.enabled = true; // Adjust always shows its menu
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagHiddenFromHud |
                     events::kModuleFlagGlobalSettings;
        base.keybind = 0;
        base.hold_mode = false;
        base.events[events::kEventGuiFramePoll] = {on_gui_frame_poll, 0, 0};
        // Below the Adjust HUD (100, notifications) and the click GUI (0).
        base.events[events::kEventGuiRender2D] = {on_gui_render_2d, 120, 0};
        base.render_settings = nullptr;
        base.visit_config = nullptr;
        events::set_module_config_key(&base, "Adjust Main Menu");
    }
};

MainMenuState g_state{};

// AdjustMainMenu statics
bool g_opening_ready = true;
bool g_initial_transition_seeded = false;
std::string g_offline_tip_template;

adjust::Animation& offline_splash_fade() noexcept {
    static adjust::Animation s_fade(adjust::Easing::Linear, 130);
    return s_fade;
}

// AdjustMainMenu.getOfflineTip()
const std::string& offline_tip() {
    if (g_offline_tip_template.empty()) {
        constexpr size_t kTipCount = sizeof(kOfflineTips) / sizeof(kOfflineTips[0]);
        std::string tip = std::string(ADJUST_FMT "f" ADJUST_FMT "lTip: " ADJUST_FMT "r") +
                          kOfflineTips[static_cast<size_t>(ui_random().next_int(static_cast<int32_t>(kTipCount)))];
        if (ui_random().next_double() >= 0.999) {
            tip = ADJUST_FMT "rThis is a special message with a chance of " ADJUST_FMT "f" ADJUST_FMT
                  "l1 in 1,000" ADJUST_FMT "r! Congratulations!";
        }
        bool open = true;
        for (size_t pos = tip.find('*'); pos != std::string::npos; pos = tip.find('*')) {
            tip.replace(pos, 1, open ? ADJUST_FMT "f" : ADJUST_FMT "r");
            open = !open;
        }
        g_offline_tip_template = std::move(tip);
    }
    return g_offline_tip_template;
}

// ---- JNI ------------------------------------------------------------------------------------------------------

struct Ids {
    jclass main_menu = nullptr;
    jclass select_world = nullptr;
    jclass multiplayer = nullptr;
    jclass options = nullptr;
    jclass language = nullptr;
    jclass mod_list = nullptr; // Forge only
    jclass resource_location = nullptr;
    jclass sound_record = nullptr;
    jclass i18n = nullptr;
    jclass session = nullptr;
    jclass sound_handler = nullptr;
    jclass object = nullptr;

    jfieldID screen_width = nullptr;
    jfieldID screen_height = nullptr;
    jfieldID splash_text = nullptr;
    jfieldID display_width = nullptr;
    jfieldID display_height = nullptr;
    jfieldID limit_framerate = nullptr;

    jmethodID handle_keyboard_input = nullptr;
    jmethodID display_gui_screen = nullptr;
    jmethodID shutdown = nullptr;
    jmethodID get_language_manager = nullptr;
    jmethodID get_sound_handler = nullptr;
    jmethodID get_session = nullptr;
    jmethodID get_username = nullptr;
    jmethodID play_sound = nullptr;
    jmethodID sound_create = nullptr;
    jmethodID resource_location_ctor = nullptr;
    jmethodID i18n_format = nullptr;
    jmethodID select_world_ctor = nullptr;
    jmethodID multiplayer_ctor = nullptr;
    jmethodID options_ctor = nullptr;
    jmethodID language_ctor = nullptr;
    jmethodID mod_list_ctor = nullptr;

    jmethodID mouse_event_button = nullptr;
    jmethodID mouse_event_button_state = nullptr;
    jmethodID mouse_event_x = nullptr;
    jmethodID mouse_event_y = nullptr;
};

const Ids& ids(events::JniContext* jni) noexcept {
    static const Ids s_ids = [jni] {
        namespace J = adjust::jni;
        Ids r{};
        if (!J::is_1_8(jni)) {
            return r;
        }
        const mc::Mappings* m = jni->mappings;
        const jclass screen = m->cls_gui_screen;
        const jclass minecraft = m->cls_minecraft;

        r.main_menu = J::find_class(jni, "aya", "net/minecraft/client/gui/GuiMainMenu");
        r.select_world = J::find_class(jni, "axv", "net/minecraft/client/gui/GuiSelectWorld");
        r.multiplayer = J::find_class(jni, "azh", "net/minecraft/client/gui/GuiMultiplayer");
        r.options = J::find_class(jni, "axn", "net/minecraft/client/gui/GuiOptions");
        r.language = J::find_class(jni, "axl", "net/minecraft/client/gui/GuiLanguage");
        r.mod_list = J::find_class(jni, nullptr, "net/minecraftforge/fml/client/GuiModList");
        r.resource_location = J::find_class(jni, "jy", "net/minecraft/util/ResourceLocation");
        r.sound_record = J::find_class(jni, "bpf", "net/minecraft/client/audio/PositionedSoundRecord");
        r.i18n = J::find_class(jni, "bnq", "net/minecraft/client/resources/I18n");
        r.session = J::find_class(jni, "avm", "net/minecraft/util/Session");
        r.sound_handler = J::find_class(jni, "bpz", "net/minecraft/client/audio/SoundHandler");
        if (jclass object = jni->env->FindClass("java/lang/Object"); object != nullptr) {
            r.object = static_cast<jclass>(jni->env->NewGlobalRef(object));
            jni->env->DeleteLocalRef(object);
        } else {
            jni->env->ExceptionClear();
        }

        r.screen_width = J::field(jni, screen, {"l", "width", "field_146294_l"}, "I", "I");
        r.screen_height = J::field(jni, screen, {"m", "height", "field_146295_m"}, "I", "I");
        r.splash_text = J::field(jni, r.main_menu, {"r", "splashText", "field_73975_c"}, "Ljava/lang/String;",
                                 "Ljava/lang/String;");
        r.display_width = J::field(jni, minecraft, {"d", "displayWidth", "field_71443_c"}, "I", "I");
        r.display_height = J::field(jni, minecraft, {"e", "displayHeight", "field_71440_d"}, "I", "I");
        r.limit_framerate = J::field(jni, m->cls_game_settings, {"g", "limitFramerate", "field_74350_i"}, "I", "I");

        r.handle_keyboard_input = J::method(jni, screen, {"l", "handleKeyboardInput", "func_146282_l"}, "()V", "()V");
        r.display_gui_screen = J::method(jni, minecraft, {"a", "displayGuiScreen", "func_147108_a"}, "(Laxu;)V",
                                         "(Lnet/minecraft/client/gui/GuiScreen;)V");
        r.shutdown = J::method(jni, minecraft, {"m", "shutdown", "func_71400_g"}, "()V", "()V");
        r.get_language_manager = J::method(jni, minecraft, {"S", "getLanguageManager", "func_135016_M"}, "()Lbns;",
                                           "()Lnet/minecraft/client/resources/LanguageManager;");
        r.get_sound_handler = J::method(jni, minecraft, {"W", "getSoundHandler", "func_147118_V"}, "()Lbpz;",
                                        "()Lnet/minecraft/client/audio/SoundHandler;");
        r.get_session = J::method(jni, minecraft, {"L", "getSession", "func_110432_I"}, "()Lavm;",
                                  "()Lnet/minecraft/util/Session;");
        r.get_username = J::method(jni, r.session, {"c", "getUsername", "func_111285_a"}, "()Ljava/lang/String;",
                                   "()Ljava/lang/String;");
        r.play_sound = J::method(jni, r.sound_handler, {"a", "playSound", "func_147682_a"}, "(Lbpj;)V",
                                 "(Lnet/minecraft/client/audio/ISound;)V");
        r.sound_create = J::static_method(
            jni, r.sound_record, {"a", "create", "func_147674_a"}, "(Ljy;F)Lbpf;",
            "(Lnet/minecraft/util/ResourceLocation;F)Lnet/minecraft/client/audio/PositionedSoundRecord;");
        r.resource_location_ctor = J::method(jni, r.resource_location, {"<init>", "<init>", "<init>"},
                                             "(Ljava/lang/String;)V", "(Ljava/lang/String;)V");
        r.i18n_format = J::static_method(jni, r.i18n, {"a", "format", "func_135052_a"},
                                         "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;",
                                         "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;");

        constexpr J::Names kCtor{"<init>", "<init>", "<init>"};
        r.select_world_ctor = J::method(jni, r.select_world, kCtor, "(Laxu;)V", "(Lnet/minecraft/client/gui/GuiScreen;)V");
        r.multiplayer_ctor = J::method(jni, r.multiplayer, kCtor, "(Laxu;)V", "(Lnet/minecraft/client/gui/GuiScreen;)V");
        r.options_ctor = J::method(jni, r.options, kCtor, "(Laxu;Lavh;)V",
                                   "(Lnet/minecraft/client/gui/GuiScreen;Lnet/minecraft/client/settings/GameSettings;)V");
        r.language_ctor = J::method(
            jni, r.language, kCtor, "(Laxu;Lavh;Lbns;)V",
            "(Lnet/minecraft/client/gui/GuiScreen;Lnet/minecraft/client/settings/GameSettings;"
            "Lnet/minecraft/client/resources/LanguageManager;)V");
        r.mod_list_ctor = J::method(jni, r.mod_list, kCtor, "(Laxu;)V", "(Lnet/minecraft/client/gui/GuiScreen;)V");

        const jclass mouse = m->cls_lwjgl_mouse;
        r.mouse_event_button = J::static_method(jni, mouse, {"getEventButton", "getEventButton", "getEventButton"},
                                                "()I", "()I");
        r.mouse_event_button_state = J::static_method(
            jni, mouse, {"getEventButtonState", "getEventButtonState", "getEventButtonState"}, "()Z", "()Z");
        r.mouse_event_x = J::static_method(jni, mouse, {"getEventX", "getEventX", "getEventX"}, "()I", "()I");
        r.mouse_event_y = J::static_method(jni, mouse, {"getEventY", "getEventY", "getEventY"}, "()I", "()I");
        return r;
    }();
    return s_ids;
}

// Exactly vanilla's GuiMainMenu (subclasses from mods / other clients keep their own look).
bool is_vanilla_main_menu(events::JniContext* jni, const Ids& r, jobject screen) noexcept {
    if (screen == nullptr || r.main_menu == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    jclass cls = env->GetObjectClass(screen);
    const bool same = cls != nullptr && env->IsSameObject(cls, r.main_menu) == JNI_TRUE;
    if (cls != nullptr) {
        env->DeleteLocalRef(cls);
    }
    return same;
}

bool takes_over(events::JniContext* jni, jobject screen) noexcept {
    if (!events::module_is_running(&g_state.base) || jni == nullptr || jni->env == nullptr ||
        !adjust::jni::is_1_8(jni)) {
        return false;
    }
    return is_vanilla_main_menu(jni, ids(jni), screen);
}

std::string i18n_format(events::JniContext* jni, const Ids& r, const char* key, const char* fallback) {
    JNIEnv* env = jni->env;
    if (r.i18n == nullptr || r.i18n_format == nullptr || r.object == nullptr) {
        return fallback;
    }
    jstring jkey = env->NewStringUTF(key);
    jobjectArray args = jkey != nullptr ? env->NewObjectArray(0, r.object, nullptr) : nullptr;
    std::string out;
    if (args != nullptr) {
        auto result = static_cast<jstring>(env->CallStaticObjectMethod(r.i18n, r.i18n_format, jkey, args));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        } else {
            out = strip_control_codes(java_string(env, result));
        }
        if (result != nullptr) {
            env->DeleteLocalRef(result);
        }
    } else {
        env->ExceptionClear();
    }
    if (args != nullptr) {
        env->DeleteLocalRef(args);
    }
    if (jkey != nullptr) {
        env->DeleteLocalRef(jkey);
    }
    return out.empty() ? std::string(fallback) : out;
}

void load_client_info(events::JniContext* jni, const Ids& r) {
    if (g_state.info_loaded) {
        return;
    }
    g_state.info_loaded = true;
    JNIEnv* env = jni->env;
    g_state.label_singleplayer = i18n_format(jni, r, "menu.singleplayer", "Singleplayer");
    g_state.label_multiplayer = i18n_format(jni, r, "menu.multiplayer", "Multiplayer");
    g_state.forge = r.mod_list != nullptr && r.mod_list_ctor != nullptr;

    jobject mc = minecraft::mc_get_instance(jni);
    if (mc != nullptr && r.get_session != nullptr && r.get_username != nullptr) {
        jobject session = env->CallObjectMethod(mc, r.get_session);
        if (!env->ExceptionCheck() && session != nullptr) {
            auto name = static_cast<jstring>(env->CallObjectMethod(session, r.get_username));
            if (!env->ExceptionCheck()) {
                g_state.username = java_string(env, name);
            }
            if (name != nullptr) {
                env->DeleteLocalRef(name);
            }
        }
        env->ExceptionClear();
        if (session != nullptr) {
            env->DeleteLocalRef(session);
        }
    }
    if (mc != nullptr) {
        env->DeleteLocalRef(mc);
    }
}

// GuiButton.playPressSound: PositionedSoundRecord.create(new ResourceLocation("gui.button.press"), 1.0F)
void play_press_sound(events::JniContext* jni, const Ids& r, jobject mc) noexcept {
    JNIEnv* env = jni->env;
    if (r.resource_location_ctor == nullptr || r.sound_create == nullptr || r.get_sound_handler == nullptr ||
        r.play_sound == nullptr) {
        return;
    }
    jstring path = env->NewStringUTF("gui.button.press");
    jobject location = path != nullptr ? env->NewObject(r.resource_location, r.resource_location_ctor, path) : nullptr;
    jobject sound = location != nullptr
                        ? env->CallStaticObjectMethod(r.sound_record, r.sound_create, location, 1.0f)
                        : nullptr;
    jobject handler = (sound != nullptr && !env->ExceptionCheck()) ? env->CallObjectMethod(mc, r.get_sound_handler)
                                                                   : nullptr;
    if (handler != nullptr && !env->ExceptionCheck()) {
        env->CallVoidMethod(handler, r.play_sound, sound);
    }
    env->ExceptionClear(); // a missing sound never breaks the menu
    for (jobject ref : {static_cast<jobject>(path), location, sound, handler}) {
        if (ref != nullptr) {
            env->DeleteLocalRef(ref);
        }
    }
}

// mc.displayGuiScreen(screen). Returns false if a Java exception is pending.
bool display_screen(JNIEnv* env, const Ids& r, jobject mc, jobject screen) noexcept {
    if (screen == nullptr) {
        return !env->ExceptionCheck();
    }
    env->CallVoidMethod(mc, r.display_gui_screen, screen);
    env->DeleteLocalRef(screen);
    return !env->ExceptionCheck();
}

// AdjustMainMenu.actionPerformed. Returns true if the current screen may have changed (or an exception is pending).
bool action_performed(events::JniContext* jni, const Ids& r, jobject menu, jobject mc, int id) noexcept {
    JNIEnv* env = jni->env;
    if (r.display_gui_screen == nullptr) {
        return false;
    }
    switch (id) {
        case 0: // Singleplayer
            if (r.select_world_ctor != nullptr) {
                display_screen(env, r, mc, env->NewObject(r.select_world, r.select_world_ctor, menu));
                return true;
            }
            return false;
        case 1: // Multiplayer
            if (r.multiplayer_ctor != nullptr) {
                display_screen(env, r, mc, env->NewObject(r.multiplayer, r.multiplayer_ctor, menu));
                return true;
            }
            return false;
        case 2: { // Account Manager slot -> Mods (Forge) / Language
            if (g_state.forge) {
                display_screen(env, r, mc, env->NewObject(r.mod_list, r.mod_list_ctor, menu));
                return true;
            }
            if (r.language_ctor == nullptr || r.get_language_manager == nullptr) {
                return false;
            }
            jobject settings = minecraft::mc_get_game_settings(jni, mc);
            jobject languages = env->CallObjectMethod(mc, r.get_language_manager);
            if (!env->ExceptionCheck() && settings != nullptr && languages != nullptr) {
                display_screen(env, r, mc, env->NewObject(r.language, r.language_ctor, menu, settings, languages));
            }
            if (languages != nullptr) {
                env->DeleteLocalRef(languages);
            }
            if (settings != nullptr) {
                env->DeleteLocalRef(settings);
            }
            return true;
        }
        case 3: { // Game Settings
            if (r.options_ctor == nullptr) {
                return false;
            }
            jobject settings = minecraft::mc_get_game_settings(jni, mc);
            if (settings != nullptr) {
                display_screen(env, r, mc, env->NewObject(r.options, r.options_ctor, menu, settings));
                env->DeleteLocalRef(settings);
            }
            return true;
        }
        case 4: // Quit Playing -> "Are you sure?" dialog
            g_state.dialog.show();
            return false;
        case 5: // Adjust: friends screen -> OpenSlinky++ click GUI (opened from the GUI poll)
            g_state.open_click_gui_requested = true;
            return false;
        default:
            return false;
    }
}

// ---- AdjustMainMenu -------------------------------------------------------------------------------------------

// createButtons()
void create_buttons() {
    MainMenuState& s = g_state;
    s.buttons.clear();
    const float width = static_cast<float>(s.width);
    float y = static_cast<float>(s.height) / 2.35f;
    const std::string labels[3] = {s.label_singleplayer, s.label_multiplayer, s.forge ? "Mods" : "Language"};
    float x = width / 2.0f - static_cast<float>(65 * 3) / 2.0f - 0.0f + 3.5f; // addIconButton(labels, y, 0)
    for (const std::string& label : labels) {
        s.buttons.push_back(make_button(static_cast<int>(s.buttons.size()), x, y, 60, 11, label));
        x += 65.0f;
    }
    x = width / 2.0f - 65.0f - 11.0f + 3.5f;
    y += 16.0f;
    s.buttons.push_back(make_button(static_cast<int>(s.buttons.size()), x, y, 60, 11, "Game Settings"));
    x += 65.0f;
    s.buttons.push_back(make_button(static_cast<int>(s.buttons.size()), x, y, 60, 11, "Quit Playing"));
    x += 65.0f;
    MenuButton gui = make_button(static_cast<int>(s.buttons.size()), x, y, 11, 11, "a");
    gui.font = kIcons2_13;
    gui.symbol_font = true;
    s.buttons.push_back(std::move(gui));
}

// initGui()
void init_gui() {
    MainMenuState& s = g_state;
    s.particles.clear();
    if (resolve_holiday_theme() == Holiday::Christmas) {
        const int count = static_cast<int>(static_cast<float>(s.height + s.width) * 0.065f);
        for (int i = 0; i < count; ++i) {
            Particle p{};
            p.x = random_float(0.75f, static_cast<float>(s.width) - 0.75f);
            p.y = random_float(0.75f, static_cast<float>(s.height) - 0.75f);
            s.particles.push_back(p);
        }
    }
    s.opening_splash = g_opening_ready;
    s.interactive = g_opening_ready;
    s.original_splash = s.opening_splash ? offline_tip() : s.splash;
    if (!g_initial_transition_seeded && g_opening_ready) {
        g_initial_transition_seeded = true;
        g_voronoi.previous_color = adjust::argb(160, 160, 160);
        g_voronoi.has_previous_color = true;
        g_voronoi.transition = adjust::Animation(adjust::Easing::Linear, 220);
        g_voronoi.has_transition = true;
        voronoi_reset();
    }
    const std::time_t t = std::time(nullptr);
    if (const std::tm* date = std::localtime(&t); date != nullptr) {
        const int month = date->tm_mon;
        const int day = date->tm_mday;
        if (month == 11 && day >= 23 && day <= 27) {
            s.original_splash = "Merry X-mas!";
        } else if (month == 0 && day < 3) {
            s.original_splash = "Happy new year!";
        } else if (month == 9 && day == 31) {
            s.original_splash = "OOoooOOOoooo! Spooky!";
        }
    }
    create_buttons();
    voronoi_resize(s.width, s.height, s.username);
    s.last_tick_ms = adjust::now_ms();
}

// onGuiClosed()
void on_gui_closed() noexcept {
    g_state.active = false;
    g_opening_ready = false;
    g_state.dialog.open = false;
    g_state.dialog.closing = false;
}

// updateScreen() (20 ticks per second) - snow particles only, the rest is Adjust backend state.
void update_screen() {
    MainMenuState& s = g_state;
    const float limit = static_cast<float>(s.height) + 0.75f;
    s.particles.erase(std::remove_if(s.particles.begin(), s.particles.end(),
                                     [limit](const Particle& p) { return p.y > limit; }),
                      s.particles.end());
    if (resolve_holiday_theme() == Holiday::Christmas && ++s.particle_ticks >= 5) {
        Particle p{};
        p.x = random_float(0.75f, std::max(0.75f, static_cast<float>(s.width) - 0.75f));
        p.y = -0.75f;
        s.particles.push_back(p);
        s.particle_ticks = 0;
    }
}

void run_ticks() {
    const int64_t now = adjust::now_ms();
    if (now - g_state.last_tick_ms > 1000) {
        g_state.last_tick_ms = now - 50; // after a stall: one tick, no catch-up burst
    }
    while (now - g_state.last_tick_ms >= 50) {
        g_state.last_tick_ms += 50;
        update_screen();
    }
}

// drawTitle(mouse)
void draw_title(float mouse_x, float mouse_y) noexcept {
    const MainMenuState& s = g_state;
    const float center_x = static_cast<float>(s.width) * 0.5f;
    const float y = static_cast<float>(s.height) / 2.35f - 7.0f;
    const float dx = mouse_x - center_x;
    const float dy = mouse_y - (y - 22.0f);
    const float distance = std::sqrt(dx * dx + dy * dy);
    int alpha = std::min(230, std::max(-50, static_cast<int>(190.0f - distance * 0.4f)) + 190);
    if (g_voronoi.has_transition) {
        alpha = std::min(alpha, static_cast<int>(voronoi_update() * 255.0f));
    }
    const Argb accent = adjust::with_alpha(adjust_hud::hud_color(), std::max(0, alpha));

    // "A" + "djust" -> first letter of the brand + the rest.
    const std::string_view brand = SLINKY_BRAND_NAME;
    const size_t first = first_char_length(brand);
    const std::string initial_text = std::string(ADJUST_FMT "l") + std::string(brand.substr(0, first));
    const std::string rest_text(brand.substr(first));
    const adjust::Font& initial = adjust::kTahoma22;
    const adjust::Font& rest = adjust::kTahoma20;
    const float title_width = initial.width(initial_text) + rest.width(rest_text);
    float x = center_x - title_width / 2.0f;
    x = initial.draw(initial_text, x, y - 7.0f, accent);
    rest.draw(rest_text, x, y - 6.0f, adjust::argb(210, 205, 209, std::max(0, alpha)));
}

// drawCornerCircles(mouse, resolution) - Halloween webs
void draw_corner_circles(NVGcontext* vg, float mouse_x, float mouse_y, float scaled_w, float scaled_h) noexcept {
    float dx = mouse_x + 1.0f;
    float dy = mouse_y - 3.0f;
    float distance = std::sqrt(dx * dx + dy * dy);
    int alpha = std::min(110, std::max(-130, static_cast<int>(190.0f - distance * 1.5f)) + 190);
    draw_web_circle(vg, 16, adjust::argb(225, 228, 228, alpha), -1.0f, 3.0f, scaled_h * 0.25f);
    const float x = scaled_w + 1.0f;
    const float y = scaled_h + 1.0f;
    dx = mouse_x - x;
    dy = mouse_y - y;
    distance = std::sqrt(dx * dx + dy * dy);
    alpha = std::min(110, std::max(-130, static_cast<int>(190.0f - distance * 2.0f)) + 190);
    draw_web_circle(vg, 12, adjust::argb(225, 228, 228, alpha), x, y, scaled_h * 0.15f);
}

// DialogScreen.render (only the quit confirmation)
void draw_dialog(float mouse_x, float mouse_y) noexcept {
    QuitDialog& d = g_state.dialog;
    if (!d.open) {
        return;
    }
    // PopupTransition.getProgress()
    d.popup.update(d.closing ? 0.0f : 1.0f);
    if (d.closing && d.popup.value == 0.0f) {
        d.open = false;
        d.closing = false;
        return;
    }
    const float screen_w = static_cast<float>(g_state.width);
    const float screen_h = static_cast<float>(g_state.height);
    const float size_x = d.width();
    const float size_y = QuitDialog::height();
    const float x = screen_w / 2.0f - size_x / 2.0f;
    const float y = screen_h / 2.0f - size_y / 2.0f;
    const float previous_alpha = adjust::global_alpha();
    adjust::set_global_alpha(d.popup.value);
    d.dim.update(115.0f);
    adjust::fill_rect(adjust::argb(0, 0, 0, static_cast<int>(d.dim.value)), 0.0f, 0.0f, screen_w, screen_h);
    adjust::fill_rect(0x9619191Cu, x, y, x + size_x, y + size_y);
    adjust::stroke_rect(true, 0x96181818u, x, y, x + size_x, y + size_y, 0.5f);
    draw_accent_gradient(x, y, size_x, 10.0f);
    adjust::kTahoma12.draw(kDialogTitle, x + 2.5f, y + 3.5f, 0xF5C8C8C8u);
    adjust::kTahoma12.draw(kDialogMessage, x + 2.5f, y + 15.0f, adjust::argb(199, 199, 199, 200));
    // drawButtons
    float right = x + size_x - 3.0f;
    for (MenuButton& b : d.buttons) {
        b.x = right - static_cast<float>(b.width);
        b.y = y + size_y - 13.0f;
        draw_button(b, mouse_x, mouse_y, true);
        right = b.x - 5.0f;
    }
    adjust::set_global_alpha(previous_alpha);
}

// drawScreen(mouseX, mouseY, partialTicks)
void draw_menu(float scaled_w, float scaled_h) noexcept {
    MainMenuState& s = g_state;
    NVGcontext* vg = adjust::vg();
    const auto mouse_x = static_cast<float>(s.mouse_x);
    const auto mouse_y = static_cast<float>(s.mouse_y);

    const int grey = s.opening_splash ? 200 : 222;
    const std::string line = std::string(ADJUST_FMT "o") + s.original_splash;
    const float text_alpha = s.interactive ? offline_splash_fade().update(1.0f) * 211.0f : voronoi_update() * 175.0f;
    const float opacity = voronoi_update();

    // Plain-GL parts of Adjust's menu: not affected by NvgDraw's global alpha.
    adjust::set_global_alpha(1.0f);
    voronoi_draw(vg, s.width, s.height, s.mouse_x, s.mouse_y, adjust_hud::hud_color());
    const Holiday theme = resolve_holiday_theme();
    if (theme == Holiday::Halloween) {
        draw_corner_circles(vg, mouse_x, mouse_y, scaled_w, scaled_h);
    }

    adjust::set_global_alpha(opacity);
    const adjust::Font& font = adjust::kTahoma12;
    font.draw_no_shadow(line, static_cast<float>(s.width) * 0.5f - font.width(line) / 2.0f,
                        static_cast<float>(s.height) / 2.35f + 35.0f,
                        adjust::argb(grey, grey, grey, static_cast<int>(text_alpha)));
    for (Particle& p : s.particles) {
        draw_particle(p, mouse_x, mouse_y);
    }
    draw_title(mouse_x, mouse_y);

    // Overlays (the quit dialog, or the click GUI drawn above us) take the hover away from the menu buttons.
    const bool hover_allowed = !s.dialog.open && !get_gui_module()->enabled;
    for (MenuButton& b : s.buttons) {
        draw_button(b, mouse_x, mouse_y, hover_allowed);
    }

    adjust::set_global_alpha(1.0f);
    draw_dialog(mouse_x, mouse_y);
}

// ---- events ---------------------------------------------------------------------------------------------------

void on_gui_frame_poll(void* event) noexcept {
    auto* ev = static_cast<events::GuiFramePollEvent*>(event);
    MainMenuState& s = g_state;
    s.last_poll_ms = adjust::now_ms();

    const bool drawn = s.drawn_since_poll;
    s.drawn_since_poll = false;
    if (drawn) {
        s.visible = true;
        ev->render_flags |= 3u; // fully animated
    } else {
        if (s.visible) {
            ev->render_flags |= 2u; // one more redraw so the menu does not stay in the offscreen layer
        }
        s.visible = false;
        if (s.active) {
            on_gui_closed(); // a frame went by without the menu: another screen (or the world) is shown
        }
    }

    if (s.open_click_gui_requested) {
        s.open_click_gui_requested = false;
        if (ev->ctx != nullptr && ev->ctx->module_manager != nullptr && !get_gui_module()->enabled) {
            ev->ctx->module_manager->request_set_enabled(get_gui_module(), true);
        }
    }
}

void on_gui_render_2d(void* event) noexcept {
    auto* ev = static_cast<events::GuiRender2DEvent*>(event);
    MainMenuState& s = g_state;
    if (!s.visible || ev->renderer == nullptr || s.width <= 0 || s.height <= 0) {
        return;
    }
    const float fb_w = ev->renderer->logical_width * ev->renderer->scale;
    const float fb_h = ev->renderer->logical_height * ev->renderer->scale;
    // Minecraft's ScaledResolution factor: width = ceil(displayWidth / factor).
    const float factor = std::max(1.0f, std::round(fb_w / static_cast<float>(s.width)));

    const adjust::HudFrame frame = adjust::begin_hud(ev->renderer, 2.0f);
    if (!frame.valid) {
        return;
    }
    adjust::set_scale(factor);
    run_ticks();
    draw_menu(fb_w / factor, fb_h / factor);
    adjust::set_global_alpha(1.0f);
    adjust::end_hud();
}

} // namespace

events::Module* get_adjust_main_menu_module() noexcept {
    return &g_state.base;
}

namespace adjust_main_menu {

bool on_draw_screen(events::JniContext* jni, jobject screen, int mouse_x, int mouse_y) noexcept {
    MainMenuState& s = g_state;
    s.draw_hook_seen = true;
    // Both hooks must be installed and the NanoVG GUI layer must be running (it draws the menu); otherwise the
    // vanilla menu is kept.
    if (!s.input_hook_seen || adjust::now_ms() - s.last_poll_ms > 2000 || !takes_over(jni, screen)) {
        return false;
    }
    const Ids& r = ids(jni);
    JNIEnv* env = jni->env;
    if (r.screen_width == nullptr || r.screen_height == nullptr) {
        return false;
    }
    const int width = env->GetIntField(screen, r.screen_width);
    const int height = env->GetIntField(screen, r.screen_height);
    if (width <= 0 || height <= 0) {
        return false;
    }

    load_client_info(jni, r);

    const bool same_instance = s.screen != nullptr && env->IsSameObject(s.screen, screen) == JNI_TRUE;
    if (!same_instance) {
        if (s.screen != nullptr) {
            env->DeleteWeakGlobalRef(s.screen);
        }
        s.screen = env->NewWeakGlobalRef(screen);
        // AdjustMainMenu constructor: the random splash (vanilla already picked one from texts/splashes.txt).
        s.splash = "missingno";
        if (r.splash_text != nullptr) {
            auto text = static_cast<jstring>(env->GetObjectField(screen, r.splash_text));
            if (text != nullptr) {
                const std::string splash = strip_control_codes(java_string(env, text));
                if (!splash.empty()) {
                    s.splash = splash;
                }
                env->DeleteLocalRef(text);
            }
        }
    }
    if (!s.active || !same_instance || width != s.width || height != s.height) {
        s.width = width;
        s.height = height;
        init_gui();
    }
    s.active = true;
    s.mouse_x = mouse_x;
    s.mouse_y = mouse_y;
    s.drawn_since_poll = true;
    return true;
}

bool on_handle_input(events::JniContext* jni, jobject screen) noexcept {
    MainMenuState& s = g_state;
    s.input_hook_seen = true;
    if (!s.draw_hook_seen || !s.active || s.screen == nullptr || !takes_over(jni, screen)) {
        return false;
    }
    JNIEnv* env = jni->env;
    if (env->IsSameObject(s.screen, screen) != JNI_TRUE) {
        return false; // a new GuiMainMenu that has not been drawn yet: vanilla input for this tick
    }
    const Ids& r = ids(jni);
    if (r.mouse_event_button == nullptr || r.mouse_event_button_state == nullptr || r.mouse_event_x == nullptr ||
        r.mouse_event_y == nullptr || r.display_width == nullptr || r.display_height == nullptr ||
        r.handle_keyboard_input == nullptr) {
        return false;
    }
    const jclass mouse = jni->mappings->cls_lwjgl_mouse;
    jobject mc = minecraft::mc_get_instance(jni);
    if (mc == nullptr) {
        return false;
    }
    const int display_w = std::max(1, static_cast<int>(env->GetIntField(mc, r.display_width)));
    const int display_h = std::max(1, static_cast<int>(env->GetIntField(mc, r.display_height)));

    // GuiScreen.handleInput: mouse queue -> handleMouseInput -> mouseClicked(x, y, button)
    while (minecraft::lwjgl_mouse_next(jni)) {
        if (env->ExceptionCheck()) {
            env->DeleteLocalRef(mc);
            return true;
        }
        const int button = env->CallStaticIntMethod(mouse, r.mouse_event_button);
        const bool pressed = env->CallStaticBooleanMethod(mouse, r.mouse_event_button_state) != JNI_FALSE;
        const int event_x = env->CallStaticIntMethod(mouse, r.mouse_event_x);
        const int event_y = env->CallStaticIntMethod(mouse, r.mouse_event_y);
        if (!pressed || button != 0 || get_gui_module()->enabled) {
            continue; // Adjust's buttons and dialog only react to left presses
        }
        const int x = event_x * s.width / display_w;
        const int y = s.height - event_y * s.height / display_h - 1;
        const auto fx = static_cast<float>(x);
        const auto fy = static_cast<float>(y);

        if (s.dialog.open) {
            // DialogScreen.mouseClicked: consumes every click while the dialog is shown
            for (MenuButton& b : s.dialog.buttons) {
                if (b.hit(fx, fy)) {
                    const bool exit = b.id == 0;
                    s.dialog.close();
                    if (exit && r.shutdown != nullptr) {
                        env->CallVoidMethod(mc, r.shutdown); // "Exit": mc.shutdown()
                        if (env->ExceptionCheck()) {
                            env->DeleteLocalRef(mc);
                            return true;
                        }
                    }
                    break;
                }
            }
            continue;
        }

        // GuiScreen.mouseClicked: first pressed button -> playPressSound + actionPerformed
        for (const MenuButton& b : s.buttons) {
            if (!b.hit(fx, fy)) {
                continue;
            }
            play_press_sound(jni, r, mc);
            if (action_performed(jni, r, screen, mc, b.id)) {
                // Another screen is shown (or a Java exception is pending): the rest of the queue belongs to it.
                env->DeleteLocalRef(mc);
                return true;
            }
            break;
        }
    }

    // Keyboard: vanilla handling (GuiMainMenu.keyTyped is empty; dispatchKeypresses keeps F2 / F11 working).
    // ESC closes the quit dialog (DialogScreen.keyTyped).
    while (minecraft::lwjgl_keyboard_next(jni)) {
        if (env->ExceptionCheck()) {
            break;
        }
        if (s.dialog.open && minecraft::lwjgl_keyboard_get_event_key_state(jni) &&
            minecraft::lwjgl_keyboard_get_event_key(jni) == 1) {
            s.dialog.close();
            continue;
        }
        env->CallVoidMethod(screen, r.handle_keyboard_input);
        if (env->ExceptionCheck()) {
            break; // stays pending for Minecraft.runTick
        }
    }
    env->DeleteLocalRef(mc);
    return true;
}

bool on_limit_framerate(events::JniContext* jni, jobject minecraft, int* out_fps) noexcept {
    MainMenuState& s = g_state;
    if (out_fps == nullptr || minecraft == nullptr || !s.active || !s.draw_hook_seen || !s.input_hook_seen ||
        !events::module_is_running(&s.base) || jni == nullptr || jni->env == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    const Ids& r = ids(jni);
    jobject world = minecraft::mc_get_world(jni, minecraft);
    if (world != nullptr) {
        env->DeleteLocalRef(world);
        return false;
    }
    jobject screen = env->GetObjectField(minecraft, jni->mappings->fid_mc_current_screen);
    const bool menu = is_vanilla_main_menu(jni, r, screen);
    if (screen != nullptr) {
        env->DeleteLocalRef(screen);
    }
    if (!menu) {
        return false;
    }
    // AdjustMainMenu.getMaxFps(): 60 while the window is not focused, else min(175, limitFramerate).
    if (!minecraft::lwjgl_display_is_active(jni)) {
        *out_fps = 60;
        return true;
    }
    int limit = 175;
    if (r.limit_framerate != nullptr) {
        jobject settings = minecraft::mc_get_game_settings(jni, minecraft);
        if (settings != nullptr) {
            limit = std::min(175, static_cast<int>(env->GetIntField(settings, r.limit_framerate)));
            env->DeleteLocalRef(settings);
        }
    }
    *out_fps = limit;
    return true;
}

} // namespace adjust_main_menu

} // namespace slinky::modules
