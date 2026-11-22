#include "modules/adjust/render/skeleton/adjust_skeleton.hpp"

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

// SkeletonTargetSettings: Enabled + Color (default 0x96F5F5F5).
struct TargetSettings {
    const char* name;
    std::string key_prefix;
    config::BoolSetting enabled;
    adjust::ColorOption color;

    TargetSettings(const char* label, const char* prefix) noexcept
        : name(label),
          key_prefix(prefix),
          enabled{{}, "Enabled", true, true, {}},
          color("Color", "Color mode", adjust::argb(0xF5, 0xF5, 0xF5, 0x96)) {}

    void render(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
        gui::widgets::render_section_header(gui, renderer, name);
        enabled.render(gui, renderer);
        if (enabled.value) {
            color.render(gui, renderer);
        }
    }

    void visit(config::ConfigArchive* ar) noexcept {
        enabled.visit(ar, (key_prefix + "_enabled").c_str());
        color.visit(ar, (key_prefix + "_color").c_str(), (key_prefix + "_color_mode").c_str());
    }
};

// SkeletonModule targets (the "Enemy" group needs Adjust's TARGET player tag, which OpenSlinky++ does not have).
TargetSettings s_players("Players", "players");
TargetSettings s_friendly("Friendly", "friendly");

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    s_players.render(gui, renderer);
    s_friendly.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    s_players.visit(ar);
    s_friendly.visit(ar);
}

events::Module create_module() noexcept {
    events::Module mod{};
    mod.name = "Skeleton";
    mod.description = "Draws the skeleton of players.";
    mod.category = events::ModuleCategory::Visual;
    mod.render_settings = &render_settings;
    mod.visit_config = &visit_config;
    mod.flags |= events::kModuleFlagStyleAdjust;
    return mod;
}

// SkeletonModule.updatePoses(): target settings for a player, nullptr = no skeleton.
TargetSettings* classify(events::JniContext* jni, jobject entity) noexcept {
    JNIEnv* env = jni->env;
    jobject mc = minecraft::mc_get_instance(jni);
    jobject local = minecraft::mc_get_player(jni, mc);
    const bool is_local = local != nullptr && env->IsSameObject(entity, local);
    R::drop(jni, local);
    TargetSettings* target = nullptr;
    if (is_local) {
        // The local player is only tracked in third person (mask 0x80) and resolves to the "Friendly" settings.
        jobject settings = minecraft::mc_get_game_settings(jni, mc);
        const bool third_person = settings != nullptr && minecraft::settings_get_third_person_view(jni, settings) != 0;
        R::drop(jni, settings);
        target = third_person ? &s_friendly : nullptr;
    } else {
        const uint16_t tags = minecraft::player_get_tag_flags(jni, entity);
        if ((tags & minecraft::kPlayerTagBot) == 0) {
            const bool teammate = (tags & (minecraft::kPlayerTagTeammate | minecraft::kPlayerTagFriend)) != 0;
            target = teammate ? &s_friendly : &s_players;
        }
    }
    R::drop(jni, mc);
    return (target != nullptr && target->enabled.value) ? target : nullptr;
}

