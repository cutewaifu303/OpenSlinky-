// Port of com.adjustclient.ui.hud.information.{Information, InformationSettings, InformationSwitchMap, Line, Segment,
// SpeedUnit}: Adjust's bottom-left information block.
//
// Layout (HUD units, bottom-left anchor): the player name in the HUD colour (Tahoma 12, alpha 220) with the edition
// label next to it (Tahoma 10), and above it one Tahoma 10 line per enabled info (coordinates, angles, speed, TPS,
// FPS), 6.0 apart, bottom up. Values are drawn in (224, 224, 224, 220), labels in (140, 140, 140, 220). While the
// chat is open the whole block slides below the screen edge (EASE_OUT_CUBIC, 420 ms).
//
// Differences to Adjust (documented in the port report):
//  - The edition label (Changelog.latest.shortTitle, e.g. "Nov 8th") shows the OpenSlinky++ build date in the same
//    abbreviated format, since OpenSlinky++ has no changelog.
//  - The name is the Minecraft session / player name (Adjust.username); Adjust's account-backed name and the
//    "Ahmed El-Sisi" easter egg (backend controlled) are not ported.
//  - Serverside angles come from OpenSlinky++'s rotation spoof state (RenderRotationTracker in Adjust).
//  - TPS is measured from S03PacketTimeUpdate (ServerTracker); see information/information_tps.hpp for the
//    incoming packet hook that the HUD module must forward.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "config/settings.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/adjust/hud/information/information_tps.hpp"

namespace slinky::modules::adjust_hud {

namespace {

using adjust::Animation;
using adjust::Argb;
using adjust::Easing;

// ---- settings (InformationSettings) ---------------------------------------------------------------------------

constexpr const char* kSpeedUnitOptions[2] = {"BPS", "KMPH"}; // SpeedUnit
enum SpeedUnit : int32_t { kSpeedBps = 0, kSpeedKmph = 1 };

struct Settings {
    // Coordinates group
    config::BoolSetting coordinates_enabled{{}, "Enabled", false, false, {}};
    config::BoolSetting nether_coordinates{{}, "Show nether coordinates", true, true, {}};
    // Angles group
    config::BoolSetting angles{{}, "Angles", false, false, {}};
    config::BoolSetting direction{{}, "Direction", true, true, {}};
    config::BoolSetting serverside{{}, "Serverside", false, false, {}}; // "Displays your serverside angles."
    // Speed group
    config::BoolSetting speed_enabled{{}, "Enabled", true, true, {}};
    config::ModeSetting speed_format{{}, "Format", 2, kSpeedBps, kSpeedUnitOptions, kSpeedBps, kSpeedBps, false, {}};
    // Directly in the Information group
    config::BoolSetting tps{{}, "TPS", false, false, {}};
    config::BoolSetting fps{{}, "FPS", false, false, {}};
};

Settings g_settings{};

// ---- state (Information) ----------------------------------------------------------------------------------------

constexpr Argb kValueColor = adjust::argb(224, 224, 224, 220);
constexpr Argb kLabelColor = adjust::argb(140, 140, 140, 220);
constexpr Argb kEditionColor = adjust::argb(208, 208, 208, 220);

struct Segment {
    std::string text;
    Argb color = 0;
    bool raised = false;
};

using Line = std::vector<Segment>;

struct State {
    Animation animation{Easing::EaseOutCubic, 420};
    std::vector<Line> lines;
    std::string name;
    int32_t displayed_counter = 0; // Information.displayedCounter (FPS)
    bool initialized = false;

