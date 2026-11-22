#include "modules/adjust/render/trajectories/projectile_sim.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "adjust/jni_lookup.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"

namespace slinky::modules::adjust_sim {

namespace ar = adjust_world;
namespace ajni = adjust::jni;

namespace {

constexpr float kPiF = static_cast<float>(math::kPi);

int64_t pos_key(int32_t x, int32_t y, int32_t z) noexcept {
    return (static_cast<int64_t>(x & 0x3FFFFFF) << 38) | (static_cast<int64_t>(z & 0x3FFFFFF) << 12) |
           static_cast<int64_t>(y & 0xFFF);
}

// AxisAlignedBB.intersectsWith (strict)
bool intersects(const math::AABB& a, const math::AABB& b) noexcept {
    return a.max.x > b.min.x && a.min.x < b.max.x && a.max.y > b.min.y && a.min.y < b.max.y && a.max.z > b.min.z &&
           a.min.z < b.max.z;
}

double sq_dist(const math::Vec3d& a, const math::Vec3d& b) noexcept {
    const double x = b.x - a.x;
    const double y = b.y - a.y;
    const double z = b.z - a.z;
    return x * x + y * y + z * z;
}

// Closest entity intercept along start -> end (shared by ProjectileSimulation and FireballSimulation).
template <typename Skip>
bool closest_entity(const SimWorld& world, const math::AABB& search, const math::Vec3d& start, const math::Vec3d& end,
                    float partial_ticks, Skip skip, SimHit* out) noexcept {
    bool found = false;
    double closest = 0.0;
    for (const SimEntity& e : world.entities()) {
        if (!intersects(e.box, search) || !e.collidable || skip(e)) {
            continue;
        }
        const double pt = static_cast<double>(partial_ticks);
        const math::AABB box = ar::box_expand(
            ar::box_offset(e.box, e.last.x + (e.pos.x - e.last.x) * pt - e.pos.x,
                           e.last.y + (e.pos.y - e.last.y) * pt - e.pos.y,
                           e.last.z + (e.pos.z - e.last.z) * pt - e.pos.z),
            0.300000011920929, 0.300000011920929, 0.300000011920929);
        math::Vec3d hit{};
        int32_t side = 0;
        if (!ar::box_calculate_intercept(box, start, end, &hit, &side)) {
            continue;
        }
        const double d = sq_dist(start, hit);
        if (d < closest || closest == 0.0) {
            closest = d;
            out->type = 2;
            out->vec = hit;
            out->side = side;
            out->entity_id = e.id;
            found = true;
        }
    }
    return found;
}

} // namespace

// =============================================================================================================
// SimWorld
// =============================================================================================================

void SimWorld::clear() noexcept {
    entities_.clear();
    index_.clear();
    blocks_.clear();
    passable_.clear();
}

void SimWorld::bind(events::JniContext* jni, jobject world) noexcept {
    jni_ = jni;
    world_ = world;
}

void SimWorld::capture(events::GameContext* ctx) noexcept {
    clear();
    bind(ctx->jni, ctx->world);
    if (ctx->world == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    jobject list = minecraft::world_get_loaded_entity_list(jni, ctx->world);
    if (list == nullptr) {
        return;
    }
    const int32_t count = minecraft::java_list_size(jni, list);
    entities_.reserve(static_cast<size_t>(std::max(count, 0)));
    for (int32_t i = 0; i < count; ++i) {
        if (env->PushLocalFrame(8) != 0) {
            ajni::clear_exception(jni);
            break;
        }
        jobject entity = minecraft::java_list_get(jni, list, i);
        if (!ajni::clear_exception(jni) && entity != nullptr) {
            SimEntity e{};
            e.id = minecraft::entity_get_id(jni, entity);
            e.collidable = minecraft::entity_can_be_collided_with(jni, entity);
            e.sneaking = minecraft::entity_is_sneaking(jni, entity);
            e.local = env->IsSameObject(entity, ctx->local_player) != JNI_FALSE;
            e.box = ar::entity_box(jni, entity);
            e.pos = minecraft::entity_get_position(jni, entity);
            e.last = ar::entity_last_tick_pos(jni, entity);
            ajni::clear_exception(jni);
            index_[e.id] = entities_.size();
            entities_.push_back(e);
        }
        env->PopLocalFrame(nullptr);
    }
    env->DeleteLocalRef(list);
}

const SimEntity* SimWorld::find(int32_t id) const noexcept {
    auto it = index_.find(id);
    return it != index_.end() ? &entities_[it->second] : nullptr;
}

ar::BlockInfo SimWorld::block(int32_t x, int32_t y, int32_t z) noexcept {
    const int64_t key = pos_key(x, y, z);
    auto it = blocks_.find(key);
    if (it != blocks_.end()) {
        return it->second;
    }
    ar::BlockInfo info{};
    if (jni_ == nullptr || world_ == nullptr || !ar::block_info_at(jni_, world_, x, y, z, &info)) {
        info = ar::BlockInfo{};
    }
    blocks_.emplace(key, info);
    return info;
}

bool SimWorld::passable(int32_t x, int32_t y, int32_t z) noexcept {
    const int64_t key = pos_key(x, y, z);
    auto it = passable_.find(key);
    if (it != passable_.end()) {
        return it->second;
    }
    const bool v = jni_ == nullptr || world_ == nullptr || ar::block_is_passable(jni_, world_, x, y, z);
    passable_.emplace(key, v);
    return v;
}

bool SimWorld::selected_box(int32_t x, int32_t y, int32_t z, math::AABB* out) noexcept {
    return jni_ != nullptr && world_ != nullptr && ar::block_selected_box(jni_, world_, x, y, z, out);
}

bool SimWorld::ray_trace(const math::Vec3d& start, const math::Vec3d& end, bool ignore_no_box, SimHit* out) noexcept {
    if (jni_ == nullptr || world_ == nullptr) {
        return false;
    }
    JNIEnv* env = jni_->env;
    if (env->PushLocalFrame(8) != 0) {
        ajni::clear_exception(jni_);
        return false;
    }
    bool hit = false;
    jobject a = minecraft::vec3_new(jni_, start);
    jobject b = minecraft::vec3_new(jni_, end);
    if (a != nullptr && b != nullptr) {
        jobject mop = minecraft::world_ray_trace_blocks(jni_, world_, a, b, false, ignore_no_box, false);
        if (!ajni::clear_exception(jni_) && mop != nullptr) {
            const int32_t type = minecraft::mop_get_type_ordinal(jni_, mop);
            if (type == 1) {
                jobject vec = minecraft::mop_get_hit_vec(jni_, mop);
                out->type = 1;
                out->vec = vec != nullptr ? minecraft::vec3_to_vec3d(jni_, vec) : end;
                out->side = minecraft::mop_get_side_hit(jni_, mop);
                const minecraft::Vec3i p = minecraft::mop_get_block_pos_vec3i(jni_, mop);
                out->bx = p.x;
                out->by = p.y;
                out->bz = p.z;
                out->entity_id = -1;
                hit = true;
            }
            ajni::clear_exception(jni_);
        }
    }
    ajni::clear_exception(jni_);
    env->PopLocalFrame(nullptr);
    return hit;
}

math::Vec3d SimWorld::liquid_flow(int32_t x, int32_t y, int32_t z, const math::Vec3d& flow) noexcept {
    const ar::Classes* c = jni_ != nullptr ? ar::classes(jni_) : nullptr;
    if (c == nullptr || world_ == nullptr) {
        return flow;
    }
    static const jmethodID m = ajni::method(
        jni_, c->block, {"a", "modifyAcceleration", "func_176197_a"}, "(Ladm;Lcj;Lpk;Laui;)Laui;",
        "(Lnet/minecraft/world/World;Lnet/minecraft/util/BlockPos;Lnet/minecraft/entity/Entity;"
        "Lnet/minecraft/util/Vec3;)Lnet/minecraft/util/Vec3;");
    if (m == nullptr) {
        return flow;
    }
    JNIEnv* env = jni_->env;
    if (env->PushLocalFrame(8) != 0) {
        ajni::clear_exception(jni_);
        return flow;
    }
    math::Vec3d result = flow;
    jobject state = ar::block_state_at(jni_, world_, x, y, z);
    jobject block = state != nullptr ? minecraft::iblock_state_get_block(jni_, state) : nullptr;
    jobject pos = minecraft::block_pos_new(jni_, x, y, z);
    jobject vec = minecraft::vec3_new(jni_, flow);
    if (block != nullptr && pos != nullptr && vec != nullptr) {
        // BlockLiquid ignores the entity argument.
        jobject out = env->CallObjectMethod(block, m, world_, pos, static_cast<jobject>(nullptr), vec);
        if (!ajni::clear_exception(jni_) && out != nullptr) {
            result = minecraft::vec3_to_vec3d(jni_, out);
        }
    }
    ajni::clear_exception(jni_);
    env->PopLocalFrame(nullptr);
    return result;
}

// =============================================================================================================
// SimBody (EntitySimulation)
// =============================================================================================================

void SimBody::reset_box() noexcept {
    const double half = static_cast<double>(width / 2.0f);
    box = math::AABB{{pos.x - half, pos.y, pos.z - half}, {pos.x + half, pos.y + height, pos.z + half}};
}

bool SimBody::water_check(SimWorld& world) noexcept {
    // boundingBox.expand(0, -0.4, 0).contract(0.001, 0.001, 0.001)
    const math::AABB b{{box.min.x + 0.001, box.min.y + 0.4000000059604645 + 0.001, box.min.z + 0.001},
                       {box.max.x - 0.001, box.max.y - 0.4000000059604645 - 0.001, box.max.z - 0.001}};
    const int32_t min_x = ar::mh_floor(b.min.x);
    const int32_t max_x = ar::mh_floor(b.max.x + 1.0);
    const int32_t min_y = ar::mh_floor(b.min.y);
    const int32_t max_y = ar::mh_floor(b.max.y + 1.0);
    const int32_t min_z = ar::mh_floor(b.min.z);
    const int32_t max_z = ar::mh_floor(b.max.z + 1.0);
    bool water = false;
    math::Vec3d flow{};
    for (int32_t x = min_x; x < max_x; ++x) {
        for (int32_t y = min_y; y < max_y; ++y) {
            for (int32_t z = min_z; z < max_z; ++z) {
                const ar::BlockInfo info = world.block(x, y, z);
                if (info.id != 8 && info.id != 9) { // Material.water: flowing_water / water
                    continue;
                }
                int32_t level = info.meta;
                if (level >= 8) {
                    level = 0;
                }
                const float surface = static_cast<float>(y + 1) - static_cast<float>(level + 1) / 9.0f;
                if (static_cast<float>(max_y) >= surface) {
                    water = true;
                    flow = world.liquid_flow(x, y, z, flow);
                }
            }
        }
    }
    const double length = std::sqrt(flow.x * flow.x + flow.y * flow.y + flow.z * flow.z);
    if (length > 0.0) {
        if (length >= 1.0E-4) {
            flow = math::Vec3d{flow.x / length, flow.y / length, flow.z / length};
        } else {
            flow = math::Vec3d{};
        }
        motion.x += flow.x * 0.014;
        motion.y += flow.y * 0.014;
        motion.z += flow.z * 0.014;
    }
    return water;
}

// =============================================================================================================
// ProjectileSim
// =============================================================================================================

void ProjectileSim::set_heading(double x, double y, double z, float velocity, float inaccuracy) noexcept {
    const double length = std::sqrt(x * x + y * y + z * z);
    motion.x = (x / length + static_cast<double>(inaccuracy) * 0.0) * static_cast<double>(velocity);
    motion.y = (y / length + static_cast<double>(inaccuracy) * 0.0) * static_cast<double>(velocity);
    motion.z = (z / length + static_cast<double>(inaccuracy) * 0.0) * static_cast<double>(velocity);
    const float horizontal = static_cast<float>(std::sqrt(motion.x * motion.x + motion.z * motion.z));
    yaw = static_cast<float>(std::atan2(motion.x, motion.z) * 180.0 / math::kPi);
    pitch = static_cast<float>(std::atan2(motion.y, static_cast<double>(horizontal)) * 180.0 / math::kPi);
}

void ProjectileSim::set_throwable_heading(float eye_height, double x, double y, double z, float yaw_deg,
                                          float pitch_deg) noexcept {
    has_collision = false;
    source_id = -1;
    thrower_id = -1;
    pos = math::Vec3d{x, y, z};
    width = 0.25f;
    height = 0.25f;
    pos.y = y + static_cast<double>(eye_height);
    yaw = yaw_deg;
    pitch = pitch_deg;
    const float yaw_rad = yaw_deg / 180.0f * kPiF;
    const float pitch_rad = pitch_deg / 180.0f * kPiF;
    pos.x -= static_cast<double>(ar::mh_cos(yaw_rad) * 0.16f);
    pos.y -= 0.1000000014901161;
    pos.z -= static_cast<double>(ar::mh_sin(yaw_rad) * 0.16f);
    reset_box();
    motion.x = static_cast<double>(-ar::mh_sin(yaw_rad) * ar::mh_cos(pitch_rad) * 0.4f);
    motion.z = static_cast<double>(ar::mh_cos(yaw_rad) * ar::mh_cos(pitch_rad) * 0.4f);
    const float pitch_offset = kind == SimKind::Potion ? -20.0f : 0.0f;
    motion.y = static_cast<double>(-ar::mh_sin((pitch_deg + pitch_offset) / 180.0f * kPiF) * 0.4f);
    set_heading(motion.x, motion.y, motion.z, kind == SimKind::Potion ? 0.5f : 1.5f, 0.0f);
    ticks_in_air = 0;
}

void ProjectileSim::init_arrow(float eye_height, double x, double y, double z, float yaw_deg, float pitch_deg,
                               float velocity) noexcept {
    has_collision = false;
    source_id = -1;
    thrower_id = -1;
    ticks_in_air = 0;
    pos = math::Vec3d{x, y + static_cast<double>(eye_height), z};
    width = 0.5f;
    height = 0.5f;
    yaw = yaw_deg;
    pitch = pitch_deg;
    const float yaw_rad = yaw_deg / 180.0f * kPiF;
    const float pitch_rad = pitch_deg / 180.0f * kPiF;
    pos.x -= static_cast<double>(ar::mh_cos(yaw_rad) * 0.16f);
    pos.y -= 0.1000000014901161;
    pos.z -= static_cast<double>(ar::mh_sin(yaw_rad) * 0.16f);
    box = math::AABB{{pos.x - 0.25, pos.y, pos.z - 0.25}, {pos.x + 0.25, pos.y + 0.5, pos.z + 0.25}};
    motion.x = static_cast<double>(-ar::mh_sin(yaw_rad) * ar::mh_cos(pitch_rad));
    motion.z = static_cast<double>(ar::mh_cos(yaw_rad) * ar::mh_cos(pitch_rad));
    motion.y = static_cast<double>(-ar::mh_sin(pitch_rad));
    set_heading(motion.x, motion.y, motion.z, velocity * 1.5f, 0.0f);
}

void ProjectileSim::init_from_entity(events::JniContext* jni, jobject entity, bool arrow) noexcept {
    has_collision = false;
    pos = minecraft::entity_get_position(jni, entity);
    width = ar::entity_width(jni, entity);
    height = ar::entity_height(jni, entity);
    reset_box();
    motion = minecraft::entity_get_motion(jni, entity);
    source_id = -1;
    if (arrow) {
        // ArrowSimulation: source = entity.shootingEntity
        jobject shooter = ar::arrow_shooting_entity(jni, entity);
        if (shooter != nullptr) {
            source_id = minecraft::entity_get_id(jni, shooter);
            jni->env->DeleteLocalRef(shooter);
        }
    }
    // EntityThrowable.getThrower() is never known client side (no thrower name is synced), so source stays null.
    thrower_id = minecraft::entity_get_id(jni, entity);
    ticks_in_air = ar::projectile_ticks_in_air(jni, entity);
    ajni::clear_exception(jni);
}

void ProjectileSim::advance(bool water) noexcept {
    pos.x += motion.x;
    pos.y += motion.y;
    pos.z += motion.z;
    const double drag = water ? (kind == SimKind::Arrow ? static_cast<double>(0.6f) : static_cast<double>(0.8f))
                              : static_cast<double>(0.99f);
    const double gravity = (kind == SimKind::Arrow || kind == SimKind::Potion) ? static_cast<double>(0.05f)
                                                                             : static_cast<double>(0.03f);
    motion.x *= drag;
    motion.y = motion.y * drag - gravity;
    motion.z *= drag;
    reset_box();
}

bool ProjectileSim::on_update(SimWorld& world) noexcept {
    const bool water = water_check(world);
    ++ticks_in_air;
    const math::Vec3d start = pos;
    math::Vec3d end{pos.x + motion.x, pos.y + motion.y, pos.z + motion.z};
    SimHit block_hit{};
    // ArrowSimulation: rayTraceBlocks(start, end, false, true, false); others: Adjust's walk == (false, false, false)
    const bool has_block = world.ray_trace(start, end, kind == SimKind::Arrow, &block_hit);
    if (has_block) {
        end = block_hit.vec;
    }
    const math::AABB search = ar::box_expand(ar::box_add_coord(box, motion.x, motion.y, motion.z), 1.0, 1.0, 1.0);
    const int32_t ignored = kind == SimKind::Potion ? target_id : -1;
    SimHit entity_hit{};
    const bool has_entity = closest_entity(
        world, search, start, end, partial_ticks,
        [&](const SimEntity& e) {
            if (e.id == thrower_id) {
                return true; // getEntitiesWithinAABBExcludingEntity(thrower, ...)
            }
            return (e.id == source_id || (ignored >= 0 && e.id == ignored)) && ticks_in_air < 5;
        },
        &entity_hit);
    if (has_entity) {
        collision = entity_hit;
        has_collision = true;
        return true;
    }
    if (has_block) {
        collision = block_hit;
        has_collision = true;
        return true;
    }
    advance(water);
    return false;
}

// =============================================================================================================
// FireballSim
// =============================================================================================================

void FireballSim::init_from_fireball(events::JniContext* jni, jobject fireball) noexcept {
    entity_id = minecraft::entity_get_id(jni, fireball);
    shooter_id = -1; // EntityFireball.shootingEntity is not synced to clients
    has_collision = false;
    accel = ar::fireball_acceleration(jni, fireball);
    motion = minecraft::entity_get_motion(jni, fireball);
    pos = minecraft::entity_get_position(jni, fireball);
    const math::Vec2f rot = minecraft::entity_get_rotation(jni, fireball);
    yaw = rot.x;
    pitch = rot.y;
    width = ar::entity_width(jni, fireball);
    height = ar::entity_height(jni, fireball);
    reset_box();
    ticks_in_air = ar::projectile_ticks_in_air(jni, fireball);
    ajni::clear_exception(jni);
}

void FireballSim::init_held(int32_t shooter, float eye_height, double x, double y, double z, float yaw_deg,
                            float pitch_deg) noexcept {
    entity_id = -1;
    has_collision = false;
    pos = math::Vec3d{x, y + static_cast<double>(eye_height), z};
    yaw = yaw_deg;
    pitch = pitch_deg;
    width = 1.0f;
    height = 1.0f;
    box = math::AABB{{x - 0.5, pos.y, z - 0.5}, {x + 0.5, pos.y + 1.0, z + 0.5}};
    const float yaw_rad = -yaw_deg * 0.017453292f - kPiF;
    const float pitch_rad = -pitch_deg * 0.017453292f;
    const float horizontal = -ar::mh_cos(pitch_rad);
    motion.x = static_cast<double>(ar::mh_sin(yaw_rad) * horizontal);
    motion.y = static_cast<double>(ar::mh_sin(pitch_rad));
    motion.z = static_cast<double>(ar::mh_cos(yaw_rad) * horizontal);
    accel = math::Vec3d{motion.x * 0.1, motion.y * 0.1, motion.z * 0.1};
    shooter_id = shooter;
    ticks_in_air = 0;
}

bool FireballSim::on_update(SimWorld& world) noexcept {
    const bool water = water_check(world);
    ++ticks_in_air;
    const math::Vec3d start = pos;
    math::Vec3d end{pos.x + motion.x, pos.y + motion.y, pos.z + motion.z};
    SimHit block_hit{};
    const bool has_block = world.ray_trace(start, end, false, &block_hit);
    if (has_block) {
        end = block_hit.vec;
    }
    const math::AABB search = ar::box_expand(ar::box_add_coord(box, motion.x, motion.y, motion.z), 1.0, 1.0, 1.0);
    SimHit entity_hit{};
    const bool has_entity = closest_entity(
        world, search, start, end, partial_ticks,
        [&](const SimEntity& e) {
            if (entity_id >= 0 && e.id == entity_id) {
                return true;
            }
            return shooter_id >= 0 && e.id == shooter_id && ticks_in_air < 25;
        },
        &entity_hit);
    if (has_entity) {
        collision = entity_hit;
        has_collision = true;
        return true;
    }
    if (has_block) {
        collision = block_hit;
        has_collision = true;
        return true;
    }
    pos.x += motion.x;
    pos.y += motion.y;
    pos.z += motion.z;
    const double drag = water ? static_cast<double>(0.8f) : static_cast<double>(0.95f);
    motion.x = (motion.x + accel.x) * drag;
    motion.y = (motion.y + accel.y) * drag;
    motion.z = (motion.z + accel.z) * drag;
    reset_box();
    return false;
}

// =============================================================================================================
// SimulationWorld
// =============================================================================================================

void explosion_affected(SimWorld& world, int32_t exploder_id, const math::Vec3d& pos, float strength,
                        ExplosionResult* out) noexcept {
    out->block_boxes.clear();
    out->entity_ids.clear();
    events::JniContext* jni = world.jni();
    if (jni == nullptr || world.world() == nullptr) {
        return;
    }

    // getAffectedEntities
    const float diameter = strength * 2.0f;
    const math::AABB area{{static_cast<double>(ar::mh_floor(pos.x - diameter - 1.0)),
                           static_cast<double>(ar::mh_floor(pos.y - diameter - 1.0)),
                           static_cast<double>(ar::mh_floor(pos.z - diameter - 1.0))},
                          {static_cast<double>(ar::mh_floor(pos.x + diameter + 1.0)),
                           static_cast<double>(ar::mh_floor(pos.y + diameter + 1.0)),
                           static_cast<double>(ar::mh_floor(pos.z + diameter + 1.0))}};
    JNIEnv* env = jni->env;
    for (const SimEntity& e : world.entities()) {
        if (e.id == exploder_id || !intersects(e.box, area)) {
            continue;
        }
        if (env->PushLocalFrame(4) != 0) {
            ajni::clear_exception(jni);
            break;
        }
        jobject entity = minecraft::world_get_entity_by_id(jni, world.world(), e.id);
        ajni::clear_exception(jni);
        if (entity != nullptr && !ar::entity_is_immune_to_explosions(jni, entity)) {
            const double dx = e.pos.x - pos.x;
            double dy = e.pos.y - pos.y;
            const double dz = e.pos.z - pos.z;
            if (static_cast<double>(static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz))) /
                    static_cast<double>(diameter) <= 1.0) {
                dy += static_cast<double>(ar::entity_eye_height(jni, entity));
                if (static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz)) != 0.0f) {
                    out->entity_ids.push_back(e.id);
                }
            }
        }
        env->PopLocalFrame(nullptr);
    }

    // getAffectedBlocks
    std::unordered_set<int64_t> affected;
    std::vector<int64_t> order;
    for (int ix = 0; ix < 16; ++ix) {
        const float fx = static_cast<float>(ix) / 15.0f * 2.0f - 1.0f;
        for (int iy = 0; iy < 16; ++iy) {
            const float fy = static_cast<float>(iy) / 15.0f * 2.0f - 1.0f;
            for (int iz = 0; iz < 16; ++iz) {
                if (ix != 0 && ix != 15 && iy != 0 && iy != 15 && iz != 0 && iz != 15) {
                    continue;
                }
                const float fz = static_cast<float>(iz) / 15.0f * 2.0f - 1.0f;
                const float length = std::sqrt(fx * fx + fy * fy + fz * fz);
                const double step_x = static_cast<double>(fx / length) * 0.300000011920929;
                const double step_y = static_cast<double>(fy / length) * 0.300000011920929;
                const double step_z = static_cast<double>(fz / length) * 0.300000011920929;
                double x = pos.x;
                double y = pos.y;
                double z = pos.z;
                for (float energy = strength; energy > 0.0f; energy -= 0.22500001f) {
                    const int32_t bx = ar::mh_floor(x);
                    const int32_t by = ar::mh_floor(y);
                    const int32_t bz = ar::mh_floor(z);
                    const ar::BlockInfo info = world.block(bx, by, bz);
                    if (info.id != 0) {
                        energy -= (ar::block_explosion_resistance(jni, info.id) + 0.3f) * 0.3f;
                    }
                    if (energy > 0.0f && !world.passable(bx, by, bz)) {
                        const int64_t key = pos_key(bx, by, bz);
                        if (affected.insert(key).second) {
                            math::AABB box{};
                            if (world.selected_box(bx, by, bz, &box)) {
                                out->block_boxes.push_back(box);
                            }
                        }
                    }
                    x += step_x;
                    y += step_y;
                    z += step_z;
                }
            }
        }
    }
}

