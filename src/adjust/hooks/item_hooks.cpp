// Adjust item hooks (Minecraft 1.8.9) for Item Animations and Item Physics.
//
// Adjust patches Minecraft's source; here each patch is a replaced call site (see the hook table at the bottom):
//
//   ItemRenderer.renderItemInFirstPerson(F)V
//     * AbstractClientPlayer.getSwingProgress(F)F -> 0 while "Hide swinging" (and remembers the swing progress)
//     * GlStateManager.pushMatrix()               -> + ItemAnimationsModule.applyPosition(itemToRender)
//     * ItemRenderer.transformFirstPersonItem(FF)  -> Adjust's patched switch: item scale, swing while eating /
//                                                    drinking / drawing a bow, the sword block animations
//     * ItemRenderer.doBlockTransformations()V     -> skipped when the block animation above already applied it
//     * ItemRenderer.renderItem(...)               -> ItemAnimationsModule.transformItem (1.7 transform; picks
//                                                    TransformType.NONE for flat items)
//   ItemRenderer.doBowTransformations(FLAbstractClientPlayer;)V
//     * GlStateManager.scale(FFF)                  -> wrapped in transformBlocking(true) / transformBlocking(false)
//   LayerHeldItem.doRenderLayer: ItemRenderer.renderItem(...)          -> marks "third-person sword block"
//   RenderItem.renderItemModelTransform: RenderItem.renderItem(stack, model) -> transformBow() when marked
//     (Adjust: RenderItem.renderItemModelForEntity, THIRD_PERSON + blocking player)
//   RenderItem.renderEffect: RenderItem.renderModel(model, color) (x2) -> custom enchantment glint colour
//   ModelBiped.setRotationAngles: putfield ModelRenderer.rotateAngleY -> blocking arm yaw 0 with "Old transform"
//     (the only write of -0.5235988F, vanilla's blocking-arm yaw, is replaced by 0; all others pass through)
//   EntityLivingBase.swingItem / updateArmSwingProgress: getArmSwingAnimationEnd()I -> swing speed
//   RenderEntityItem.doRender(Entity,...) bridge: doRender(EntityItem,...) -> Item Physics renderer
//
// Every native performs the original instruction (through JNI) when its module is not running, so vanilla
// behaviour is unchanged. Exceptions thrown by the original call stay pending for the Java caller.

#include "adjust/hooks/hooks.hpp"

#include "adjust/jni_lookup.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/item_animations/item_animations.hpp"
#include "modules/adjust/render/item_animations/item_gl.hpp"
#include "modules/adjust/render/item_physics/item_physics.hpp"