    // Fallback FPS counter (render calls per second) when Minecraft.debugFPS cannot be resolved.
    int32_t frames = 0;
    int32_t frames_last_second = 0;
    int64_t frames_window_start = 0;
};

State g_state{};

// ---- JNI members not covered by the central mapping table (Minecraft 1.8.9) -------------------------------------

struct InfoJni {
    bool resolved = false;
    jfieldID sfid_debug_fps = nullptr;         // Minecraft.debugFPS (static I)
    jfieldID fid_world_provider = nullptr;     // World.provider (WorldProvider)
    jmethodID mid_dimension_id = nullptr;      // WorldProvider.getDimensionId()I
    jclass cls_gui_chat = nullptr;             // GuiChat
};

InfoJni g_jni{};

// Classes used on the Netty thread by the TPS tracker (published once resolved on the main thread).
std::atomic<jclass> g_cls_time_update{nullptr}; // S03PacketTimeUpdate
std::atomic<jclass> g_cls_join_game{nullptr};   // S01PacketJoinGame
std::atomic<float> g_tps{20.0f};                // ServerTracker.tps
std::atomic<int64_t> g_last_time_update{0};     // ServerTracker.lastTimeUpdate

void resolve_jni(events::JniContext* jni) noexcept {
    if (g_jni.resolved || jni == nullptr || jni->mappings == nullptr || jni->env == nullptr) {
        return;
    }
    g_jni.resolved = true;
    if (!adjust::jni::is_1_8(jni)) {
        return; // Adjust targets 1.8.9 only: FPS falls back to our own counter, no dimension / chat detection
    }
    namespace aj = adjust::jni;
    g_jni.sfid_debug_fps = aj::static_field(jni, jni->mappings->cls_minecraft,
                                            {"ao", "debugFPS", "field_71470_ab"}, "I", "I");
    g_jni.fid_world_provider = aj::field(jni, jni->mappings->cls_world, {"t", "provider", "field_73011_w"},
                                         "Lanm;", "Lnet/minecraft/world/WorldProvider;");
    if (const jclass provider = aj::find_class(jni, "anm", "net/minecraft/world/WorldProvider"); provider != nullptr) {
        g_jni.mid_dimension_id = aj::method(jni, provider, {"q", "getDimensionId", "func_177502_q"}, "()I", "()I");
    }
    g_jni.cls_gui_chat = aj::find_class(jni, "awv", "net/minecraft/client/gui/GuiChat");
    g_cls_time_update.store(aj::find_class(jni, "hu", "net/minecraft/network/play/server/S03PacketTimeUpdate"),
                            std::memory_order_release);
    g_cls_join_game.store(aj::find_class(jni, "gt", "net/minecraft/network/play/server/S01PacketJoinGame"),
                          std::memory_order_release);
}

// mc.theWorld.provider.getDimensionId(); `ok` = false when it cannot be read.
int32_t dimension_id(events::GameContext* ctx, bool* ok) noexcept {
    *ok = false;
    if (g_jni.fid_world_provider == nullptr || g_jni.mid_dimension_id == nullptr || ctx->world == nullptr) {
        return 0;
    }
    JNIEnv* env = ctx->jni->env;
    jobject provider = env->GetObjectField(ctx->world, g_jni.fid_world_provider);
    if (provider == nullptr) {
        adjust::jni::clear_exception(ctx->jni);
        return 0;
    }
    const jint dim = env->CallIntMethod(provider, g_jni.mid_dimension_id);
    env->DeleteLocalRef(provider);
    if (adjust::jni::clear_exception(ctx->jni)) {
        return 0;
    }
    *ok = true;
    return dim;
}

bool chat_open(events::GameContext* game) noexcept {
    if (game == nullptr || game->jni == nullptr || game->jni->env == nullptr || game->minecraft == nullptr ||
        g_jni.cls_gui_chat == nullptr || game->jni->mappings->fid_mc_current_screen == nullptr) {
        return false;
    }
    JNIEnv* env = game->jni->env;
    jobject screen = env->GetObjectField(game->minecraft, game->jni->mappings->fid_mc_current_screen);
    if (screen == nullptr) {
        return false;
    }
    const bool result = env->IsInstanceOf(screen, g_jni.cls_gui_chat) != JNI_FALSE;
    env->DeleteLocalRef(screen);
    return result;
}

std::string jstring_to_utf8(JNIEnv* env, jstring str) {
    std::string out;
    if (str == nullptr) {
        return out;
    }
    const char* chars = env->GetStringUTFChars(str, nullptr);
    if (chars != nullptr) {
        out = chars;
        env->ReleaseStringUTFChars(str, chars);
    }
    return out;
}

// ---- Java number semantics ---------------------------------------------------------------------------------------

// RandomUtil.round: BigDecimal.valueOf(value).setScale(places, HALF_UP). BigDecimal.valueOf uses the shortest decimal
// representation (Double.toString), so the rounding is done on that decimal string. Infinite -> 0.
double round_half_up(double value, int places) noexcept {
    if (!std::isfinite(value)) {
        return 0.0;
    }
    char buf[400];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed);
    if (res.ec != std::errc{}) {
        return value;
    }
    std::string s(buf, res.ptr);
    const bool negative = !s.empty() && s[0] == '-';
    if (negative) {
        s.erase(0, 1);
    }
    const size_t dot = s.find('.');
    double result = 0.0;
    if (dot == std::string::npos || s.size() - dot - 1 <= static_cast<size_t>(places)) {
        result = std::fabs(value);
    } else {
        std::string digits = s.substr(0, dot) + s.substr(dot + 1, static_cast<size_t>(places));
        const char next = s[dot + 1 + static_cast<size_t>(places)];
        if (next >= '5') {
            size_t i = digits.size();
            bool carry = true;
            while (carry && i > 0) {
                --i;
                if (digits[i] == '9') {
                    digits[i] = '0';
                } else {
                    ++digits[i];
                    carry = false;
                }
            }
            if (carry) {
                digits.insert(digits.begin(), '1');
            }
        }
        if (places > 0) {
            digits.insert(digits.size() - static_cast<size_t>(places), 1, '.');
        }
        result = std::strtod(digits.c_str(), nullptr);
    }
    if (result == 0.0) {
        return 0.0; // BigDecimal has no negative zero
    }
    return negative ? -result : result;
}

float round_half_up(float value, int places) noexcept {
    if (std::isinf(value)) {
        return 0.0f;
    }
    return static_cast<float>(round_half_up(static_cast<double>(value), places));
}

// Float.toString / Double.toString: shortest representation, plain notation in [1e-3, 1e7) with at least one
// fractional digit, otherwise "d.dddE<n>".
template <typename T>
std::string java_to_string(T value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0 ? "Infinity" : "-Infinity";
    }
    if (value == T(0)) {
        return std::signbit(value) ? "-0.0" : "0.0";
    }
    char buf[400];
    const T magnitude = std::fabs(value);
    if (magnitude >= T(1e-3) && magnitude < T(1e7)) {
        const auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed);
        std::string s(buf, res.ec == std::errc{} ? res.ptr : buf);
        if (s.find('.') == std::string::npos) {
            s += ".0";
        }
        return s;
    }
    const auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::scientific);
    std::string s(buf, res.ec == std::errc{} ? res.ptr : buf);
    const size_t e = s.find('e');
    if (e == std::string::npos) {
        return s;
    }
    std::string mantissa = s.substr(0, e);
    if (mantissa.find('.') == std::string::npos) {
        mantissa += ".0";
    }
    const long exponent = std::strtol(s.c_str() + e + 1, nullptr, 10);
    return mantissa + "E" + std::to_string(exponent);
}

