// Port of com.adjustclient.module.impl.render.hud.widget.SessionInformationWidget, plus the parts of
// com.adjustclient.service.session.SessionService it reads (per-account play time).
//
// Layout, colours and texts are Adjust's: account head, bold account name, KDR (right), play time (left) and a
// kills / (kills + deaths) bar in the HUD colour.
//
// Differences to Adjust (see the porting report):
//  - Kills/deaths: Adjust feeds them from PlayerKillEvent, which only its Hypixel / test-server kill-message parsers
//    raise. Those depend on Adjust's Hypixel game tracking and are not ported, so they stay 0 ("0 KDR", empty bar).
//  - Play time: SessionService counts while connected, not paused and active (any movement/attack/use key in the
//    last 20-30 s). It is kept in memory per account for the lifetime of the client (Adjust persists it in its
//    encrypted sessions.dat) and only advances while the Adjust HUD runs. The Hypixel-only "in game" time variant
//    is not ported (total time is always shown).
//  - Head: Adjust draws the session profile skin it downloads itself; here the local player's loaded skin texture
//    (AbstractClientPlayer.getLocationSkin) is wrapped as a NanoVG image (face + hat layer, 16x16).
//  - The Streamer module name replacement is not ported.

#include "modules/adjust/hud/widgets/widgets.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glad/glad.h>
#define NANOVG_GL3
extern "C" {
#include "nanovg_gl.h"
}

#include "adjust/jni_lookup.hpp"
#include "config/settings.hpp"
#include "minecraft/mc_wrappers.hpp"