namespace slinky::adjust::hooks {

namespace {

using bytecode::ExtraHookSpec;
using bytecode::HookInsnKind;
namespace ia = modules::item_animations;
namespace gl = adjust::item_gl;

// Render-thread state of the first-person / held-item passes.
struct ItemHookState {
    float swing = 0.0f;              // swing progress returned to renderItemInFirstPerson (f1)
    bool block_handled = false;      // the next doBlockTransformations() was already applied by our animation
    bool third_person_block = false; // LayerHeldItem is rendering a blocking player's sword
};

ItemHookState g_state{};

// ---------------------------------------------------------------------------------------------------------------
// JNI ids
// ---------------------------------------------------------------------------------------------------------------

struct Ids {
    jclass living = nullptr;                // EntityLivingBase
    jmethodID get_swing_progress = nullptr; // EntityLivingBase.getSwingProgress(F)F
    jmethodID arm_swing_end = nullptr;      // EntityLivingBase.getArmSwingAnimationEnd()I (private)
    jclass player = nullptr;                // EntityPlayer
    jclass item_renderer = nullptr;         // ItemRenderer
    jfieldID item_to_render = nullptr;      // ItemRenderer.itemToRender
    jmethodID transform_first_person = nullptr; // ItemRenderer.transformFirstPersonItem(FF)V (private)
    jmethodID block_transformations = nullptr;  // ItemRenderer.doBlockTransformations()V (private)
    jmethodID ir_render_item = nullptr;     // ItemRenderer.renderItem(EntityLivingBase, ItemStack, TransformType)V
    jfieldID mc_item_renderer = nullptr;    // Minecraft.itemRenderer
    jclass render_item = nullptr;           // RenderItem
    jmethodID ri_render_item = nullptr;     // RenderItem.renderItem(ItemStack, IBakedModel)V
    jmethodID ri_render_model = nullptr;    // RenderItem.renderModel(IBakedModel, int)V (private)
    jfieldID rotate_angle_y = nullptr;      // ModelRenderer.rotateAngleY
    jmethodID do_render_item = nullptr;     // RenderEntityItem.doRender(EntityItem, DDDFF)V
    jobject transform_none = nullptr;       // TransformType.NONE (global ref)
    jobject transform_third_person = nullptr; // TransformType.THIRD_PERSON (global ref)
};

constexpr const char* kTransformTypeDesc =
    "Lnet/minecraft/client/renderer/block/model/ItemCameraTransforms$TransformType;";

jobject static_object_global(events::JniContext* jni, jclass cls, const jni::Names& names, const char* vanilla_desc,
                             const char* named_desc) noexcept {
    const jfieldID fid = jni::static_field(jni, cls, names, vanilla_desc, named_desc);
    if (fid == nullptr) {
        return nullptr;
    }
    jobject local = jni->env->GetStaticObjectField(cls, fid);
    if (jni::clear_exception(jni) || local == nullptr) {
        return nullptr;
    }
    jobject global = jni->env->NewGlobalRef(local);
    jni->env->DeleteLocalRef(local);
    return global;
}

const Ids& ids(events::JniContext* jni) noexcept {
    static const Ids s_ids = [&] {
        Ids i{};
        i.living = jni::find_class(jni, "pr", "net/minecraft/entity/EntityLivingBase");
        i.get_swing_progress = jni::method(jni, i.living, {"l", "getSwingProgress", "func_70678_g"}, "(F)F", "(F)F");
        i.arm_swing_end = jni::method(jni, i.living, {"n", "getArmSwingAnimationEnd", "func_82166_i"}, "()I", "()I");
        i.player = jni::find_class(jni, "wn", "net/minecraft/entity/player/EntityPlayer");

        i.item_renderer = jni::find_class(jni, "bfn", "net/minecraft/client/renderer/ItemRenderer");
        i.item_to_render = jni::field(jni, i.item_renderer, {"d", "itemToRender", "field_78453_b"},
                                      "Lzx;", "Lnet/minecraft/item/ItemStack;");
        i.transform_first_person = jni::method(
            jni, i.item_renderer, {"b", "transformFirstPersonItem", "func_178096_b"}, "(FF)V", "(FF)V");
        i.block_transformations = jni::method(
            jni, i.item_renderer, {"d", "doBlockTransformations", "func_178103_d"}, "()V", "()V");
        i.ir_render_item = jni::method(
            jni, i.item_renderer, {"a", "renderItem", "func_178099_a"}, "(Lpr;Lzx;Lbgr$b;)V",
            "(Lnet/minecraft/entity/EntityLivingBase;Lnet/minecraft/item/ItemStack;"
            "Lnet/minecraft/client/renderer/block/model/ItemCameraTransforms$TransformType;)V");

        const jclass mc = jni::find_class(jni, "ave", "net/minecraft/client/Minecraft");
        i.mc_item_renderer = jni::field(jni, mc, {"ac", "itemRenderer", "field_175620_Y"},
                                        "Lbfn;", "Lnet/minecraft/client/renderer/ItemRenderer;");

        i.render_item = jni::find_class(jni, "bjh", "net/minecraft/client/renderer/entity/RenderItem");
        i.ri_render_item = jni::method(
            jni, i.render_item, {"a", "renderItem", "func_180454_a"}, "(Lzx;Lboq;)V",
            "(Lnet/minecraft/item/ItemStack;Lnet/minecraft/client/resources/model/IBakedModel;)V");
        i.ri_render_model = jni::method(jni, i.render_item, {"a", "renderModel", "func_175035_a"}, "(Lboq;I)V",
                                        "(Lnet/minecraft/client/resources/model/IBakedModel;I)V");

        const jclass model_renderer = jni::find_class(jni, "bct", "net/minecraft/client/model/ModelRenderer");
        i.rotate_angle_y = jni::field(jni, model_renderer, {"g", "rotateAngleY", "field_78796_g"}, "F", "F");

        const jclass render_entity_item =
            jni::find_class(jni, "bjf", "net/minecraft/client/renderer/entity/RenderEntityItem");
        i.do_render_item = jni::method(jni, render_entity_item, {"a", "doRender", "func_76986_a"}, "(Luz;DDDFF)V",
                                       "(Lnet/minecraft/entity/item/EntityItem;DDDFF)V");

        const jclass transform_type = jni::find_class(
            jni, "bgr$b", "net/minecraft/client/renderer/block/model/ItemCameraTransforms$TransformType");
        i.transform_none = static_object_global(jni, transform_type, {"a", "NONE", "NONE"}, "Lbgr$b;",
                                                kTransformTypeDesc);
        i.transform_third_person = static_object_global(
            jni, transform_type, {"b", "THIRD_PERSON", "THIRD_PERSON"}, "Lbgr$b;", kTransformTypeDesc);
        return i;
    }();
    return s_ids;
}

// ItemRenderer.itemToRender of Minecraft's item renderer (local ref or nullptr).
jobject current_item_to_render(events::JniContext* jni, const Ids& i) noexcept {
    if (i.mc_item_renderer == nullptr || i.item_to_render == nullptr) {
        return nullptr;
    }
    JNIEnv* env = jni->env;
    jobject mc = minecraft::mc_get_instance(jni);
    if (mc == nullptr) {
        return nullptr;
    }
    jobject item_renderer = env->GetObjectField(mc, i.mc_item_renderer);
    env->DeleteLocalRef(mc);
    if (item_renderer == nullptr) {
        return nullptr;
    }
    jobject stack = env->GetObjectField(item_renderer, i.item_to_render);
    env->DeleteLocalRef(item_renderer);
    return stack;
}

// ---------------------------------------------------------------------------------------------------------------
// Adjust's patched ItemRenderer pieces (GlStateManager, same order as Adjust)
// ---------------------------------------------------------------------------------------------------------------

// ItemRenderer.transformFirstPersonItem with Adjust's scale patch.
void transform_first_person_item(events::JniContext* jni, jobject stack, float equip_progress,
                                 float swing_progress) noexcept {
    gl::translate(jni, 0.56f, -0.52f, -0.71999997f);
    gl::translate(jni, 0.0f, equip_progress * -0.6f, 0.0f);
    gl::rotate(jni, 45.0f, 0.0f, 1.0f, 0.0f);
    const float f = gl::mc_sin(swing_progress * swing_progress * gl::kPi);
    const float f1 = gl::mc_sin(gl::mc_sqrt(swing_progress) * gl::kPi);
    gl::rotate(jni, f * -20.0f, 0.0f, 1.0f, 0.0f);
    gl::rotate(jni, f1 * -20.0f, 0.0f, 0.0f, 1.0f);
    gl::rotate(jni, f1 * -80.0f, 1.0f, 0.0f, 0.0f);
    float scale = 0.4f;
    if ((stack != nullptr && ia::is_sword_stack(jni, stack)) || !ia::sword_only()) {
        scale *= ia::position_scale();
    }
    gl::scale(jni, scale, scale, scale);
}

// ItemRenderer.doBlockTransformations
void do_block_transformations(events::JniContext* jni) noexcept {
    gl::translate(jni, -0.5f, 0.2f, 0.0f);
    gl::rotate(jni, 30.0f, 0.0f, 1.0f, 0.0f);
    gl::rotate(jni, -80.0f, 1.0f, 0.0f, 0.0f);
    gl::rotate(jni, 60.0f, 0.0f, 1.0f, 0.0f);
}

// ItemRenderer.doItemUsedTransformations
void do_item_used_transformations(events::JniContext* jni, float swing_progress) noexcept {
    const float f = -0.4f * gl::mc_sin(gl::mc_sqrt(swing_progress) * gl::kPi);
    const float f1 = 0.2f * gl::mc_sin(gl::mc_sqrt(swing_progress) * gl::kPi * 2.0f);
    const float f2 = -0.2f * gl::mc_sin(swing_progress * gl::kPi);
    gl::translate(jni, f, f1, f2);
}

// renderItemInFirstPerson, case BLOCK while Item Animations is enabled. `f` = equip progress, `f1` = swing.
void block_animation(events::JniContext* jni, jobject stack, float f, float f1) noexcept {
    const float s = gl::mc_sin(gl::mc_sqrt(f1) * gl::kPi);
    const float e = f * ia::equip_animation();
    const ia::SwordAnimation animation = ia::animation();

    if (!ia::sword_enabled() || animation == ia::SwordAnimation::None) {
        transform_first_person_item(jni, stack, e, 0.0f);
        do_block_transformations(jni);
        return;
    }
    switch (animation) {
        case ia::SwordAnimation::Tap:
            transform_first_person_item(jni, stack, e, 0.0f);
            gl::translate(jni, -0.25f, 0.2f, -0.01f);
            gl::rotate(jni, -s * 42.0f, s / 1.9f, 0.0f, 9.0f);       // GL11.glRotated in Adjust
            gl::rotate(jni, -s * 50.0f, 0.7f, s / 1.98f, 0.0f);      // GL11.glRotated in Adjust
            do_block_transformations(jni);
            break;
        case ia::SwordAnimation::Bounce:
            transform_first_person_item(jni, stack, e, 0.0f);
            do_block_transformations(jni);
            gl::translate(jni, -0.12f, 0.23f, 0.01f);
            gl::rotate(jni, -s * 52.0f, -1.5f, -9.0f, 5.5f);
            break;
        case ia::SwordAnimation::Swank:
            transform_first_person_item(jni, stack, e / 2.0f, f1);
            gl::rotate(jni, s * 30.0f, -s, -0.0f, 9.0f);
            gl::rotate(jni, s * 40.0f, 1.0f, -s, -0.0f);
            gl::translate(jni, -0.5f, 0.2f, 0.0f);
            gl::rotate(jni, 30.0f, 0.0f, 1.0f, 0.0f);
            gl::rotate(jni, -80.0f, 1.0f, 0.0f, 0.0f);
            gl::rotate(jni, 60.0f, 0.0f, 1.0f, 0.0f);
            break;
        case ia::SwordAnimation::Punch:
            transform_first_person_item(jni, stack, e, 0.0f);
            gl::translate(jni, -0.5f, 0.2f, 0.0f);
            gl::rotate(jni, 30.0f, 0.0f, 1.0f, 0.0f);
            gl::rotate(jni, -80.0f, 1.0f, 0.0f, 0.0f);
            gl::rotate(jni, 60.0f, 0.0f, 1.0f, 0.0f);
            gl::rotate(jni, -s * 30.0f, -5.0f, 0.0f, 9.0f);
            gl::rotate(jni, -s * 10.0f, 1.0f, -0.4f, -0.5f);
            break;
        case ia::SwordAnimation::Dig:
            transform_first_person_item(jni, stack, e, f1);
            gl::rotate(jni, s * 32.0f, -s, 0.0f, 8.5f);
            gl::rotate(jni, s * 40.0f, 0.5f, -s, 0.0f);
            gl::translate(jni, -0.2f, 0.259f, 0.1f);
            do_block_transformations(jni);
            break;
        case ia::SwordAnimation::Horizontal:
            transform_first_person_item(jni, stack, e, 0.0f);
            do_block_transformations(jni);
            gl::translate(jni, -0.16f, 0.139f, 0.06f);
            gl::rotate(jni, -s * 32.0f, 84.0f, -0.0f, 133.0f);
            break;
        case ia::SwordAnimation::Shove:
            transform_first_person_item(jni, stack, e, f1);
            gl::rotate(jni, s * 15.0f, -s, -0.0f, 9.0f);
            gl::rotate(jni, s * 40.0f, 1.0f, -s * 0.5f, -0.0f);
            do_block_transformations(jni);
            break;
        case ia::SwordAnimation::Stasis:
            transform_first_person_item(jni, stack, e, 0.0f);
            gl::rotate(jni, -s * 20.0f, s / 2.0f, -0.0f, 9.0f);
            gl::rotate(jni, -s * 30.0f, 1.0f, s / 2.0f, -0.0f);
            do_block_transformations(jni);
            do_item_used_transformations(jni, f1);
            break;
        case ia::SwordAnimation::Slide:
            transform_first_person_item(jni, stack, e, 0.0f);
            gl::rotate(jni, -s * 20.0f, 1.0f, 0.0f, 0.0f);
            do_block_transformations(jni);
            break;
        case ia::SwordAnimation::Attract:
            transform_first_person_item(jni, stack, e, 0.0f);
            gl::rotate(jni, s * 15.0f, 0.0f, 1.0f, 0.0f);
            gl::rotate(jni, s * 10.0f, 0.0f, 0.0f, 1.0f);
            gl::translate(jni, 0.0f, s * 0.1f, 0.0f);
            do_block_transformations(jni);
            break;
        case ia::SwordAnimation::Detract:
            transform_first_person_item(jni, stack, e, 0.0f);
            gl::rotate(jni, -s * 27.5f, -8.0f, -0.0f, 9.0f);
            gl::rotate(jni, -s * 45.0f, 1.0f, s * 0.5f, -0.0f);
            do_block_transformations(jni);
            break;
        case ia::SwordAnimation::Vanilla:
        default:
            transform_first_person_item(jni, stack, e, f1);
            do_block_transformations(jni);
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Natives: ItemRenderer.renderItemInFirstPerson
// ---------------------------------------------------------------------------------------------------------------

// AbstractClientPlayer.getSwingProgress(F)F
jfloat JNICALL native_swing_progress(JNIEnv* env, jclass /*cls*/, jobject player, jfloat partial_ticks) noexcept {
    float swing = 0.0f;
    if (!ia::should_hide_swinging()) { // Adjust: f1 = 0 while hideSwinging
        events::JniContext jni = jni_for(env);
        const Ids& i = ids(&jni);
        if (i.get_swing_progress == nullptr || player == nullptr) {
            return 0.0f;
        }
        swing = env->CallFloatMethod(player, i.get_swing_progress, partial_ticks);
        if (env->ExceptionCheck()) {
            return swing;
        }
    }
    g_state.swing = swing;
    return swing;
}

// GlStateManager.pushMatrix()V (the matrix push before the held item is transformed)
void JNICALL native_first_person_push_matrix(JNIEnv* env, jclass /*cls*/) noexcept {
    events::JniContext jni = jni_for(env);
    gl::push_matrix(&jni);
    g_state.block_handled = false;
    if (!ia::running()) {
        return;
    }
    jobject stack = current_item_to_render(&jni, ids(&jni));
    if (stack != nullptr) { // if (this.itemToRender != null) { animations.applyPosition(this.itemToRender); ...
        ia::apply_position(&jni, stack);
        env->DeleteLocalRef(stack);
    }
}

// ItemRenderer.transformFirstPersonItem(FF)V - called from every branch of renderItemInFirstPerson except maps.
void JNICALL native_transform_first_person(JNIEnv* env, jclass /*cls*/, jobject item_renderer, jfloat equip,
                                           jfloat swing) noexcept {
    events::JniContext jni = jni_for(env);
    const Ids& i = ids(&jni);
    if (!ia::running()) {
        if (i.item_renderer != nullptr && i.transform_first_person != nullptr && item_renderer != nullptr) {
            env->CallNonvirtualVoidMethod(item_renderer, i.item_renderer, i.transform_first_person, equip, swing);
        }
        return;
    }
    jobject stack = (item_renderer != nullptr && i.item_to_render != nullptr)
                        ? env->GetObjectField(item_renderer, i.item_to_render)
                        : nullptr;
    if (stack != nullptr && ia::local_item_in_use_count(&jni) > 0) {
        // else if (abstractclientplayer.getItemInUseCount() > 0) { switch (enumaction) { ... } } (animEnabled)
        switch (ia::use_action_ordinal(&jni, stack)) {
            case ia::kActionNone:
                transform_first_person_item(&jni, stack, equip, 0.0f);
                break;
            case ia::kActionEat:
            case ia::kActionDrink:
                transform_first_person_item(&jni, stack, equip, g_state.swing);
                break;
            case ia::kActionBlock:
                block_animation(&jni, stack, equip, g_state.swing);
                g_state.block_handled = true; // vanilla's following doBlockTransformations() is skipped
                break;
            case ia::kActionBow:
                transform_first_person_item(&jni, stack, equip, g_state.swing);
                break;
            default:
                transform_first_person_item(&jni, stack, equip, swing);
                break;
        }
    } else {
        // doItemUsedTransformations(f1); transformFirstPersonItem(f, f1);
        transform_first_person_item(&jni, stack, equip, swing);
    }
    if (stack != nullptr) {
        env->DeleteLocalRef(stack);
    }
}

// ItemRenderer.doBlockTransformations()V
void JNICALL native_block_transformations(JNIEnv* env, jclass /*cls*/, jobject item_renderer) noexcept {
    if (g_state.block_handled) {
        g_state.block_handled = false;
        return;
    }
    events::JniContext jni = jni_for(env);
    const Ids& i = ids(&jni);
    if (i.item_renderer != nullptr && i.block_transformations != nullptr && item_renderer != nullptr) {
        env->CallNonvirtualVoidMethod(item_renderer, i.item_renderer, i.block_transformations);
    }
}

// ItemRenderer.renderItem(EntityLivingBase, ItemStack, TransformType)V in renderItemInFirstPerson
void JNICALL native_render_first_person(JNIEnv* env, jclass /*cls*/, jobject item_renderer, jobject entity,
                                        jobject stack, jobject transform) noexcept {
    g_state.block_handled = false;
    events::JniContext jni = jni_for(env);
    const Ids& i = ids(&jni);
    if (i.ir_render_item == nullptr || item_renderer == nullptr) {
        return;
    }
    jobject type = transform;
    // animations.transformItem(this.itemToRender) ? FIRST_PERSON : NONE
    if (ia::running() && !ia::transform_item(&jni, stack) && i.transform_none != nullptr) {
        type = i.transform_none;
    }
    env->CallVoidMethod(item_renderer, i.ir_render_item, entity, stack, type);
}

// GlStateManager.scale(FFF)V in ItemRenderer.doBowTransformations
void JNICALL native_bow_scale(JNIEnv* env, jclass /*cls*/, jfloat x, jfloat y, jfloat z) noexcept {
    events::JniContext jni = jni_for(env);
    const bool animated = ia::running();
    if (animated) {
        ia::transform_blocking(&jni, true);
    }
    gl::scale(&jni, x, y, z);
    if (animated) {
        ia::transform_blocking(&jni, false);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Natives: third-person held item, glint, arm pose, swing speed
// ---------------------------------------------------------------------------------------------------------------

// ItemRenderer.renderItem(EntityLivingBase, ItemStack, TransformType)V in LayerHeldItem.doRenderLayer
void JNICALL native_held_item_render(JNIEnv* env, jclass /*cls*/, jobject item_renderer, jobject entity,
                                     jobject stack, jobject transform) noexcept {
    events::JniContext jni = jni_for(env);
    const Ids& i = ids(&jni);
    if (i.ir_render_item == nullptr || item_renderer == nullptr) {
        return;
    }
    bool blocking = false;
    if (ia::running() && entity != nullptr && stack != nullptr && i.player != nullptr &&
        i.transform_third_person != nullptr && env->IsSameObject(transform, i.transform_third_person) != JNI_FALSE &&
        env->IsInstanceOf(entity, i.player) != JNI_FALSE) {
        // heldItem != null && player.getItemInUseCount() >= 1 && heldItem.getItemUseAction() == BLOCK
        blocking = minecraft::player_get_item_in_use_count(&jni, entity) >= 1 &&
                   ia::use_action_ordinal(&jni, stack) == ia::kActionBlock;
    }
    const bool previous = g_state.third_person_block;
    g_state.third_person_block = blocking;
    env->CallVoidMethod(item_renderer, i.ir_render_item, entity, stack, transform);
    g_state.third_person_block = previous;
}

// RenderItem.renderItem(ItemStack, IBakedModel)V in RenderItem.renderItemModelTransform
void JNICALL native_model_transform_render(JNIEnv* env, jclass /*cls*/, jobject render_item, jobject stack,
                                           jobject model) noexcept {
    events::JniContext jni = jni_for(env);
    const Ids& i = ids(&jni);
    if (i.ri_render_item == nullptr || render_item == nullptr) {
        return;
    }
    if (g_state.third_person_block) {
        g_state.third_person_block = false;
        if (ia::running()) {
            ia::transform_bow(&jni); // after the model's THIRD_PERSON camera transform, before rendering
        }
    }
    env->CallVoidMethod(render_item, i.ri_render_item, stack, model);
}

// RenderItem.renderModel(IBakedModel, int)V in RenderItem.renderEffect (colour -8372020)
void JNICALL native_glint_render_model(JNIEnv* env, jclass /*cls*/, jobject render_item, jobject model,
                                       jint color) noexcept {
    events::JniContext jni = jni_for(env);
    const Ids& i = ids(&jni);
    if (i.render_item == nullptr || i.ri_render_model == nullptr || render_item == nullptr) {
        return;
    }
    adjust::Argb custom = 0;
    if (ia::custom_glint_color(&custom)) {
        color = static_cast<jint>(custom);
    }
    env->CallNonvirtualVoidMethod(render_item, i.render_item, i.ri_render_model, model, color);
}

// putfield ModelRenderer.rotateAngleY in ModelBiped.setRotationAngles
void JNICALL native_biped_rotate_angle_y(JNIEnv* env, jclass /*cls*/, jobject model_renderer,
                                         jfloat value) noexcept {
    // case 3 (blocking): bipedRightArm.rotateAngleY = oldTransform ? 0.0F : -0.5235988F
    if (value == -0.5235988f && ia::running() && ia::old_transform()) {
        value = 0.0f;
    }
    events::JniContext jni = jni_for(env);
    const Ids& i = ids(&jni);
    if (i.rotate_angle_y == nullptr || model_renderer == nullptr) {
        return;
    }
    env->SetFloatField(model_renderer, i.rotate_angle_y, value);
}

// EntityLivingBase.getArmSwingAnimationEnd()I in swingItem / updateArmSwingProgress (client and integrated server)
jint JNICALL native_arm_swing_animation_end(JNIEnv* env, jclass /*cls*/, jobject living) noexcept {
    events::JniContext jni = jni_for(env);
    const Ids& i = ids(&jni);
    if (i.living == nullptr || i.arm_swing_end == nullptr || living == nullptr) {
        return 6;
    }
    const jint vanilla = env->CallNonvirtualIntMethod(living, i.living, i.arm_swing_end);
    if (env->ExceptionCheck()) {
        return vanilla;
    }
    return ia::patch_arm_swing_animation_end(&jni, living, vanilla);
}

// ---------------------------------------------------------------------------------------------------------------
// Native: Item Physics
// ---------------------------------------------------------------------------------------------------------------

// RenderEntityItem.doRender(EntityItem, DDDFF)V in the doRender(Entity, DDDFF)V bridge
void JNICALL native_item_physics_do_render(JNIEnv* env, jclass /*cls*/, jobject renderer, jobject entity_item,
                                           jdouble x, jdouble y, jdouble z, jfloat entity_yaw,
                                           jfloat partial_ticks) noexcept {
    events::JniContext jni = jni_for(env);
    if (modules::item_physics::running()) {
        modules::item_physics::render_item(&jni, renderer, entity_item, x, y, z);
        return;
    }
    const Ids& i = ids(&jni);
    if (i.do_render_item == nullptr || renderer == nullptr) {
        return;
    }
    env->CallVoidMethod(renderer, i.do_render_item, entity_item, x, y, z, entity_yaw, partial_ticks);
}

// ---------------------------------------------------------------------------------------------------------------
// Hook table
// ---------------------------------------------------------------------------------------------------------------

constexpr const char* kItemRenderer = "net/minecraft/client/renderer/ItemRenderer";
constexpr const char* kGlStateManager = "net/minecraft/client/renderer/GlStateManager";
constexpr const char* kRenderItem = "net/minecraft/client/renderer/entity/RenderItem";
constexpr const char* kLayerHeldItem = "net/minecraft/client/renderer/entity/layers/LayerHeldItem";
constexpr const char* kModelBiped = "net/minecraft/client/model/ModelBiped";
constexpr const char* kModelRenderer = "net/minecraft/client/model/ModelRenderer";
constexpr const char* kEntityLivingBase = "net/minecraft/entity/EntityLivingBase";
constexpr const char* kAbstractClientPlayer = "net/minecraft/client/entity/AbstractClientPlayer";
constexpr const char* kRenderEntityItem = "net/minecraft/client/renderer/entity/RenderEntityItem";

constexpr const char* kDescObject4Void =
    "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V";

// ItemRenderer.renderItemInFirstPerson(F)V
constexpr bytecode::ExtraMemberNames kRenderItemInFirstPerson{
    "a", "(F)V", "renderItemInFirstPerson", "(F)V", "func_78440_a", "(F)V"};

// ItemRenderer.renderItem(EntityLivingBase, ItemStack, TransformType)V
constexpr bytecode::ExtraMemberNames kItemRendererRenderItem{
    "a", "(Lpr;Lzx;Lbgr$b;)V",
    "renderItem",
    "(Lnet/minecraft/entity/EntityLivingBase;Lnet/minecraft/item/ItemStack;"
    "Lnet/minecraft/client/renderer/block/model/ItemCameraTransforms$TransformType;)V",
    "func_178099_a",
    "(Lnet/minecraft/entity/EntityLivingBase;Lnet/minecraft/item/ItemStack;"
    "Lnet/minecraft/client/renderer/block/model/ItemCameraTransforms$TransformType;)V"};

const ExtraHookSpec kHookSwingProgress{
    "bfn", kItemRenderer,
    kRenderItemInFirstPerson,
    "bet", kAbstractClientPlayer,
    {"l", "(F)F", "getSwingProgress", "(F)F", "func_70678_g", "(F)F"},
    HookInsnKind::InvokeInstance,
    "adjustItemSwingProgress", "(Ljava/lang/Object;F)F",
    reinterpret_cast<void*>(&native_swing_progress),
};

const ExtraHookSpec kHookFirstPersonPushMatrix{
    "bfn", kItemRenderer,
    kRenderItemInFirstPerson,
    "bfl", kGlStateManager,
    {"E", "()V", "pushMatrix", "()V", "func_179094_E", "()V"},
    HookInsnKind::InvokeStatic,
    "adjustItemFirstPersonPushMatrix", "()V",
    reinterpret_cast<void*>(&native_first_person_push_matrix),
};

const ExtraHookSpec kHookTransformFirstPerson{
    "bfn", kItemRenderer,
    kRenderItemInFirstPerson,
    "bfn", kItemRenderer,
    {"b", "(FF)V", "transformFirstPersonItem", "(FF)V", "func_178096_b", "(FF)V"},
    HookInsnKind::InvokeInstance,
    "adjustItemTransformFirstPerson", "(Ljava/lang/Object;FF)V",
    reinterpret_cast<void*>(&native_transform_first_person),
};

const ExtraHookSpec kHookBlockTransformations{
    "bfn", kItemRenderer,
    kRenderItemInFirstPerson,
    "bfn", kItemRenderer,
    {"d", "()V", "doBlockTransformations", "()V", "func_178103_d", "()V"},
    HookInsnKind::InvokeInstance,
    "adjustItemBlockTransformations", "(Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_block_transformations),
};

const ExtraHookSpec kHookRenderFirstPerson{
    "bfn", kItemRenderer,
    kRenderItemInFirstPerson,
    "bfn", kItemRenderer,
    kItemRendererRenderItem,
    HookInsnKind::InvokeInstance,
    "adjustItemRenderFirstPerson", kDescObject4Void,
    reinterpret_cast<void*>(&native_render_first_person),
};

// ItemRenderer.doBowTransformations(F, AbstractClientPlayer)V: GlStateManager.scale(1, 1, 1 + f1 * 0.2)
const ExtraHookSpec kHookBowScale{
    "bfn", kItemRenderer,
    {"a", "(FLbet;)V",
     "doBowTransformations", "(FLnet/minecraft/client/entity/AbstractClientPlayer;)V",
     "func_178098_a", "(FLnet/minecraft/client/entity/AbstractClientPlayer;)V"},
    "bfl", kGlStateManager,
    {"a", "(FFF)V", "scale", "(FFF)V", "func_179152_a", "(FFF)V"},
    HookInsnKind::InvokeStatic,
    "adjustItemBowScale", "(FFF)V",
    reinterpret_cast<void*>(&native_bow_scale),
};

// LayerHeldItem.doRenderLayer(EntityLivingBase, FFFFFFF)V -> ItemRenderer.renderItem(entity, stack, THIRD_PERSON)
const ExtraHookSpec kHookHeldItemRender{
    "bky", kLayerHeldItem,
    {"a", "(Lpr;FFFFFFF)V",
     "doRenderLayer", "(Lnet/minecraft/entity/EntityLivingBase;FFFFFFF)V",
     "func_177141_a", "(Lnet/minecraft/entity/EntityLivingBase;FFFFFFF)V"},
    "bfn", kItemRenderer,
    kItemRendererRenderItem,
    HookInsnKind::InvokeInstance,
    "adjustItemHeldItemRender", kDescObject4Void,
    reinterpret_cast<void*>(&native_held_item_render),
};

// RenderItem.renderItemModelTransform(ItemStack, IBakedModel, TransformType)V -> this.renderItem(stack, model)
const ExtraHookSpec kHookModelTransformRender{
    "bjh", kRenderItem,
    {"a", "(Lzx;Lboq;Lbgr$b;)V",
     "renderItemModelTransform",
     "(Lnet/minecraft/item/ItemStack;Lnet/minecraft/client/resources/model/IBakedModel;"
     "Lnet/minecraft/client/renderer/block/model/ItemCameraTransforms$TransformType;)V",
     "func_175040_a",
     "(Lnet/minecraft/item/ItemStack;Lnet/minecraft/client/resources/model/IBakedModel;"
     "Lnet/minecraft/client/renderer/block/model/ItemCameraTransforms$TransformType;)V"},
    "bjh", kRenderItem,
    {"a", "(Lzx;Lboq;)V",
     "renderItem", "(Lnet/minecraft/item/ItemStack;Lnet/minecraft/client/resources/model/IBakedModel;)V",
     "func_180454_a", "(Lnet/minecraft/item/ItemStack;Lnet/minecraft/client/resources/model/IBakedModel;)V"},
    HookInsnKind::InvokeInstance,
    "adjustItemModelTransformRender", "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_model_transform_render),
};

// RenderItem.renderEffect(IBakedModel)V -> this.renderModel(model, -8372020) (both calls)
const ExtraHookSpec kHookGlintRenderModel{
    "bjh", kRenderItem,
    {"a", "(Lboq;)V",
     "renderEffect", "(Lnet/minecraft/client/resources/model/IBakedModel;)V",
     "func_180451_a", "(Lnet/minecraft/client/resources/model/IBakedModel;)V"},
    "bjh", kRenderItem,
    {"a", "(Lboq;I)V",
     "renderModel", "(Lnet/minecraft/client/resources/model/IBakedModel;I)V",
     "func_175035_a", "(Lnet/minecraft/client/resources/model/IBakedModel;I)V"},
    HookInsnKind::InvokeInstance,
    "adjustItemGlintRenderModel", "(Ljava/lang/Object;Ljava/lang/Object;I)V",
    reinterpret_cast<void*>(&native_glint_render_model),
};

// ModelBiped.setRotationAngles(FFFFFF, Entity)V -> every putfield ModelRenderer.rotateAngleY
const ExtraHookSpec kHookBipedRotateAngleY{
    "bbj", kModelBiped,
    {"a", "(FFFFFFLpk;)V",
     "setRotationAngles", "(FFFFFFLnet/minecraft/entity/Entity;)V",
     "func_78087_a", "(FFFFFFLnet/minecraft/entity/Entity;)V"},
    "bct", kModelRenderer,
    {"g", "F", "rotateAngleY", "F", "field_78796_g", "F"},
    HookInsnKind::PutField,
    "adjustItemBipedRotateAngleY", "(Ljava/lang/Object;F)V",
    reinterpret_cast<void*>(&native_biped_rotate_angle_y),
};

constexpr bytecode::ExtraMemberNames kArmSwingAnimationEnd{
    "n", "()I", "getArmSwingAnimationEnd", "()I", "func_82166_i", "()I"};

// EntityLivingBase.swingItem()V -> this.getArmSwingAnimationEnd()
const ExtraHookSpec kHookArmSwingEndSwingItem{
    "pr", kEntityLivingBase,
    {"bw", "()V", "swingItem", "()V", "func_71038_i", "()V"},
    "pr", kEntityLivingBase,
    kArmSwingAnimationEnd,
    HookInsnKind::InvokeInstance,
    "adjustItemArmSwingAnimationEnd", "(Ljava/lang/Object;)I",
    reinterpret_cast<void*>(&native_arm_swing_animation_end),
};

// EntityLivingBase.updateArmSwingProgress()V -> this.getArmSwingAnimationEnd()
const ExtraHookSpec kHookArmSwingEndUpdate{
    "pr", kEntityLivingBase,
    {"bx", "()V", "updateArmSwingProgress", "()V", "func_82168_bl", "()V"},
    "pr", kEntityLivingBase,
    kArmSwingAnimationEnd,
    HookInsnKind::InvokeInstance,
    "adjustItemArmSwingAnimationEnd", "(Ljava/lang/Object;)I",
    reinterpret_cast<void*>(&native_arm_swing_animation_end),
};

// RenderEntityItem.doRender(Entity, DDDFF)V (generic bridge) -> this.doRender((EntityItem) entity, ...)
const ExtraHookSpec kHookItemPhysicsDoRender{
    "bjf", kRenderEntityItem,
    {"a", "(Lpk;DDDFF)V",
     "doRender", "(Lnet/minecraft/entity/Entity;DDDFF)V",
     "func_76986_a", "(Lnet/minecraft/entity/Entity;DDDFF)V"},
    "bjf", kRenderEntityItem,
    {"a", "(Luz;DDDFF)V",
     "doRender", "(Lnet/minecraft/entity/item/EntityItem;DDDFF)V",
     "func_76986_a", "(Lnet/minecraft/entity/item/EntityItem;DDDFF)V"},
    HookInsnKind::InvokeInstance,
    "adjustItemPhysicsDoRender", "(Ljava/lang/Object;Ljava/lang/Object;DDDFF)V",
    reinterpret_cast<void*>(&native_item_physics_do_render),
};

} // namespace

const bytecode::ExtraHookSpec* const* item_hooks() noexcept {
    static const ExtraHookSpec* const s_hooks[] = {
        &kHookSwingProgress,
        &kHookFirstPersonPushMatrix,
        &kHookTransformFirstPerson,
        &kHookBlockTransformations,
        &kHookRenderFirstPerson,
        &kHookBowScale,
        &kHookHeldItemRender,
        &kHookModelTransformRender,
        &kHookGlintRenderModel,
        &kHookBipedRotateAngleY,
        &kHookArmSwingEndSwingItem,
        &kHookArmSwingEndUpdate,
        &kHookItemPhysicsDoRender,
        nullptr,
    };
    return s_hooks;
}

} // namespace slinky::adjust::hooks
