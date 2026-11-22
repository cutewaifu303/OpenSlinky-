// Port of com.adjustclient.module.impl.render.freecam.FreecamModule / FreecamCamera (+ the camera parts of
// com.adjustclient.util.SimulationPlayer).
//
// Settings (as in Adjust): Speed 2 (1 .. 9, step 1, "Camera movement speed."), Noclip off ("Controls if your camera
// can pass through blocks."). Module hidden from the array list by default (Adjust Module default).
//
// Ported behaviour:
//   * onEnable: requires a player and a world (FreecamModule.toggle); the camera spawns at the player's position
//     with the rendered rotation (server spoof rotation while a silent rotation is active, else the player's) and
//     becomes the view entity. onDisable: the player is the view entity again.
//   * World change / respawn (Minecraft.loadWorld / setDimensionAndSpawnPlayer -> FreecamModule.setCamera): the
//     camera is recreated at the new player. Any other change of the view entity is overridden (Adjust's
//     cameraOverride always wins over renderViewEntity).
//   * handleMotion (EntityPlayerSP.onUpdate, priority -10 -> kEventPreLivingUpdate):
//       handleKeys: keyboard movement keys -> speed ramp with the sprint key (field76/field84), speed =
//       Speed * 10 / 20 * max(0.7, ramp); Noclip: the camera flies freely (forward/strafe on the camera yaw,
//       jump/sneak up/down); otherwise the input drives the collision simulation (handleKeyboard + simulation tick:
//       walking with gravity, jumping, sprint-jumping, double-tap jump to fly, AI move speed
//       Speed * 0.14 - (Speed >= 4 ? Speed * 0.05 : 0), fly speed = move speed / 2.54).
//   * Mouse look turns the camera (EntityRenderer.updateMouseInput cameraOverride branch), the player keeps its
//     rotation and gets no movement input (MovementInputFromOptions patch) - see camera_hooks.cpp.
//   * Attack / use keys do nothing for the player (handleInput cancels them; their tool actions are not ported).
//   * render (Render3DEvent): the block the camera looks at (up to 85 blocks) gets a black 40% outline, line width
//     2, depth tested, without depth writes.
//
// Not ported (and why):
//   * The camera tool hotbar (Speed / Magic Wand / No-Clip / Housing / Player Control / Back items, scroll and
//     number-key slot selection, held tool rendering, nuke/teleport tools, FreecamController packet batching,
//     pathfinder, "Packet Balance Regain" and "Pathfinder timeout" settings): needs custom ItemStacks rendered in a
//     replaced hotbar plus GuiIngame / ItemRenderer / Minecraft input patches, and the Housing tools depend on
//     Hypixel game state tracking and packet batching (NetworkManager flush patch). While Freecam is active the
//     vanilla hotbar of the player is shown instead and no hand is rendered.
//   * PlayerContextMenu spectate (activateForPlayer) and the TeleportEvent camera sync (bed timer of Adjust's
//     anticheat ServerTracker): no counterpart in OpenSlinky++.
//   * Simulation details that need overriding Java methods: water / lava / ladder physics, block slipperiness
//     (the ground friction of a normal block, 0.6, is used), pushOutOfBlocks, unloaded-chunk gravity rule.

#include "modules/adjust/render/freecam/freecam.hpp"

#include <glad/glad.h>

#include <algorithm>
#include <cmath>

#include "adjust/jni_lookup.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/block_overlay/render_common.hpp"

namespace slinky::modules {

namespace {

namespace ajni = adjust::jni;
namespace ar = adjust_world;

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;
void on_module_state_changed(void* raw) noexcept;
void on_pre_living_update(void* raw) noexcept;
void on_game_tick(void* raw) noexcept;
void on_reach_query(void* raw) noexcept;
void on_pre_attack_or_use(void* raw) noexcept;
void on_pre_move_with_heading(void* raw) noexcept;
void on_render_world_3d(void* raw) noexcept;
void on_client_shutdown(void* raw) noexcept;

constexpr float kGroundSlipperiness = 0.6f; // Block.slipperiness of every non-ice block

// SimulationPlayer state (the camera entity itself is the simulated body: position, bounding box, motion,
// onGround and isCollidedHorizontally live in its Entity fields).
struct Simulation {
    bool flying = false;
    int32_t fly_toggle_timer = 0;
    bool sprinting = false;
    int32_t sprinting_ticks_left = 0;
    int32_t jump_ticks = 0;
    bool jump_input = false;
    float jump_movement_factor = 0.02f;
};

struct FreecamState {
    events::Module base{};

