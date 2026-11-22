// Adjust camera hooks (Minecraft 1.8.9) for Third Person and Freecam:
//   * EntityRenderer.orientCamera: Vec3.distanceTo                -> View Clip (camera ignores blocks)
//   * EntityRenderer.updateCameraAndRender: thePlayer.setAngles   -> mouse look drives the Freecam camera / the
//                                                                    free look rotation instead of the player
//   * EntityRenderer.renderWorldPass: this.renderHand             -> no first person hand while Freecam is active
//   * MovementInputFromOptions.updatePlayerMoveState: KeyBinding.isKeyDown
//                                                                 -> no player movement input while Freecam is active
//   * EntityPlayerSP.onUpdateWalkingPlayer / updateEntityActionState / onLivingUpdate: this.isCurrentViewEntity
//                                                                 -> the player stays the logical view entity while
//                                                                    the camera renders (C03 packets keep being sent
//                                                                    with the unchanged position / rotation, and the
//                                                                    zeroed movement input is applied)
//   * GuiIngame.renderTooltip / renderPlayerStats: mc.getRenderViewEntity
//                                                                 -> hotbar and health / food / armor keep showing the
//                                                                    player while the camera is the view entity
// plus the bridge functions declared in camera_hooks.hpp for the base hooks (orientCamera, ActiveRenderInfo,
// RenderManager.playerViewX/Y).
//
// Each native calls the original member when its feature is not active (Java exceptions stay pending).
//
// How Adjust keeps the player still (studied in its patched sources): MovementInputFromOptions clears all movement
// keys while Freecam is enabled, mouse deltas go to the camera (EntityRenderer.updateMouseInput cameraOverride
// branch) and the attack / use bindings are cancelled; the player keeps ticking and sending its normal (standing)
// movement packets. The NetworkManager patch only delays flushes for the Housing tool controller (not ported).

#include "adjust/hooks/camera_hooks.hpp"

#include <limits>

#include "adjust/hooks/hooks.hpp"
#include "adjust/jni_lookup.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/freecam/freecam.hpp"
#include "modules/adjust/render/third_person/third_person.hpp"

