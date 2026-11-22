#include "modules/adjust/render/chams/adjust_chams.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <glad/glad.h>

#include "adjust/jni_lookup.hpp"
#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/esp/adjust_render_jni.hpp"

namespace slinky::modules {

namespace {

namespace J = adjust::jni;
namespace R = adjust_render;

constexpr const char* kModeOptions[2] = {"Normal", "Colored"}; // ChamsMode

enum class ChamsMode : int32_t { Normal = 0, Colored = 1 };

adjust::Argb occluded_default(adjust::Argb color) noexcept {
    // new Color((int) (r * 0.7), (int) (g * 0.7), (int) (b * 0.7), a)
    return adjust::argb(static_cast<int>(adjust::red_of(color) * 0.7), static_cast<int>(adjust::green_of(color) * 0.7),
                        static_cast<int>(adjust::blue_of(color) * 0.7), adjust::alpha_of(color));
}

// Port of ChamsTargetSettings.
struct TargetSettings {
    const char* name;
    std::string key_prefix;
    config::BoolSetting enabled;
    config::ModeSetting mode;
    adjust::ColorOption color;
    config::BoolSetting through_walls;
    adjust::ColorOption through_walls_color;

    TargetSettings(const char* label, const char* prefix, bool enabled_default, adjust::Argb color_default) noexcept
        : name(label),
          key_prefix(prefix),
          enabled{{}, "Enabled", enabled_default, enabled_default, {}},
          mode{{}, "Mode", 2, 0, kModeOptions, static_cast<int32_t>(ChamsMode::Colored),
               static_cast<int32_t>(ChamsMode::Colored), false, {}},
          color("Color", "Color mode", color_default),
          through_walls{{}, "Through walls", true, true, {}},
          through_walls_color("Through walls color", "Through walls color mode", occluded_default(color_default)) {}

    ChamsMode mode_value() const noexcept { return static_cast<ChamsMode>(mode.value); }

    void render(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
        gui::widgets::render_section_header(gui, renderer, name);
        enabled.render(gui, renderer);
        if (!enabled.value) {
            return;
        }
        mode.render(gui, renderer);
        if (mode_value() != ChamsMode::Colored) {
            return;
        }
        color.render(gui, renderer);
        through_walls.render(gui, renderer);
        if (through_walls.value) {
            through_walls_color.render(gui, renderer);
        }
    }

