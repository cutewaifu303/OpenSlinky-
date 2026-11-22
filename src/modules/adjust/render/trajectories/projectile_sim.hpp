#pragma once

// Ports of Adjust's projectile physics used by Trajectories and Indicators:
//
//   com.adjustclient.util.EntitySimulation              -> SimBody::water_check
//   com.adjustclient.util.simulation.ProjectileSimulation / ArrowSimulation / PotionSimulation -> ProjectileSim
//   com.adjustclient.util.simulation.FireballSimulation -> FireballSim
//   com.adjustclient.util.SimulationWorld               -> explosion_affected
//   TrackedThrowable / TrackedFireball shooter guessing  -> ShooterTracker
//
// Entities are read once per tick into a SimWorld snapshot (positions only change on ticks), so simulations that
// run every frame (held items) do not re-read every entity through JNI. Block ray traces go through
// World.rayTraceBlocks (Adjust's own block walk is the same algorithm with canCollideCheck(state, false)).

#include <jni.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "events/events.hpp"
#include "modules/adjust/render/block_overlay/render_common.hpp"
#include "util/math.hpp"

namespace slinky::modules::adjust_sim {

struct SimEntity {
    int32_t id = 0;
    bool collidable = false; // canBeCollidedWith
    bool sneaking = false;
    bool local = false;      // mc.thePlayer
    math::AABB box{};        // getEntityBoundingBox
    math::Vec3d pos{};
    math::Vec3d last{};      // lastTickPos
};

// MovingObjectPosition
struct SimHit {
    int32_t type = 0;        // 0 = miss, 1 = block, 2 = entity
    math::Vec3d vec{};       // hitVec
    int32_t side = 1;        // EnumFacing ordinal
    int32_t entity_id = -1;  // entityHit
    int32_t bx = 0, by = 0, bz = 0; // blockPos
};

class SimWorld {
public:
    // Per tick: snapshot of loaded entities, clears the block cache.
    void capture(events::GameContext* ctx) noexcept;
    // Before simulating (any event): JNI context + world of the current call.
    void bind(events::JniContext* jni, jobject world) noexcept;
    void clear() noexcept;

    const SimEntity* find(int32_t id) const noexcept;
    const std::vector<SimEntity>& entities() const noexcept { return entities_; }

    adjust_world::BlockInfo block(int32_t x, int32_t y, int32_t z) noexcept;
    bool passable(int32_t x, int32_t y, int32_t z) noexcept;
    // World.rayTraceBlocks(start, end, false, ignore_no_box, false)
    bool ray_trace(const math::Vec3d& start, const math::Vec3d& end, bool ignore_no_box, SimHit* out) noexcept;
    // Block.modifyAcceleration (liquid flow) added to `flow`.
    math::Vec3d liquid_flow(int32_t x, int32_t y, int32_t z, const math::Vec3d& flow) noexcept;
    bool selected_box(int32_t x, int32_t y, int32_t z, math::AABB* out) noexcept;

    events::JniContext* jni() const noexcept { return jni_; }
    jobject world() const noexcept { return world_; }

private:
    events::JniContext* jni_ = nullptr;
    jobject world_ = nullptr;
    std::vector<SimEntity> entities_;
    std::unordered_map<int32_t, size_t> index_;
    std::unordered_map<int64_t, adjust_world::BlockInfo> blocks_;
    std::unordered_map<int64_t, bool> passable_;
};

// EntitySimulation
struct SimBody {
    math::AABB box{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    math::Vec3d pos{};
    math::Vec3d motion{};
    float width = 0.0f;
    float height = 0.0f;

    bool water_check(SimWorld& world) noexcept; // EntitySimulation.getBlockState(entity)
    void reset_box() noexcept;                  // box from pos / width / height
};

enum class SimKind : uint8_t { Throwable, Potion, Arrow };

// ProjectileSimulation (+ ArrowSimulation / PotionSimulation by kind)
struct ProjectileSim : SimBody {
    SimKind kind = SimKind::Throwable;
    int32_t source_id = -1;  // source (shooter)
    int32_t thrower_id = -1; // thrower (the projectile itself when simulating an entity)
    int32_t target_id = -1;  // PotionSimulation.target (never set by Adjust)
    float partial_ticks = 1.0f;
    int32_t ticks_in_air = 0;
    bool has_collision = false;
    SimHit collision{};

    void set_heading(double x, double y, double z, float velocity, float inaccuracy) noexcept;
    // ProjectileSimulation.setThrowableHeading(world, eyeHeight, x, y, z, yaw, pitch)
    void set_throwable_heading(float eye_height, double x, double y, double z, float yaw_deg, float pitch_deg) noexcept;
    // ArrowSimulation.initialize(world, eyeHeight, x, y, z, yaw, pitch, velocity)
    void init_arrow(float eye_height, double x, double y, double z, float yaw_deg, float pitch_deg,
                    float velocity) noexcept;
    // ProjectileSimulation.initialize(EntityThrowable) / ArrowSimulation.initialize(EntityArrow)
    void init_from_entity(events::JniContext* jni, jobject entity, bool arrow) noexcept;
    bool on_update(SimWorld& world) noexcept;

private:
    void advance(bool water) noexcept;
};

// FireballSimulation
struct FireballSim : SimBody {
    int32_t entity_id = -1;  // the fireball (excluded from collisions), -1 for held fire charges
    int32_t shooter_id = -1;
    math::Vec3d accel{};
    float partial_ticks = 1.0f;
    int32_t ticks_in_air = 0;
    bool has_collision = false;
    SimHit collision{};

    void init_from_fireball(events::JniContext* jni, jobject fireball) noexcept;
    void init_held(int32_t shooter, float eye_height, double x, double y, double z, float yaw_deg,
                   float pitch_deg) noexcept;
    bool on_update(SimWorld& world) noexcept;
};

// SimulationWorld (strength 1 fireball explosion, no random ray energy).
struct ExplosionResult {
    std::vector<math::AABB> block_boxes;  // getSelectedBoundingBox of every affected block (world coordinates)
    std::vector<int32_t> entity_ids;      // affected entities
};
void explosion_affected(SimWorld& world, int32_t exploder_id, const math::Vec3d& pos, float strength,
                        ExplosionResult* out) noexcept;

// Shooter attribution of thrown items and fireballs (TrackedThrowable.update / TrackedFireball.update): when a
// projectile is first seen right after spawning, the player whose launch position matches best is its shooter.
class ShooterTracker {
public:
    struct Info {
        int32_t shooter_id = -1;
        bool fireball = false;
        bool has_launch = false;
        math::Vec3d launch{};       // TrackedThrowable.previousPosition
        float shooter_eye = 1.53f;  // TrackedThrowable.shooterEyeHeight
        bool seen = false;
    };

    // Once per tick. `ctx->world` must be valid.
    void update(events::GameContext* ctx) noexcept;
    void clear() noexcept { infos_.clear(); }
    const Info* find(int32_t projectile_id) const noexcept;

private:
    std::unordered_map<int32_t, Info> infos_;
};

} // namespace slinky::modules::adjust_sim
