// Adjust Hurtcam hooks (Minecraft 1.8.9). Ports the HurtcamModule patches of Adjust's Minecraft sources:
//
//   * EntityRenderer.hurtCameraEffect(F)V: early return while "Local > Disabled" is on.
//       -> every `this.hurtCameraEffect(partialTicks)` call in EntityRenderer (setupCameraTransform, renderHand and
//          OptiFine's renderHand overload) is replaced; the native skips it or calls the original.
//   * RendererLivingEntity.setBrightness: the hurt tint (1, 0, 0, 0.3) uses "Model color > Color" while enabled.
//       -> the GL11.glTexEnv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, brightnessBuffer) call (and glTexEnvfv for
//          LWJGL 3 based clients) is replaced; when the buffer holds exactly vanilla's hurt colour its RGB is
//          rewritten before the original call. OptiFine shaders: Shaders.setEntityColor(1, 0, 0, 0.3) likewise.
//   * LayerArmorBase.shouldCombineTextures(): true while "Armor" is on, so renderLayers() wraps the armour layer in
//     setBrightness(entity, partialTicks, true) / unsetBrightness().
//       -> invokeinterface (LayerRenderer.shouldCombineTextures in renderLayers) cannot be rewritten, so each
//          `this.renderLayer(...)` call in LayerArmorBase.doRenderLayer is wrapped instead: for a hurt / dying
//          entity the native calls renderer.setBrightness(entity, partialTicks, true), the original renderLayer, and
//          unsetBrightness() if brightness was applied. Not hurt -> combineTextures makes no difference in vanilla's
//          setBrightness, so the original is called alone.
//
// Every native calls the original member unchanged while the Adjust Hurtcam is not running.

#include "adjust/hooks/hooks.hpp"

#include "adjust/jni_lookup.hpp"
#include "modules/adjust/render/hurtcam/hurtcam.hpp"

