// Adjust render hooks (Minecraft 1.8.9):
//   * GuiIngame.showCrosshair()           -> Adjust Crosshair replaces vanilla's crosshair
//   * RenderGlobal.drawSelectionBoundingBox -> Adjust Block Overlay replaces vanilla's selection outline
//   * RendererLivingEntity.renderModel     -> Adjust True Sight renders invisible players translucent
//
// Each native calls the original member when its feature is not active.

#include "adjust/hooks/hooks.hpp"

#include "adjust/jni_lookup.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/block_overlay/block_overlay.hpp"
#include "modules/adjust/render/crosshair/crosshair.hpp"
#include "modules/adjust/render/true_sight/true_sight.hpp"

namespace slinky::adjust::hooks {

namespace {

using bytecode::ExtraHookSpec;
using bytecode::HookInsnKind;

// ---------------------------------------------------------------------------------------------------------------
// GuiIngame.showCrosshair()Z
// ---------------------------------------------------------------------------------------------------------------

jboolean JNICALL native_show_crosshair(JNIEnv* env, jclass /*cls*/, jobject gui_ingame) noexcept {
    if (modules::adjust_crosshair_replaces_vanilla()) {
        return JNI_FALSE;
    }
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "avo", "net/minecraft/client/gui/GuiIngame");
    static const jmethodID mid = jni::method(&jni, cls, {"b", "showCrosshair", "func_175183_b"}, "()Z", "()Z");
    if (mid == nullptr) {
        return JNI_TRUE;
    }
    return env->CallBooleanMethod(gui_ingame, mid); // a Java exception stays pending for the caller
}

// ---------------------------------------------------------------------------------------------------------------
// RenderGlobal.drawSelectionBoundingBox(AxisAlignedBB)V (static)
// ---------------------------------------------------------------------------------------------------------------

void JNICALL native_draw_selection_bounding_box(JNIEnv* env, jclass /*cls*/, jobject aabb) noexcept {
    if (modules::adjust_block_overlay_replaces_vanilla_outline()) {
        return; // Block Overlay draws the looked-at block itself
    }
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "bfr", "net/minecraft/client/renderer/RenderGlobal");
    static const jmethodID mid = jni::static_method(
        &jni, cls, {"a", "drawSelectionBoundingBox", "func_181561_a"},
        "(Laug;)V", "(Lnet/minecraft/util/AxisAlignedBB;)V");
    if (mid != nullptr) {
        env->CallStaticVoidMethod(cls, mid, aabb);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// RendererLivingEntity.renderModel(EntityLivingBase, float x6)V
// ---------------------------------------------------------------------------------------------------------------

struct RenderModelIds {
    jmethodID render_model = nullptr;
    jmethodID is_invisible = nullptr;
    jmethodID set_invisible = nullptr;
    jclass gl_state = nullptr;
    jmethodID depth_mask = nullptr;
    jmethodID blend_func_separate = nullptr;
    jmethodID alpha_func = nullptr;
};

const RenderModelIds& render_model_ids(events::JniContext* jni) noexcept {
    static const RenderModelIds s_ids = [&] {
        RenderModelIds ids{};
        const jclass renderer = jni::find_class(jni, "bjl", "net/minecraft/client/renderer/entity/RendererLivingEntity");
        const jclass entity = jni::find_class(jni, "pk", "net/minecraft/entity/Entity");
        ids.render_model = jni::method(jni, renderer, {"a", "renderModel", "func_77036_a"},
                                       "(Lpr;FFFFFF)V", "(Lnet/minecraft/entity/EntityLivingBase;FFFFFF)V");
        ids.is_invisible = jni::method(jni, entity, {"ax", "isInvisible", "func_82150_aj"}, "()Z", "()Z");
        ids.set_invisible = jni::method(jni, entity, {"e", "setInvisible", "func_82142_c"}, "(Z)V", "(Z)V");
        ids.gl_state = jni::find_class(jni, "bfl", "net/minecraft/client/renderer/GlStateManager");
        ids.depth_mask = jni::static_method(jni, ids.gl_state, {"a", "depthMask", "func_179132_a"}, "(Z)V", "(Z)V");
        ids.blend_func_separate = jni::static_method(
            jni, ids.gl_state, {"a", "tryBlendFuncSeparate", "func_179120_a"}, "(IIII)V", "(IIII)V");
        ids.alpha_func = jni::static_method(jni, ids.gl_state, {"a", "alphaFunc", "func_179092_a"}, "(IF)V", "(IF)V");
        return ids;
    }();
    return s_ids;
}

void JNICALL native_render_model(JNIEnv* env, jclass /*cls*/, jobject renderer, jobject entity, jfloat limb_swing,
                                 jfloat limb_swing_amount, jfloat age_in_ticks, jfloat head_yaw, jfloat head_pitch,
                                 jfloat scale) noexcept {
    events::JniContext jni = jni_for(env);
    const RenderModelIds& ids = render_model_ids(&jni);
    if (ids.render_model == nullptr) {
        return;
    }
    const auto call_original = [&]() noexcept {
        env->CallVoidMethod(renderer, ids.render_model, entity, limb_swing, limb_swing_amount, age_in_ticks, head_yaw,
                            head_pitch, scale);
    };

    // True Sight: an invisible player is rendered as if visible, with the configured opacity (Adjust's
    // EntityOpacityEvent handling in its patched RendererLivingEntity).
    bool invisible = false;
    float body_alpha = 1.0f;
    if (ids.is_invisible != nullptr && ids.set_invisible != nullptr) {
        invisible = env->CallBooleanMethod(entity, ids.is_invisible) != JNI_FALSE;
    }
    const bool was_invisible = invisible;
    if (!was_invisible || !modules::adjust_true_sight_apply(&jni, entity, &invisible, &body_alpha) || invisible) {
        call_original();
        return;
    }

    const bool translucent = body_alpha < 1.0f;
    if (translucent && ids.depth_mask != nullptr && ids.blend_func_separate != nullptr && ids.alpha_func != nullptr) {
        minecraft::gl_state_color(&jni, 1.0f, 1.0f, 1.0f, body_alpha);
        env->CallStaticVoidMethod(ids.gl_state, ids.depth_mask, JNI_FALSE);
        minecraft::gl_state_enable_blend(&jni);
        env->CallStaticVoidMethod(ids.gl_state, ids.blend_func_separate, 770, 771, 1, 0);
        env->CallStaticVoidMethod(ids.gl_state, ids.alpha_func, 516, 0.003921569f);
    }

    env->CallVoidMethod(entity, ids.set_invisible, JNI_FALSE);
    call_original();
    env->CallVoidMethod(entity, ids.set_invisible, JNI_TRUE);

    if (translucent && ids.depth_mask != nullptr && ids.blend_func_separate != nullptr && ids.alpha_func != nullptr) {
        minecraft::gl_state_color(&jni, 1.0f, 1.0f, 1.0f, 1.0f);
        minecraft::gl_state_disable_blend(&jni);
        env->CallStaticVoidMethod(ids.gl_state, ids.alpha_func, 516, 0.1f);
        env->CallStaticVoidMethod(ids.gl_state, ids.depth_mask, JNI_TRUE);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hook table
// ---------------------------------------------------------------------------------------------------------------

constexpr const char* kGuiIngame = "net/minecraft/client/gui/GuiIngame";
constexpr const char* kGuiIngameForge = "net/minecraftforge/client/GuiIngameForge";
constexpr const char* kRenderGlobal = "net/minecraft/client/renderer/RenderGlobal";
constexpr const char* kRendererLiving = "net/minecraft/client/renderer/entity/RendererLivingEntity";

constexpr bytecode::ExtraMemberNames kShowCrosshair{
    "b", "()Z", "showCrosshair", "()Z", "func_175183_b", "()Z"};

// Vanilla / OptiFine: GuiIngame.renderGameOverlay(F)V calls this.showCrosshair().
const ExtraHookSpec kHookCrosshair{
    "avo", kGuiIngame,
    {"a", "(F)V", "renderGameOverlay", "(F)V", "func_175180_a", "(F)V"},
    "avo", kGuiIngame,
    kShowCrosshair,
    HookInsnKind::InvokeInstance,
    "adjustShowCrosshair", "(Ljava/lang/Object;)Z",
    reinterpret_cast<void*>(&native_show_crosshair),
};

// Forge: GuiIngameForge renders the crosshair itself (renderCrosshairs -> this.showCrosshair()).
const ExtraHookSpec kHookCrosshairForge{
    nullptr, kGuiIngameForge,
    {},
    nullptr, kGuiIngameForge,
    kShowCrosshair,
    HookInsnKind::InvokeInstance,
    "adjustShowCrosshair", "(Ljava/lang/Object;)Z",
    reinterpret_cast<void*>(&native_show_crosshair),
};

// RenderGlobal.drawSelectionBox(...) calls RenderGlobal.drawSelectionBoundingBox(box) for the black outline.
const ExtraHookSpec kHookSelectionBox{
    "bfr", kRenderGlobal,
    {"a", "(Lwn;Lauh;IF)V",
     "drawSelectionBox", "(Lnet/minecraft/entity/player/EntityPlayer;Lnet/minecraft/util/MovingObjectPosition;IF)V",
     "func_72731_b", "(Lnet/minecraft/entity/player/EntityPlayer;Lnet/minecraft/util/MovingObjectPosition;IF)V"},
    "bfr", kRenderGlobal,
    {"a", "(Laug;)V",
     "drawSelectionBoundingBox", "(Lnet/minecraft/util/AxisAlignedBB;)V",
     "func_181561_a", "(Lnet/minecraft/util/AxisAlignedBB;)V"},
    HookInsnKind::InvokeStatic,
    "adjustDrawSelectionBoundingBox", "(Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_draw_selection_bounding_box),
};

// RendererLivingEntity.doRender(EntityLivingBase, ...) calls this.renderModel(...).
const ExtraHookSpec kHookRenderModel{
    "bjl", kRendererLiving,
    {"a", "(Lpr;DDDFF)V",
     "doRender", "(Lnet/minecraft/entity/EntityLivingBase;DDDFF)V",
     "func_76986_a", "(Lnet/minecraft/entity/EntityLivingBase;DDDFF)V"},
    "bjl", kRendererLiving,
    {"a", "(Lpr;FFFFFF)V",
     "renderModel", "(Lnet/minecraft/entity/EntityLivingBase;FFFFFF)V",
     "func_77036_a", "(Lnet/minecraft/entity/EntityLivingBase;FFFFFF)V"},
    HookInsnKind::InvokeInstance,
    "adjustRenderModel", "(Ljava/lang/Object;Ljava/lang/Object;FFFFFF)V",
    reinterpret_cast<void*>(&native_render_model),
};

} // namespace

const bytecode::ExtraHookSpec* const* render_hooks() noexcept {
    static const ExtraHookSpec* const s_hooks[] = {
        &kHookCrosshair,
        &kHookCrosshairForge,
        &kHookSelectionBox,
        &kHookRenderModel,
        nullptr,
    };
    return s_hooks;
}

} // namespace slinky::adjust::hooks