    config::FloatSetting speed{{}, "Speed", "%.0f", 1.0f, 0.0f, 1.0f, 9.0f, 2.0f, 2.0f};
    config::BoolSetting no_clip{{}, "Noclip", false, false, {}};

    jobject camera = nullptr; // FreecamCamera (global ref)
    jweak world = nullptr;    // world the camera was created in
    jweak player = nullptr;   // player the camera was created for
    float speed_ramp = 0.0f;  // field76
    int32_t sprint_ticks = 0; // field84
    Simulation sim{};

    FreecamState() noexcept {
        base.name = "Freecam";
        base.description = "Detaches the camera from your player.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagHiddenFromHud; // Adjust default: hidden
        base.events[events::kEventModuleStateChanged] = {&on_module_state_changed, 0};
        base.events[events::kEventPreLivingUpdate] = {&on_pre_living_update, -10};
        base.events[events::kEventGameTick] = {&on_game_tick, 10000};
        base.events[events::kEventReachRaycastQuery] = {&on_reach_query, -10000};
        base.events[events::kEventPreAttackOrUseEntity] = {&on_pre_attack_or_use, 10000};
        base.events[events::kEventPreMoveWithHeading] = {&on_pre_move_with_heading, -10000};
        base.events[events::kEventRenderWorld3D] = {&on_render_world_3d, 1000};
        base.events[events::kEventClientShutdown] = {&on_client_shutdown, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Freecam");
    }
};

FreecamState g_state{};

// ---------------------------------------------------------------------------------------------------------------
// JNI members not covered by the central mapping table (Minecraft 1.8.9)
// ---------------------------------------------------------------------------------------------------------------

struct Ids {
    jclass armor_stand = nullptr;
    jmethodID armor_stand_ctor = nullptr; // EntityArmorStand(World)
    jmethodID set_size = nullptr;         // Entity.setSize(FF)V (protected)
    jmethodID set_position = nullptr;     // Entity.setPosition(DDD)V
    jmethodID move_entity = nullptr;      // Entity.moveEntity(DDD)V
    jfieldID prev_x = nullptr;            // Entity.prevPosX/Y/Z
    jfieldID prev_y = nullptr;
    jfieldID prev_z = nullptr;
    jfieldID last_x = nullptr;            // Entity.lastTickPosX/Y/Z
    jfieldID last_y = nullptr;
    jfieldID last_z = nullptr;
    jfieldID collided_horizontally = nullptr; // Entity.isCollidedHorizontally
    jmethodID set_render_view_entity = nullptr; // Minecraft.setRenderViewEntity(Entity)
    jfieldID capabilities = nullptr;      // EntityPlayer.capabilities
    jfieldID is_flying = nullptr;         // PlayerCapabilities.isFlying