struct LimbAngles {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct BipedPose {
    LimbAngles right_leg;
    LimbAngles left_leg;
    LimbAngles right_arm;
    LimbAngles left_arm;
    LimbAngles head;
    bool sneak = false; // ModelBiped.isSneak
};

bool read_pose(events::JniContext* jni, jobject model, BipedPose* out) noexcept {
    static const jclass cls_biped = J::find_class(jni, "bbj", "net/minecraft/client/model/ModelBiped");
    static const jclass cls_part = J::find_class(jni, "bct", "net/minecraft/client/model/ModelRenderer");
    static const jfieldID f_head = J::field(jni, cls_biped, {"e", "bipedHead", "field_78116_c"}, "Lbct;",
                                            "Lnet/minecraft/client/model/ModelRenderer;");
    static const jfieldID f_right_arm = J::field(jni, cls_biped, {"h", "bipedRightArm", "field_178723_h"}, "Lbct;",
                                                 "Lnet/minecraft/client/model/ModelRenderer;");
    static const jfieldID f_left_arm = J::field(jni, cls_biped, {"i", "bipedLeftArm", "field_178724_i"}, "Lbct;",
                                                "Lnet/minecraft/client/model/ModelRenderer;");
    static const jfieldID f_right_leg = J::field(jni, cls_biped, {"j", "bipedRightLeg", "field_178721_j"}, "Lbct;",
                                                 "Lnet/minecraft/client/model/ModelRenderer;");
    static const jfieldID f_left_leg = J::field(jni, cls_biped, {"k", "bipedLeftLeg", "field_178722_k"}, "Lbct;",
                                                "Lnet/minecraft/client/model/ModelRenderer;");
    static const jfieldID f_sneak = J::field(jni, cls_biped, {"n", "isSneak", "field_78117_n"}, "Z", "Z");
    static const jfieldID f_rx = J::field(jni, cls_part, {"f", "rotateAngleX", "field_78795_f"}, "F", "F");
    static const jfieldID f_ry = J::field(jni, cls_part, {"g", "rotateAngleY", "field_78796_g"}, "F", "F");
    static const jfieldID f_rz = J::field(jni, cls_part, {"h", "rotateAngleZ", "field_78808_h"}, "F", "F");
    if (cls_biped == nullptr || f_head == nullptr || f_right_arm == nullptr || f_left_arm == nullptr ||
        f_right_leg == nullptr || f_left_leg == nullptr || f_sneak == nullptr || f_rx == nullptr || f_ry == nullptr ||
        f_rz == nullptr || !jni->env->IsInstanceOf(model, cls_biped)) {
        return false;
    }
    JNIEnv* env = jni->env;
    const auto read = [&](jfieldID part_field, LimbAngles* angles) noexcept {
        jobject part = env->GetObjectField(model, part_field);
        if (part != nullptr) {
            angles->x = env->GetFloatField(part, f_rx);
            angles->y = env->GetFloatField(part, f_ry);
            angles->z = env->GetFloatField(part, f_rz);
            env->DeleteLocalRef(part);
        }
    };
    read(f_right_leg, &out->right_leg);
    read(f_left_leg, &out->left_leg);
    read(f_right_arm, &out->right_arm);
    read(f_left_arm, &out->left_arm);
    read(f_head, &out->head);
    out->sneak = env->GetBooleanField(model, f_sneak) != JNI_FALSE;
    return true;
}

void line(double x1, double y1, double z1, double x2, double y2, double z2) noexcept {
    glBegin(GL_LINE_STRIP);
    glVertex3d(x1, y1, z1);
    glVertex3d(x2, y2, z2);
    glEnd();
}

constexpr float kAngleToDegrees = 48.701412f; // Adjust's constant (not 180 / PI)

// GL state of SkeletonModule.beginSkeletons / endSkeletons, restored to the exact previous values.
struct SavedState {
    bool blend, line_smooth, depth, texture, lighting, lightmap;
    GLint blend_src, blend_dst, line_hint;
    GLfloat line_width;
    GLfloat color[4];
};

SavedState begin_skeletons() noexcept {
    SavedState s{};
    s.blend = glIsEnabled(GL_BLEND) != GL_FALSE;
    s.line_smooth = glIsEnabled(GL_LINE_SMOOTH) != GL_FALSE;
    s.depth = glIsEnabled(GL_DEPTH_TEST) != GL_FALSE;
    s.texture = glIsEnabled(GL_TEXTURE_2D) != GL_FALSE;
    s.lighting = glIsEnabled(GL_LIGHTING) != GL_FALSE;
    glGetIntegerv(GL_BLEND_SRC, &s.blend_src);
    glGetIntegerv(GL_BLEND_DST, &s.blend_dst);
    glGetIntegerv(GL_LINE_SMOOTH_HINT, &s.line_hint);
    glGetFloatv(GL_LINE_WIDTH, &s.line_width);
    glGetFloatv(GL_CURRENT_COLOR, s.color);
    glActiveTexture(GL_TEXTURE1);
    s.lightmap = glIsEnabled(GL_TEXTURE_2D) != GL_FALSE;
    glDisable(GL_TEXTURE_2D); // entityRenderer.disableLightmap()
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_BLEND);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return s;
}