// Java (int) cast of a double: truncation, NaN -> 0, saturating.
int32_t java_d2i(double v) noexcept {
    if (std::isnan(v)) {
        return 0;
    }
    if (v >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    if (v <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
        return std::numeric_limits<int32_t>::min();
    }
    return static_cast<int32_t>(v);
}

// MathHelper.wrapAngleTo180_float
float wrap_angle_180(float value) noexcept {
    value = std::fmod(value, 360.0f);
    if (value >= 180.0f) {
        value -= 360.0f;
    }
    if (value < -180.0f) {
        value += 360.0f;
    }
    return value;
}

// ---- line building (Information.format / update) ----------------------------------------------------------------

// SLF4J-style formatter: every "{}" is replaced by the next argument (value colour), the literal text between the
// placeholders becomes label coloured segments. A missing argument stays "{}".
Line format_line(const char* pattern, std::initializer_list<std::string> args) {
    Line segments;
    std::string literal;
    auto arg = args.begin();
    for (size_t i = 0; pattern[i] != '\0';) {
        if (pattern[i] == '{' && pattern[i + 1] == '}') {
            std::string value = arg != args.end() ? *arg++ : std::string("{}");
            if (!literal.empty()) {
                segments.push_back({literal, kLabelColor, false});
                literal.clear();
            }
            segments.push_back({std::move(value), kValueColor, false});
            i += 2;
        } else {
            literal.push_back(pattern[i]);
            ++i;
        }
    }
    if (!literal.empty()) {
        segments.push_back({literal, kLabelColor, false});
    }
    return segments;
}

// EnumFacing.HORIZONTALS order (SOUTH, WEST, NORTH, EAST) with InformationSwitchMap's axis labels.
constexpr const char* kFacingNames[4] = {"south", "west", "north", "east"};
constexpr const char* kFacingAxes[4] = {"Z+", "X-", "Z-", "X+"};

// Information.getSpeed: horizontal distance moved during the last tick * 20 * timer speed (blocks per second).
// (Adjust's accumulatedSpeed is never written in the client, so it is always 0 and not ported.)
float get_speed(events::GameContext* ctx) noexcept {
    events::JniContext* jni = ctx->jni;
    const math::Vec3d pos = mc::entity_get_position(jni, ctx->local_player);
    const math::Vec3d last = mc::entity_get_prev_position(jni, ctx->local_player);
    const double dist = std::hypot(pos.x - last.x, pos.z - last.z);
    float timer_speed = 1.0f;
    if (ctx->minecraft != nullptr) {
        jobject timer = mc::mc_get_timer(jni, ctx->minecraft);
        if (timer != nullptr) {
            timer_speed = mc::timer_get_speed(jni, timer);
            jni->env->DeleteLocalRef(timer);
        }
    }
    const double speed = dist * 20.0 * static_cast<double>(timer_speed);
    return speed >= 0.0 ? static_cast<float>(speed) : 0.0f;
}

void update(events::GameContext* ctx) noexcept {
    g_state.lines.clear();
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;

    // Player name (Adjust.username = session name, "Offline" when empty)
    if (jstring name = mc::entity_get_name(jni, ctx->local_player); name != nullptr) {
        g_state.name = jstring_to_utf8(env, name);
        env->DeleteLocalRef(name);
    } else {
        adjust::jni::clear_exception(jni);
    }
    if (g_state.name.empty()) {
        g_state.name = "Offline";
    }

    // FPS counter (Minecraft.debugFPS)
    if (g_jni.sfid_debug_fps != nullptr) {
        g_state.displayed_counter = env->GetStaticIntField(jni->mappings->cls_minecraft, g_jni.sfid_debug_fps);
    } else {
        g_state.displayed_counter = g_state.frames_last_second;
    }

    // ---- Coordinates ----
    if (g_settings.coordinates_enabled.value) {
        const math::Vec3d pos = mc::entity_get_position(jni, ctx->local_player);
        bool dim_ok = false;
        const int32_t dim = g_settings.nether_coordinates.value ? dimension_id(ctx, &dim_ok) : 0;
        if (!g_settings.nether_coordinates.value || !dim_ok || dim == 1) {
            g_state.lines.push_back(format_line("{}, {}, {}", {std::to_string(java_d2i(pos.x)),
                                                                std::to_string(java_d2i(pos.y)),
                                                                std::to_string(java_d2i(pos.z))}));
        } else {
            const double factor = dim == 0 ? 0.125 : 8.0;
            g_state.lines.push_back(format_line(
                "{}, {}, {} [{}, {}]",
                {std::to_string(java_d2i(pos.x)), std::to_string(java_d2i(pos.y)), std::to_string(java_d2i(pos.z)),
                 std::to_string(java_d2i(pos.x * factor)), std::to_string(java_d2i(pos.z * factor))}));
        }
    }

    // ---- Angles ----
    if (g_settings.angles.value) {
        math::Vec2f rotation = mc::entity_get_rotation(jni, ctx->local_player);
        if (g_settings.serverside.value && mc::is_rotation_spoof_active()) {
            rotation = mc::get_server_spoof_rotation();
        }
        float yaw = wrap_angle_180(rotation.x);
        yaw = round_half_up(yaw, 1);
        const float pitch = round_half_up(rotation.y, 1);
        if (g_settings.direction.value) {
            const int index = static_cast<int>(std::floor(static_cast<double>(yaw) / 90.0 + 0.5)) & 3;
            g_state.lines.push_back(format_line("{} / {} [{}] {}", {java_to_string(yaw), java_to_string(pitch),
                                                                    kFacingAxes[index], kFacingNames[index]}));
        } else {
            g_state.lines.push_back(format_line("{} / {}", {java_to_string(yaw), java_to_string(pitch)}));
        }
    }

    // ---- Speed ----
    if (g_settings.speed_enabled.value) {
        double speed = get_speed(ctx);
        const char* unit = "B/s";
        if (g_settings.speed_format.value == kSpeedKmph) {
            speed *= 3.6;
            unit = "Km/h";
        }
        const double rounded = round_half_up(speed, 2);
        g_state.lines.push_back({{java_to_string(rounded), kValueColor, false}, {unit, kLabelColor, true}});
    }

    // ---- TPS ----
    if (g_settings.tps.value) {
        const float tps = round_half_up(information_server_tps(), 2);
        g_state.lines.push_back({{java_to_string(tps), kValueColor, false}, {"T/s", kLabelColor, true}});
    }

    // ---- FPS ----
    if (g_settings.fps.value) {
        g_state.lines.push_back(
            {{std::to_string(g_state.displayed_counter) + " ", kValueColor, false}, {"FPS", kLabelColor, false}});
    }
}

// Changelog.latest.shortTitle equivalent: the build date abbreviated like ChangelogLink.abbreviateTitle
// ("June"/"July" spelled out, other months cut to three letters, e.g. "Nov 22nd").
const std::string& edition_label() {
    static const std::string s_label = [] {
        constexpr const char* kDate = __DATE__; // "Mmm dd yyyy"
        constexpr const char* kMonths[12][2] = {
            {"Jan", "January"}, {"Feb", "February"}, {"Mar", "March"},     {"Apr", "April"},
            {"May", "May"},     {"Jun", "June"},     {"Jul", "July"},      {"Aug", "August"},
            {"Sep", "September"}, {"Oct", "October"}, {"Nov", "November"}, {"Dec", "December"},
        };
        std::string month(kDate, 3);
        const int day = std::atoi(kDate + 4);
        for (const auto& m : kMonths) {
            if (month == m[0]) {
                const std::string full = m[1];
                month = (full == "June" || full == "July") ? full : full.substr(0, 3);
                break;
            }
        }
        const char* suffix = "th";
        if (day % 100 < 11 || day % 100 > 13) {
            switch (day % 10) {
                case 1: suffix = "st"; break;
                case 2: suffix = "nd"; break;
                case 3: suffix = "rd"; break;
                default: break;
            }
        }
        return month + " " + std::to_string(day) + suffix;
    }();
    return s_label;
}

// ---- element callbacks ------------------------------------------------------------------------------------------

void ensure_initialized() noexcept {
    if (!g_state.initialized) {
        g_state.initialized = true;
        g_state.animation = Animation(Easing::EaseOutCubic, 420);
        g_state.animation.value = -2.0f;
    }
}

void tick(events::GameContext* ctx) noexcept {
    if (ctx == nullptr || ctx->jni == nullptr || ctx->jni->env == nullptr || ctx->local_player == nullptr) {
        return;
    }
    resolve_jni(ctx->jni);
    update(ctx);
}

void render(const HudContext& ctx) noexcept {
    if (ctx.frame == nullptr) {
        return;
    }
    ensure_initialized();

    // Fallback FPS counter
    const int64_t now = adjust::now_ms();
    ++g_state.frames;
    if (now - g_state.frames_window_start >= 1000) {
        g_state.frames_last_second = g_state.frames;
        g_state.frames = 0;
        g_state.frames_window_start = now;
    }

    const adjust::Font& font12 = adjust::kTahoma12;
    const adjust::Font& font10 = adjust::kTahoma10;

    float target = font12.size();
    if (chat_open(ctx.game)) {
        target -= static_cast<float>(13 * (static_cast<int>(g_state.lines.size()) + 1));
    }
    g_state.animation.update(target);
    const auto height = static_cast<float>(ctx.frame->scaled_height);
    const float anim = g_state.animation.value;

    // Name + edition
    const float y = height - anim;
    Argb color = hud_color();
    if (adjust::alpha_of(color) != 220) {
        color = adjust::with_alpha(color, 220);
    }
    font12.draw(g_state.name, 2.0f, y, color);
    const float name_width = font12.width(g_state.name);
    font10.draw(edition_label(), name_width + 3.5f, (height - anim) + 1.0f, kEditionColor);

    // Information lines, bottom up, 6.0 apart
    float step = 6.0f;
    for (const Line& line : g_state.lines) {
        if (!line.empty()) {
            const float base_y = (height - anim) - step;
            float x = 2.5f;
            for (const Segment& segment : line) {
                float draw_y = base_y;
                if (segment.raised) {
                    x += 0.5f;
                    draw_y = base_y - 1.0f;
                }
                font10.render(segment.text, x + 0.5f, draw_y + 0.5f, segment.color, true);
                x = font10.render(segment.text, x, draw_y, segment.color, false);
            }
        }
        step += 6.0f;
    }
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    // Plain settings of the Information group, then its three expandable groups as sub-sections.
    g_settings.tps.render(gui, renderer);
    g_settings.fps.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Coordinates");
    g_settings.coordinates_enabled.render(gui, renderer);
    g_settings.nether_coordinates.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Angles");
    g_settings.angles.render(gui, renderer);
    g_settings.direction.render(gui, renderer);
    g_settings.serverside.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Speed");
    g_settings.speed_enabled.render(gui, renderer);
    g_settings.speed_format.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    g_settings.coordinates_enabled.visit(ar, "information_coordinates_enabled");
    g_settings.nether_coordinates.visit(ar, "information_nether_coordinates");
    g_settings.angles.visit(ar, "information_angles");
    g_settings.direction.visit(ar, "information_direction");
    g_settings.serverside.visit(ar, "information_serverside");
    g_settings.speed_enabled.visit(ar, "information_speed_enabled");
    g_settings.speed_format.visit(ar, "information_speed_format");
    g_settings.tps.visit(ar, "information_tps");
    g_settings.fps.visit(ar, "information_fps");
}

void on_client_shutdown() noexcept {
    g_state.lines.clear();
    g_cls_time_update.store(nullptr, std::memory_order_release);
    g_cls_join_game.store(nullptr, std::memory_order_release);
}

} // namespace