namespace slinky::adjust::hooks {

namespace {

using bytecode::ExtraHookSpec;
using bytecode::HookInsnKind;

// ---------------------------------------------------------------------------------------------------------------
// JNI members
// ---------------------------------------------------------------------------------------------------------------

struct Ids {
    jfieldID third_person_distance = nullptr;      // EntityRenderer.thirdPersonDistance (F)
    jfieldID third_person_distance_temp = nullptr; // EntityRenderer.thirdPersonDistanceTemp (F)
    jmethodID render_hand = nullptr;               // EntityRenderer.renderHand(FI)V (private)
    jmethodID vec3_distance_to = nullptr;          // Vec3.distanceTo(Vec3)D
    jmethodID key_is_down = nullptr;               // KeyBinding.isKeyDown()Z
    jmethodID is_current_view_entity = nullptr;    // EntityPlayerSP.isCurrentViewEntity()Z (protected)
    jmethodID get_render_view_entity = nullptr;    // Minecraft.getRenderViewEntity()Entity
};

const Ids& ids(events::JniContext* jni) noexcept {
    static const Ids s_ids = [&] {
        Ids out{};
        const jclass renderer = jni::find_class(jni, "bfk", "net/minecraft/client/renderer/EntityRenderer");
        const jclass vec3 = jni::find_class(jni, "aui", "net/minecraft/util/Vec3");
        const jclass key_binding = jni::find_class(jni, "avb", "net/minecraft/client/settings/KeyBinding");
        const jclass player_sp = jni::find_class(jni, "bew", "net/minecraft/client/entity/EntityPlayerSP");
        const jclass minecraft = jni::find_class(jni, "ave", "net/minecraft/client/Minecraft");
        out.third_person_distance =
            jni::field(jni, renderer, {"q", "thirdPersonDistance", "field_78490_B"}, "F", "F");
        out.third_person_distance_temp =
            jni::field(jni, renderer, {"r", "thirdPersonDistanceTemp", "field_78491_C"}, "F", "F");
        out.render_hand = jni::method(jni, renderer, {"b", "renderHand", "func_78476_b"}, "(FI)V", "(FI)V");
        out.vec3_distance_to = jni::method(jni, vec3, {"f", "distanceTo", "func_72438_d"}, "(Laui;)D",
                                           "(Lnet/minecraft/util/Vec3;)D");
        out.key_is_down = jni::method(jni, key_binding, {"d", "isKeyDown", "func_151470_d"}, "()Z", "()Z");
        out.is_current_view_entity =
            jni::method(jni, player_sp, {"A", "isCurrentViewEntity", "func_175160_A"}, "()Z", "()Z");
        out.get_render_view_entity = jni::method(jni, minecraft, {"ac", "getRenderViewEntity", "func_175606_aa"},
                                                 "()Lpk;", "()Lnet/minecraft/entity/Entity;");
        return out;
    }();
    return s_ids;
}

// True if the render view entity is the local player (free look only applies to EntityPlayerSP).
bool view_entity_is_player(events::JniContext* jni, jobject* out_player) noexcept {
    JNIEnv* env = jni->env;
    jobject mc = minecraft::mc_get_instance(jni);
    if (mc == nullptr) {
        return false;
    }
    jobject player = minecraft::mc_get_player(jni, mc);
    jobject view = minecraft::mc_get_render_view_entity(jni, mc);
    const bool same = player != nullptr && view != nullptr && env->IsSameObject(player, view) != JNI_FALSE;
    if (view != nullptr) {
        env->DeleteLocalRef(view);
    }
    env->DeleteLocalRef(mc);
    if (same && out_player != nullptr) {
        *out_player = player;
    } else if (player != nullptr) {
        env->DeleteLocalRef(player);
    }
    return same;
}

// ---------------------------------------------------------------------------------------------------------------
// EntityRenderer.orientCamera: Vec3.distanceTo(Vec3)D (View Clip)
// ---------------------------------------------------------------------------------------------------------------

// Vanilla clips the third person distance with `d7 = hit.hitVec.distanceTo(eye); if (d7 < d3) d3 = d7;` for each
// of its 8 camera rays. Adjust skips those rays while View Clip is enabled; returning +inf keeps d3 unchanged.
jdouble JNICALL native_view_clip_distance(JNIEnv* env, jclass /*cls*/, jobject vec, jobject other) noexcept {
    events::JniContext jni = jni_for(env);
    if (modules::adjust_third_person_view_clip(&jni)) {
        return std::numeric_limits<double>::infinity();
    }
    const jmethodID mid = ids(&jni).vec3_distance_to;
    if (mid == nullptr) {
        return std::numeric_limits<double>::infinity();
    }
    return env->CallDoubleMethod(vec, mid, other); // a Java exception stays pending for the caller
}

// ---------------------------------------------------------------------------------------------------------------
// EntityRenderer.updateCameraAndRender: thePlayer.setAngles(FF)V (mouse look)
// ---------------------------------------------------------------------------------------------------------------

void JNICALL native_set_angles(JNIEnv* env, jclass /*cls*/, jobject entity, jfloat yaw, jfloat pitch) noexcept {
    events::JniContext jni = jni_for(env);
    if (modules::adjust_freecam_consume_mouse(&jni, yaw, pitch)) {
        return; // cameraOverride.setAngles(...)
    }
    if (modules::adjust_third_person_consume_mouse(&jni, yaw, pitch)) {
        return; // MouseDeltaEvent consumed by ThirdPersonModule.onMouseInput
    }
    minecraft::entity_set_angles(&jni, entity, yaw, pitch);
}

// ---------------------------------------------------------------------------------------------------------------
// EntityRenderer.renderWorldPass: this.renderHand(FI)V
// ---------------------------------------------------------------------------------------------------------------

void JNICALL native_render_hand(JNIEnv* env, jclass /*cls*/, jobject entity_renderer, jfloat partial_ticks,
                                jint pass) noexcept {
    events::JniContext jni = jni_for(env);
    if (modules::adjust_freecam_active(&jni)) {
        return; // the camera holds no item (its tool items are not ported) and renders no arm
    }
    const jmethodID mid = ids(&jni).render_hand;
    if (mid != nullptr) {
        env->CallVoidMethod(entity_renderer, mid, partial_ticks, pass);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// MovementInputFromOptions.updatePlayerMoveState: KeyBinding.isKeyDown()Z
// ---------------------------------------------------------------------------------------------------------------

jboolean JNICALL native_movement_key_down(JNIEnv* env, jclass /*cls*/, jobject key_binding) noexcept {
    events::JniContext jni = jni_for(env);
    if (modules::adjust_freecam_active(&jni)) {
        return JNI_FALSE; // forward / back / left / right / jump / sneak = false
    }
    const jmethodID mid = ids(&jni).key_is_down;
    if (mid == nullptr) {
        return minecraft::key_binding_is_pressed(&jni, key_binding) ? JNI_TRUE : JNI_FALSE;
    }
    return env->CallBooleanMethod(key_binding, mid);
}

// ---------------------------------------------------------------------------------------------------------------
// EntityPlayerSP: this.isCurrentViewEntity()Z
// ---------------------------------------------------------------------------------------------------------------

jboolean JNICALL native_is_current_view_entity(JNIEnv* env, jclass /*cls*/, jobject player) noexcept {
    events::JniContext jni = jni_for(env);
    if (modules::adjust_freecam_active(&jni)) {
        return JNI_TRUE; // Adjust's camera is a separate override: the player remains the view entity
    }
    const jmethodID mid = ids(&jni).is_current_view_entity;
    if (mid != nullptr) {
        return env->CallBooleanMethod(player, mid);
    }
    jobject mc = minecraft::mc_get_instance(&jni);
    jobject view = mc != nullptr ? minecraft::mc_get_render_view_entity(&jni, mc) : nullptr;
    return (view != nullptr && env->IsSameObject(view, player) != JNI_FALSE) ? JNI_TRUE : JNI_FALSE;
}

// ---------------------------------------------------------------------------------------------------------------
// GuiIngame.renderTooltip / renderPlayerStats: mc.getRenderViewEntity()
// ---------------------------------------------------------------------------------------------------------------

// Both callers only use the result with `instanceof EntityPlayer` and `checkcast EntityPlayer`, so returning it as
// java.lang.Object keeps the methods verifiable.
jobject JNICALL native_hud_view_entity(JNIEnv* env, jclass /*cls*/, jobject mc) noexcept {
    events::JniContext jni = jni_for(env);
    if (modules::adjust_freecam_active(&jni)) {
        jobject player = minecraft::mc_get_player(&jni, mc);
        if (player != nullptr) {
            return player;
        }
    }
    const jmethodID mid = ids(&jni).get_render_view_entity;
    if (mid != nullptr) {
        return env->CallObjectMethod(mc, mid);
    }
    return minecraft::mc_get_render_view_entity(&jni, mc);
}

// ---------------------------------------------------------------------------------------------------------------
// Hook table
// ---------------------------------------------------------------------------------------------------------------

constexpr const char* kEntityRenderer = "net/minecraft/client/renderer/EntityRenderer";
constexpr const char* kVec3 = "net/minecraft/util/Vec3";
constexpr const char* kEntity = "net/minecraft/entity/Entity";
constexpr const char* kEntityPlayerSP = "net/minecraft/client/entity/EntityPlayerSP";
constexpr const char* kMovementInputFromOptions = "net/minecraft/util/MovementInputFromOptions";
constexpr const char* kKeyBinding = "net/minecraft/client/settings/KeyBinding";
constexpr const char* kGuiIngame = "net/minecraft/client/gui/GuiIngame";
constexpr const char* kMinecraft = "net/minecraft/client/Minecraft";

constexpr bytecode::ExtraMemberNames kSetAngles{
    "c", "(FF)V", "setAngles", "(FF)V", "func_70082_c", "(FF)V"};
constexpr bytecode::ExtraMemberNames kUpdateCameraAndRender{
    "a", "(FJ)V", "updateCameraAndRender", "(FJ)V", "func_181560_a", "(FJ)V"};
constexpr bytecode::ExtraMemberNames kIsCurrentViewEntity{
    "A", "()Z", "isCurrentViewEntity", "()Z", "func_175160_A", "()Z"};
constexpr bytecode::ExtraMemberNames kGetRenderViewEntity{
    "ac", "()Lpk;", "getRenderViewEntity", "()Lnet/minecraft/entity/Entity;",
    "func_175606_aa", "()Lnet/minecraft/entity/Entity;"};

// EntityRenderer.orientCamera(F)V: `movingobjectposition.hitVec.distanceTo(new Vec3(d0, d1, d2))`.
const ExtraHookSpec kHookViewClip{
    "bfk", kEntityRenderer,
    {"f", "(F)V", "orientCamera", "(F)V", "func_78467_g", "(F)V"},
    "aui", kVec3,
    {"f", "(Laui;)D", "distanceTo", "(Lnet/minecraft/util/Vec3;)D",
     "func_72438_d", "(Lnet/minecraft/util/Vec3;)D"},
    HookInsnKind::InvokeInstance,
    "adjustCameraViewClipDistance", "(Ljava/lang/Object;Ljava/lang/Object;)D",
    reinterpret_cast<void*>(&native_view_clip_distance),
};

// EntityRenderer.updateCameraAndRender(FJ)V: `this.mc.thePlayer.setAngles(f2, f3 * i)` (normal and smooth camera).
// javac references the member through the receiver's static type (EntityPlayerSP); the Entity-owner variant
// covers builds that reference the declaring class.
const ExtraHookSpec kHookSetAnglesPlayerSP{
    "bfk", kEntityRenderer,
    kUpdateCameraAndRender,
    "bew", kEntityPlayerSP,
    kSetAngles,
    HookInsnKind::InvokeInstance,
    "adjustCameraSetAngles", "(Ljava/lang/Object;FF)V",
    reinterpret_cast<void*>(&native_set_angles),
};

const ExtraHookSpec kHookSetAnglesEntity{
    "bfk", kEntityRenderer,
    kUpdateCameraAndRender,
    "pk", kEntity,
    kSetAngles,
    HookInsnKind::InvokeInstance,
    "adjustCameraSetAngles", "(Ljava/lang/Object;FF)V",
    reinterpret_cast<void*>(&native_set_angles),
};

// EntityRenderer.renderWorldPass(IFJ)V: `this.renderHand(partialTicks, pass)` (private -> invokespecial).
const ExtraHookSpec kHookRenderHand{
    "bfk", kEntityRenderer,
    {"a", "(IFJ)V", "renderWorldPass", "(IFJ)V", "func_175068_a", "(IFJ)V"},
    "bfk", kEntityRenderer,
    {"b", "(FI)V", "renderHand", "(FI)V", "func_78476_b", "(FI)V"},
    HookInsnKind::InvokeInstance,
    "adjustCameraRenderHand", "(Ljava/lang/Object;FI)V",
    reinterpret_cast<void*>(&native_render_hand),
};

// MovementInputFromOptions.updatePlayerMoveState()V: `this.gameSettings.keyBind<X>.isKeyDown()` (6 calls).
const ExtraHookSpec kHookMovementKeys{
    "bev", kMovementInputFromOptions,
    {"a", "()V", "updatePlayerMoveState", "()V", "func_78898_a", "()V"},
    "avb", kKeyBinding,
    {"d", "()Z", "isKeyDown", "()Z", "func_151470_d", "()Z"},
    HookInsnKind::InvokeInstance,
    "adjustCameraMovementKeyDown", "(Ljava/lang/Object;)Z",
    reinterpret_cast<void*>(&native_movement_key_down),
};

// EntityPlayerSP: `if (this.isCurrentViewEntity())` in onUpdateWalkingPlayer (C03 movement packets),
// updateEntityActionState (movement input -> moveStrafing / moveForward / isJumping) and onLivingUpdate (creative
// flight up / down).
const ExtraHookSpec kHookViewEntityWalking{
    "bew", kEntityPlayerSP,
    {"p", "()V", "onUpdateWalkingPlayer", "()V", "func_175161_p", "()V"},
    "bew", kEntityPlayerSP,
    kIsCurrentViewEntity,
    HookInsnKind::InvokeInstance,
    "adjustCameraIsCurrentViewEntity", "(Ljava/lang/Object;)Z",
    reinterpret_cast<void*>(&native_is_current_view_entity),
};

const ExtraHookSpec kHookViewEntityAction{
    "bew", kEntityPlayerSP,
    {"bK", "()V", "updateEntityActionState", "()V", "func_70626_be", "()V"},
    "bew", kEntityPlayerSP,
    kIsCurrentViewEntity,
    HookInsnKind::InvokeInstance,
    "adjustCameraIsCurrentViewEntity", "(Ljava/lang/Object;)Z",
    reinterpret_cast<void*>(&native_is_current_view_entity),
};

const ExtraHookSpec kHookViewEntityLiving{
    "bew", kEntityPlayerSP,
    {"m", "()V", "onLivingUpdate", "()V", "func_70636_d", "()V"},
    "bew", kEntityPlayerSP,
    kIsCurrentViewEntity,
    HookInsnKind::InvokeInstance,
    "adjustCameraIsCurrentViewEntity", "(Ljava/lang/Object;)Z",
    reinterpret_cast<void*>(&native_is_current_view_entity),
};

// GuiIngame.renderTooltip(ScaledResolution, float) (hotbar) and renderPlayerStats(ScaledResolution) (health, food,
// armor, air): `this.mc.getRenderViewEntity() instanceof EntityPlayer` / `(EntityPlayer)this.mc.getRenderViewEntity()`.
const ExtraHookSpec kHookHudHotbar{
    "avo", kGuiIngame,
    {"a", "(Lavr;F)V",
     "renderTooltip", "(Lnet/minecraft/client/gui/ScaledResolution;F)V",
     "func_180479_a", "(Lnet/minecraft/client/gui/ScaledResolution;F)V"},
    "ave", kMinecraft,
    kGetRenderViewEntity,
    HookInsnKind::InvokeInstance,
    "adjustCameraHudViewEntity", "(Ljava/lang/Object;)Ljava/lang/Object;",
    reinterpret_cast<void*>(&native_hud_view_entity),
};

const ExtraHookSpec kHookHudStats{
    "avo", kGuiIngame,
    {"d", "(Lavr;)V",
     "renderPlayerStats", "(Lnet/minecraft/client/gui/ScaledResolution;)V",
     "func_180477_d", "(Lnet/minecraft/client/gui/ScaledResolution;)V"},
    "ave", kMinecraft,
    kGetRenderViewEntity,
    HookInsnKind::InvokeInstance,
    "adjustCameraHudViewEntity", "(Ljava/lang/Object;)Ljava/lang/Object;",
    reinterpret_cast<void*>(&native_hud_view_entity),
};

} // namespace

const bytecode::ExtraHookSpec* const* camera_hooks() noexcept {
    static const ExtraHookSpec* const s_hooks[] = {
        &kHookViewClip,
        &kHookSetAnglesPlayerSP,
        &kHookSetAnglesEntity,
        &kHookRenderHand,
        &kHookMovementKeys,
        &kHookViewEntityWalking,
        &kHookViewEntityAction,
        &kHookViewEntityLiving,
        &kHookHudHotbar,
        &kHookHudStats,
        nullptr,
    };
    return s_hooks;
}

// ---------------------------------------------------------------------------------------------------------------
// Bridge functions for the base hooks (see camera_hooks.hpp)
// ---------------------------------------------------------------------------------------------------------------

void camera_orient_begin(events::JniContext* jni, jobject entity_renderer, CameraOrientState* state) noexcept {
    if (state == nullptr) {
        return;
    }
    *state = CameraOrientState{};
    if (jni == nullptr || jni->env == nullptr || entity_renderer == nullptr || !jni::is_1_8(jni)) {
        return;
    }
    JNIEnv* env = jni->env;
    const Ids& id = ids(jni);

    // EntityRenderer.orientCamera: `if (ThirdPersonModule enabled) d3 += distance` - d3 is interpolated between
    // thirdPersonDistanceTemp and thirdPersonDistance, so offsetting both offsets d3.
    const float extra = modules::adjust_third_person_extra_distance();
    if (extra != 0.0f && id.third_person_distance != nullptr && id.third_person_distance_temp != nullptr) {
        state->saved_distance = env->GetFloatField(entity_renderer, id.third_person_distance);
        state->saved_distance_temp = env->GetFloatField(entity_renderer, id.third_person_distance_temp);
        env->SetFloatField(entity_renderer, id.third_person_distance, state->saved_distance + extra);
        env->SetFloatField(entity_renderer, id.third_person_distance_temp, state->saved_distance_temp + extra);
        state->distance_applied = true;
    }

    // Free look: curYaw / prevYaw / curPitch / prevPitch come from ThirdPersonModule for the local player.
    if (modules::adjust_third_person_free_looking() && !modules::adjust_freecam_active(jni)) {
        jobject player = nullptr;
        if (view_entity_is_player(jni, &player)) {
            state->saved_rotation = minecraft::entity_get_rotation(jni, player);
            state->saved_prev_rotation = minecraft::entity_get_prev_rotation(jni, player);
            const math::Vec2f rot = modules::adjust_third_person_free_look_rotation();
            minecraft::entity_set_rotation(jni, player, rot);
            minecraft::entity_set_prev_rotation(jni, player, rot);
            state->entity = player;
            state->rotation_applied = true;
        }
    }
}

void camera_orient_end(events::JniContext* jni, jobject entity_renderer, CameraOrientState* state) noexcept {
    if (state == nullptr || jni == nullptr || jni->env == nullptr) {
        return;
    }
    JNIEnv* env = jni->env;
    if (state->distance_applied && entity_renderer != nullptr) {
        const Ids& id = ids(jni);
        env->SetFloatField(entity_renderer, id.third_person_distance, state->saved_distance);
        env->SetFloatField(entity_renderer, id.third_person_distance_temp, state->saved_distance_temp);
        state->distance_applied = false;
    }
    if (state->rotation_applied && state->entity != nullptr) {
        minecraft::entity_set_rotation(jni, state->entity, state->saved_rotation);
        minecraft::entity_set_prev_rotation(jni, state->entity, state->saved_prev_rotation);
        state->rotation_applied = false;
    }
    if (state->entity != nullptr) {
        env->DeleteLocalRef(state->entity);
        state->entity = nullptr;
    }
}

bool camera_render_info_begin(events::JniContext* jni, jobject player, bool third_person_front,
                              CameraRenderInfoState* state) noexcept {
    if (state == nullptr) {
        return third_person_front;
    }
    *state = CameraRenderInfoState{};
    if (jni == nullptr || jni->env == nullptr || player == nullptr || !jni::is_1_8(jni)) {
        return third_person_front;
    }
    math::Vec2f rot{};
    bool front = third_person_front;
    if (modules::adjust_freecam_active(jni)) {
        rot = modules::adjust_freecam_rotation(jni); // updateRenderInfo(cameraOverride, ...)
    } else if (modules::adjust_third_person_free_looking()) {
        rot = modules::adjust_third_person_free_look_rotation();
        front = false; // thirdPersonView == 2 && !freeLooking
    } else {
        return third_person_front;
    }
    state->saved_rotation = minecraft::entity_get_rotation(jni, player);
    state->saved_prev_rotation = minecraft::entity_get_prev_rotation(jni, player);
    minecraft::entity_set_rotation(jni, player, rot);
    minecraft::entity_set_prev_rotation(jni, player, rot);
    state->rotation_applied = true;
    return front;
}

void camera_render_info_end(events::JniContext* jni, jobject player, const CameraRenderInfoState* state) noexcept {
    if (state == nullptr || !state->rotation_applied || jni == nullptr || jni->env == nullptr || player == nullptr) {
        return;
    }
    minecraft::entity_set_rotation(jni, player, state->saved_rotation);
    minecraft::entity_set_prev_rotation(jni, player, state->saved_prev_rotation);
}

bool camera_override_player_view_x(events::JniContext* jni, float* inout_pitch) noexcept {
    if (jni == nullptr || jni->env == nullptr || inout_pitch == nullptr) {
        return false;
    }
    if (modules::adjust_freecam_active(jni)) {
        return true; // computed by vanilla from the camera (the view entity)
    }
    if (modules::adjust_third_person_free_looking() && view_entity_is_player(jni, nullptr)) {
        *inout_pitch = modules::adjust_third_person_free_look_rotation().y;
        return true;
    }
    return false;
}

bool camera_override_player_view_y(events::JniContext* jni, float* inout_yaw) noexcept {
    if (jni == nullptr || jni->env == nullptr || inout_yaw == nullptr) {
        return false;
    }
    if (modules::adjust_freecam_active(jni)) {
        return true;
    }
    if (modules::adjust_third_person_free_looking() && view_entity_is_player(jni, nullptr)) {
        *inout_yaw = modules::adjust_third_person_free_look_rotation().x;
        return true;
    }
    return false;
}

} // namespace slinky::adjust::hooks