namespace slinky::adjust::hooks {

namespace {

using bytecode::ExtraHookSpec;
using bytecode::HookInsnKind;

// ---------------------------------------------------------------------------------------------------------------
// EntityRenderer.hurtCameraEffect(F)V (private)
// ---------------------------------------------------------------------------------------------------------------

void JNICALL native_hurt_camera_effect(JNIEnv* env, jclass /*cls*/, jobject entity_renderer,
                                       jfloat partial_ticks) noexcept {
    if (modules::adjust_hurtcam_disables_local()) {
        return; // HurtcamModule: localDisabled -> no hurt shake, no death roll
    }
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "bfk", "net/minecraft/client/renderer/EntityRenderer");
    static const jmethodID mid = jni::method(&jni, cls, {"d", "hurtCameraEffect", "func_78482_e"}, "(F)V", "(F)V");
    if (cls == nullptr || mid == nullptr) {
        return;
    }
    env->CallNonvirtualVoidMethod(entity_renderer, cls, mid, partial_ticks); // exceptions stay pending
}

// ---------------------------------------------------------------------------------------------------------------
// RendererLivingEntity.setBrightness: GL11.glTexEnv / glTexEnvfv (IILjava/nio/FloatBuffer;)V (static)
// ---------------------------------------------------------------------------------------------------------------

struct BufferIds {
    jmethodID position = nullptr; // Buffer.position()I
    jmethodID limit = nullptr;    // Buffer.limit()I
    jmethodID get = nullptr;      // FloatBuffer.get(I)F
    jmethodID put = nullptr;      // FloatBuffer.put(IF)Ljava/nio/FloatBuffer;
    bool ok = false;
};

const BufferIds& buffer_ids(JNIEnv* env) noexcept {
    static const BufferIds s_ids = [&] {
        BufferIds ids{};
        jclass buffer = env->FindClass("java/nio/Buffer");
        jclass float_buffer = env->FindClass("java/nio/FloatBuffer");
        if (buffer != nullptr && float_buffer != nullptr) {
            ids.position = env->GetMethodID(buffer, "position", "()I");
            ids.limit = env->GetMethodID(buffer, "limit", "()I");
            ids.get = env->GetMethodID(float_buffer, "get", "(I)F");
            ids.put = env->GetMethodID(float_buffer, "put", "(IF)Ljava/nio/FloatBuffer;");
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        if (buffer != nullptr) {
            env->DeleteLocalRef(buffer);
        }
        if (float_buffer != nullptr) {
            env->DeleteLocalRef(float_buffer);
        }
        ids.ok = ids.position != nullptr && ids.limit != nullptr && ids.get != nullptr && ids.put != nullptr;
        return ids;
    }();
    return s_ids;
}

// Vanilla's hurt colour, written by setBrightness when entity.hurtTime > 0 || entity.deathTime > 0. No other
// branch can produce exactly these floats (the colour-multiplier branch writes 1 - (a / 255), never 0.3F).
bool is_vanilla_hurt_color(float r, float g, float b, float a) noexcept {
    return r == 1.0f && g == 0.0f && b == 0.0f && a == 0.3f;
}

void apply_model_color(float rgb[3], adjust::Argb color) noexcept {
    rgb[0] = static_cast<float>(adjust::red_of(color)) / 255.0f;
    rgb[1] = static_cast<float>(adjust::green_of(color)) / 255.0f;
    rgb[2] = static_cast<float>(adjust::blue_of(color)) / 255.0f;
}

// Rewrites the RGB of `buffer` if it holds vanilla's hurt colour and Model color is enabled. Never leaves an
// exception pending.
void maybe_recolor_buffer(JNIEnv* env, jobject buffer) noexcept {
    adjust::Argb color = 0;
    if (buffer == nullptr || !modules::adjust_hurtcam_model_color(&color)) {
        return;
    }
    const BufferIds& ids = buffer_ids(env);
    if (!ids.ok) {
        return;
    }
    const jint pos = env->CallIntMethod(buffer, ids.position);
    const jint lim = env->CallIntMethod(buffer, ids.limit);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    if (lim - pos < 4) {
        return;
    }
    float v[4];
    for (int i = 0; i < 4; ++i) {
        v[i] = env->CallFloatMethod(buffer, ids.get, pos + i);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    if (!is_vanilla_hurt_color(v[0], v[1], v[2], v[3])) {
        return;
    }
    float rgb[3];
    apply_model_color(rgb, color);
    for (int i = 0; i < 3; ++i) {
        jobject self = env->CallObjectMethod(buffer, ids.put, pos + i, rgb[i]);
        if (self != nullptr) {
            env->DeleteLocalRef(self);
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

jclass gl11_class(events::JniContext* jni) noexcept {
    static const jclass cls = jni::find_class(jni, "org/lwjgl/opengl/GL11", "org/lwjgl/opengl/GL11");
    return cls;
}

void JNICALL native_gl_tex_env(JNIEnv* env, jclass /*cls*/, jint target, jint pname, jobject buffer) noexcept {
    maybe_recolor_buffer(env, buffer);
    events::JniContext jni = jni_for(env);
    const jclass gl11 = gl11_class(&jni);
    static const jmethodID mid = jni::static_method(&jni, gl11, {"glTexEnv", "glTexEnv", "glTexEnv"},
                                                    "(IILjava/nio/FloatBuffer;)V", "(IILjava/nio/FloatBuffer;)V");
    if (gl11 != nullptr && mid != nullptr) {
        env->CallStaticVoidMethod(gl11, mid, target, pname, buffer);
    }
}

void JNICALL native_gl_tex_envfv(JNIEnv* env, jclass /*cls*/, jint target, jint pname, jobject buffer) noexcept {
    maybe_recolor_buffer(env, buffer);
    events::JniContext jni = jni_for(env);
    const jclass gl11 = gl11_class(&jni);
    static const jmethodID mid = jni::static_method(&jni, gl11, {"glTexEnvfv", "glTexEnvfv", "glTexEnvfv"},
                                                    "(IILjava/nio/FloatBuffer;)V", "(IILjava/nio/FloatBuffer;)V");
    if (gl11 != nullptr && mid != nullptr) {
        env->CallStaticVoidMethod(gl11, mid, target, pname, buffer);
    }
}

// OptiFine: Shaders.setEntityColor(FFFF)V (static), called with the same colour as the brightness buffer.
void JNICALL native_shaders_set_entity_color(JNIEnv* env, jclass /*cls*/, jfloat r, jfloat g, jfloat b,
                                             jfloat a) noexcept {
    adjust::Argb color = 0;
    if (is_vanilla_hurt_color(r, g, b, a) && modules::adjust_hurtcam_model_color(&color)) {
        float rgb[3];
        apply_model_color(rgb, color);
        r = rgb[0];
        g = rgb[1];
        b = rgb[2];
    }
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "shadersmod/client/Shaders", "shadersmod/client/Shaders");
    static const jmethodID mid = jni::static_method(&jni, cls, {"setEntityColor", "setEntityColor", "setEntityColor"},
                                                    "(FFFF)V", "(FFFF)V");
    if (cls != nullptr && mid != nullptr) {
        env->CallStaticVoidMethod(cls, mid, r, g, b, a);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// LayerArmorBase.doRenderLayer: this.renderLayer(EntityLivingBase, float x7, int)V (private)
// ---------------------------------------------------------------------------------------------------------------

struct ArmorIds {
    jclass layer = nullptr;
    jmethodID render_layer = nullptr;   // LayerArmorBase.renderLayer(EntityLivingBase, FFFFFFF, I)V
    jfieldID renderer = nullptr;        // LayerArmorBase.renderer
    jmethodID set_brightness = nullptr; // RendererLivingEntity.setBrightness(EntityLivingBase, F, Z)Z
    jmethodID unset_brightness = nullptr; // RendererLivingEntity.unsetBrightness()V
    jfieldID hurt_time = nullptr;       // EntityLivingBase.hurtTime
    jfieldID death_time = nullptr;      // EntityLivingBase.deathTime
    bool tint_ok = false;
};

const ArmorIds& armor_ids(events::JniContext* jni) noexcept {
    static const ArmorIds s_ids = [&] {
        ArmorIds ids{};
        ids.layer = jni::find_class(jni, "bkn", "net/minecraft/client/renderer/entity/layers/LayerArmorBase");
        const jclass renderer = jni::find_class(jni, "bjl", "net/minecraft/client/renderer/entity/RendererLivingEntity");
        const jclass living = jni::find_class(jni, "pr", "net/minecraft/entity/EntityLivingBase");
        ids.render_layer = jni::method(jni, ids.layer, {"a", "renderLayer", "func_177182_a"}, "(Lpr;FFFFFFFI)V",
                                       "(Lnet/minecraft/entity/EntityLivingBase;FFFFFFFI)V");
        ids.renderer = jni::field(jni, ids.layer, {"a", "renderer", "field_177190_a"}, "Lbjl;",
                                  "Lnet/minecraft/client/renderer/entity/RendererLivingEntity;");
        ids.set_brightness = jni::method(jni, renderer, {"a", "setBrightness", "func_177092_a"}, "(Lpr;FZ)Z",
                                         "(Lnet/minecraft/entity/EntityLivingBase;FZ)Z");
        ids.unset_brightness = jni::method(jni, renderer, {"f", "unsetBrightness", "func_177091_f"}, "()V", "()V");
        ids.hurt_time = jni::field(jni, living, {"au", "hurtTime", "field_70737_aN"}, "I", "I");
        ids.death_time = jni::field(jni, living, {"ax", "deathTime", "field_70725_aQ"}, "I", "I");
        ids.tint_ok = ids.renderer != nullptr && ids.set_brightness != nullptr && ids.unset_brightness != nullptr &&
                      ids.hurt_time != nullptr && ids.death_time != nullptr;
        return ids;
    }();
    return s_ids;
}

void JNICALL native_render_armor_layer(JNIEnv* env, jclass /*cls*/, jobject layer, jobject entity, jfloat limb_swing,
                                       jfloat limb_swing_amount, jfloat partial_ticks, jfloat age_in_ticks,
                                       jfloat head_yaw, jfloat head_pitch, jfloat scale, jint armor_slot) noexcept {
    events::JniContext jni = jni_for(env);
    const ArmorIds& ids = armor_ids(&jni);
    if (ids.layer == nullptr || ids.render_layer == nullptr) {
        return;
    }
    const auto call_original = [&]() noexcept {
        env->CallNonvirtualVoidMethod(layer, ids.layer, ids.render_layer, entity, limb_swing, limb_swing_amount,
                                      partial_ticks, age_in_ticks, head_yaw, head_pitch, scale, armor_slot);
    };

    if (!modules::adjust_hurtcam_tints_armor() || !ids.tint_ok || entity == nullptr) {
        call_original();
        return;
    }
    // setBrightness(entity, pt, true) differs from the vanilla `false` only for a hurt / dying entity.
    const bool hurt = env->GetIntField(entity, ids.hurt_time) > 0 || env->GetIntField(entity, ids.death_time) > 0;
    if (!hurt) {
        call_original();
        return;
    }
    jobject renderer = env->GetObjectField(layer, ids.renderer);
    if (renderer == nullptr) {
        call_original();
        return;
    }
    bool brightness = env->CallBooleanMethod(renderer, ids.set_brightness, entity, partial_ticks, JNI_TRUE) != JNI_FALSE;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        brightness = false;
    }
    call_original();
    if (brightness && !env->ExceptionCheck()) { // a pending exception from renderLayer propagates to Java
        env->CallVoidMethod(renderer, ids.unset_brightness);
    }
    env->DeleteLocalRef(renderer);
}

// ---------------------------------------------------------------------------------------------------------------
// Hook table
// ---------------------------------------------------------------------------------------------------------------

constexpr const char* kEntityRenderer = "net/minecraft/client/renderer/EntityRenderer";
constexpr const char* kRendererLiving = "net/minecraft/client/renderer/entity/RendererLivingEntity";
constexpr const char* kLayerArmorBase = "net/minecraft/client/renderer/entity/layers/LayerArmorBase";
constexpr const char* kGl11 = "org/lwjgl/opengl/GL11";
constexpr const char* kShaders = "shadersmod/client/Shaders";

constexpr bytecode::ExtraMemberNames kSetBrightness{
    "a", "(Lpr;FZ)Z",
    "setBrightness", "(Lnet/minecraft/entity/EntityLivingBase;FZ)Z",
    "func_177092_a", "(Lnet/minecraft/entity/EntityLivingBase;FZ)Z"};

// EntityRenderer (all methods: setupCameraTransform, renderHand + OptiFine overload) -> this.hurtCameraEffect(pt).
const ExtraHookSpec kHookHurtCameraEffect{
    "bfk", kEntityRenderer,
    {},
    "bfk", kEntityRenderer,
    {"d", "(F)V", "hurtCameraEffect", "(F)V", "func_78482_e", "(F)V"},
    HookInsnKind::InvokeInstance,
    "adjustHurtcamHurtCameraEffect", "(Ljava/lang/Object;F)V",
    reinterpret_cast<void*>(&native_hurt_camera_effect),
};

// RendererLivingEntity.setBrightness -> GL11.glTexEnv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, brightnessBuffer).
const ExtraHookSpec kHookTexEnv{
    "bjl", kRendererLiving,
    kSetBrightness,
    kGl11, kGl11,
    {"glTexEnv", "(IILjava/nio/FloatBuffer;)V",
     "glTexEnv", "(IILjava/nio/FloatBuffer;)V",
     "glTexEnv", "(IILjava/nio/FloatBuffer;)V"},
    HookInsnKind::InvokeStatic,
    "adjustHurtcamTexEnv", "(IILjava/lang/Object;)V",
    reinterpret_cast<void*>(&native_gl_tex_env),
};

// Same call on LWJGL 3 based clients (GL11.glTexEnvfv).
const ExtraHookSpec kHookTexEnvfv{
    "bjl", kRendererLiving,
    kSetBrightness,
    kGl11, kGl11,
    {"glTexEnvfv", "(IILjava/nio/FloatBuffer;)V",
     "glTexEnvfv", "(IILjava/nio/FloatBuffer;)V",
     "glTexEnvfv", "(IILjava/nio/FloatBuffer;)V"},
    HookInsnKind::InvokeStatic,
    "adjustHurtcamTexEnvfv", "(IILjava/lang/Object;)V",
    reinterpret_cast<void*>(&native_gl_tex_envfv),
};

// OptiFine: RendererLivingEntity.setBrightness -> Shaders.setEntityColor(r, g, b, a).
const ExtraHookSpec kHookShadersEntityColor{
    "bjl", kRendererLiving,
    kSetBrightness,
    kShaders, kShaders,
    {"setEntityColor", "(FFFF)V", "setEntityColor", "(FFFF)V", "setEntityColor", "(FFFF)V"},
    HookInsnKind::InvokeStatic,
    "adjustHurtcamShadersEntityColor", "(FFFF)V",
    reinterpret_cast<void*>(&native_shaders_set_entity_color),
};

// LayerArmorBase.doRenderLayer -> this.renderLayer(entity, ..., armorSlot) (4 calls, slots 4..1).
const ExtraHookSpec kHookArmorRenderLayer{
    "bkn", kLayerArmorBase,
    {"a", "(Lpr;FFFFFFF)V",
     "doRenderLayer", "(Lnet/minecraft/entity/EntityLivingBase;FFFFFFF)V",
     "func_177141_a", "(Lnet/minecraft/entity/EntityLivingBase;FFFFFFF)V"},
    "bkn", kLayerArmorBase,
    {"a", "(Lpr;FFFFFFFI)V",
     "renderLayer", "(Lnet/minecraft/entity/EntityLivingBase;FFFFFFFI)V",
     "func_177182_a", "(Lnet/minecraft/entity/EntityLivingBase;FFFFFFFI)V"},
    HookInsnKind::InvokeInstance,
    "adjustHurtcamRenderArmorLayer", "(Ljava/lang/Object;Ljava/lang/Object;FFFFFFFI)V",
    reinterpret_cast<void*>(&native_render_armor_layer),
};

} // namespace

const bytecode::ExtraHookSpec* const* hurtcam_hooks() noexcept {
    static const ExtraHookSpec* const s_hooks[] = {
        &kHookHurtCameraEffect,
        &kHookTexEnv,
        &kHookTexEnvfv,
        &kHookShadersEntityColor,
        &kHookArmorRenderLayer,
        nullptr,
    };
    return s_hooks;
}

} // namespace slinky::adjust::hooks
