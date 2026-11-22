#pragma once

// Shared helpers for the ported Adjust world visuals (Block Overlay, Bed Plates, Trajectories, Indicators).
//
//   com.adjustclient.render.RenderUtil  -> gl_color, begin3d/end3d, begin_lines/end_lines, draw_filled_box,
//                                          draw_outlined_box, draw_rect / draw_bordered_rect (2D GL)
//   net.minecraft.util.MathHelper       -> mh_sin / mh_cos / mh_floor / wrap_degrees
//   Minecraft 1.8.9 members that the central mapping table does not cover (resolved with adjust::jni and
//   cached in function-local statics).
//
// Everything here targets Minecraft 1.8.9 (the version Adjust supports); on 1.7.10 the JNI helpers fail
// gracefully (return false / nullptr) and the modules do nothing.
//
// GL helpers are meant for the kEventRenderWorld3D pass, which OpenSlinky++ runs with the camera transform active,
// depth test off, texturing off and blending on. end3d()/end_lines() restore exactly that state (instead of
// Adjust's "back to vanilla" state) so modules that run after ours in the same pass are unaffected.

#include <jni.h>

#include <cstdint>
#include <initializer_list>
#include <unordered_map>
#include <vector>

#include "adjust/kit.hpp"
#include "events/events.hpp"
#include "util/math.hpp"

