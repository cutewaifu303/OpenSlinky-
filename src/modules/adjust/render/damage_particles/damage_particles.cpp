// Port of com.adjustclient.module.impl.render.DamageParticlesModule (+ DamageParticle, DamageParticlesDisplay,
// DamageParticlesColorCondition, UnusedDamageParticleData): 3D floating text showing health lost or gained.
//
// Detection: Adjust listens to S19PacketEntityStatus (op 2 = hurt) and S1CPacketEntityMetadata (health, watcher
// id 6) on the network thread and reads the old health from the entity. OpenSlinky++ forbids JNI world access on
// the Netty thread, so this port tracks every player's health once per player update on the main thread and
// emits a particle when it changes. "Damage only" requires a fresh hurt animation (hurtTime >= 8, i.e. a hurt
// status within the last ~100 ms) exactly like Adjust's 100 ms pending-status window.
//
// Text is rendered with Minecraft's font renderer in the world pass, billboarded like Adjust (scale -0.03).
// UnusedDamageParticleData (an empty holder) has no behaviour. The ServiceUtil "ahmedElSisiMode" text prefix is a
// backend joke flag and is skipped.

#include "modules/adjust/render/damage_particles/damage_particles.hpp"

#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "adjust/kit.hpp"
#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "gui/widgets.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "util/math.hpp"
#include "util/random.hpp"

