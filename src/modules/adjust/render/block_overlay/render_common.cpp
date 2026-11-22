#include "modules/adjust/render/block_overlay/render_common.hpp"

#include <algorithm>
#include <cmath>
#include <glad/glad.h>

#include "adjust/jni_lookup.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"

namespace slinky::modules::adjust_world {

namespace ajni = adjust::jni;

// =============================================================================================================
// MathHelper
// =============================================================================================================

namespace {

float table_sin(int index) noexcept {
    // MathHelper.SIN_TABLE[i] = (float) Math.sin(i * Math.PI * 2.0 / 65536.0)
    return static_cast<float>(std::sin(static_cast<double>(index) * math::kPi * 2.0 / 65536.0));
}

} // namespace

float mh_sin(float value) noexcept {
    return table_sin(static_cast<int>(value * 10430.378f) & 65535);
}

float mh_cos(float value) noexcept {
    return table_sin(static_cast<int>(value * 10430.378f + 16384.0f) & 65535);
}

int mh_floor(double value) noexcept {
    const int i = static_cast<int>(value);
    return value < static_cast<double>(i) ? i - 1 : i;
}

float wrap_degrees(float value) noexcept {
    value = std::fmod(value, 360.0f);
    if (value >= 180.0f) {
        value -= 360.0f;
    }
    if (value < -180.0f) {
        value += 360.0f;
    }
    return value;
}

const int32_t kFacingOffsets[6][3] = {
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
};

math::AABB box_offset(const math::AABB& b, double dx, double dy, double dz) noexcept {
    return math::AABB{{b.min.x + dx, b.min.y + dy, b.min.z + dz}, {b.max.x + dx, b.max.y + dy, b.max.z + dz}};
}

math::AABB box_union(const math::AABB& a, const math::AABB& b) noexcept {
    return math::AABB{{std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
                      {std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}};
}

math::AABB box_expand(const math::AABB& b, double x, double y, double z) noexcept {
    return math::AABB{{b.min.x - x, b.min.y - y, b.min.z - z}, {b.max.x + x, b.max.y + y, b.max.z + z}};
}

math::AABB box_add_coord(const math::AABB& b, double x, double y, double z) noexcept {
    math::AABB r = b;
    if (x < 0.0) r.min.x += x; else if (x > 0.0) r.max.x += x;
    if (y < 0.0) r.min.y += y; else if (y > 0.0) r.max.y += y;
    if (z < 0.0) r.min.z += z; else if (z > 0.0) r.max.z += z;
    return r;
}

namespace {

// Vec3.getIntermediateWith{X,Y,Z}Value
bool intermediate(const math::Vec3d& a, const math::Vec3d& b, int axis, double value, math::Vec3d* out) noexcept {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double dz = b.z - a.z;
    const double d = axis == 0 ? dx : (axis == 1 ? dy : dz);
    if (d * d < 1.0000000116860974E-7) {
        return false;
    }
    const double base = axis == 0 ? a.x : (axis == 1 ? a.y : a.z);
    const double t = (value - base) / d;
    if (t < 0.0 || t > 1.0) {
        return false;
    }
    *out = math::Vec3d{a.x + dx * t, a.y + dy * t, a.z + dz * t};
    return true;
}

double dist_sq(const math::Vec3d& a, const math::Vec3d& b) noexcept {
    const double x = b.x - a.x;
    const double y = b.y - a.y;
    const double z = b.z - a.z;
    return x * x + y * y + z * z;
}

} // namespace

bool box_calculate_intercept(const math::AABB& box, const math::Vec3d& a, const math::Vec3d& b,
                             math::Vec3d* out_hit, int32_t* out_side) noexcept {
    // Candidate order and side mapping exactly like AxisAlignedBB.calculateIntercept.
    struct Candidate {
        int axis;
        double value;
        int32_t side;
    };
    const Candidate candidates[6] = {
        {0, box.min.x, kWest}, {0, box.max.x, kEast}, {1, box.min.y, kDown},
        {1, box.max.y, kUp},   {2, box.min.z, kNorth}, {2, box.max.z, kSouth},
    };
    bool found = false;
    math::Vec3d best{};
    int32_t best_side = 0;
    for (const Candidate& c : candidates) {
        math::Vec3d v{};
        if (!intermediate(a, b, c.axis, c.value, &v)) {
            continue;
        }
        bool inside = false;
        if (c.axis == 0) {
            inside = v.y >= box.min.y && v.y <= box.max.y && v.z >= box.min.z && v.z <= box.max.z;
        } else if (c.axis == 1) {
            inside = v.x >= box.min.x && v.x <= box.max.x && v.z >= box.min.z && v.z <= box.max.z;
        } else {
            inside = v.x >= box.min.x && v.x <= box.max.x && v.y >= box.min.y && v.y <= box.max.y;
        }
        if (!inside) {
            continue;
        }
        if (!found || dist_sq(a, v) < dist_sq(a, best)) {
            best = v;
            best_side = c.side;
            found = true;
        }
    }
    if (found) {
        *out_hit = best;
        *out_side = best_side;
    }
    return found;
}

// =============================================================================================================
// GL
// =============================================================================================================

void gl_color(adjust::Argb argb) noexcept {
    glColor4f(static_cast<float>((argb >> 16) & 255u) / 255.0f, static_cast<float>((argb >> 8) & 255u) / 255.0f,
              static_cast<float>(argb & 255u) / 255.0f, static_cast<float>(argb >> 24) / 255.0f);
}

void begin3d() noexcept {
    glPushMatrix();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_LINE_SMOOTH);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glLineWidth(0.4f);
}

void end3d() noexcept {
    glDepthMask(GL_TRUE);
    glPopMatrix();
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    // World-pass state (see header).
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
}

void begin_lines() noexcept {
    glPushMatrix();
    glEnable(GL_BLEND);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glDisable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
}

void end_lines() noexcept {
    glPopMatrix();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glEnable(GL_LINE_SMOOTH);
}

void draw_filled_box(const math::AABB& box) noexcept {
    const double x1 = box.min.x + 0.0019;
    const double y1 = box.min.y + 0.0019;
    const double z1 = box.min.z + 0.0019;
    const double x2 = box.max.x - 0.0019;
    const double y2 = box.max.y - 0.0019;
    const double z2 = box.max.z - 0.0019;
    glBegin(GL_QUAD_STRIP);
    glVertex3d(x1, y1, z1); glVertex3d(x1, y2, z1);
    glVertex3d(x2, y1, z1); glVertex3d(x2, y2, z1);
    glVertex3d(x2, y1, z2); glVertex3d(x2, y2, z2);
    glVertex3d(x1, y1, z2); glVertex3d(x1, y2, z2);
    glVertex3d(x1, y1, z1); glVertex3d(x1, y2, z1);
    glEnd();
    glBegin(GL_QUADS);
    glVertex3d(x1, y1, z1); glVertex3d(x2, y1, z1);
    glVertex3d(x2, y1, z2); glVertex3d(x1, y1, z2);
    glEnd();
    glCullFace(GL_FRONT);
    glBegin(GL_QUADS);
    glVertex3d(x1, y2, z1); glVertex3d(x2, y2, z1);
    glVertex3d(x2, y2, z2); glVertex3d(x1, y2, z2);
    glEnd();
    glCullFace(GL_BACK);
}

void draw_outlined_box(const math::AABB& box) noexcept {
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    for (const double y : {box.min.y, box.max.y}) {
        glBegin(GL_LINE_STRIP);
        glVertex3d(box.min.x, y, box.min.z);
        glVertex3d(box.max.x, y, box.min.z);
        glVertex3d(box.max.x, y, box.max.z);
        glVertex3d(box.min.x, y, box.max.z);
        glVertex3d(box.min.x, y, box.min.z);
        glEnd();
    }
    glBegin(GL_LINES);
    glVertex3d(box.min.x, box.min.y, box.min.z); glVertex3d(box.min.x, box.max.y, box.min.z);
    glVertex3d(box.max.x, box.min.y, box.min.z); glVertex3d(box.max.x, box.max.y, box.min.z);
    glVertex3d(box.max.x, box.min.y, box.max.z); glVertex3d(box.max.x, box.max.y, box.max.z);
    glVertex3d(box.min.x, box.min.y, box.max.z); glVertex3d(box.min.x, box.max.y, box.max.z);
    glEnd();
    // Adjust disables line smoothing here; the world pass keeps it enabled (restored by end3d / end_lines).
}

void draw_quad_2d(float x1, float y1, float x2, float y2) noexcept {
    const float right = std::max(x1, x2);
    const float left = std::min(x1, x2);
    const float bottom = std::max(y1, y2);
    const float top = std::min(y1, y2);
    glBegin(GL_QUADS);
    glVertex3f(right, top, 0.0f);
    glVertex3f(left, top, 0.0f);
    glVertex3f(left, bottom, 0.0f);
    glVertex3f(right, bottom, 0.0f);
    glEnd();
}

void draw_rect_outline_2d(float x1, float y1, float x2, float y2, float thickness) noexcept {
    draw_quad_2d(x1, y1, x2, y1 - thickness);
    draw_quad_2d(x1, y1 - thickness, x1 - thickness, y2 + thickness);
    draw_quad_2d(x1, y2, x2 + thickness, y2 + thickness);
    draw_quad_2d(x2, y1 - thickness, x2 + thickness, y2);
}

// =============================================================================================================
// JNI extras
// =============================================================================================================

const Classes* classes(events::JniContext* jni) noexcept {
    static Classes s_classes{};
    static int s_state = 0; // 0 = unresolved, 1 = ok, -1 = failed
    if (s_state == 0) {
        if (!ajni::is_1_8(jni)) {
            s_state = -1;
            return nullptr;
        }
        Classes c{};
        c.entity = ajni::find_class(jni, "pk", "net/minecraft/entity/Entity");
        c.living = ajni::find_class(jni, "pr", "net/minecraft/entity/EntityLivingBase");
        c.entity_living = ajni::find_class(jni, "ps", "net/minecraft/entity/EntityLiving");
        c.player = ajni::find_class(jni, "wn", "net/minecraft/entity/player/EntityPlayer");
        c.throwable = ajni::find_class(jni, "wy", "net/minecraft/entity/projectile/EntityThrowable");
        c.arrow = ajni::find_class(jni, "wq", "net/minecraft/entity/projectile/EntityArrow");
        c.fireball = ajni::find_class(jni, "ws", "net/minecraft/entity/projectile/EntityFireball");
        c.snowball = ajni::find_class(jni, "wx", "net/minecraft/entity/projectile/EntitySnowball");
        c.egg = ajni::find_class(jni, "wz", "net/minecraft/entity/projectile/EntityEgg");
        c.ender_pearl = ajni::find_class(jni, "xa", "net/minecraft/entity/item/EntityEnderPearl");
        c.potion = ajni::find_class(jni, "xc", "net/minecraft/entity/projectile/EntityPotion");
        c.mob = ajni::find_class(jni, "vv", "net/minecraft/entity/monster/EntityMob");
        c.iron_golem = ajni::find_class(jni, "ty", "net/minecraft/entity/monster/EntityIronGolem");
        c.armor_stand = ajni::find_class(jni, "um", "net/minecraft/entity/item/EntityArmorStand");
        c.item = ajni::find_class(jni, "uz", "net/minecraft/entity/item/EntityItem");
        c.tnt = ajni::find_class(jni, "vj", "net/minecraft/entity/item/EntityTNTPrimed");
        c.block = ajni::find_class(jni, "afh", "net/minecraft/block/Block");
        c.item_cls = ajni::find_class(jni, "zw", "net/minecraft/item/Item");
        c.item_stack = ajni::find_class(jni, "zx", "net/minecraft/item/ItemStack");
        const jclass all[] = {c.entity, c.living, c.entity_living, c.player, c.throwable, c.arrow, c.fireball,
                              c.snowball, c.egg, c.ender_pearl, c.potion, c.mob, c.iron_golem, c.armor_stand,
                              c.item, c.tnt, c.block, c.item_cls, c.item_stack};
        bool ok = true;
        for (const jclass cls : all) {
            ok = ok && cls != nullptr;
        }
        s_classes = c;
        s_state = ok ? 1 : -1;
    }
    return s_state == 1 ? &s_classes : nullptr;
}

bool is_instance(events::JniContext* jni, jobject obj, jclass cls) noexcept {
    return obj != nullptr && cls != nullptr && jni->env->IsInstanceOf(obj, cls) != JNI_FALSE;
}

namespace {

jclass entity_class(events::JniContext* jni) noexcept {
    static const jclass cls = ajni::find_class(jni, "pk", "net/minecraft/entity/Entity");
    return cls;
}

} // namespace

math::Vec3d entity_last_tick_pos(events::JniContext* jni, jobject entity) noexcept {
    static const jfieldID fx = ajni::field(jni, entity_class(jni), {"P", "lastTickPosX", "field_70142_S"}, "D", "D");
    static const jfieldID fy = ajni::field(jni, entity_class(jni), {"Q", "lastTickPosY", "field_70137_T"}, "D", "D");
    static const jfieldID fz = ajni::field(jni, entity_class(jni), {"R", "lastTickPosZ", "field_70136_U"}, "D", "D");
    if (fx == nullptr || fy == nullptr || fz == nullptr) {
        return minecraft::entity_get_prev_position(jni, entity);
    }
    JNIEnv* env = jni->env;
    return math::Vec3d{env->GetDoubleField(entity, fx), env->GetDoubleField(entity, fy), env->GetDoubleField(entity, fz)};
}

math::Vec3d entity_server_pos(events::JniContext* jni, jobject entity) noexcept {
    static const jfieldID fx = ajni::field(jni, entity_class(jni), {"bW", "serverPosX", "field_70118_ct"}, "I", "I");
    static const jfieldID fy = ajni::field(jni, entity_class(jni), {"bX", "serverPosY", "field_70117_cu"}, "I", "I");
    static const jfieldID fz = ajni::field(jni, entity_class(jni), {"bY", "serverPosZ", "field_70116_cv"}, "I", "I");
    if (fx == nullptr || fy == nullptr || fz == nullptr) {
        return math::vec3_scale(minecraft::entity_get_position(jni, entity), 32.0);
    }
    JNIEnv* env = jni->env;
    return math::Vec3d{static_cast<double>(env->GetIntField(entity, fx)),
                       static_cast<double>(env->GetIntField(entity, fy)),
                       static_cast<double>(env->GetIntField(entity, fz))};
}

float entity_width(events::JniContext* jni, jobject entity) noexcept {
    static const jfieldID f = ajni::field(jni, entity_class(jni), {"J", "width", "field_70130_N"}, "F", "F");
    return f != nullptr ? jni->env->GetFloatField(entity, f) : 0.6f;
}

float entity_height(events::JniContext* jni, jobject entity) noexcept {
    static const jfieldID f = ajni::field(jni, entity_class(jni), {"K", "height", "field_70131_O"}, "F", "F");
    return f != nullptr ? jni->env->GetFloatField(entity, f) : 1.8f;
}

float entity_eye_height(events::JniContext* jni, jobject entity) noexcept {
    static const jmethodID m = ajni::method(jni, entity_class(jni), {"aS", "getEyeHeight", "func_70047_e"}, "()F", "()F");
    if (m == nullptr) {
        return entity_height(jni, entity) * 0.85f;
    }
    const float v = jni->env->CallFloatMethod(entity, m);
    ajni::clear_exception(jni);
    return v;
}

bool entity_is_invisible(events::JniContext* jni, jobject entity) noexcept {
    static const jmethodID m = ajni::method(jni, entity_class(jni), {"ax", "isInvisible", "func_82150_aj"}, "()Z", "()Z");
    if (m == nullptr) {
        return false;
    }
    const bool v = jni->env->CallBooleanMethod(entity, m) != JNI_FALSE;
    ajni::clear_exception(jni);
    return v;
}

bool entity_is_immune_to_explosions(events::JniContext* jni, jobject entity) noexcept {
    static const jmethodID m =
        ajni::method(jni, entity_class(jni), {"aW", "isImmuneToExplosions", "func_180427_aV"}, "()Z", "()Z");
    if (m == nullptr) {
        return false;
    }
    const bool v = jni->env->CallBooleanMethod(entity, m) != JNI_FALSE;
    ajni::clear_exception(jni);
    return v;
}

math::AABB entity_box(events::JniContext* jni, jobject entity) noexcept {
    jobject box = minecraft::entity_get_bounding_box(jni, entity);
    if (box == nullptr) {
        const math::Vec3d p = minecraft::entity_get_position(jni, entity);
        return math::AABB{p, p};
    }
    const math::AABB r = minecraft::aabb_to_aabb(jni, box);
    jni->env->DeleteLocalRef(box);
    return r;
}

namespace {

struct ProjectileFields {
    jfieldID throwable_ticks = nullptr;
    jfieldID throwable_ground = nullptr;
    jfieldID arrow_ticks = nullptr;
    jfieldID arrow_shooter = nullptr;
    jfieldID fireball_ticks = nullptr;
    jfieldID fireball_ground = nullptr;
    jfieldID fireball_ax = nullptr;
    jfieldID fireball_ay = nullptr;
    jfieldID fireball_az = nullptr;
};

const ProjectileFields& projectile_fields(events::JniContext* jni) noexcept {
    static ProjectileFields s{};
    static bool s_resolved = false;
    if (!s_resolved) {
        s_resolved = true;
        const Classes* c = classes(jni);
        if (c != nullptr) {
            s.throwable_ticks = ajni::field(jni, c->throwable, {"ar", "ticksInAir", "field_70195_i"}, "I", "I");
            s.throwable_ground = ajni::field(jni, c->throwable, {"a", "inGround", "field_174854_a"}, "Z", "Z");
            s.arrow_ticks = ajni::field(jni, c->arrow, {"as", "ticksInAir", "field_70257_an"}, "I", "I");
            s.arrow_shooter = ajni::field(jni, c->arrow, {"c", "shootingEntity", "field_70250_c"}, "Lpk;",
                                          "Lnet/minecraft/entity/Entity;");
            s.fireball_ticks = ajni::field(jni, c->fireball, {"as", "ticksInAir", "field_70234_an"}, "I", "I");
            s.fireball_ground = ajni::field(jni, c->fireball, {"i", "inGround", "field_70238_i"}, "Z", "Z");
            s.fireball_ax = ajni::field(jni, c->fireball, {"b", "accelerationX", "field_70232_b"}, "D", "D");
            s.fireball_ay = ajni::field(jni, c->fireball, {"c", "accelerationY", "field_70233_c"}, "D", "D");
            s.fireball_az = ajni::field(jni, c->fireball, {"d", "accelerationZ", "field_70230_d"}, "D", "D");
        }
    }
    return s;
}

} // namespace

int32_t projectile_ticks_in_air(events::JniContext* jni, jobject entity) noexcept {
    const Classes* c = classes(jni);
    if (c == nullptr) {
        return 0;
    }
    const ProjectileFields& f = projectile_fields(jni);
    jfieldID fid = nullptr;
    if (is_instance(jni, entity, c->throwable)) fid = f.throwable_ticks;
    else if (is_instance(jni, entity, c->arrow)) fid = f.arrow_ticks;
    else if (is_instance(jni, entity, c->fireball)) fid = f.fireball_ticks;
    return fid != nullptr ? jni->env->GetIntField(entity, fid) : 0;
}

bool projectile_in_ground(events::JniContext* jni, jobject entity) noexcept {
    const Classes* c = classes(jni);
    if (c == nullptr) {
        return false;
    }
    const ProjectileFields& f = projectile_fields(jni);
    if (is_instance(jni, entity, c->arrow)) {
        return minecraft::entity_arrow_is_in_ground(jni, entity);
    }
    jfieldID fid = nullptr;
    if (is_instance(jni, entity, c->throwable)) fid = f.throwable_ground;
    else if (is_instance(jni, entity, c->fireball)) fid = f.fireball_ground;
    return fid != nullptr && jni->env->GetBooleanField(entity, fid) != JNI_FALSE;
}

math::Vec3d fireball_acceleration(events::JniContext* jni, jobject fireball) noexcept {
    const ProjectileFields& f = projectile_fields(jni);
    if (f.fireball_ax == nullptr || f.fireball_ay == nullptr || f.fireball_az == nullptr) {
        return math::Vec3d{};
    }
    JNIEnv* env = jni->env;
    return math::Vec3d{env->GetDoubleField(fireball, f.fireball_ax), env->GetDoubleField(fireball, f.fireball_ay),
                       env->GetDoubleField(fireball, f.fireball_az)};
}

jobject arrow_shooting_entity(events::JniContext* jni, jobject arrow) noexcept {
    const ProjectileFields& f = projectile_fields(jni);
    return f.arrow_shooter != nullptr ? jni->env->GetObjectField(arrow, f.arrow_shooter) : nullptr;
}

int32_t potion_damage(events::JniContext* jni, jobject potion) noexcept {
    const Classes* c = classes(jni);
    if (c == nullptr) {
        return 0;
    }
    static const jmethodID m = ajni::method(jni, c->potion, {"o", "getPotionDamage", "func_70196_i"}, "()I", "()I");
    if (m == nullptr) {
        return 0;
    }
    const int32_t v = jni->env->CallIntMethod(potion, m);
    ajni::clear_exception(jni);
    return v;
}

jobject player_held_stack(events::JniContext* jni, jobject player) noexcept {
    jobject inv = minecraft::player_get_inventory(jni, player);
    if (inv == nullptr) {
        return nullptr;
    }
    const int32_t slot = minecraft::inv_player_get_current_item(jni, inv);
    jobject stack = nullptr;
    if (slot >= 0 && slot < 9) {
        jobjectArray main_inv = minecraft::inv_player_get_main_inventory(jni, inv);
        if (main_inv != nullptr) {
            stack = jni->env->GetObjectArrayElement(main_inv, slot);
            jni->env->DeleteLocalRef(main_inv);
        }
    }
    jni->env->DeleteLocalRef(inv);
    return stack;
}

// ---- blocks --------------------------------------------------------------------------------------------------

jobject block_state_at(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z) noexcept {
    if (jni->mappings->mid_world_get_block_state_1_8 == nullptr || world == nullptr) {
        return nullptr;
    }
    jobject pos = minecraft::block_pos_new(jni, x, y, z);
    if (pos == nullptr) {
        ajni::clear_exception(jni);
        return nullptr;
    }
    jobject state = jni->env->CallObjectMethod(world, jni->mappings->mid_world_get_block_state_1_8, pos);
    ajni::clear_exception(jni);
    jni->env->DeleteLocalRef(pos);
    return state;
}

namespace {

jmethodID meta_from_state_method(events::JniContext* jni) noexcept {
    const Classes* c = classes(jni);
    if (c == nullptr) {
        return nullptr;
    }
    static const jmethodID m = ajni::method(jni, c->block, {"c", "getMetaFromState", "func_176201_c"}, "(Lalz;)I",
                                            "(Lnet/minecraft/block/state/IBlockState;)I");
    return m;
}

} // namespace

bool block_info_at(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z, BlockInfo* out) noexcept {
    const jmethodID meta_mid = meta_from_state_method(jni);
    if (meta_mid == nullptr) {
        return false;
    }
    jobject state = block_state_at(jni, world, x, y, z);
    if (state == nullptr) {
        return false;
    }
    jobject block = minecraft::iblock_state_get_block(jni, state);
    bool ok = false;
    if (block != nullptr) {
        out->id = minecraft::block_get_id(jni, block);
        out->meta = jni->env->CallIntMethod(block, meta_mid, state);
        ok = !ajni::clear_exception(jni);
        jni->env->DeleteLocalRef(block);
    }
    jni->env->DeleteLocalRef(state);
    return ok;
}

bool block_selected_box(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z,
                        math::AABB* out) noexcept {
    const Classes* c = classes(jni);
    if (c == nullptr) {
        return false;
    }
    static const jmethodID bounds_mid =
        ajni::method(jni, c->block, {"a", "setBlockBoundsBasedOnState", "func_180654_a"}, "(Ladq;Lcj;)V",
                     "(Lnet/minecraft/world/IBlockAccess;Lnet/minecraft/util/BlockPos;)V");
    static const jmethodID selected_mid =
        ajni::method(jni, c->block, {"b", "getSelectedBoundingBox", "func_180646_a"}, "(Ladm;Lcj;)Laug;",
                     "(Lnet/minecraft/world/World;Lnet/minecraft/util/BlockPos;)Lnet/minecraft/util/AxisAlignedBB;");
    if (selected_mid == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(8) != 0) {
        ajni::clear_exception(jni);
        return false;
    }
    bool ok = false;
    jobject pos = minecraft::block_pos_new(jni, x, y, z);
    jobject state = pos != nullptr ? env->CallObjectMethod(world, jni->mappings->mid_world_get_block_state_1_8, pos)
                                   : nullptr;
    ajni::clear_exception(jni);
    jobject block = state != nullptr ? minecraft::iblock_state_get_block(jni, state) : nullptr;
    if (block != nullptr) {
        if (bounds_mid != nullptr) {
            env->CallVoidMethod(block, bounds_mid, world, pos);
            ajni::clear_exception(jni);
        }
        jobject box = env->CallObjectMethod(block, selected_mid, world, pos);
        if (!ajni::clear_exception(jni) && box != nullptr) {
            *out = minecraft::aabb_to_aabb(jni, box);
            ok = true;
        }
    }
    env->PopLocalFrame(nullptr);
    return ok;
}

namespace {

jobject block_by_id(events::JniContext* jni, int32_t block_id) noexcept {
    const Classes* c = classes(jni);
    if (c == nullptr) {
        return nullptr;
    }
    static const jmethodID m = ajni::static_method(jni, c->block, {"c", "getBlockById", "func_149729_e"}, "(I)Lafh;",
                                                   "(I)Lnet/minecraft/block/Block;");
    if (m == nullptr) {
        return nullptr;
    }
    jobject b = jni->env->CallStaticObjectMethod(c->block, m, block_id);
    ajni::clear_exception(jni);
    return b;
}

} // namespace

bool block_is_full_block(events::JniContext* jni, int32_t block_id) noexcept {
    static int8_t s_cache[4096]{}; // 0 = unknown, 1 = no, 2 = yes
    if (block_id < 0 || block_id >= 4096) {
        return false;
    }
    if (s_cache[block_id] == 0) {
        const Classes* c = classes(jni);
        static const jmethodID m = c != nullptr
            ? ajni::method(jni, c->block, {"o", "isFullBlock", "func_149730_j"}, "()Z", "()Z")
            : nullptr;
        jobject block = block_by_id(jni, block_id);
        bool full = false;
        if (block != nullptr && m != nullptr) {
            full = jni->env->CallBooleanMethod(block, m) != JNI_FALSE;
            ajni::clear_exception(jni);
        }
        if (block != nullptr) {
            jni->env->DeleteLocalRef(block);
        }
        s_cache[block_id] = full ? 2 : 1;
    }
    return s_cache[block_id] == 2;
}

int32_t block_damage_dropped(events::JniContext* jni, int32_t block_id, int32_t meta) noexcept {
    static int16_t s_cache[4096 * 16]{}; // value + 1, 0 = unknown
    if (block_id < 0 || block_id >= 4096) {
        return 0;
    }
    const int32_t key = (block_id << 4) | (meta & 15);
    if (s_cache[key] == 0) {
        const Classes* c = classes(jni);
        static const jmethodID dropped = c != nullptr
            ? ajni::method(jni, c->block, {"a", "damageDropped", "func_180651_a"}, "(Lalz;)I",
                           "(Lnet/minecraft/block/state/IBlockState;)I")
            : nullptr;
        static const jmethodID from_meta = c != nullptr
            ? ajni::method(jni, c->block, {"a", "getStateFromMeta", "func_176203_a"}, "(I)Lalz;",
                           "(I)Lnet/minecraft/block/state/IBlockState;")
            : nullptr;
        int32_t value = 0;
        jobject block = block_by_id(jni, block_id);
        if (block != nullptr && dropped != nullptr && from_meta != nullptr) {
            jobject state = jni->env->CallObjectMethod(block, from_meta, meta & 15);
            if (!ajni::clear_exception(jni) && state != nullptr) {
                value = jni->env->CallIntMethod(block, dropped, state);
                if (ajni::clear_exception(jni)) {
                    value = 0;
                }
                jni->env->DeleteLocalRef(state);
            }
        }
        if (block != nullptr) {
            jni->env->DeleteLocalRef(block);
        }
        s_cache[key] = static_cast<int16_t>(std::clamp(value, 0, 32000) + 1);
    }
    return s_cache[key] - 1;
}

float block_explosion_resistance(events::JniContext* jni, int32_t block_id) noexcept {
    static float s_cache[4096]{};
    static bool s_known[4096]{};
    if (block_id < 0 || block_id >= 4096) {
        return 0.0f;
    }
    if (!s_known[block_id]) {
        const Classes* c = classes(jni);
        static const jmethodID m = c != nullptr
            ? ajni::method(jni, c->block, {"a", "getExplosionResistance", "func_149638_a"}, "(Lpk;)F",
                           "(Lnet/minecraft/entity/Entity;)F")
            : nullptr;
        float value = 0.0f;
        jobject block = block_by_id(jni, block_id);
        if (block != nullptr && m != nullptr) {
            value = jni->env->CallFloatMethod(block, m, static_cast<jobject>(nullptr));
            if (ajni::clear_exception(jni)) {
                value = 0.0f;
            }
        }
        if (block != nullptr) {
            jni->env->DeleteLocalRef(block);
        }
        s_cache[block_id] = value;
        s_known[block_id] = true;
    }
    return s_cache[block_id];
}

bool block_is_passable(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z) noexcept {
    const Classes* c = classes(jni);
    if (c == nullptr) {
        return true;
    }
    static const jmethodID m = ajni::method(jni, c->block, {"b", "isPassable", "func_176205_b"}, "(Ladq;Lcj;)Z",
                                            "(Lnet/minecraft/world/IBlockAccess;Lnet/minecraft/util/BlockPos;)Z");
    if (m == nullptr) {
        return true;
    }
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(8) != 0) {
        ajni::clear_exception(jni);
        return true;
    }
    bool passable = true;
    jobject pos = minecraft::block_pos_new(jni, x, y, z);
    jobject state = pos != nullptr ? env->CallObjectMethod(world, jni->mappings->mid_world_get_block_state_1_8, pos)
                                   : nullptr;
    ajni::clear_exception(jni);
    jobject block = state != nullptr ? minecraft::iblock_state_get_block(jni, state) : nullptr;
    if (block != nullptr) {
        passable = env->CallBooleanMethod(block, m, world, pos) != JNI_FALSE;
        if (ajni::clear_exception(jni)) {
            passable = true;
        }
    }
    env->PopLocalFrame(nullptr);
    return passable;
}