namespace slinky::modules::adjust_world {

// =============================================================================================================
// MathHelper
// =============================================================================================================

float mh_sin(float value) noexcept; // MathHelper.sin (65536-entry table semantics)
float mh_cos(float value) noexcept; // MathHelper.cos
int mh_floor(double value) noexcept; // MathHelper.floor_double
float wrap_degrees(float value) noexcept; // MathHelper.wrapAngleTo180_float

// EnumFacing indices (ordinal) and helpers.
enum Facing : int32_t { kDown = 0, kUp = 1, kNorth = 2, kSouth = 3, kWest = 4, kEast = 5 };
extern const int32_t kFacingOffsets[6][3];
inline int32_t facing_opposite(int32_t f) noexcept { return f ^ 1; }
inline int32_t facing_axis(int32_t f) noexcept { return f <= kUp ? 1 : (f <= kSouth ? 2 : 0); } // 0=X 1=Y 2=Z

// AxisAlignedBB helpers
math::AABB box_offset(const math::AABB& b, double dx, double dy, double dz) noexcept;
math::AABB box_union(const math::AABB& a, const math::AABB& b) noexcept;
math::AABB box_expand(const math::AABB& b, double x, double y, double z) noexcept;
math::AABB box_add_coord(const math::AABB& b, double x, double y, double z) noexcept;

// AxisAlignedBB.calculateIntercept (1.8.9). Returns false if the segment misses; *out_side = EnumFacing ordinal.
bool box_calculate_intercept(const math::AABB& box, const math::Vec3d& a, const math::Vec3d& b,
                             math::Vec3d* out_hit, int32_t* out_side) noexcept;

// =============================================================================================================
// GL (RenderUtil)
// =============================================================================================================

void gl_color(adjust::Argb argb) noexcept;  // RenderUtil.glColor
void begin3d() noexcept;                    // RenderUtil.begin3d (depth mask off, line width 0.4)
void end3d() noexcept;                      // RenderUtil.end3d (restores the world-pass state)
void begin_lines() noexcept;                // RenderUtil.i
void end_lines() noexcept;                  // RenderUtil.h (restores the world-pass state)

// Boxes in camera-relative coordinates.
void draw_filled_box(const math::AABB& box) noexcept;   // RenderUtil.drawFilledBox (0.0019 inset)
void draw_outlined_box(const math::AABB& box) noexcept; // RenderUtil.drawOutlinedBox

// 2D (current GL transform): Gui.drawRect / RenderUtil.drawBorderedRect / RenderUtil.drawQuad.
void draw_quad_2d(float x1, float y1, float x2, float y2) noexcept;
void draw_rect_outline_2d(float x1, float y1, float x2, float y2, float thickness) noexcept;

// =============================================================================================================
// JNI extras (1.8.9)
// =============================================================================================================

struct Classes {
    jclass entity = nullptr;
    jclass living = nullptr;            // EntityLivingBase
    jclass entity_living = nullptr;     // EntityLiving
    jclass player = nullptr;            // EntityPlayer
    jclass throwable = nullptr;         // EntityThrowable
    jclass arrow = nullptr;             // EntityArrow
    jclass fireball = nullptr;          // EntityFireball
    jclass snowball = nullptr;          // EntitySnowball
    jclass egg = nullptr;               // EntityEgg
    jclass ender_pearl = nullptr;       // EntityEnderPearl
    jclass potion = nullptr;            // EntityPotion
    jclass mob = nullptr;               // EntityMob
    jclass iron_golem = nullptr;        // EntityIronGolem
    jclass armor_stand = nullptr;       // EntityArmorStand
    jclass item = nullptr;              // EntityItem
    jclass tnt = nullptr;               // EntityTNTPrimed
    jclass block = nullptr;             // Block
    jclass item_cls = nullptr;          // Item
    jclass item_stack = nullptr;        // ItemStack
};

// Resolved once; nullptr if any class is missing (1.7.10 / unsupported mapping).
const Classes* classes(events::JniContext* jni) noexcept;

bool is_instance(events::JniContext* jni, jobject obj, jclass cls) noexcept;

math::Vec3d entity_last_tick_pos(events::JniContext* jni, jobject entity) noexcept; // lastTickPosX/Y/Z
math::Vec3d entity_server_pos(events::JniContext* jni, jobject entity) noexcept;    // serverPosX/Y/Z (raw ints)
float entity_width(events::JniContext* jni, jobject entity) noexcept;
float entity_height(events::JniContext* jni, jobject entity) noexcept;
float entity_eye_height(events::JniContext* jni, jobject entity) noexcept;
bool entity_is_invisible(events::JniContext* jni, jobject entity) noexcept;
bool entity_is_immune_to_explosions(events::JniContext* jni, jobject entity) noexcept;
math::AABB entity_box(events::JniContext* jni, jobject entity) noexcept; // getEntityBoundingBox

// Projectile fields (return defaults when missing).
int32_t projectile_ticks_in_air(events::JniContext* jni, jobject entity) noexcept; // throwable/arrow/fireball
bool projectile_in_ground(events::JniContext* jni, jobject entity) noexcept;      // throwable/arrow/fireball
math::Vec3d fireball_acceleration(events::JniContext* jni, jobject fireball) noexcept;
jobject arrow_shooting_entity(events::JniContext* jni, jobject arrow) noexcept;   // local ref or nullptr
int32_t potion_damage(events::JniContext* jni, jobject potion) noexcept;          // EntityPotion.getPotionDamage

// Held item stack of a player (local ref or nullptr).
jobject player_held_stack(events::JniContext* jni, jobject player) noexcept;

// Blocks: state at a position (local refs).
struct BlockInfo {
    int32_t id = 0;
    int32_t meta = 0;
};
jobject block_state_at(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z) noexcept;
bool block_info_at(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z,
                   BlockInfo* out) noexcept;
// Block.setBlockBoundsBasedOnState + Block.getSelectedBoundingBox (world coordinates).
bool block_selected_box(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z,
                        math::AABB* out) noexcept;
bool block_is_full_block(events::JniContext* jni, int32_t block_id) noexcept; // cached per id
int32_t block_damage_dropped(events::JniContext* jni, int32_t block_id, int32_t meta) noexcept; // cached
float block_explosion_resistance(events::JniContext* jni, int32_t block_id) noexcept;          // cached
bool block_is_passable(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z) noexcept;

// =============================================================================================================
// Block id tracker (replacement for Adjust's BlockRenderStateEvent, which needs a Minecraft hook): keeps the
// positions of selected block ids in loaded chunks up to date from kEventWorldBlockUpdate.
// =============================================================================================================

struct TrackedBlock {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t id = 0;
};

class BlockIdTracker {
public:
    explicit BlockIdTracker(std::initializer_list<int32_t> ids) noexcept;

    void clear() noexcept;
    void scan_all(events::GameContext* ctx) noexcept;                 // rescans every loaded chunk
    void on_world_block_update(const events::WorldBlockUpdateEvent* ev) noexcept;

    uint64_t revision() const noexcept { return revision_; }        // changes whenever the set changes
    void collect(std::vector<TrackedBlock>* out) const;              // snapshot of all tracked blocks

    // Block changes inside tracked chunks (any id), consumed by the owner: positions changed since last call.
    void take_changes(std::vector<TrackedBlock>* out);

private:
    bool wanted(int32_t id) const noexcept;
    void scan_chunk(events::JniContext* jni, jobject world, int32_t cx, int32_t cz) noexcept;

    std::vector<int32_t> ids_;
    std::unordered_map<int64_t, std::vector<TrackedBlock>> chunks_;
    std::vector<TrackedBlock> changes_;
    uint64_t revision_ = 1;
};

} // namespace slinky::modules::adjust_world