    bool ok() const noexcept {
        return armor_stand != nullptr && armor_stand_ctor != nullptr && set_size != nullptr &&
               set_position != nullptr && move_entity != nullptr && prev_x != nullptr && prev_y != nullptr &&
               prev_z != nullptr && last_x != nullptr && last_y != nullptr && last_z != nullptr &&
               collided_horizontally != nullptr && set_render_view_entity != nullptr;
    }
};

const Ids& ids(events::JniContext* jni) noexcept {
    static const Ids s_ids = [&] {
        Ids out{};
        const jclass entity = ajni::find_class(jni, "pk", "net/minecraft/entity/Entity");
        const jclass minecraft = ajni::find_class(jni, "ave", "net/minecraft/client/Minecraft");
        const jclass player = ajni::find_class(jni, "wn", "net/minecraft/entity/player/EntityPlayer");
        const jclass caps = ajni::find_class(jni, "wl", "net/minecraft/entity/player/PlayerCapabilities");
        out.armor_stand = ajni::find_class(jni, "um", "net/minecraft/entity/item/EntityArmorStand");
        out.armor_stand_ctor = ajni::method(jni, out.armor_stand, {"<init>", "<init>", "<init>"}, "(Ladm;)V",
                                            "(Lnet/minecraft/world/World;)V");
        out.set_size = ajni::method(jni, entity, {"a", "setSize", "func_70105_a"}, "(FF)V", "(FF)V");
        out.set_position = ajni::method(jni, entity, {"b", "setPosition", "func_70107_b"}, "(DDD)V", "(DDD)V");
        out.move_entity = ajni::method(jni, entity, {"d", "moveEntity", "func_70091_d"}, "(DDD)V", "(DDD)V");
        out.prev_x = ajni::field(jni, entity, {"p", "prevPosX", "field_70169_q"}, "D", "D");
        out.prev_y = ajni::field(jni, entity, {"q", "prevPosY", "field_70167_r"}, "D", "D");
        out.prev_z = ajni::field(jni, entity, {"r", "prevPosZ", "field_70166_s"}, "D", "D");
        out.last_x = ajni::field(jni, entity, {"P", "lastTickPosX", "field_70142_S"}, "D", "D");
        out.last_y = ajni::field(jni, entity, {"Q", "lastTickPosY", "field_70137_T"}, "D", "D");
        out.last_z = ajni::field(jni, entity, {"R", "lastTickPosZ", "field_70136_U"}, "D", "D");
        out.collided_horizontally =
            ajni::field(jni, entity, {"D", "isCollidedHorizontally", "field_70123_F"}, "Z", "Z");
        out.set_render_view_entity = ajni::method(jni, minecraft, {"a", "setRenderViewEntity", "func_175607_a"},
                                                  "(Lpk;)V", "(Lnet/minecraft/entity/Entity;)V");
        out.capabilities = ajni::field(jni, player, {"bA", "capabilities", "field_71075_bZ"}, "Lwl;",
                                       "Lnet/minecraft/entity/player/PlayerCapabilities;");
        out.is_flying = ajni::field(jni, caps, {"b", "isFlying", "field_75100_b"}, "Z", "Z");
        return out;
    }();
    return s_ids;
}

bool running() noexcept {
    return events::module_is_running(&g_state.base);
}

// ---------------------------------------------------------------------------------------------------------------
// Camera lifetime
// ---------------------------------------------------------------------------------------------------------------

void set_tick_positions(events::JniContext* jni, const Ids& id, jobject entity, const math::Vec3d& p) noexcept {
    JNIEnv* env = jni->env;
    env->SetDoubleField(entity, id.prev_x, p.x);
    env->SetDoubleField(entity, id.prev_y, p.y);
    env->SetDoubleField(entity, id.prev_z, p.z);
    env->SetDoubleField(entity, id.last_x, p.x);
    env->SetDoubleField(entity, id.last_y, p.y);
    env->SetDoubleField(entity, id.last_z, p.z);
}

void set_view_entity(events::JniContext* jni, jobject mc, jobject entity) noexcept {
    const Ids& id = ids(jni);
    if (id.set_render_view_entity != nullptr) {
        jni->env->CallVoidMethod(mc, id.set_render_view_entity, entity); // also reloads the entity shader
        ajni::clear_exception(jni);
    } else {
        jni->env->SetObjectField(mc, jni->mappings->fid_mc_render_view_entity, entity);
    }
}

// FreecamModule.onDisable (camera part): the player becomes the view entity again.
void destroy_camera(events::JniContext* jni) noexcept {
    if (g_state.camera == nullptr) {
        return;
    }
    if (jni != nullptr && jni->env != nullptr) {
        JNIEnv* env = jni->env;
        if (env->PushLocalFrame(16) == 0) {
            jobject mc = minecraft::mc_get_instance(jni);
            jobject view = mc != nullptr ? minecraft::mc_get_render_view_entity(jni, mc) : nullptr;
            if (view != nullptr && env->IsSameObject(view, g_state.camera) != JNI_FALSE) {
                jobject player = minecraft::mc_get_player(jni, mc);
                if (player != nullptr) {
                    set_view_entity(jni, mc, player);
                }
            }
            ajni::clear_exception(jni);
            env->PopLocalFrame(nullptr);
        } else {
            ajni::clear_exception(jni);
        }
        env->DeleteGlobalRef(g_state.camera);
        if (g_state.world != nullptr) {
            env->DeleteWeakGlobalRef(g_state.world);
        }
        if (g_state.player != nullptr) {
            env->DeleteWeakGlobalRef(g_state.player);
        }
    }
    g_state.camera = nullptr;
    g_state.world = nullptr;
    g_state.player = nullptr;
}

// FreecamModule.onEnable (camera part). Returns false if there is no player / world (Adjust refuses to enable).
bool spawn_camera(events::JniContext* jni) noexcept {
    if (jni == nullptr || jni->env == nullptr || !ajni::is_1_8(jni)) {
        return false;
    }
    const Ids& id = ids(jni);
    if (!id.ok()) {
        return false;
    }
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(32) != 0) {
        ajni::clear_exception(jni);
        return false;
    }
    jobject mc = minecraft::mc_get_instance(jni);
    jobject player = mc != nullptr ? minecraft::mc_get_player(jni, mc) : nullptr;
    jobject world = mc != nullptr ? minecraft::mc_get_world(jni, mc) : nullptr;
    if (player == nullptr || world == nullptr) {
        env->PopLocalFrame(nullptr);
        return false;
    }
    jobject camera = env->NewObject(id.armor_stand, id.armor_stand_ctor, world);
    if (ajni::clear_exception(jni) || camera == nullptr) {
        env->PopLocalFrame(nullptr);
        return false;
    }
    // FreecamCamera is an EntityLivingBase with a player-sized body and eye height 1.62.
    env->CallVoidMethod(camera, id.set_size, 0.6f, 1.8f);
    const math::Vec3d pos = minecraft::entity_get_position(jni, player);
    env->CallVoidMethod(camera, id.set_position, pos.x, pos.y, pos.z);
    set_tick_positions(jni, id, camera, pos);
    const math::Vec2f rot = minecraft::is_rotation_spoof_active() ? minecraft::get_server_spoof_rotation()
                                                                  : minecraft::entity_get_rotation(jni, player);
    minecraft::entity_set_rotation(jni, camera, rot);
    minecraft::entity_set_prev_rotation(jni, camera, rot);
    // FreecamCamera.updateSimulation: the simulation starts with the player's motion and flight state.
    minecraft::entity_set_motion(jni, camera, minecraft::entity_get_motion(jni, player));
    g_state.sim = Simulation{};
    if (id.capabilities != nullptr && id.is_flying != nullptr) {
        jobject caps = env->GetObjectField(player, id.capabilities);
        if (caps != nullptr) {
            g_state.sim.flying = env->GetBooleanField(caps, id.is_flying) != JNI_FALSE;
        }
    }
    if (ajni::clear_exception(jni)) {
        env->PopLocalFrame(nullptr);
        return false;
    }
    g_state.camera = env->NewGlobalRef(camera);
    g_state.world = env->NewWeakGlobalRef(world);
    g_state.player = env->NewWeakGlobalRef(player);
    set_view_entity(jni, mc, g_state.camera); // mc.cameraOverride = camera
    env->PopLocalFrame(nullptr);
    return g_state.camera != nullptr;
}

void request_disable(events::GameContext* ctx) noexcept {
    if (ctx != nullptr && ctx->module_manager != nullptr) {
        ctx->module_manager->request_set_enabled(&g_state.base, false);
    }
}

// FreecamModule.setCamera / the cameraOverride priority: keeps the camera valid for the current world and player.
// Returns false if the camera could not be kept (module then disables itself).
bool validate_camera(events::GameContext* ctx) noexcept {
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    if (ctx->local_player == nullptr || ctx->world == nullptr) {
        destroy_camera(jni);
        request_disable(ctx);
        return false;
    }
    if (g_state.camera == nullptr || env->IsSameObject(g_state.world, ctx->world) == JNI_FALSE ||
        env->IsSameObject(g_state.player, ctx->local_player) == JNI_FALSE) {
        destroy_camera(jni);
        if (!spawn_camera(jni)) {
            request_disable(ctx);
            return false;
        }
        return true;
    }
    jobject mc = minecraft::mc_get_instance(jni);
    if (mc == nullptr) {
        return false;
    }
    jobject view = minecraft::mc_get_render_view_entity(jni, mc);
    if (view == nullptr || env->IsSameObject(view, g_state.camera) == JNI_FALSE) {
        set_view_entity(jni, mc, g_state.camera);
    }
    if (view != nullptr) {
        env->DeleteLocalRef(view);
    }
    env->DeleteLocalRef(mc);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Camera movement
// ---------------------------------------------------------------------------------------------------------------

struct MovementInput {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool jump = false;
    bool sneak = false;
};

bool key_down(events::JniContext* jni, jobject key) noexcept {
    if (key == nullptr) {
        return false;
    }
    const bool down = minecraft::key_binding_is_pressed(jni, key);
    jni->env->DeleteLocalRef(key);
    return down;
}

// MovementInputEvent.fromKeyboard
MovementInput read_keyboard(events::JniContext* jni, jobject settings) noexcept {
    MovementInput in{};
    in.forward = key_down(jni, minecraft::settings_get_key_forward(jni, settings));
    in.back = key_down(jni, minecraft::settings_get_key_back(jni, settings));
    in.left = key_down(jni, minecraft::settings_get_key_left(jni, settings));
    in.right = key_down(jni, minecraft::settings_get_key_right(jni, settings));
    in.jump = key_down(jni, minecraft::settings_get_key_jump(jni, settings));
    in.sneak = key_down(jni, minecraft::settings_get_key_sneak(jni, settings));
    // MovementInputEvent(forward > 0, forward < 0, strafe > 0, strafe < 0, ...)
    if (in.forward && in.back) {
        in.forward = in.back = false;
    }
    if (in.left && in.right) {
        in.left = in.right = false;
    }
    return in;
}

int32_t camera_speed() noexcept {
    return std::clamp(static_cast<int32_t>(std::lround(g_state.speed.value)), 1, 9);
}

// Entity.moveFlying
void move_flying(math::Vec3d* motion, float yaw, float strafe, float forward, float friction) noexcept {
    float f = strafe * strafe + forward * forward;
    if (f >= 1.0E-4f) {
        f = std::sqrt(f);
        if (f < 1.0f) {
            f = 1.0f;
        }
        f = friction / f;
        strafe *= f;
        forward *= f;
        const float s = ar::mh_sin(yaw * static_cast<float>(math::kPi) / 180.0f);
        const float c = ar::mh_cos(yaw * static_cast<float>(math::kPi) / 180.0f);
        motion->x += static_cast<double>(strafe * c - forward * s);
        motion->z += static_cast<double>(forward * c + strafe * s);
    }
}

// FreecamCamera.handleKeyboard + SimulationPlayer.tick (EntityPlayer / EntityLivingBase movement of one tick).
void simulation_tick(events::JniContext* jni, const Ids& id, jobject cam, float forward_in, float strafe_in,
                     bool jump, bool sneak) noexcept {
    JNIEnv* env = jni->env;
    Simulation& sim = g_state.sim;
    const int32_t speed = camera_speed();
    const double movement_speed = speed * 0.14 - (speed >= 4 ? speed * 0.05 : 0.0);
    const float ai_move_speed = static_cast<float>(movement_speed);
    const float fly_speed = static_cast<float>(movement_speed / 2.54);
    const bool sprint_key_down = forward_in != 0.0f || strafe_in != 0.0f;
    const bool on_ground = minecraft::entity_is_on_ground(jni, cam);
    const bool collided_horizontally = env->GetBooleanField(cam, id.collided_horizontally) != JNI_FALSE;
    const float yaw = minecraft::entity_get_rotation(jni, cam).x;
    math::Vec3d m = minecraft::entity_get_motion(jni, cam);

    // handleKeyboard: double-tap jump toggles flight, vertical flight input
    const bool old_jump = sim.jump_input;
    sim.jump_input = jump;
    if (!old_jump && jump) {
        if (sim.fly_toggle_timer != 0) {
            sim.flying = !sim.flying;
            sim.fly_toggle_timer = 0;
        } else {
            sim.fly_toggle_timer = 7;
        }
    }
    if (sim.flying) {
        const float vertical = static_cast<float>(movement_speed / 2.54 * 3.0);
        if (sneak) {
            m.y -= vertical;
        }
        if (jump) {
            m.y += vertical;
        }
        if (on_ground) {
            sim.flying = false;
        }
    }

    // Entity.onEntityUpdate
    const math::Vec3d pos = minecraft::entity_get_position(jni, cam);
    set_tick_positions(jni, id, cam, pos);

    // SimulationPlayer.onLivingUpdate (sprinting)
    if (sim.sprinting_ticks_left > 0 && --sim.sprinting_ticks_left == 0) {
        sim.sprinting = false;
    }
    if (!sim.sprinting && forward_in >= 0.8f && sprint_key_down) {
        sim.sprinting = true;
        sim.sprinting_ticks_left = 600;
    }
    if (sim.sprinting && (forward_in < 0.8f || collided_horizontally)) {
        sim.sprinting = false;
        sim.sprinting_ticks_left = 0;
    }
    // EntityPlayer.onLivingUpdate
    if (sim.fly_toggle_timer > 0) {
        --sim.fly_toggle_timer;
    }
    // EntityLivingBase.onLivingUpdate
    if (sim.jump_ticks > 0) {
        --sim.jump_ticks;
    }
    if (std::abs(m.x) < 0.005) {
        m.x = 0.0;
    }
    if (std::abs(m.y) < 0.005) {
        m.y = 0.0;
    }
    if (std::abs(m.z) < 0.005) {
        m.z = 0.0;
    }
    if (jump) {
        if (on_ground && sim.jump_ticks == 0) { // EntityLivingBase.jump
            m.y = 0.41999998688697815;
            if (sim.sprinting) {
                const float f = yaw * 0.017453292f;
                m.x -= static_cast<double>(ar::mh_sin(f) * 0.2f);
                m.z += static_cast<double>(ar::mh_cos(f) * 0.2f);
            }
            sim.jump_ticks = 10;
        }
    } else {
        sim.jump_ticks = 0;
    }
    const float move_strafe = strafe_in * 0.98f;
    const float move_forward = forward_in * 0.98f;

    // EntityPlayer.moveEntityWithHeading -> EntityLivingBase.moveEntityWithHeading
    const double motion_y_before = m.y;
    const float jump_movement_factor =
        sim.flying ? fly_speed * static_cast<float>(sim.sprinting ? 2 : 1) : sim.jump_movement_factor;
    const float f4 = on_ground ? kGroundSlipperiness * 0.91f : 0.91f;
    const float accel = 0.16277136f / (f4 * f4 * f4);
    move_flying(&m, yaw, move_strafe, move_forward, on_ground ? ai_move_speed * accel : jump_movement_factor);
    minecraft::entity_set_motion(jni, cam, m);
    env->CallVoidMethod(cam, id.move_entity, m.x, m.y, m.z);
    if (ajni::clear_exception(jni)) {
        return;
    }
    m = minecraft::entity_get_motion(jni, cam); // moveEntity clears blocked components
    m.y -= 0.08;
    m.y *= 0.9800000190734863;
    m.x *= f4;
    m.z *= f4;
    if (sim.flying) {
        m.y = motion_y_before * 0.6;
    }
    minecraft::entity_set_motion(jni, cam, m);

    // EntityPlayer.onLivingUpdate after the movement: jumpMovementFactor of the next tick
    sim.jump_movement_factor = 0.02f;
    if (sim.sprinting) {
        sim.jump_movement_factor = static_cast<float>(0.02f + 0.02f * 0.3);
    }
}

// FreecamModule.handleMotion -> handleKeys (+ resetBalance / simulation tick)
void tick_camera(events::GameContext* ctx) noexcept {
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    const Ids& id = ids(jni);
    jobject cam = g_state.camera;
    jobject settings = minecraft::mc_get_game_settings(jni, ctx->minecraft);
    if (settings == nullptr) {
        return;
    }
    const MovementInput in = read_keyboard(jni, settings);

    double forward = (in.forward ? 1.0 : 0.0) - (in.back ? 1.0 : 0.0);
    double strafe = (in.left ? 1.0 : 0.0) - (in.right ? 1.0 : 0.0);
    jobject sprint_key = minecraft::settings_get_key_sprint(jni, settings);
    const bool sprint = sprint_key != nullptr && minecraft::key_binding_is_physically_down(jni, sprint_key);
    if (sprint_key != nullptr) {
        env->DeleteLocalRef(sprint_key);
    }
    env->DeleteLocalRef(settings);

    double target = 1.0;
    if (sprint) {
        target += (++g_state.sprint_ticks < 26 ? 0.4 : 0.7) * static_cast<double>(g_state.speed_ramp + 1.0f);
    } else {
        g_state.sprint_ticks = 0;
    }
    if (target < static_cast<double>(g_state.speed_ramp)) {
        g_state.speed_ramp = std::max(0.0f, g_state.speed_ramp - 0.5f);
    } else {
        const double ramp = static_cast<double>(g_state.speed_ramp) + 0.1;
        g_state.speed_ramp = static_cast<float>(std::min(target, ramp + (target - ramp) * 0.2000000029802322));
    }
    const double speed = camera_speed() * 10.0 / 20.0 * std::max(0.7, static_cast<double>(g_state.speed_ramp));
    const double length = std::hypot(forward, strafe);
    if (length > 1.0) {
        forward /= length;
        strafe /= length;
    }

    if (g_state.no_clip.value) {
        forward *= speed;
        strafe *= speed;
        const math::Vec3d pos = minecraft::entity_get_position(jni, cam);
        const float cam_yaw = minecraft::entity_get_rotation(jni, cam).x;
        const double y = in.sneak ? pos.y - speed : (in.jump ? pos.y + speed : pos.y);
        const double yaw = static_cast<double>(cam_yaw) * math::kPi / 180.0;
        const double strafe_yaw = static_cast<double>(cam_yaw - 90.0f) * math::kPi / 180.0;
        const double x = pos.x - std::sin(yaw) * forward - std::sin(strafe_yaw) * strafe;
        const double z = pos.z + std::cos(yaw) * forward + std::cos(strafe_yaw) * strafe;
        set_tick_positions(jni, id, cam, pos);
        env->CallVoidMethod(cam, id.set_position, x, y, z);
        minecraft::entity_set_motion(jni, cam, math::make_vec3(0.0, 0.0, 0.0)); // FreecamCamera.setPosition
        ajni::clear_exception(jni);
        return;
    }

    // resetBalance -> camera.strafe / forward / jump / sneak, then handleKeyboard + simulation.tick
    float cam_strafe = (in.left ? 1.0f : 0.0f) - (in.right ? 1.0f : 0.0f);
    float cam_forward = (in.forward ? 1.0f : 0.0f) - (in.back ? 1.0f : 0.0f);
    if (in.sneak) {
        cam_strafe *= 0.3f;
        cam_forward *= 0.3f;
    }
    simulation_tick(jni, id, cam, cam_forward, cam_strafe, in.jump, in.sneak);
}

// ---------------------------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------------------------

void on_module_state_changed(void* raw) noexcept {
    const auto* ev = static_cast<const events::ModuleStateChangedEvent*>(raw);
    if (ev->module != &g_state.base) {
        return;
    }
    events::JniContext* jni = ev->ctx != nullptr ? ev->ctx->jni : nullptr;
    if (ev->enabled) {
        destroy_camera(jni);
        if (!spawn_camera(jni)) {
            request_disable(ev->ctx); // FreecamModule.toggle: enabled only with a player and a world
        }
    } else {
        destroy_camera(jni);
    }
}

void on_pre_living_update(void* raw) noexcept {
    const auto* ev = static_cast<const events::ContextEvent*>(raw);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->jni->env == nullptr) {
        return;
    }
    JNIEnv* env = ctx->jni->env;
    if (env->PushLocalFrame(32) != 0) {
        ajni::clear_exception(ctx->jni);
        return;
    }
    if (validate_camera(ctx) && g_state.camera != nullptr && ids(ctx->jni).ok()) {
        tick_camera(ctx);
    }
    ajni::clear_exception(ctx->jni);
    env->PopLocalFrame(nullptr);
}

// The camera is the render view entity, so getMouseOver traces from the camera: clear the result so the player
// never breaks / uses / picks what the camera looks at (Adjust cancels the attack and use key bindings).
void on_game_tick(void* raw) noexcept {
    const auto* ev = static_cast<const events::ContextEvent*>(raw);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->minecraft == nullptr || g_state.camera == nullptr) {
        return;
    }
    if (ctx->world == nullptr || ctx->local_player == nullptr) {
        // Disconnected (Minecraft.loadWorld(null)): release the camera and its world. The module stays enabled
        // and spawns a new camera in the next world, like Adjust's setCamera() on loadWorld.
        destroy_camera(ctx->jni);
        return;
    }
    minecraft::mc_set_object_mouse_over(ctx->jni, ctx->minecraft, nullptr);
}

void on_reach_query(void* raw) noexcept {
    auto* ev = static_cast<events::ReachRaycastQueryEvent*>(raw);
    if (g_state.camera != nullptr) {
        ev->override_raycast = false; // keep objectMouseOver empty
    }
}

// FreecamModule.handleInput: keyBindAttack / keyBindUseItem never reach the player.
void on_pre_attack_or_use(void* raw) noexcept {
    auto* ev = static_cast<events::AttackOrUseEntityEvent*>(raw);
    if (g_state.camera != nullptr) {
        ev->cancelled = true;
    }
}

// Defence in depth for the MovementInputFromOptions patch: the player gets no horizontal movement input.
void on_pre_move_with_heading(void* raw) noexcept {
    auto* ev = static_cast<events::MoveWithHeadingEvent*>(raw);
    if (g_state.camera != nullptr) {
        ev->move_strafe = 0.0f;
        ev->move_forward = 0.0f;
    }
}

// FreecamModule.render(Render3DEvent): outline of the block the camera looks at.
void on_render_world_3d(void* raw) noexcept {
    const auto* ev = static_cast<const events::RenderWorld3DEvent*>(raw);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->world == nullptr || g_state.camera == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    minecraft::mc_set_object_mouse_over(jni, ctx->minecraft, nullptr); // per-frame getMouseOver traced the camera
    if (ar::classes(jni) == nullptr || env->PushLocalFrame(16) != 0) {
        ajni::clear_exception(jni);
        return;
    }
    jobject hit = minecraft::living_ray_trace(jni, g_state.camera, 85.0, ev->partial_ticks);
    if (!ajni::clear_exception(jni) && hit != nullptr && minecraft::mop_get_type_ordinal(jni, hit) == 1) {
        const minecraft::Vec3i pos = minecraft::mop_get_block_pos_vec3i(jni, hit);
        ar::BlockInfo info{};
        math::AABB box{};
        if (ar::block_info_at(jni, ctx->world, pos.x, pos.y, pos.z, &info) && info.id != 0 &&
            ar::block_selected_box(jni, ctx->world, pos.x, pos.y, pos.z, &box)) {
            const math::Vec3d cam = ev->render_pos;
            box = ar::box_offset(
                ar::box_expand(box, 0.0020000000949949026, 0.0020000000949949026, 0.0020000000949949026), -cam.x,
                -cam.y, -cam.z);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(0.0f, 0.0f, 0.0f, 0.4f);
            glLineWidth(2.0f);
            glDisable(GL_TEXTURE_2D);
            glDepthMask(GL_FALSE);
            glEnable(GL_DEPTH_TEST);
            ar::draw_outlined_box(box);
            glDepthMask(GL_TRUE);
            glDisable(GL_DEPTH_TEST); // back to the world-pass state (depth test off, texturing off, blending on)
            glLineWidth(1.0f);
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }
    ajni::clear_exception(jni);
    env->PopLocalFrame(nullptr);
}

void on_client_shutdown(void* raw) noexcept {
    const auto* ev = static_cast<const events::ContextEvent*>(raw);
    destroy_camera(ev->ctx != nullptr ? ev->ctx->jni : nullptr);
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_state.speed.render(gui, renderer);
    g_state.no_clip.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.speed.visit(archive, "speed");
    g_state.no_clip.visit(archive, "no_clip");
}

} // namespace

events::Module* get_adjust_freecam_module() noexcept {
    return &g_state.base;
}

bool adjust_freecam_active(events::JniContext* jni) noexcept {
    if (g_state.camera == nullptr) {
        return false;
    }
    if (!running()) {
        destroy_camera(jni); // module stopped without a state-changed event reaching us
        return false;
    }
    return true;
}

jobject adjust_freecam_camera() noexcept {
    return g_state.camera;
}

math::Vec2f adjust_freecam_rotation(events::JniContext* jni) noexcept {
    if (g_state.camera == nullptr || jni == nullptr || jni->env == nullptr) {
        return {};
    }
    return minecraft::entity_get_rotation(jni, g_state.camera);
}

bool adjust_freecam_consume_mouse(events::JniContext* jni, float yaw_arg, float pitch_arg) noexcept {
    if (jni == nullptr || jni->env == nullptr || !adjust_freecam_active(jni)) {
        return false;
    }
    // cameraOverride.setAngles(dx, dy * invert); FreecamModule.render keeps prevRotation == rotation.
    minecraft::entity_set_angles(jni, g_state.camera, yaw_arg, pitch_arg);
    if (!ajni::clear_exception(jni)) {
        minecraft::entity_set_prev_rotation(jni, g_state.camera, minecraft::entity_get_rotation(jni, g_state.camera));
    }
    return true;
}

} // namespace slinky::modules