namespace slinky::modules {

namespace {

using adjust::Argb;

constexpr const char* kDisplayOptions[2] = {"Always", "Damage only"}; // DamageParticlesDisplay
enum Display : int32_t { kDisplayAlways = 0, kDisplayDamageOnly = 1 };

constexpr double kMaxDistanceSq = 576.0; // 24 blocks
constexpr int kFreshHurtTime = 8;        // hurtTime is 10 right after S19 op 2; >= 8 ~ within 100 ms

void on_post_living_update(void* event) noexcept;
void on_render_world_3d(void* event) noexcept;
void on_module_state_changed(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

// DamageParticle
struct Particle {
    std::string text;
    jstring jtext = nullptr; // global ref for FontRenderer.drawString
    float text_x = 0.0f;     // -width / 2
    math::Vec3d position{};
    int64_t created_ms = 0;  // timer.lastMs
    Argb color = 0;
};

struct TrackedHealth {
    float health = 0.0f;
    uint32_t generation = 0;
};

struct DamageParticlesState {
    events::Module base{};

    config::ModeSetting display{{}, "Display", 2, 0, kDisplayOptions, kDisplayDamageOnly, kDisplayDamageOnly,
                                false, {}};
    // Custom color
    config::BoolSetting custom_enabled{{}, "Enabled", true, true, {}};
    adjust::ColorOption custom_color{"Color", "Color mode", 0xFF435AD1u, adjust::ColorMode::Plain};
    config::FloatSetting timeout{{}, "Timeout", "%.1f", 0.5f, 0.0f, 1.0f, 6.0f, 3.5f, 3.5f};

    std::vector<Particle> particles;
    std::unordered_map<int32_t, TrackedHealth> health;
    uint32_t generation = 0;
    jweak last_world = nullptr;

    DamageParticlesState() noexcept {
        base.name = "Damage Particles";
        base.description = "Shows 3D floating text displaying health lost or gained.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagHiddenFromHud; // Adjust default: hidden
        base.events[events::kEventPostLivingUpdate] = {on_post_living_update, 0, 0};
        base.events[events::kEventRenderWorld3D] = {on_render_world_3d, 0, 0};
        base.events[events::kEventModuleStateChanged] = {on_module_state_changed, 0, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Damage Particles");
    }
};

DamageParticlesState g_state{};

int duration_ms() noexcept {
    return static_cast<int>(g_state.timeout.value * 1000.0f);
}

void release_particle(JNIEnv* env, Particle& particle) noexcept {
    if (particle.jtext != nullptr && env != nullptr) {
        env->DeleteGlobalRef(particle.jtext);
    }
    particle.jtext = nullptr;
}

// DamageParticlesModule.clear()
void clear_all(JNIEnv* env) noexcept {
    for (Particle& particle : g_state.particles) {
        release_particle(env, particle);
    }
    g_state.particles.clear();
    g_state.health.clear();
}

// RandomUtil.round(float, 1): BigDecimal HALF_UP (away from zero on ties).
float round_one(float value) noexcept {
    if (std::isinf(value)) {
        return 0.0f;
    }
    const double v = static_cast<double>(value);
    const double scaled = std::floor(std::fabs(v) * 10.0 + 0.5) / 10.0;
    return static_cast<float>(v < 0.0 ? -scaled : scaled);
}

// Float.toString for the one-decimal values produced by round_one.
std::string java_float_string(float value) noexcept {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(value));
    return buf;
}

// java.awt.Color.brighter() (FACTOR 0.7)
Argb awt_brighter(Argb c) noexcept {
    int r = adjust::red_of(c);
    int g = adjust::green_of(c);
    int b = adjust::blue_of(c);
    const int a = adjust::alpha_of(c);
    const int i = static_cast<int>(1.0 / (1.0 - 0.7));
    if (r == 0 && g == 0 && b == 0) {
        return adjust::argb(i, i, i, a);
    }
    if (r > 0 && r < i) r = i;
    if (g > 0 && g < i) g = i;
    if (b > 0 && b < i) b = i;
    return adjust::argb(std::min(static_cast<int>(r / 0.7), 255), std::min(static_cast<int>(g / 0.7), 255),
                        std::min(static_cast<int>(b / 0.7), 255), a);
}

float eye_height(events::JniContext* jni, jobject entity) noexcept {
    static const jmethodID mid = adjust::jni::method(jni, jni->mappings->cls_entity,
                                                     {"aS", "getEyeHeight", "func_70047_e"}, "()F", "()F");
    if (mid == nullptr) {
        return static_cast<float>(math::kPlayerEyeHeight);
    }
    const float h = jni->env->CallFloatMethod(entity, mid);
    if (adjust::jni::clear_exception(jni) || !std::isfinite(h)) {
        return static_cast<float>(math::kPlayerEyeHeight);
    }
    return h;
}

double random_unit() noexcept {
    return static_cast<double>(random::uniform_float(0.0f, 1.0f));
}

// DamageParticlesModule.addParticle + DamageParticle constructor
void add_particle(events::GameContext* ctx, jobject entity, bool is_signed, float old_health,
                  float new_health) noexcept {
    std::string text;
    float fraction = 0.0f;
    if (is_signed) {
        const float delta = round_one(new_health - old_health);
        if (delta > 0.0f) {
            text = "+" + java_float_string(delta);
            fraction = 0.0f;
        } else if (delta < 0.0f) {
            text = java_float_string(delta);
            fraction = std::min(-delta / 6.0f, 1.0f);
        } else {
            return;
        }
    } else {
        if (old_health <= new_health) {
            return;
        }
        const float delta = round_one(old_health - new_health);
        text = java_float_string(delta);
        fraction = std::min(delta / 6.0f, 1.0f);
    }

    Argb color = 0;
    if (g_state.custom_enabled.value) {
        const Argb selected = g_state.custom_color.get();
        const Argb dark = adjust::argb(static_cast<int>(adjust::red_of(selected) * 0.63f),
                                       static_cast<int>(adjust::green_of(selected) * 0.63f),
                                       static_cast<int>(adjust::blue_of(selected) * 0.63f),
                                       adjust::alpha_of(selected));
        color = adjust::blend(dark, awt_brighter(selected), static_cast<double>(fraction));
    } else {
        color = adjust::hsb_to_rgb((1.0f - fraction) * 0.33333334f, 0.6f, 0.6f) | 0xFF000000u;
    }

    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;

    Particle particle{};
    particle.text = std::move(text);
    particle.color = color;
    particle.created_ms = adjust::now_ms();

    jstring local = env->NewStringUTF(particle.text.c_str());
    if (local == nullptr) {
        adjust::jni::clear_exception(jni);
        return;
    }
    particle.jtext = static_cast<jstring>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (particle.jtext == nullptr) {
        return;
    }

    jobject render_manager = minecraft::mc_get_render_manager(jni, ctx->minecraft);
    jobject font_renderer =
        render_manager != nullptr ? minecraft::render_manager_get_font_renderer(jni, render_manager) : nullptr;
    const int width =
        font_renderer != nullptr ? minecraft::font_renderer_get_string_width(jni, font_renderer, particle.jtext) : 0;
    particle.text_x = static_cast<float>(-width) * 0.5f;
    if (font_renderer != nullptr) {
        env->DeleteLocalRef(font_renderer);
    }
    if (render_manager != nullptr) {
        env->DeleteLocalRef(render_manager);
    }

    const math::Vec3d pos = minecraft::entity_get_position(jni, entity);
    const double ox = (random_unit() - 0.5) * 1.5;
    const double oy = (random_unit() - 0.5) * 0.4;
    const double oz = (random_unit() - 0.5) * 1.5;
    particle.position = math::make_vec3(pos.x + ox, pos.y + static_cast<double>(eye_height(jni, entity)) - 0.2 + oy,
                                        pos.z + oz);
    g_state.particles.push_back(std::move(particle));
}

// Resets everything when the world object changes (S01PacketJoinGame / dimension change).
void check_world(events::GameContext* ctx) noexcept {
    JNIEnv* env = ctx->jni->env;
    if (g_state.last_world != nullptr && env->IsSameObject(g_state.last_world, ctx->world)) {
        return;
    }
    clear_all(env);
    if (g_state.last_world != nullptr) {
        env->DeleteWeakGlobalRef(g_state.last_world);
    }
    g_state.last_world = env->NewWeakGlobalRef(ctx->world);
}

// DamageParticlesModule.onTick: expire particles, then (port) poll health changes.
void on_post_living_update(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->local_player == nullptr || ctx->world == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    check_world(ctx);

    jobject camera = minecraft::mc_get_render_view_entity(jni, ctx->minecraft);
    const math::Vec3d camera_pos =
        camera != nullptr ? minecraft::entity_get_position(jni, camera) : minecraft::entity_get_position(jni, ctx->local_player);

    // isExpired
    const int64_t now = adjust::now_ms();
    const int64_t lifetime = static_cast<int64_t>(duration_ms()) + 470;
    auto expired = [&](Particle& particle) noexcept {
        const bool dead = particle.created_ms + lifetime <= now ||
                          math::vec3_dist_sq(camera_pos, particle.position) >= kMaxDistanceSq;
        if (dead) {
            release_particle(env, particle);
        }
        return dead;
    };
    g_state.particles.erase(std::remove_if(g_state.particles.begin(), g_state.particles.end(), expired),
                            g_state.particles.end());

    // Health tracking (replaces the S19 / S1C packet listener).
    const bool always = g_state.display.value == kDisplayAlways;
    const uint32_t gen = ++g_state.generation;
    jobject players = minecraft::world_get_player_entities(jni, ctx->world);
    const int32_t count = players != nullptr ? minecraft::java_list_size(jni, players) : 0;
    for (int32_t i = 0; i < count; ++i) {
        if (env->PushLocalFrame(32) != 0) {
            adjust::jni::clear_exception(jni);
            break;
        }
        jobject entity = minecraft::java_list_get(jni, players, i);
        if (entity != nullptr && minecraft::as_entity_player(jni, entity) != nullptr) {
            const int32_t id = minecraft::entity_get_id(jni, entity);
            const float health = minecraft::living_get_health(jni, entity);
            auto it = g_state.health.find(id);
            if (it == g_state.health.end()) {
                g_state.health.emplace(id, TrackedHealth{health, gen});
            } else {
                const float old_health = it->second.health;
                it->second.health = health;
                it->second.generation = gen;
                if (health != old_health) {
                    const bool is_self = env->IsSameObject(entity, ctx->local_player) != JNI_FALSE;
                    if (always) {
                        add_particle(ctx, entity, true, old_health, health);
                    } else if (!is_self &&
                               math::vec3_dist_sq(minecraft::entity_get_position(jni, entity), camera_pos) <=
                                   kMaxDistanceSq &&
                               minecraft::living_get_hurt_time(jni, entity) >= kFreshHurtTime) {
                        add_particle(ctx, entity, false, old_health, health);
                    }
                }
            }
        }
        env->PopLocalFrame(nullptr);
    }
    for (auto it = g_state.health.begin(); it != g_state.health.end();) {
        it = it->second.generation != gen ? g_state.health.erase(it) : std::next(it);
    }

    if (players != nullptr) {
        env->DeleteLocalRef(players);
    }
    if (camera != nullptr) {
        env->DeleteLocalRef(camera);
    }
}

// DamageParticlesModule.onRender + renderParticle
void on_render_world_3d(void* event) noexcept {
    if (g_state.particles.empty()) {
        return;
    }
    auto* ev = static_cast<events::RenderWorld3DEvent*>(event);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->minecraft == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    const bool legacy = minecraft::is_version_1_7_10(jni);

    jobject render_manager = minecraft::mc_get_render_manager(jni, ctx->minecraft);
    if (render_manager == nullptr) {
        return;
    }
    jobject font_renderer = minecraft::render_manager_get_font_renderer(jni, render_manager);
    if (font_renderer == nullptr) {
        env->DeleteLocalRef(render_manager);
        return;
    }
    const float view_x = minecraft::render_manager_get_player_view_x(jni, render_manager);
    const float view_y = minecraft::render_manager_get_player_view_y(jni, render_manager);
    jobject settings = minecraft::mc_get_game_settings(jni, ctx->minecraft);
    const int third_person = settings != nullptr ? minecraft::settings_get_third_person_view(jni, settings) : 0;
    if (settings != nullptr) {
        env->DeleteLocalRef(settings);
    }
    const float pitch_axis = third_person == 2 ? -1.0f : 1.0f;

    // GlStateManager.enableBlend / tryBlendFuncSeparate(770, 771, 1, 0) / disableDepth / depthMask(false):
    // blend and depth are already set up by the world pass; the font needs textures.
    glDepthMask(GL_FALSE);
    if (legacy) {
        glEnable(GL_TEXTURE_2D);
    } else {
        minecraft::gl_state_enable_texture_2d(jni);
        minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    const int64_t now = adjust::now_ms();
    const int duration = duration_ms();
    for (const Particle& particle : g_state.particles) {
        if (particle.jtext == nullptr) {
            continue;
        }
        int alpha = static_cast<int>(particle.color >> 24);
        const int64_t elapsed = now - particle.created_ms;
        if (elapsed >= duration) {
            const int64_t remaining = static_cast<int64_t>(duration) + 500 - elapsed;
            if (remaining < 30) {
                continue;
            }
            alpha = static_cast<int>(static_cast<float>(remaining) / 500.0f * static_cast<float>(alpha));
        }
        if (alpha < 25) {
            continue;
        }

        glPushMatrix();
        glTranslated(particle.position.x - ev->render_pos.x, particle.position.y - ev->render_pos.y,
                     particle.position.z - ev->render_pos.z);
        glRotatef(-view_y, 0.0f, 1.0f, 0.0f);
        glRotatef(view_x, pitch_axis, 0.0f, 0.0f);
        glScalef(-0.03f, -0.03f, -0.03f);
        const auto color = static_cast<int32_t>((static_cast<uint32_t>(alpha) << 24) | (particle.color & 0xFFFFFFu));
        minecraft::font_renderer_draw_string(jni, font_renderer, particle.jtext, particle.text_x, 0.0f, color, true);
        adjust::jni::clear_exception(jni);
        glPopMatrix();
    }

    if (legacy) {
        glDisable(GL_TEXTURE_2D);
    } else {
        minecraft::gl_state_disable_texture_2d(jni);
    }
    glDepthMask(GL_TRUE);

    env->DeleteLocalRef(font_renderer);
    env->DeleteLocalRef(render_manager);
}

// onDisable: particles.clear(); pending.clear()
void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    if (ev->module != &g_state.base || ev->enabled) {
        return;
    }
    JNIEnv* env = (ev->ctx != nullptr && ev->ctx->jni != nullptr) ? ev->ctx->jni->env : nullptr;
    clear_all(env);
    if (g_state.last_world != nullptr && env != nullptr) {
        env->DeleteWeakGlobalRef(g_state.last_world);
    }
    g_state.last_world = nullptr;
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_state.display.render(gui, renderer);
    gui::widgets::render_section_header(gui, renderer, "Custom color");
    g_state.custom_enabled.render(gui, renderer);
    if (g_state.custom_enabled.value) {
        g_state.custom_color.render(gui, renderer);
    }
    gui::widgets::render_section_header(gui, renderer, "Other");
    g_state.timeout.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.display.visit(archive, "display");
    g_state.custom_enabled.visit(archive, "custom_color_enabled");
    g_state.custom_color.visit(archive, "custom_color", "custom_color_mode");
    g_state.timeout.visit(archive, "timeout");
}

} // namespace

events::Module* get_adjust_damage_particles_module() noexcept {
    return &g_state.base;
}

} // namespace slinky::modules