    void visit(config::ConfigArchive* ar) noexcept {
        enabled.visit(ar, (key_prefix + "_enabled").c_str());
        mode.visit(ar, (key_prefix + "_mode").c_str());
        color.visit(ar, (key_prefix + "_color").c_str(), (key_prefix + "_color_mode").c_str());
        through_walls.visit(ar, (key_prefix + "_through_walls").c_str());
        through_walls_color.visit(ar, (key_prefix + "_through_walls_color").c_str(),
                                  (key_prefix + "_through_walls_color_mode").c_str());
    }
};

// ChamsModule targets (the "Enemy" group needs Adjust's TARGET player tag, which OpenSlinky++ does not have).
TargetSettings s_local("Local", "local", true, adjust::argb(0x90, 0xB0, 0x53));
TargetSettings s_players("Players", "players", true, adjust::argb(0x43, 0x5A, 0xD1));
TargetSettings s_friendly("Friendly", "friendly", true, adjust::argb(0x5F, 0x80, 0xFF));
TargetSettings s_hostile("Hostile", "hostile", false, adjust::argb(0xFF, 0x57, 0x57));
TargetSettings s_passive("Passive", "passive", false, adjust::argb(0xFF, 0xC7, 0x66));

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    s_local.render(gui, renderer);
    s_players.render(gui, renderer);
    s_friendly.render(gui, renderer);
    s_hostile.render(gui, renderer);
    s_passive.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    s_local.visit(ar);
    s_players.visit(ar);
    s_friendly.visit(ar);
    s_hostile.visit(ar);
    s_passive.visit(ar);
}

events::Module create_module() noexcept {
    events::Module mod{};
    mod.name = "Chams";
    mod.description = "Renders entities through walls or with a flat color.";
    mod.category = events::ModuleCategory::Visual;
    mod.render_settings = &render_settings;
    mod.visit_config = &visit_config;
    mod.flags |= events::kModuleFlagStyleAdjust;
    return mod;
}

// ChamsModule.updateOverrides(): which target settings apply to `entity` (nullptr = none).
TargetSettings* classify(events::JniContext* jni, jobject entity) noexcept {
    JNIEnv* env = jni->env;
    jobject mc = minecraft::mc_get_instance(jni);
    jobject local = minecraft::mc_get_player(jni, mc);
    const bool is_local = local != nullptr && env->IsSameObject(entity, local);
    R::drop(jni, local);
    R::drop(jni, mc);
    if (is_local) {
        return &s_local;
    }
    if (R::is_instance(jni, entity, R::Cls::EntityPlayer)) {
        const uint16_t tags = minecraft::player_get_tag_flags(jni, entity);
        if ((tags & minecraft::kPlayerTagBot) != 0 || R::entity_is_invisible(jni, entity)) {
            return nullptr; // AntiBot removes bots from the tracker; invisible entities are not requested (no 0x40)
        }
        const bool teammate = (tags & (minecraft::kPlayerTagTeammate | minecraft::kPlayerTagFriend)) != 0;
        if (teammate) {
            return s_friendly.enabled.value ? &s_friendly : nullptr;
        }
        return s_players.enabled.value ? &s_players : nullptr;
    }
    if (!R::is_instance(jni, entity, R::Cls::EntityLiving) || R::is_instance(jni, entity, R::Cls::EntityArmorStand) ||
        R::entity_is_invisible(jni, entity)) {
        return nullptr;
    }
    const bool hostile = R::is_instance(jni, entity, R::Cls::EntityMob) ||
                         R::is_instance(jni, entity, R::Cls::EntityIronGolem);
    TargetSettings* target = hostile ? &s_hostile : &s_passive;
    return target->enabled.value ? target : nullptr;
}

// ---- RendererLivingEntity.doRender model pass, re-issued through JNI ----------------------------------------------

struct ModelMembers {
    jmethodID swing_progress = nullptr;  // RendererLivingEntity.getSwingProgress(EntityLivingBase, F)F
    jmethodID interpolate = nullptr;     // RendererLivingEntity.interpolateRotation(FFF)F
    jmethodID living_at = nullptr;       // renderLivingAt(EntityLivingBase, DDD)V
    jmethodID rotation_float = nullptr;  // handleRotationFloat(EntityLivingBase, F)F
    jmethodID rotate_corpse = nullptr;   // rotateCorpse(EntityLivingBase, FFF)V
    jmethodID pre_render = nullptr;      // preRenderCallback(EntityLivingBase, F)V
    jmethodID render_model = nullptr;    // renderModel(EntityLivingBase, FFFFFF)V
    jmethodID living_animations = nullptr; // ModelBase.setLivingAnimations(EntityLivingBase, FFF)V
    jmethodID rotation_angles = nullptr; // ModelBase.setRotationAngles(FFFFFF, Entity)V
    jfieldID model_swing = nullptr;      // ModelBase.swingProgress
    jfieldID model_riding = nullptr;     // ModelBase.isRiding
    jfieldID model_child = nullptr;      // ModelBase.isChild
    bool ok = false;
};

const ModelMembers& members(events::JniContext* jni) noexcept {
    static const ModelMembers s = [&]() {
        ModelMembers m{};
        if (!R::available(jni)) {
            return m;
        }
        jclass rle = jni->mappings->cls_renderer_living_entity;
        static const jclass model = J::find_class(jni, "bbo", "net/minecraft/client/model/ModelBase");
        m.swing_progress = J::method(jni, rle, {"d", "getSwingProgress", "func_77040_d"}, "(Lpr;F)F",
                                     "(Lnet/minecraft/entity/EntityLivingBase;F)F");
        m.interpolate = J::method(jni, rle, {"a", "interpolateRotation", "func_77034_a"}, "(FFF)F", "(FFF)F");
        m.living_at = J::method(jni, rle, {"a", "renderLivingAt", "func_77039_a"}, "(Lpr;DDD)V",
                                "(Lnet/minecraft/entity/EntityLivingBase;DDD)V");
        m.rotation_float = J::method(jni, rle, {"b", "handleRotationFloat", "func_77044_a"}, "(Lpr;F)F",
                                     "(Lnet/minecraft/entity/EntityLivingBase;F)F");
        m.rotate_corpse = J::method(jni, rle, {"a", "rotateCorpse", "func_77043_a"}, "(Lpr;FFF)V",
                                    "(Lnet/minecraft/entity/EntityLivingBase;FFF)V");
        m.pre_render = J::method(jni, rle, {"a", "preRenderCallback", "func_77041_b"}, "(Lpr;F)V",
                                 "(Lnet/minecraft/entity/EntityLivingBase;F)V");
        m.render_model = J::method(jni, rle, {"a", "renderModel", "func_77036_a"}, "(Lpr;FFFFFF)V",
                                   "(Lnet/minecraft/entity/EntityLivingBase;FFFFFF)V");
        m.living_animations = J::method(jni, model, {"a", "setLivingAnimations", "func_78086_a"}, "(Lpr;FFF)V",
                                        "(Lnet/minecraft/entity/EntityLivingBase;FFF)V");
        m.rotation_angles = J::method(jni, model, {"a", "setRotationAngles", "func_78087_a"}, "(FFFFFFLpk;)V",
                                      "(FFFFFFLnet/minecraft/entity/Entity;)V");
        m.model_swing = J::field(jni, model, {"p", "swingProgress", "field_78095_p"}, "F", "F");
        m.model_riding = J::field(jni, model, {"q", "isRiding", "field_78093_q"}, "Z", "Z");
        m.model_child = J::field(jni, model, {"r", "isChild", "field_78091_s"}, "Z", "Z");
        m.ok = m.swing_progress != nullptr && m.interpolate != nullptr && m.living_at != nullptr &&
               m.rotation_float != nullptr && m.rotate_corpse != nullptr && m.pre_render != nullptr &&
               m.render_model != nullptr && m.living_animations != nullptr && m.rotation_angles != nullptr &&
               m.model_swing != nullptr && m.model_riding != nullptr && m.model_child != nullptr;
        return m;
    }();
    return s;
}

float wrap_angle_180(float value) noexcept { // MathHelper.wrapAngleTo180_float
    value = std::fmod(value, 360.0f);
    if (value >= 180.0f) {
        value -= 360.0f;
    }
    if (value < -180.0f) {
        value += 360.0f;
    }
    return value;
}

// Arguments of mainModel.render / renderModel for this frame (f6, f5, f8, f2, f7 in RendererLivingEntity.doRender).
struct ModelPose {
    float limb_swing = 0.0f;        // f6
    float limb_swing_amount = 0.0f; // f5
    float age = 0.0f;               // f8
    float head_yaw = 0.0f;          // f2
    float pitch = 0.0f;             // f7
    float body_yaw = 0.0f;          // f
};

// Replays the transform + model animation part of RendererLivingEntity.doRender (after pushMatrix).
// Returns false when a JNI call failed (the matrix is still pushed by the caller).
bool setup_model(events::JniContext* jni, const ModelMembers& m, jobject renderer, jobject entity, jobject model,
                 double x, double y, double z, float pt, ModelPose* pose) noexcept {
    JNIEnv* env = jni->env;
    const float swing = env->CallFloatMethod(renderer, m.swing_progress, entity, pt);
    if (J::clear_exception(jni)) {
        return false;
    }
    env->SetFloatField(model, m.model_swing, swing);
    env->SetBooleanField(model, m.model_riding, R::entity_is_riding(jni, entity) ? JNI_TRUE : JNI_FALSE);
    const bool child = R::living_is_child(jni, entity);
    env->SetBooleanField(model, m.model_child, child ? JNI_TRUE : JNI_FALSE);

    const auto interpolate = [&](float prev, float cur) noexcept {
        return env->CallFloatMethod(renderer, m.interpolate, prev, cur, pt);
    };
    float f = interpolate(R::living_prev_render_yaw_offset(jni, entity), R::living_render_yaw_offset(jni, entity));
    const float f1 = interpolate(R::living_prev_head_yaw(jni, entity), R::living_head_yaw(jni, entity));
    float f2 = f1 - f;
    if (R::entity_is_riding(jni, entity)) {
        jobject mount = minecraft::entity_get_riding_entity(jni, entity);
        if (mount != nullptr && R::is_instance(jni, mount, R::Cls::EntityLivingBase)) {
            f = interpolate(R::living_prev_render_yaw_offset(jni, mount), R::living_render_yaw_offset(jni, mount));
            f2 = f1 - f;
            float f3 = wrap_angle_180(f2);
            f3 = std::clamp(f3, -85.0f, 85.0f);
            f = f1 - f3;
            if (f3 * f3 > 2500.0f) {
                f += f3 * 0.2f;
            }
        }
        R::drop(jni, mount);
    }
    const float prev_pitch = R::entity_prev_pitch(jni, entity);
    const float f7 = prev_pitch + (R::entity_pitch(jni, entity) - prev_pitch) * pt;

    env->CallVoidMethod(renderer, m.living_at, entity, x, y, z);
    const float f8 = env->CallFloatMethod(renderer, m.rotation_float, entity, pt);
    env->CallVoidMethod(renderer, m.rotate_corpse, entity, f8, f, pt);
    if (J::clear_exception(jni)) {
        return false;
    }
    glScalef(-1.0f, -1.0f, 1.0f);
    env->CallVoidMethod(renderer, m.pre_render, entity, pt);
    glTranslatef(0.0f, -1.5078125f, 0.0f);

    const float prev_amount = R::living_prev_limb_swing_amount(jni, entity);
    const float amount = R::living_limb_swing_amount(jni, entity);
    float f5 = prev_amount + (amount - prev_amount) * pt;
    float f6 = R::living_limb_swing(jni, entity) - amount * (1.0f - pt);
    if (child) {
        f6 *= 3.0f;
    }
    if (f5 > 1.0f) {
        f5 = 1.0f;
    }
    env->CallVoidMethod(model, m.living_animations, entity, f6, f5, pt);
    env->CallVoidMethod(model, m.rotation_angles, f6, f5, f8, f2, f7, 0.0625f, entity);
    if (J::clear_exception(jni)) {
        return false;
    }
    pose->limb_swing = f6;
    pose->limb_swing_amount = f5;
    pose->age = f8;
    pose->head_yaw = f2;
    pose->pitch = f7;
    pose->body_yaw = f;
    return true;
}

// Texture combiner setup of RendererLivingEntity.setBrightness(..., customChams = true, ...): unit 0 = skin * lightmap,
// unit 1 = interpolate(chams colour, previous, chams alpha), alpha = skin alpha.
void apply_chams_combiner(const float rgba[4]) noexcept {
    glActiveTexture(GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);

    glActiveTexture(GL_TEXTURE1);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_CONSTANT);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_CONSTANT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, rgba);

    // customChams branch after the brightness unit setup: unit 0 sources the lightmap texel as second operand.
    glActiveTexture(GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE1);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
}