void set_enabled(GLenum cap, bool enabled) noexcept {
    if (enabled) {
        glEnable(cap);
    } else {
        glDisable(cap);
    }
}

void end_skeletons(const SavedState& s) noexcept {
    set_enabled(GL_BLEND, s.blend);
    set_enabled(GL_LINE_SMOOTH, s.line_smooth);
    set_enabled(GL_DEPTH_TEST, s.depth);
    set_enabled(GL_TEXTURE_2D, s.texture);
    set_enabled(GL_LIGHTING, s.lighting);
    glBlendFunc(static_cast<GLenum>(s.blend_src), static_cast<GLenum>(s.blend_dst));
    glHint(GL_LINE_SMOOTH_HINT, static_cast<GLenum>(s.line_hint));
    glLineWidth(s.line_width);
    glColor4f(s.color[0], s.color[1], s.color[2], s.color[3]);
    glActiveTexture(GL_TEXTURE1);
    set_enabled(GL_TEXTURE_2D, s.lightmap);
    glActiveTexture(GL_TEXTURE0);
}

void rotate_limb(const LimbAngles& a, float z_sign) noexcept {
    if (a.x != 0.0f) {
        glRotatef(a.x * kAngleToDegrees, 1.0f, 0.0f, 0.0f);
    }
    if (a.y != 0.0f) {
        glRotatef(a.y * kAngleToDegrees, 0.0f, 1.0f, -1.0f);
    }
    if (a.z != 0.0f) {
        glRotatef(a.z * kAngleToDegrees, 0.0f, 0.0f, z_sign);
    }
}

// Port of SkeletonModule.drawSkeleton (without Adjust's ServerPositionTracker, which OpenSlinky++ does not have).
void draw_skeleton(events::JniContext* jni, jobject player, const BipedPose& model, adjust::Argb color,
                   const math::Vec3d& viewer, float pt) noexcept {
    const bool sneaking = model.sneak;
    glPushMatrix();
    glLineWidth(1.25f);
    glColor4f(static_cast<float>(adjust::red_of(color)) / 255.0f, static_cast<float>(adjust::green_of(color)) / 255.0f,
              static_cast<float>(adjust::blue_of(color)) / 255.0f, static_cast<float>(adjust::alpha_of(color)) / 255.0f);

    const math::Vec3d pos = minecraft::entity_get_position(jni, player);
    const math::Vec3d old = minecraft::entity_get_prev_position(jni, player);
    const float previous_yaw = R::living_prev_render_yaw_offset(jni, player);
    const float yaw = R::living_render_yaw_offset(jni, player);
    const double p = static_cast<double>(pt);
    glTranslated((pos.x - old.x) * p + old.x - viewer.x, (pos.y - old.y) * p + old.y - viewer.y + 0.0625,
                 (pos.z - old.z) * p + old.z - viewer.z);
    const float body_yaw = previous_yaw + (yaw - previous_yaw) * pt;
    glRotatef(-body_yaw, 0.0f, 1.0f, 0.0f);
    const int death_time = R::living_death_time(jni, player);
    if (death_time > 0) {
        float death = std::sqrt(((static_cast<float>(death_time) + pt - 1.0f) / 20.0f) * 1.6f);
        if (death > 1.0f) {
            death = 1.0f;
        }
        glRotatef(death * 90.0f, 0.0f, 0.0f, -1.0f);
    }
    const bool crouched = minecraft::entity_is_sneaking(jni, player);
    if (crouched) {
        glTranslated(0.0, 0.0, -0.235);
    }
    const double hips = crouched ? 0.6000000238418579 : 0.75;
    const double shoulders = crouched ? 1.1500000238418579 : 1.3;
    const double leg_length = crouched ? -0.550000011920929 : -0.699999988079071;

    const LimbAngles* legs[2] = {&model.right_leg, &model.left_leg};
    for (int side = 0; side < 2; ++side) {
        glPushMatrix();
        glTranslated(side == 0 ? -0.125 : 0.125, hips, 0.0);
        rotate_limb(*legs[side], 1.0f);
        line(0.0, 0.0, 0.0, 0.0, leg_length, 0.0);
        glPopMatrix();
    }

    const double arm_x = sneaking ? 0.325 : 0.35;
    if (crouched) {
        glTranslated(0.0, 0.0, 0.25);
    }
    glPushMatrix();
    if (crouched) {
        glTranslated(0.0, -0.05, -0.01725);
    }
    const LimbAngles* arms[2] = {&model.right_arm, &model.left_arm};
    for (int side = 0; side < 2; ++side) {
        glPushMatrix();
        glTranslated(side == 0 ? -arm_x : arm_x, shoulders, 0.0);
        rotate_limb(*arms[side], -1.0f);
        line(0.0, 0.0, 0.0, 0.0, -0.5, 0.0);
        glPopMatrix();
    }
    const float prev_head = R::living_prev_head_yaw(jni, player);
    const float head = R::living_head_yaw(jni, player);
    glRotatef(body_yaw - (prev_head + (head - prev_head) * pt), 0.0f, 1.0f, 0.0f);
    glPushMatrix();
    glTranslated(0.0, shoulders, 0.0);
    if (model.head.x != 0.0f) {
        glRotatef(model.head.x * kAngleToDegrees, 1.0f, 0.0f, 0.0f);
    }
    line(0.0, 0.0, 0.0, 0.0, 0.3, 0.0);
    glPopMatrix();
    glPopMatrix();

    if (crouched) {
        glRotatef(25.0f, 1.0f, 0.0f, 0.0f);
        glTranslated(0.0, -0.16175, -0.48025);
    }
    glPushMatrix();
    glTranslated(0.0, hips, 0.0);
    line(-0.125, 0.0, 0.0, 0.125, 0.0, 0.0);
    glPopMatrix();
    glPushMatrix();
    glTranslated(0.0, hips, 0.0);
    line(0.0, 0.0, 0.0, 0.0, 0.55, 0.0);
    glPopMatrix();
    glPushMatrix();
    glTranslated(0.0, shoulders, 0.0);
    line(-arm_x, 0.0, 0.0, arm_x, 0.0, 0.0);
    glPopMatrix();
    glPopMatrix();
}

} // namespace