// =============================================================================================================
// BlockIdTracker
// =============================================================================================================

namespace {

int64_t chunk_key(int32_t cx, int32_t cz) noexcept {
    return (static_cast<int64_t>(cx) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(cz));
}

} // namespace

BlockIdTracker::BlockIdTracker(std::initializer_list<int32_t> ids) noexcept : ids_(ids) {}

bool BlockIdTracker::wanted(int32_t id) const noexcept {
    return std::find(ids_.begin(), ids_.end(), id) != ids_.end();
}

void BlockIdTracker::clear() noexcept {
    chunks_.clear();
    changes_.clear();
    ++revision_;
}

void BlockIdTracker::collect(std::vector<TrackedBlock>* out) const {
    out->clear();
    for (const auto& [key, blocks] : chunks_) {
        out->insert(out->end(), blocks.begin(), blocks.end());
    }
}

void BlockIdTracker::take_changes(std::vector<TrackedBlock>* out) {
    out->swap(changes_);
    changes_.clear();
}

void BlockIdTracker::scan_chunk(events::JniContext* jni, jobject world, int32_t cx, int32_t cz) noexcept {
    JNIEnv* env = jni->env;
    std::vector<TrackedBlock>& list = chunks_[chunk_key(cx, cz)];
    list.clear();
    if (env->PushLocalFrame(48) != 0) {
        ajni::clear_exception(jni);
        return;
    }
    jobject chunk = minecraft::world_get_chunk_from_chunk_coords(jni, world, cx, cz);
    ajni::clear_exception(jni);
    jobjectArray storages = chunk != nullptr ? minecraft::chunk_get_storage_arrays(jni, chunk) : nullptr;
    if (storages != nullptr) {
        const jsize sections = std::min<jsize>(env->GetArrayLength(storages), 16);
        for (jsize sy = 0; sy < sections; ++sy) {
            jobject ebs = env->GetObjectArrayElement(storages, sy);
            if (ebs == nullptr) {
                continue;
            }
            auto data = static_cast<jarray>(minecraft::extended_block_storage_get_data(jni, ebs));
            if (data != nullptr && env->GetArrayLength(data) >= 4096) {
                // 1.8.9: char[4096] block states, index = y << 8 | z << 4 | x, id = state >> 4.
                auto* raw = static_cast<const uint16_t*>(env->GetPrimitiveArrayCritical(data, nullptr));
                if (raw != nullptr) {
                    for (int32_t i = 0; i < 4096; ++i) {
                        const int32_t id = raw[i] >> 4;
                        if (id != 0 && wanted(id)) {
                            list.push_back(TrackedBlock{(cx << 4) + (i & 15), (sy << 4) + (i >> 8),
                                                        (cz << 4) + ((i >> 4) & 15), id});
                        }
                    }
                    env->ReleasePrimitiveArrayCritical(data, const_cast<uint16_t*>(raw), JNI_ABORT);
                }
            }
            if (data != nullptr) {
                env->DeleteLocalRef(data);
            }
            env->DeleteLocalRef(ebs);
        }
    }
    env->PopLocalFrame(nullptr);
    if (list.empty()) {
        chunks_.erase(chunk_key(cx, cz));
    }
    ++revision_;
}