// RendererLivingEntity.unsetBrightness() for units 0 and 1 (+ the previous enable state of the lightmap unit).
void restore_combiner(bool lightmap_enabled) noexcept {
    glActiveTexture(GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);

    glActiveTexture(GL_TEXTURE1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
    if (!lightmap_enabled) {
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
}

void set_enabled(GLenum cap, bool enabled) noexcept {
    if (enabled) {
        glEnable(cap);
    } else {
        glDisable(cap);
    }
}

enum class ColoredPass { ThroughWalls, Visible };

// One renderModel call with the chams colour, like the Colored branch of Adjust's RendererLivingEntity.doRender.
void render_colored_model(events::JniContext* jni, jobject renderer, jobject entity, double x, double y, double z,
                          float pt, adjust::Argb color, ColoredPass pass) noexcept {
    const ModelMembers& m = members(jni);
    if (!m.ok) {
        return;
    }
    jobject model = R::renderer_main_model(jni, renderer);
    if (model == nullptr) {
        return;
    }

    // setBrightness: a = 1 - colorMultiplierAlpha (0 for players); the visible pass fades with hurt/death time.
    float a = 1.0f;
    if (pass == ColoredPass::Visible) {
        const float chams_a = static_cast<float>(adjust::alpha_of(color)) / 255.0f;
        const int hurt_time = minecraft::living_get_hurt_time(jni, entity);
        const int death_time = R::living_death_time(jni, entity);
        if (hurt_time >= 1) {
            a = (a - (static_cast<float>(hurt_time) - pt) * 0.035f) * chams_a;
        } else if (death_time >= 1) {
            a = chams_a * (a - (static_cast<float>(death_time) - pt) * 0.075f);
        } else {
            a = chams_a;
        }
        a = std::clamp(a, 0.0f, 1.0f);
    }
    const float rgba[4] = {std::min(static_cast<float>(adjust::red_of(color)) / 255.0f, 1.0f),
                           std::min(static_cast<float>(adjust::green_of(color)) / 255.0f, 1.0f),
                           std::min(static_cast<float>(adjust::blue_of(color)) / 255.0f, 1.0f), a};

    // Raw GL state touched here is restored to its exact previous value so GlStateManager's cache stays valid.
    const bool cull = glIsEnabled(GL_CULL_FACE) != GL_FALSE;
    const bool depth = glIsEnabled(GL_DEPTH_TEST) != GL_FALSE;
    const bool rescale = glIsEnabled(GL_RESCALE_NORMAL) != GL_FALSE;
    const bool alpha_test = glIsEnabled(GL_ALPHA_TEST) != GL_FALSE;
    const bool offset_fill = glIsEnabled(GL_POLYGON_OFFSET_FILL) != GL_FALSE;
    GLfloat offset_factor = 0.0f;
    GLfloat offset_units = 0.0f;
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &offset_factor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &offset_units);
    glActiveTexture(GL_TEXTURE1);
    const bool lightmap = glIsEnabled(GL_TEXTURE_2D) != GL_FALSE;
    glActiveTexture(GL_TEXTURE0);

    glPushMatrix();
    glDisable(GL_CULL_FACE);
    glEnable(GL_RESCALE_NORMAL);
    ModelPose pose{};
    if (setup_model(jni, m, renderer, entity, model, x, y, z, pt, &pose)) {
        glEnable(GL_ALPHA_TEST);
        if (pass == ColoredPass::ThroughWalls) {
            glDisable(GL_DEPTH_TEST);
        } else {
            // Drawn after the vanilla model: win the depth test against it, lose against everything in front.
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1.0f, -10.0f);
        }
        apply_chams_combiner(rgba);
        jni->env->CallVoidMethod(renderer, m.render_model, entity, pose.limb_swing, pose.limb_swing_amount, pose.age,
                                 pose.head_yaw, pose.pitch, 0.0625f);
        J::clear_exception(jni);
        restore_combiner(lightmap);
    }
    glPopMatrix();

    set_enabled(GL_CULL_FACE, cull);
    set_enabled(GL_DEPTH_TEST, depth);
    set_enabled(GL_RESCALE_NORMAL, rescale);
    set_enabled(GL_ALPHA_TEST, alpha_test);
    set_enabled(GL_POLYGON_OFFSET_FILL, offset_fill);
    glPolygonOffset(offset_factor, offset_units);
    R::drop(jni, model);
}

} // namespace