namespace slinky::modules::adjust_hud::widgets {

namespace {

config::BoolSetting g_enabled{{}, "Session Information", false, false, {}};

// SessionStats (only the fields the widget reads).
struct SessionStats {
    std::string key; // user name + '\n' + player id (Adjust keys the map by the account UUID)
    std::string name;
    int64_t total_time = 0;
    int kills = 0;
    int deaths = 0;
};

struct SessionState {
    std::vector<SessionStats> sessions;
    std::string user_name; // Session.getUsername() of the last tick
    std::string player_id; // Session.getPlayerID() of the last tick
    bool has_account = false;
    int64_t total_timer = 0;    // SessionService.totalTimer.lastMs
    int64_t activity_timer = 0; // SessionService.activityTimer.lastMs
    bool timers_started = false;
};

SessionState g_state{};

// NanoVG image wrapping the local player's skin GL texture.
struct HeadImage {
    NVGcontext* vg = nullptr;
    GLuint texture = 0;
    int image = 0;
};

HeadImage g_head{};

SessionStats& stats_for(const std::string& name, const std::string& player_id) {
    const std::string key = name + '\n' + player_id;
    for (SessionStats& s : g_state.sessions) {
        if (s.key == key) {
            s.name = name;
            return s;
        }
    }
    g_state.sessions.push_back(SessionStats{key, name, 0, 0, 0});
    return g_state.sessions.back();
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

// Minecraft.getSession() -> user name / player id. False if unavailable.
bool read_session(events::JniContext* jni, jobject mc, std::string* out_name, std::string* out_id) {
    static const jmethodID get_session = adjust::jni::method(
        jni, jni->mappings->cls_minecraft, {"L", "getSession", "func_110432_I"}, "()Lavm;",
        "()Lnet/minecraft/util/Session;");
    static const jclass session_cls = adjust::jni::find_class(jni, "avm", "net/minecraft/util/Session");
    static const jmethodID get_username =
        adjust::jni::method(jni, session_cls, {"c", "getUsername", "func_111285_a"}, "()Ljava/lang/String;",
                            "()Ljava/lang/String;");
    static const jmethodID get_player_id =
        adjust::jni::method(jni, session_cls, {"b", "getPlayerID", "func_148255_b"}, "()Ljava/lang/String;",
                            "()Ljava/lang/String;");
    if (get_session == nullptr || get_username == nullptr || get_player_id == nullptr || mc == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    jobject session = env->CallObjectMethod(mc, get_session);
    if (adjust::jni::clear_exception(jni) || session == nullptr) {
        return false;
    }
    auto name = static_cast<jstring>(env->CallObjectMethod(session, get_username));
    adjust::jni::clear_exception(jni);
    auto id = static_cast<jstring>(env->CallObjectMethod(session, get_player_id));
    adjust::jni::clear_exception(jni);
    *out_name = jstring_to_utf8(env, name);
    *out_id = jstring_to_utf8(env, id);
    if (name != nullptr) {
        env->DeleteLocalRef(name);
    }
    if (id != nullptr) {
        env->DeleteLocalRef(id);
    }
    env->DeleteLocalRef(session);
    return !out_name->empty();
}

bool is_game_paused(events::JniContext* jni, jobject mc) noexcept {
    static const jmethodID mid = adjust::jni::method(jni, jni->mappings->cls_minecraft,
                                                     {"V", "isGamePaused", "func_147113_T"}, "()Z", "()Z");
    if (mid == nullptr || mc == nullptr) {
        return false;
    }
    const bool paused = jni->env->CallBooleanMethod(mc, mid) != JNI_FALSE;
    adjust::jni::clear_exception(jni);
    return paused;
}

bool key_down(events::JniContext* jni, jobject key_binding) noexcept {
    if (key_binding == nullptr) {
        return false;
    }
    const bool down = minecraft::key_binding_is_pressed(jni, key_binding);
    jni->env->DeleteLocalRef(key_binding);
    return down;
}

// SessionService.isActive(): active for 20 s after the last detected key press; after that the movement / attack /
// use keys are checked (pressing one resets the timer), and it stays active until 30 s have elapsed.
bool is_active(events::GameContext* ctx, int64_t now) noexcept {
    if (now - g_state.activity_timer < 20000) {
        return true;
    }
    events::JniContext* jni = ctx->jni;
    jobject settings = minecraft::mc_get_game_settings(jni, ctx->minecraft);
    if (settings != nullptr) {
        const bool any = key_down(jni, minecraft::settings_get_key_use_item(jni, settings)) ||
                         key_down(jni, minecraft::settings_get_key_attack(jni, settings)) ||
                         key_down(jni, minecraft::settings_get_key_forward(jni, settings)) ||
                         key_down(jni, minecraft::settings_get_key_back(jni, settings)) ||
                         key_down(jni, minecraft::settings_get_key_left(jni, settings)) ||
                         key_down(jni, minecraft::settings_get_key_right(jni, settings));
        jni->env->DeleteLocalRef(settings);
        if (any) {
            g_state.activity_timer = now;
            return true;
        }
    }
    return now - g_state.activity_timer < 30000;
}

// SessionInformationWidget.formatDuration.
void format_duration(int64_t millis, char* out, size_t out_size) noexcept {
    const int64_t seconds = millis / 1000 % 60;
    const int64_t minutes = millis / 60000;
    const int64_t hours = millis / 3600000;
    const int64_t days = millis / 86400000;
    if (days >= 1) {
        std::snprintf(out, out_size, "%" PRId64 "d %" PRId64 "h %" PRId64 "m", days, hours % 24, minutes % 60);
    } else if (hours % 24 > 0) {
        std::snprintf(out, out_size, "%" PRId64 "h %" PRId64 "m %" PRId64 "s", hours % 24, minutes % 60, seconds);
    } else if (minutes % 60 > 0) {
        std::snprintf(out, out_size, "%" PRId64 "m %" PRId64 "s", minutes % 60, seconds);
    } else {
        std::snprintf(out, out_size, "%" PRId64 "s", seconds);
    }
}

// GL texture id of the local player's skin (AbstractClientPlayer.getLocationSkin -> TextureManager.getTexture ->
// ITextureObject.getGlTextureId), 0 if unavailable.
GLuint skin_texture(events::GameContext* game) noexcept {
    events::JniContext* jni = game->jni;
    static const jclass player_cls =
        adjust::jni::find_class(jni, "bet", "net/minecraft/client/entity/AbstractClientPlayer");
    static const jmethodID get_location_skin = adjust::jni::method(
        jni, player_cls, {"i", "getLocationSkin", "func_110306_p"}, "()Ljy;", "()Lnet/minecraft/util/ResourceLocation;");
    static const jclass texture_manager_cls =
        adjust::jni::find_class(jni, "bmj", "net/minecraft/client/renderer/texture/TextureManager");
    static const jmethodID get_texture = adjust::jni::method(
        jni, texture_manager_cls, {"b", "getTexture", "func_110581_b"}, "(Ljy;)Lbmk;",
        "(Lnet/minecraft/util/ResourceLocation;)Lnet/minecraft/client/renderer/texture/ITextureObject;");
    static const jclass texture_object_cls =
        adjust::jni::find_class(jni, "bmk", "net/minecraft/client/renderer/texture/ITextureObject");
    static const jmethodID get_gl_texture_id = adjust::jni::method(
        jni, texture_object_cls, {"b", "getGlTextureId", "func_110552_b"}, "()I", "()I");
    if (get_location_skin == nullptr || get_texture == nullptr || get_gl_texture_id == nullptr ||
        game->local_player == nullptr || game->minecraft == nullptr) {
        return 0;
    }
    JNIEnv* env = jni->env;
    if (env->IsInstanceOf(game->local_player, player_cls) == JNI_FALSE) {
        return 0;
    }
    GLuint result = 0;
    if (env->PushLocalFrame(8) != 0) {
        env->ExceptionClear();
        return 0;
    }
    jobject location = env->CallObjectMethod(game->local_player, get_location_skin);
    if (!adjust::jni::clear_exception(jni) && location != nullptr) {
        jobject texture_manager = minecraft::mc_get_texture_manager(jni, game->minecraft);
        if (texture_manager != nullptr) {
            jobject texture = env->CallObjectMethod(texture_manager, get_texture, location);
            if (!adjust::jni::clear_exception(jni) && texture != nullptr) {
                const jint id = env->CallIntMethod(texture, get_gl_texture_id);
                if (!adjust::jni::clear_exception(jni) && id > 0) {
                    result = static_cast<GLuint>(id);
                }
            }
        }
    }
    env->PopLocalFrame(nullptr);
    return result;
}

void release_head_image() noexcept {
    if (g_head.image != 0 && g_head.vg != nullptr && g_head.vg == adjust::vg()) {
        nvgDeleteImage(g_head.vg, g_head.image); // NVG_IMAGE_NODELETE: the Minecraft texture stays alive
    }
    g_head = HeadImage{};
}

// AccountHead.draw at half scale: grey square, then the face (8,8)-(16,16) and the hat layer (40,8)-(48,16) of the
// 64x64 skin, 16x16 HUD units at (x, y).
void draw_head(events::GameContext* game, float x, float y) noexcept {
    adjust::fill_rect(0xA0909090u, x, y, x + 16.0f, y + 16.0f);
    NVGcontext* vg = adjust::vg();
    if (vg == nullptr || game == nullptr || game->jni == nullptr) {
        return;
    }
    const GLuint texture = skin_texture(game);
    if (texture == 0 || glIsTexture(texture) == GL_FALSE) {
        return;
    }
    if (g_head.vg != vg || g_head.texture != texture || g_head.image == 0) {
        release_head_image();
        g_head.vg = vg;
        g_head.texture = texture;
        constexpr int kFlags = static_cast<int>(NVG_IMAGE_NEAREST) | static_cast<int>(NVG_IMAGE_NODELETE);
        g_head.image = nvglCreateImageFromHandleGL3(vg, texture, 64, 64, kFlags);
        if (g_head.image == 0) {
            g_head = HeadImage{};
            return;
        }
    }
    constexpr float kTexel = 2.0f; // 8 texels -> 16 units
    const float extent = 64.0f * kTexel;
    const NVGpaint face = nvgImagePattern(vg, x - 8.0f * kTexel, y - 8.0f * kTexel, extent, extent, 0.0f,
                                          g_head.image, 1.0f);
    nvgBeginPath(vg);
    nvgRect(vg, x, y, 16.0f, 16.0f);
    nvgFillPaint(vg, face);
    nvgFill(vg);
    const NVGpaint hat = nvgImagePattern(vg, x - 40.0f * kTexel, y - 8.0f * kTexel, extent, extent, 0.0f,
                                         g_head.image, 1.0f);
    nvgBeginPath(vg);
    nvgRect(vg, x, y, 16.0f, 16.0f);
    nvgFillPaint(vg, hat);
    nvgFill(vg);
}

} // namespace

void session_render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    g_enabled.render(gui, renderer);
}

void session_visit_config(config::ConfigArchive* ar) noexcept {
    g_enabled.visit(ar, "widgets_session_information");
}

// SessionService.onTick (runs whether or not the widget is shown, like the Adjust service).
void session_tick(events::GameContext* ctx) noexcept {
    if (ctx == nullptr || ctx->jni == nullptr || ctx->world == nullptr || !adjust::jni::is_1_8(ctx->jni)) {
        return;
    }
    const int64_t now = adjust::now_ms();
    if (!g_state.timers_started) {
        g_state.total_timer = now;
        g_state.activity_timer = now;
        g_state.timers_started = true;
    }

    std::string name;
    std::string id;
    g_state.has_account = read_session(ctx->jni, ctx->minecraft, &name, &id);
    if (g_state.has_account) {
        g_state.user_name = name;
        g_state.player_id = id;
    }

    // serverState == null (not connected) never happens here: ticks only run with a world.
    if (is_game_paused(ctx->jni, ctx->minecraft)) {
        g_state.total_timer = now;
        return;
    }
    if (!g_state.has_account) {
        return;
    }
    SessionStats& stats = stats_for(g_state.user_name, g_state.player_id);
    if (is_active(ctx, now)) {
        stats.total_time += std::min<int64_t>(now - g_state.total_timer, 2000);
    }
    g_state.total_timer = now;
}

void session_render(const HudContext& ctx) noexcept {
    if (!g_enabled.value || !g_state.has_account || g_state.user_name.empty()) {
        return;
    }
    const std::string name = std::string(ADJUST_FMT "l") + g_state.user_name;
    const float width = std::max(
        93.0f, static_cast<float>(static_cast<int>(adjust::kTahoma12.width(name) * 1.42f + 35.0f)));
    const WidgetBox box = draw_frame("Session Information", kSessionX, kSessionY, width, 22.5f);
    const SessionStats& stats = stats_for(g_state.user_name, g_state.player_id);

    const float x = box.x;
    const float y = box.y;
    adjust::kTahoma12.draw(name, x + 23.5f, y + 13.5f, adjust::argb(215, 219, 213, 215));

    float kdr = static_cast<float>(stats.kills);
    if (stats.deaths != 0) {
        kdr = kdr / static_cast<float>(stats.deaths);
    }
    char number[32]{};
    format_number(static_cast<double>(kdr), number, sizeof(number));
    char kdr_text[48]{};
    std::snprintf(kdr_text, sizeof(kdr_text), "%s KDR", number);
    const float right = x + box.width - 3.0f;
    adjust::kTahoma10.draw(kdr_text, right - adjust::kTahoma10.width(kdr_text), y + 24.5f,
                           adjust::argb(230, 225, 220, 235));

    char duration[48]{};
    format_duration(stats.total_time, duration, sizeof(duration));
    adjust::kTahoma10.draw(duration, x + 22.5f, y + 24.5f, adjust::argb(215, 219, 213, 215));

    // kills / (kills + deaths) bar
    const float bar_width = box.width - 25.5f;
    const float bar_left = x + 22.5f;
    const float bar_top = y + 20.0f;
    const float bar_bottom = bar_top + 1.0f;
    const float kill_fraction =
        static_cast<float>(stats.kills) / static_cast<float>(stats.deaths + stats.kills); // NaN with no kills/deaths
    adjust::fill_rounded_rect(adjust::argb(162, 162, 162, 140), bar_left, bar_top, bar_left + bar_width, bar_bottom,
                              1.0f);
    if (std::isfinite(kill_fraction)) {
        adjust::fill_rounded_rect((hud_color() & 0xFFFFFFu) | 0xC3000000u, bar_left, bar_top,
                                  kill_fraction * bar_width + bar_left, bar_bottom, 1.0f);
    }

    // Head (Adjust: post task drawn after the NanoVG frame) at (x + 4, y + 12.5).
    draw_head(ctx.game, x + 4.0f, y + 12.5f);
}

void session_shutdown() noexcept {
    // The NanoVG context is destroyed with the renderer; only forget the handle (the GL texture is Minecraft's).
    g_head = HeadImage{};
}

} // namespace slinky::modules::adjust_hud::widgets