void adjust_skeleton_render_player(
    events::JniContext* jni,
    jobject renderer,
    jobject entity,
    float partial_ticks) noexcept
{
    // RenderPlayer: SkeletonModule enabled && partialTicks != 1.0F (not the inventory preview) && pose present.
    if (!events::module_is_running(get_adjust_skeleton_module()) || partial_ticks == 1.0f || !R::available(jni) ||
        entity == nullptr || renderer == nullptr) {
        return;
    }
    if (!R::is_instance(jni, entity, R::Cls::EntityPlayer)) {
        return;
    }
    TargetSettings* target = classify(jni, entity);
    if (target == nullptr) {
        return;
    }
    jobject riding = minecraft::entity_get_riding_entity(jni, entity);
    const bool is_riding = riding != nullptr;
    R::drop(jni, riding);
    if (minecraft::player_is_sleeping(jni, entity) || minecraft::entity_is_dead(jni, entity) ||
        R::entity_is_invisible(jni, entity) || is_riding) {
        return;
    }

    jobject model = R::renderer_main_model(jni, renderer);
    if (model == nullptr) {
        return;
    }
    BipedPose pose{};
    const bool have_pose = read_pose(jni, model, &pose);
    R::drop(jni, model);
    if (!have_pose) {
        return;
    }

    jobject mc = minecraft::mc_get_instance(jni);
    jobject render_manager = minecraft::mc_get_render_manager(jni, mc);
    const math::Vec3d viewer = R::render_manager_viewer_pos(jni, render_manager);
    R::drop(jni, render_manager);
    R::drop(jni, mc);

    const SavedState saved = begin_skeletons();
    draw_skeleton(jni, entity, pose, target->color.get(), viewer, partial_ticks);
    end_skeletons(saved);
    J::clear_exception(jni);
}

events::Module* get_adjust_skeleton_module() noexcept {
    static events::Module s_module = [] {
        events::Module mod = create_module();
        events::set_module_config_key(&mod, "Adjust Skeleton");
        return mod;
    }();
    return &s_module;
}

} // namespace slinky::modules