void BlockIdTracker::scan_all(events::GameContext* ctx) noexcept {
    chunks_.clear();
    ++revision_;
    if (ctx == nullptr || ctx->world == nullptr || !ajni::is_1_8(ctx->jni)) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(16) != 0) {
        ajni::clear_exception(jni);
        return;
    }
    jobject provider = minecraft::world_get_chunk_provider_client(jni, ctx->world);
    jobject listing = provider != nullptr ? minecraft::chunk_provider_get_chunk_listing(jni, provider) : nullptr;
    if (listing != nullptr) {
        const int32_t count = minecraft::java_list_size(jni, listing);
        for (int32_t i = 0; i < count; ++i) {
            jobject chunk = minecraft::java_list_get(jni, listing, i);
            if (chunk == nullptr) {
                continue;
            }
            const int32_t cx = minecraft::chunk_get_x(jni, chunk);
            const int32_t cz = minecraft::chunk_get_z(jni, chunk);
            env->DeleteLocalRef(chunk);
            scan_chunk(jni, ctx->world, cx, cz);
        }
    }
    ajni::clear_exception(jni);
    env->PopLocalFrame(nullptr);
}

void BlockIdTracker::on_world_block_update(const events::WorldBlockUpdateEvent* ev) noexcept {
    if (ev == nullptr || ev->jni == nullptr || !ajni::is_1_8(ev->jni)) {
        return;
    }
    if (ev->update_kind == 0 || ev->update_kind == 1) {
        chunks_.erase(chunk_key(ev->x, ev->y_or_cz));
        ++revision_;
        if (ev->update_kind == 1 && ev->world != nullptr) {
            scan_chunk(ev->jni, ev->world, ev->x, ev->y_or_cz);
        }
        return;
    }
    if (ev->update_kind != 2) {
        return;
    }
    const int32_t x = ev->x;
    const int32_t y = ev->y_or_cz;
    const int32_t z = ev->z;
    changes_.push_back(TrackedBlock{x, y, z, ev->block_id});
    if (changes_.size() > 4096) {
        changes_.erase(changes_.begin(), changes_.begin() + 2048);
    }
    const int64_t key = chunk_key(x >> 4, z >> 4);
    auto it = chunks_.find(key);
    if (it != chunks_.end()) {
        std::vector<TrackedBlock>& list = it->second;
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].x == x && list[i].y == y && list[i].z == z) {
                if (wanted(ev->block_id)) {
                    list[i].id = ev->block_id;
                } else {
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                    if (list.empty()) {
                        chunks_.erase(it);
                    }
                }
                ++revision_;
                return;
            }
        }
    }
    if (wanted(ev->block_id)) {
        chunks_[key].push_back(TrackedBlock{x, y, z, ev->block_id});
        ++revision_;
    }
}

} // namespace slinky::modules::adjust_world