// =============================================================================================================
// ShooterTracker
// =============================================================================================================

const ShooterTracker::Info* ShooterTracker::find(int32_t projectile_id) const noexcept {
    auto it = infos_.find(projectile_id);
    return it != infos_.end() ? &it->second : nullptr;
}

void ShooterTracker::update(events::GameContext* ctx) noexcept {
    events::JniContext* jni = ctx->jni;
    const ar::Classes* c = ar::classes(jni);
    if (c == nullptr || ctx->world == nullptr) {
        infos_.clear();
        return;
    }
    JNIEnv* env = jni->env;
    for (auto& [id, info] : infos_) {
        info.seen = false;
    }

    jobject list = minecraft::world_get_loaded_entity_list(jni, ctx->world);
    if (list == nullptr) {
        infos_.clear();
        return;
    }
    const int32_t count = minecraft::java_list_size(jni, list);
    for (int32_t i = 0; i < count; ++i) {
        if (env->PushLocalFrame(64) != 0) {
            ajni::clear_exception(jni);
            break;
        }
        jobject entity = minecraft::java_list_get(jni, list, i);
        if (entity == nullptr || ajni::clear_exception(jni)) {
            env->PopLocalFrame(nullptr);
            continue;
        }
        const int32_t id = minecraft::entity_get_id(jni, entity);
        auto existing = infos_.find(id);
        if (existing != infos_.end()) {
            existing->second.seen = true;
            env->PopLocalFrame(nullptr);
            continue;
        }
        // EntityTracker only tracks snowballs, eggs and ender pearls among the throwables.
        const bool throwable = ar::is_instance(jni, entity, c->snowball) || ar::is_instance(jni, entity, c->egg) ||
                               ar::is_instance(jni, entity, c->ender_pearl);
        const bool fireball = !throwable && ar::is_instance(jni, entity, c->fireball);
        if (!throwable && !fireball) {
            env->PopLocalFrame(nullptr);
            continue;
        }
        Info info{};
        info.fireball = fireball;
        info.seen = true;
        const int32_t ticks = minecraft::entity_get_ticks_existed(jni, entity);
        if (ticks <= 1) {
            // Spawn position / rotation (the first client update already moved the entity once).
            const math::Vec3d spawn = ticks == 0 ? minecraft::entity_get_position(jni, entity)
                                                 : ar::entity_last_tick_pos(jni, entity);
            const float spawn_yaw = ticks == 0 ? minecraft::entity_get_rotation(jni, entity).x
                                               : minecraft::entity_get_prev_rotation(jni, entity).x;

            jobject players = minecraft::world_get_player_entities(jni, ctx->world);
            const int32_t player_count = players != nullptr ? minecraft::java_list_size(jni, players) : 0;
            double best = 0.0;
            int32_t best_id = -1;
            float best_eye = 1.53f;
            for (int32_t p = 0; p < player_count; ++p) {
                jobject player = minecraft::java_list_get(jni, players, p);
                if (player == nullptr) {
                    continue;
                }
                if ((minecraft::player_get_tag_flags(jni, player) & minecraft::kPlayerTagBot) != 0) {
                    env->DeleteLocalRef(player);
                    continue;
                }
                const bool local = env->IsSameObject(player, ctx->local_player) != JNI_FALSE;
                const math::Vec3d ppos = minecraft::entity_get_position(jni, player);
                math::Vec3d base{};
                if (local) {
                    base = math::Vec3d{ar::mh_floor(ppos.x * 32.0) / 32.0, ar::mh_floor(ppos.y * 32.0) / 32.0,
                                       ar::mh_floor(ppos.z * 32.0) / 32.0};
                } else {
                    const math::Vec3d server = ar::entity_server_pos(jni, player);
                    base = math::Vec3d{server.x / 32.0, server.y / 32.0, server.z / 32.0};
                }
                const float eye = ar::entity_eye_height(jni, player);
                double distance = 0.0;
                double movement = 0.0;
                if (throwable) {
                    // TrackedThrowable.getLaunchDistance
                    const float yaw_rad = minecraft::entity_get_rotation(jni, player).x / 180.0f * kPiF;
                    const double lx = ar::mh_floor((base.x - static_cast<double>(ar::mh_cos(yaw_rad) * 0.16f)) * 32.0) / 32.0;
                    const double ly = ar::mh_floor((base.y + static_cast<double>(eye) - 0.1000000014901161) * 32.0) / 32.0;
                    const double lz = ar::mh_floor((base.z - static_cast<double>(ar::mh_sin(yaw_rad) * 0.16f)) * 32.0) / 32.0;
                    distance = std::abs(spawn.x - lx) + std::abs(spawn.y - ly) + std::abs(spawn.z - lz);
                    const math::Vec3d prev = minecraft::entity_get_prev_position(jni, player);
                    movement = std::abs(ppos.x - prev.x) + std::abs(ppos.y - prev.y) + std::abs(ppos.z - prev.z);
                    if (distance > movement * 2.75 + 1.5) {
                        env->DeleteLocalRef(player);
                        continue;
                    }
                } else {
                    // TrackedFireball.getLaunchDistance
                    base.y += minecraft::entity_is_sneaking(jni, player) ? 1.53125 : 1.59375;
                    distance = std::abs(spawn.x - base.x) + std::abs(spawn.y - base.y) + std::abs(spawn.z - base.z);
                    const math::Vec3d last = ar::entity_last_tick_pos(jni, player);
                    movement = std::abs(ppos.x - last.x) + std::abs(ppos.y - last.y) + std::abs(ppos.z - last.z);
                    if (distance > movement * 2.75 + 2.5) {
                        env->DeleteLocalRef(player);
                        continue;
                    }
                }
                if (best_id < 0 || distance < best) {
                    best = distance;
                    best_id = minecraft::entity_get_id(jni, player);
                    best_eye = eye;
                }
                env->DeleteLocalRef(player);
            }
            ajni::clear_exception(jni);
            info.shooter_id = best_id;
            if (throwable) {
                // TrackedThrowable.initializeLaunchPosition(candidate or null)
                const float eye = best_id >= 0 ? best_eye : 1.53f;
                const float yaw_rad = spawn_yaw / 180.0f * kPiF;
                info.shooter_eye = eye;
                info.launch = math::Vec3d{spawn.x + static_cast<double>(ar::mh_cos(yaw_rad) * 0.16f),
                                          spawn.y - static_cast<double>(eye) + 0.1000000014901161,
                                          spawn.z + static_cast<double>(ar::mh_sin(yaw_rad) * 0.16f)};
                info.has_launch = true;
            }
        }
        infos_.emplace(id, info);
        env->PopLocalFrame(nullptr);
    }
    env->DeleteLocalRef(list);
    for (auto it = infos_.begin(); it != infos_.end();) {
        if (!it->second.seen) {
            it = infos_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace slinky::modules::adjust_sim