// ServerTracker: S01PacketJoinGame resets the TPS to 20, S03PacketTimeUpdate (sent once per second of server time)
// measures the real interval: tps = clamp(20 / ((now - last) / 50 / 20), 0, 20) rounded to 2 places.
void information_on_incoming_packet(events::NettyPacketEvent* ev) noexcept {
    if (ev == nullptr || ev->jni == nullptr || ev->jni->env == nullptr || ev->packet == nullptr ||
        ev->elapsed_ms != 0) {
        return; // only the initial arrival, not re-dispatches of held packets
    }
    JNIEnv* env = ev->jni->env;
    const jclass time_update = g_cls_time_update.load(std::memory_order_acquire);
    const jclass join_game = g_cls_join_game.load(std::memory_order_acquire);
    if (time_update != nullptr && env->IsInstanceOf(ev->packet, time_update) != JNI_FALSE) {
        const int64_t now = adjust::now_ms();
        const int64_t last = g_last_time_update.exchange(now, std::memory_order_acq_rel);
        const float rate = 20.0f / (static_cast<float>(now - last) / 50.0f / 20.0f);
        float clamped = rate;
        if (!(clamped >= 0.0f)) {
            clamped = 0.0f; // MathHelper.clamp_float(NaN) keeps NaN; RandomUtil.round would throw - treat as 0
        }
        clamped = std::min(clamped, 20.0f);
        g_tps.store(round_half_up(clamped, 2), std::memory_order_release);
    } else if (join_game != nullptr && env->IsInstanceOf(ev->packet, join_game) != JNI_FALSE) {
        g_tps.store(20.0f, std::memory_order_release);
    }
}

float information_server_tps() noexcept {
    return g_tps.load(std::memory_order_acquire);
}

const HudElement& element_information() noexcept {
    static const HudElement s_element = [] {
        HudElement e{};
        e.name = "Information";
        e.render_settings = render_settings;
        e.visit_config = visit_config;
        e.tick = tick;
        e.render = render;
        e.on_client_shutdown = on_client_shutdown;
        e.on_incoming_packet = information_on_incoming_packet; // TPS (ServerTracker)
        return e;
    }();
    return s_element;
}

} // namespace slinky::modules::adjust_hud