bool adjust_chams_render_living(
    events::JniContext* jni,
    jobject renderer,
    jobject entity,
    double x,
    double y,
    double z,
    float entity_yaw,
    float partial_ticks) noexcept
{
    if (!events::module_is_running(get_adjust_chams_module()) || !R::available(jni) || entity == nullptr ||
        renderer == nullptr) {
        return false;
    }
    TargetSettings* target = classify(jni, entity);
    if (target == nullptr || !target->enabled.value) {
        return false;
    }

    if (target->mode_value() == ChamsMode::Normal) {
        // GlStateManager.enablePolygonOffset(); doPolygonOffset(1.0F, -1100000.0F); render model + layers.
        // Adjust then disables it and sets doPolygonOffset(1, 1100000); the previous raw values are restored instead
        // so GlStateManager's cached polygon offset keeps matching the GL state.
        const bool offset_fill = glIsEnabled(GL_POLYGON_OFFSET_FILL) != GL_FALSE;
        GLfloat offset_factor = 0.0f;
        GLfloat offset_units = 0.0f;
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &offset_factor);
        glGetFloatv(GL_POLYGON_OFFSET_UNITS, &offset_units);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, -1100000.0f);
        minecraft::renderer_living_do_render_super(jni, renderer, entity, x, y, z, entity_yaw, partial_ticks);
        J::clear_exception(jni);
        glPolygonOffset(offset_factor, offset_units);
        set_enabled(GL_POLYGON_OFFSET_FILL, offset_fill);
        return true;
    }

    // Colored: occluded colour with depth testing off, then the model in the visible colour. The vanilla doRender
    // supplies the layers (armour, held item, cape) and the name tag; the coloured model is drawn over its textured
    // body (same depth), which matches Adjust where the layers are drawn after the coloured model.
    if (target->through_walls.value) {
        render_colored_model(jni, renderer, entity, x, y, z, partial_ticks, target->through_walls_color.get(),
                             ColoredPass::ThroughWalls);
    }
    minecraft::renderer_living_do_render_super(jni, renderer, entity, x, y, z, entity_yaw, partial_ticks);
    J::clear_exception(jni);
    render_colored_model(jni, renderer, entity, x, y, z, partial_ticks, target->color.get(), ColoredPass::Visible);
    return true;
}

events::Module* get_adjust_chams_module() noexcept {
    static events::Module s_module = [] {
        events::Module mod = create_module();
        events::set_module_config_key(&mod, "Adjust Chams");
        return mod;
    }();
    return &s_module;
}

} // namespace slinky::modules
