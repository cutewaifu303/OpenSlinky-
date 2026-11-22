// Adjust world rendering hooks (Minecraft 1.8.9) for Motion Blur and Atmosphere.
//
// Motion Blur (modules/adjust/render/motion_blur):
//   * EntityRenderer.updateCameraAndRender: this.renderWorld(F, J)          -> adjustWorldFxRenderWorld
//       beginFrame before, renderMonkey + renderPost after the original.
//   * EntityRenderer.renderWorldPass: this.setupCameraTransform(F, I)        -> adjustWorldFxSetupCameraTransform
//       captureMatrices (Monkey mode) after the original.
//   * RenderGlobal.renderEntities: RenderManager.renderEntitySimple(Entity, F) -> adjustWorldFxRenderEntitySimple
//       entity mask around the original (Monkey mode, local player).
// Atmosphere (modules/adjust/render/atmosphere):
//   * EntityRenderer.updateLightmap: GameSettings.gammaSetting (getfield)    -> adjustWorldFxGamma
//   * EntityRenderer.updateLightmap: EntityPlayerSP / EntityLivingBase.isPotionActive(Potion)
//   * EntityRenderer.updateFogColor: EntityLivingBase.isPotionActive(Potion) -> adjustWorldFxIsPotionActive
//   * EntityRenderer.updateLightmap: DynamicTexture.updateDynamicTexture()   -> adjustWorldFxUpdateLightmapTexture
//   * EntityRenderer.updateLightmap: Config.isCustomColors() (OptiFine)      -> adjustWorldFxIsCustomColors
//   * EntityRenderer.updateFogColor: GlStateManager.clearColor(FFFF)         -> adjustWorldFxFogClearColor
//   * EntityRenderer.updateFogColor: Shaders.setClearColor(FFFF) (OptiFine)  -> adjustWorldFxFogClearColorShaders
//   * EntityRenderer.setupFog: GlStateManager.enableFog()                    -> adjustWorldFxSetupFogEnd
//   * World.getCelestialAngle / getMoonPhase / getCurrentMoonPhaseFactor: WorldInfo.getWorldTime()
//                                                                            -> adjustWorldFxWorldInfoTime
//   * World.getCelestialAngle / getMoonPhase (Forge): World.getWorldTime()   -> adjustWorldFxWorldTime
//
// Every native calls the original member when its module is not running, so vanilla behaviour is unchanged.
// JNI ids are resolved before the original call; after it, nothing but raw GL runs while a Java exception is
// pending (the exception is left for the caller).

#include "adjust/hooks/hooks.hpp"

#include "adjust/jni_lookup.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/atmosphere/atmosphere.hpp"
#include "modules/adjust/render/motion_blur/motion_blur.hpp"

namespace slinky::adjust::hooks {

const bytecode::ExtraHookSpec* const* world_fx_hooks() noexcept;

namespace {

using bytecode::ExtraHookSpec;
using bytecode::HookInsnKind;

constexpr const char* kEntityRendererNamed = "net/minecraft/client/renderer/EntityRenderer";
constexpr const char* kGlStateNamed = "net/minecraft/client/renderer/GlStateManager";

// ===============================================================================================================
// Motion Blur
// ===============================================================================================================

// EntityRenderer.renderWorld(FJ)V, called from updateCameraAndRender
void JNICALL native_render_world(JNIEnv* env, jclass /*cls*/, jobject entity_renderer, jfloat partial_ticks,
                                 jlong finish_time_nano) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "bfk", kEntityRendererNamed);
    static const jmethodID mid = jni::method(&jni, cls, {"b", "renderWorld", "func_78471_a"}, "(FJ)V", "(FJ)V");
    if (mid == nullptr) {
        return;
    }
    modules::adjust_motion_blur_begin_frame(&jni);
    env->CallVoidMethod(entity_renderer, mid, partial_ticks, finish_time_nano);
    if (env->ExceptionCheck()) {
        return;
    }
    modules::adjust_motion_blur_end_frame();
}

// EntityRenderer.setupCameraTransform(FI)V, called from renderWorldPass
void JNICALL native_setup_camera_transform(JNIEnv* env, jclass /*cls*/, jobject entity_renderer,
                                           jfloat partial_ticks, jint pass) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "bfk", kEntityRendererNamed);
    static const jmethodID mid =
        jni::method(&jni, cls, {"a", "setupCameraTransform", "func_78479_a"}, "(FI)V", "(FI)V");
    if (mid == nullptr) {
        return;
    }
    env->CallVoidMethod(entity_renderer, mid, partial_ticks, pass);
    if (env->ExceptionCheck() || !modules::adjust_motion_blur_wants_matrices()) {
        return;
    }
    modules::adjust_motion_blur_capture_matrices(&jni, partial_ticks);
}

// RenderManager.renderEntitySimple(Entity, F)Z, called from RenderGlobal.renderEntities
jboolean JNICALL native_render_entity_simple(JNIEnv* env, jclass /*cls*/, jobject render_manager, jobject entity,
                                             jfloat partial_ticks) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "biu", "net/minecraft/client/renderer/entity/RenderManager");
    static const jmethodID mid = jni::method(&jni, cls, {"a", "renderEntitySimple", "func_147937_a"}, "(Lpk;F)Z",
                                             "(Lnet/minecraft/entity/Entity;F)Z");
    static const jfieldID render_outlines =
        jni::field(&jni, cls, {"r", "renderOutlines", "field_178639_r"}, "Z", "Z");
    if (mid == nullptr) {
        return JNI_FALSE;
    }
    bool capturing = false;
    if (modules::adjust_motion_blur_mask_possible() && render_outlines != nullptr &&
        env->GetBooleanField(render_manager, render_outlines) == JNI_FALSE) {
        capturing = modules::adjust_motion_blur_begin_entity_mask(&jni, entity);
    }
    const jboolean result = env->CallBooleanMethod(render_manager, mid, entity, partial_ticks);
    if (capturing) {
        modules::adjust_motion_blur_end_entity_mask(); // raw GL only, also with a pending exception
    }
    return result;
}

// ===============================================================================================================
// Atmosphere - lightmap
// ===============================================================================================================

// getfield GameSettings.gammaSetting inside EntityRenderer.updateLightmap (GammaEvent)
jfloat JNICALL native_gamma(JNIEnv* env, jclass /*cls*/, jobject settings) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "avh", "net/minecraft/client/settings/GameSettings");
    static const jfieldID fid = jni::field(&jni, cls, {"aJ", "gammaSetting", "field_74333_Y"}, "F", "F");
    float gamma = 0.0f;
    if (modules::adjust_atmosphere_gamma(&gamma)) {
        return gamma;
    }
    return fid != nullptr ? env->GetFloatField(settings, fid) : 0.0f;
}

// EntityLivingBase.isPotionActive(Potion)Z inside updateLightmap / updateFogColor ("No Night Vision")
jboolean JNICALL native_is_potion_active(JNIEnv* env, jclass /*cls*/, jobject entity, jobject potion) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass living = jni::find_class(&jni, "pr", "net/minecraft/entity/EntityLivingBase");
    static const jmethodID mid = jni::method(&jni, living, {"a", "isPotionActive", "func_70644_a"}, "(Lpe;)Z",
                                             "(Lnet/minecraft/potion/Potion;)Z");
    static const jobject night_vision = [&]() noexcept -> jobject {
        const jclass potion_cls = jni::find_class(&jni, "pe", "net/minecraft/potion/Potion");
        const jfieldID fid = jni::static_field(&jni, potion_cls, {"r", "nightVision", "field_76439_r"}, "Lpe;",
                                               "Lnet/minecraft/potion/Potion;");
        if (fid == nullptr) {
            return nullptr;
        }
        jobject local = env->GetStaticObjectField(potion_cls, fid);
        if (local == nullptr) {
            return nullptr;
        }
        jobject global = env->NewGlobalRef(local);
        env->DeleteLocalRef(local);
        return global;
    }();
    if (mid == nullptr) {
        return JNI_FALSE;
    }
    if (potion != nullptr && night_vision != nullptr && modules::adjust_atmosphere_disables_night_vision() &&
        env->IsSameObject(potion, night_vision) != JNI_FALSE) {
        return JNI_FALSE;
    }
    return env->CallBooleanMethod(entity, mid, potion);
}

// DynamicTexture.updateDynamicTexture() inside updateLightmap (Lighting = Custom: every texel = lighting colour)
void JNICALL native_update_lightmap_texture(JNIEnv* env, jclass /*cls*/, jobject texture) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "blz", "net/minecraft/client/renderer/texture/DynamicTexture");
    static const jmethodID mid =
        jni::method(&jni, cls, {"d", "updateDynamicTexture", "func_110564_a"}, "()V", "()V");
    static const jfieldID data_fid = jni::field(&jni, cls, {"f", "dynamicTextureData", "field_110566_b"}, "[I", "[I");
    if (mid == nullptr) {
        return;
    }
    adjust::Argb color = 0;
    if (data_fid != nullptr && texture != nullptr && modules::adjust_atmosphere_lighting_color(&color)) {
        auto data = static_cast<jintArray>(env->GetObjectField(texture, data_fid));
        if (data != nullptr) {
            constexpr jsize kMax = 256; // 16x16 lightmap
            const jsize length = env->GetArrayLength(data);
            if (length > 0 && length <= kMax) {
                jint pixels[kMax];
                const jint argb = static_cast<jint>(0xFF000000u | (color & 0x00FFFFFFu)); // 255 << 24 | k, l, i1
                for (jsize i = 0; i < length; ++i) {
                    pixels[i] = argb;
                }
                env->SetIntArrayRegion(data, 0, length, pixels);
            }
            env->DeleteLocalRef(data);
        }
    }
    env->CallVoidMethod(texture, mid);
}

// OptiFine Config.isCustomColors() inside updateLightmap (LightmapStateEvent.customColors = false)
jboolean JNICALL native_is_custom_colors(JNIEnv* env, jclass /*cls*/) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "Config", "Config");
    static const jmethodID mid =
        jni::static_method(&jni, cls, {"isCustomColors", "isCustomColors", "isCustomColors"}, "()Z", "()Z");
    if (modules::adjust_atmosphere_disables_custom_colors() || mid == nullptr) {
        return JNI_FALSE;
    }
    return env->CallStaticBooleanMethod(cls, mid);
}

// ===============================================================================================================
// Atmosphere - fog colour
// ===============================================================================================================

struct FogColorFields {
    jfieldID red = nullptr;
    jfieldID green = nullptr;
    jfieldID blue = nullptr;
};

const FogColorFields& fog_color_fields(events::JniContext* jni) noexcept {
    static const FogColorFields s_fields = [&] {
        FogColorFields f{};
        const jclass cls = jni::find_class(jni, "bfk", kEntityRendererNamed);
        f.red = jni::field(jni, cls, {"Q", "fogColorRed", "field_175080_Q"}, "F", "F");
        f.green = jni::field(jni, cls, {"R", "fogColorGreen", "field_175082_R"}, "F", "F");
        f.blue = jni::field(jni, cls, {"S", "fogColorBlue", "field_175081_S"}, "F", "F");
        return f;
    }();
    return s_fields;
}

// FogRgbEvent at the end of updateFogColor: blends the colour and writes it back into EntityRenderer.fogColor*.
// The arguments are this.fogColorRed/Green/Blue and 0.0F.
void apply_fog_rgb(events::JniContext* jni, float* red, float* green, float* blue) noexcept {
    const FogColorFields& f = fog_color_fields(jni);
    if (f.red == nullptr || f.green == nullptr || f.blue == nullptr) {
        return;
    }
    float r = *red;
    float g = *green;
    float b = *blue;
    if (!modules::adjust_atmosphere_fog_color(&r, &g, &b)) {
        return;
    }
    JNIEnv* env = jni->env;
    jobject mc = minecraft::mc_get_instance(jni);
    jobject renderer = (mc != nullptr) ? minecraft::mc_get_entity_renderer(jni, mc) : nullptr;
    if (renderer != nullptr) {
        env->SetFloatField(renderer, f.red, r);
        env->SetFloatField(renderer, f.green, g);
        env->SetFloatField(renderer, f.blue, b);
        *red = r;
        *green = g;
        *blue = b;
        env->DeleteLocalRef(renderer);
    }
    if (mc != nullptr) {
        env->DeleteLocalRef(mc);
    }
}

// GlStateManager.clearColor(FFFF)V inside updateFogColor (vanilla / Forge)
void JNICALL native_fog_clear_color(JNIEnv* env, jclass /*cls*/, jfloat red, jfloat green, jfloat blue,
                                    jfloat alpha) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "bfl", kGlStateNamed);
    static const jmethodID mid =
        jni::static_method(&jni, cls, {"a", "clearColor", "func_179082_a"}, "(FFFF)V", "(FFFF)V");
    if (mid == nullptr) {
        return;
    }
    apply_fog_rgb(&jni, &red, &green, &blue);
    env->CallStaticVoidMethod(cls, mid, red, green, blue, alpha);
}

// OptiFine Shaders.setClearColor(FFFF)V inside updateFogColor
void JNICALL native_fog_clear_color_shaders(JNIEnv* env, jclass /*cls*/, jfloat red, jfloat green, jfloat blue,
                                            jfloat alpha) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "shadersmod/client/Shaders", "shadersmod/client/Shaders");
    static const jmethodID mid = jni::static_method(
        &jni, cls, {"setClearColor", "setClearColor", "setClearColor"}, "(FFFF)V", "(FFFF)V");
    if (mid == nullptr) {
        return;
    }
    apply_fog_rgb(&jni, &red, &green, &blue);
    env->CallStaticVoidMethod(cls, mid, red, green, blue, alpha);
}

// ===============================================================================================================
// Atmosphere - fog parameters
// ===============================================================================================================

struct FogIds {
    jclass gl = nullptr;
    jmethodID enable_fog = nullptr;
    jmethodID set_fog = nullptr;
    jmethodID set_fog_start = nullptr;
    jmethodID set_fog_end = nullptr;
    jmethodID set_fog_density = nullptr;
    jfieldID fog_state = nullptr;     // GlStateManager.fogState (static)
    jfieldID state_mode = nullptr;    // GlStateManager$FogState.mode
    jfieldID state_density = nullptr; // GlStateManager$FogState.density
    jfieldID state_start = nullptr;   // GlStateManager$FogState.start
    jfieldID state_end = nullptr;     // GlStateManager$FogState.end
    jfieldID far_plane = nullptr;     // EntityRenderer.farPlaneDistance
    jfieldID fog_standard = nullptr;  // EntityRenderer.fogStandard (OptiFine only)
    jclass living = nullptr;
    jmethodID is_potion_active_id = nullptr; // EntityLivingBase.isPotionActive(I)Z
    bool usable = false;
};

const FogIds& fog_ids(events::JniContext* jni) noexcept {
    static const FogIds s_ids = [&] {
        FogIds ids{};
        ids.gl = jni::find_class(jni, "bfl", kGlStateNamed);
        ids.enable_fog = jni::static_method(jni, ids.gl, {"m", "enableFog", "func_179127_m"}, "()V", "()V");
        ids.set_fog = jni::static_method(jni, ids.gl, {"d", "setFog", "func_179093_d"}, "(I)V", "(I)V");
        ids.set_fog_start = jni::static_method(jni, ids.gl, {"b", "setFogStart", "func_179102_b"}, "(F)V", "(F)V");
        ids.set_fog_end = jni::static_method(jni, ids.gl, {"c", "setFogEnd", "func_179153_c"}, "(F)V", "(F)V");
        ids.set_fog_density =
            jni::static_method(jni, ids.gl, {"a", "setFogDensity", "func_179095_a"}, "(F)V", "(F)V");
        ids.fog_state = jni::static_field(jni, ids.gl, {"g", "fogState", "field_179155_g"}, "Lbfl$k;",
                                          "Lnet/minecraft/client/renderer/GlStateManager$FogState;");
        const jclass state = jni::find_class(jni, "bfl$k", "net/minecraft/client/renderer/GlStateManager$FogState");
        ids.state_mode = jni::field(jni, state, {"b", "mode", "field_179047_b"}, "I", "I");
        ids.state_density = jni::field(jni, state, {"c", "density", "field_179048_c"}, "F", "F");
        ids.state_start = jni::field(jni, state, {"d", "start", "field_179045_d"}, "F", "F");
        ids.state_end = jni::field(jni, state, {"e", "end", "field_179046_e"}, "F", "F");
        const jclass renderer = jni::find_class(jni, "bfk", kEntityRendererNamed);
        ids.far_plane = jni::field(jni, renderer, {"k", "farPlaneDistance", "field_78530_s"}, "F", "F");
        ids.fog_standard = jni::field(jni, renderer, {"fogStandard", "fogStandard", "fogStandard"}, "Z", "Z");
        ids.living = jni::find_class(jni, "pr", "net/minecraft/entity/EntityLivingBase");
        ids.is_potion_active_id =
            jni::method(jni, ids.living, {"k", "isPotionActive", "func_82165_m"}, "(I)Z", "(I)Z");
        ids.usable = ids.set_fog != nullptr && ids.set_fog_start != nullptr && ids.set_fog_end != nullptr &&
                     ids.set_fog_density != nullptr && ids.fog_state != nullptr && ids.state_mode != nullptr &&
                     ids.state_density != nullptr && ids.state_start != nullptr && ids.state_end != nullptr &&
                     ids.far_plane != nullptr;
        return ids;
    }();
    return s_ids;
}

// GLContext.getCapabilities().GL_NV_fog_distance (constant for the context; resolved once).
bool nv_fog_distance(events::JniContext* jni) noexcept {
    static const bool s_supported = [&]() noexcept {
        JNIEnv* env = jni->env;
        const jclass context = jni::find_class(jni, "org/lwjgl/opengl/GLContext", "org/lwjgl/opengl/GLContext");
        const jclass caps_cls =
            jni::find_class(jni, "org/lwjgl/opengl/ContextCapabilities", "org/lwjgl/opengl/ContextCapabilities");
        if (context == nullptr || caps_cls == nullptr) {
            return false;
        }
        const jmethodID get_caps =
            env->GetStaticMethodID(context, "getCapabilities", "()Lorg/lwjgl/opengl/ContextCapabilities;");
        const jfieldID nv = env->GetFieldID(caps_cls, "GL_NV_fog_distance", "Z");
        if (get_caps == nullptr || nv == nullptr) {
            env->ExceptionClear();
            return false;
        }
        jobject caps = env->CallStaticObjectMethod(context, get_caps);
        if (env->ExceptionCheck() || caps == nullptr) {
            env->ExceptionClear();
            return false;
        }
        const bool supported = env->GetBooleanField(caps, nv) != JNI_FALSE;
        env->DeleteLocalRef(caps);
        return supported;
    }();
    return s_supported;
}

// The blindness branch of setupFog also uses linear fog but posts radial = false.
bool view_entity_blind(events::JniContext* jni, const FogIds& ids, jobject mc) noexcept {
    if (ids.living == nullptr || ids.is_potion_active_id == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    jobject entity = minecraft::mc_get_render_view_entity(jni, mc);
    if (entity == nullptr) {
        return false;
    }
    bool blind = false;
    if (env->IsInstanceOf(entity, ids.living) != JNI_FALSE) {
        blind = env->CallBooleanMethod(entity, ids.is_potion_active_id, 15) != JNI_FALSE; // Potion.blindness
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            blind = false;
        }
    }
    env->DeleteLocalRef(entity);
    return blind;
}

// FogParametersEvent: setupFog's branch has just set mode/start/end/density through GlStateManager; read them back
// from its cache, let Atmosphere change them and apply the result (EntityRenderer.applyFogParameters).
void apply_atmosphere_fog(events::JniContext* jni, const FogIds& ids) noexcept {
    JNIEnv* env = jni->env;
    jobject mc = minecraft::mc_get_instance(jni);
    jobject renderer = (mc != nullptr) ? minecraft::mc_get_entity_renderer(jni, mc) : nullptr;
    jobject state = env->GetStaticObjectField(ids.gl, ids.fog_state);
    if (renderer != nullptr && state != nullptr) {
        modules::AtmosphereFogParams params{};
        params.far_plane_distance = env->GetFloatField(renderer, ids.far_plane);
        params.mode = env->GetIntField(state, ids.state_mode);
        params.density = env->GetFloatField(state, ids.state_density);
        params.start = env->GetFloatField(state, ids.state_start);
        params.end = env->GetFloatField(state, ids.state_end);
        params.radial = params.mode == 9729 && nv_fog_distance(jni) && !view_entity_blind(jni, ids, mc);
        if (modules::adjust_atmosphere_apply_fog(&params)) {
            env->CallStaticVoidMethod(ids.gl, ids.set_fog, params.mode);
            env->CallStaticVoidMethod(ids.gl, ids.set_fog_start, params.start);
            env->CallStaticVoidMethod(ids.gl, ids.set_fog_end, params.end);
            env->CallStaticVoidMethod(ids.gl, ids.set_fog_density, params.density);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
        }
        // this.fogStandard = atmosphereFogActive ? false : event.fogStandard (OptiFine field)
        if (ids.fog_standard != nullptr) {
            env->SetBooleanField(renderer, ids.fog_standard, JNI_FALSE);
        }
    }
    if (state != nullptr) {
        env->DeleteLocalRef(state);
    }
    if (renderer != nullptr) {
        env->DeleteLocalRef(renderer);
    }
    if (mc != nullptr) {
        env->DeleteLocalRef(mc);
    }
}

// GlStateManager.enableFog() at the end of EntityRenderer.setupFog
void JNICALL native_setup_fog_end(JNIEnv* env, jclass /*cls*/) noexcept {
    events::JniContext jni = jni_for(env);
    const FogIds& ids = fog_ids(&jni);
    if (ids.enable_fog == nullptr) {
        return;
    }
    if (ids.usable && modules::adjust_atmosphere_fog_enabled()) {
        apply_atmosphere_fog(&jni, ids);
    }
    env->CallStaticVoidMethod(ids.gl, ids.enable_fog);
}

// ===============================================================================================================
// Atmosphere - world time
// ===============================================================================================================

// Only the client world's time is overridden (Adjust's patched World also affected the integrated server).
bool is_client_world_info(events::JniContext* jni, jobject world_info) noexcept {
    static const jclass world_cls = jni::find_class(jni, "adm", "net/minecraft/world/World");
    static const jfieldID info_fid = jni::field(jni, world_cls, {"x", "worldInfo", "field_72986_A"}, "Lato;",
                                                "Lnet/minecraft/world/storage/WorldInfo;");
    if (info_fid == nullptr || world_info == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    jobject mc = minecraft::mc_get_instance(jni);
    jobject world = (mc != nullptr) ? minecraft::mc_get_world(jni, mc) : nullptr;
    bool same = false;
    if (world != nullptr) {
        jobject info = env->GetObjectField(world, info_fid);
        same = info != nullptr && env->IsSameObject(info, world_info) != JNI_FALSE;
        if (info != nullptr) {
            env->DeleteLocalRef(info);
        }
        env->DeleteLocalRef(world);
    }
    if (mc != nullptr) {
        env->DeleteLocalRef(mc);
    }
    return same;
}

// WorldInfo.getWorldTime()J inside World.getCelestialAngle / getMoonPhase / getCurrentMoonPhaseFactor
jlong JNICALL native_world_info_time(JNIEnv* env, jclass /*cls*/, jobject world_info) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "ato", "net/minecraft/world/storage/WorldInfo");
    static const jmethodID mid = jni::method(&jni, cls, {"g", "getWorldTime", "func_76073_f"}, "()J", "()J");
    int64_t time = 0;
    if (modules::adjust_atmosphere_time_override(&time) && is_client_world_info(&jni, world_info)) {
        return static_cast<jlong>(time);
    }
    return mid != nullptr ? env->CallLongMethod(world_info, mid) : 0;
}

// World.getWorldTime()J inside World.getCelestialAngle / getMoonPhase (Forge routes the time through it)
jlong JNICALL native_world_time(JNIEnv* env, jclass /*cls*/, jobject world) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "adm", "net/minecraft/world/World");
    static const jclass client_cls = jni::find_class(&jni, "bdb", "net/minecraft/client/multiplayer/WorldClient");
    static const jmethodID mid = jni::method(&jni, cls, {"L", "getWorldTime", "func_72820_D"}, "()J", "()J");
    int64_t time = 0;
    if (client_cls != nullptr && world != nullptr && modules::adjust_atmosphere_time_override(&time) &&
        env->IsInstanceOf(world, client_cls) != JNI_FALSE) {
        return static_cast<jlong>(time);
    }
    return mid != nullptr ? env->CallLongMethod(world, mid) : 0;
}

// ===============================================================================================================
// Hook table
// ===============================================================================================================

constexpr const char* kRenderGlobalNamed = "net/minecraft/client/renderer/RenderGlobal";
constexpr const char* kRenderManagerNamed = "net/minecraft/client/renderer/entity/RenderManager";
constexpr const char* kGameSettingsNamed = "net/minecraft/client/settings/GameSettings";
constexpr const char* kEntityPlayerSPNamed = "net/minecraft/client/entity/EntityPlayerSP";
constexpr const char* kEntityLivingNamed = "net/minecraft/entity/EntityLivingBase";
constexpr const char* kDynamicTextureNamed = "net/minecraft/client/renderer/texture/DynamicTexture";
constexpr const char* kShadersNamed = "shadersmod/client/Shaders";
constexpr const char* kWorldNamed = "net/minecraft/world/World";
constexpr const char* kWorldInfoNamed = "net/minecraft/world/storage/WorldInfo";

// Patched methods
constexpr bytecode::ExtraMemberNames kUpdateCameraAndRender{
    "a", "(FJ)V", "updateCameraAndRender", "(FJ)V", "func_181560_a", "(FJ)V"};
constexpr bytecode::ExtraMemberNames kRenderWorldPass{
    "a", "(IFJ)V", "renderWorldPass", "(IFJ)V", "func_175068_a", "(IFJ)V"};
constexpr bytecode::ExtraMemberNames kUpdateLightmap{"g", "(F)V", "updateLightmap", "(F)V", "func_78472_g", "(F)V"};
constexpr bytecode::ExtraMemberNames kUpdateFogColor{"i", "(F)V", "updateFogColor", "(F)V", "func_78466_h", "(F)V"};
constexpr bytecode::ExtraMemberNames kSetupFog{"a", "(IF)V", "setupFog", "(IF)V", "func_78468_a", "(IF)V"};
constexpr bytecode::ExtraMemberNames kRenderEntities{
    "a", "(Lpk;Lbia;F)V",
    "renderEntities", "(Lnet/minecraft/entity/Entity;Lnet/minecraft/client/renderer/culling/ICamera;F)V",
    "func_180446_a", "(Lnet/minecraft/entity/Entity;Lnet/minecraft/client/renderer/culling/ICamera;F)V"};
constexpr bytecode::ExtraMemberNames kGetCelestialAngle{
    "c", "(F)F", "getCelestialAngle", "(F)F", "func_72826_c", "(F)F"};
constexpr bytecode::ExtraMemberNames kGetMoonPhase{"x", "()I", "getMoonPhase", "()I", "func_72853_d", "()I"};
constexpr bytecode::ExtraMemberNames kGetCurrentMoonPhaseFactor{
    "y", "()F", "getCurrentMoonPhaseFactor", "()F", "func_130001_d", "()F"};

// Replaced members
constexpr bytecode::ExtraMemberNames kRenderWorld{"b", "(FJ)V", "renderWorld", "(FJ)V", "func_78471_a", "(FJ)V"};
constexpr bytecode::ExtraMemberNames kSetupCameraTransform{
    "a", "(FI)V", "setupCameraTransform", "(FI)V", "func_78479_a", "(FI)V"};
constexpr bytecode::ExtraMemberNames kRenderEntitySimple{
    "a", "(Lpk;F)Z",
    "renderEntitySimple", "(Lnet/minecraft/entity/Entity;F)Z",
    "func_147937_a", "(Lnet/minecraft/entity/Entity;F)Z"};
constexpr bytecode::ExtraMemberNames kGammaSetting{"aJ", "F", "gammaSetting", "F", "field_74333_Y", "F"};
constexpr bytecode::ExtraMemberNames kIsPotionActive{
    "a", "(Lpe;)Z",
    "isPotionActive", "(Lnet/minecraft/potion/Potion;)Z",
    "func_70644_a", "(Lnet/minecraft/potion/Potion;)Z"};
constexpr bytecode::ExtraMemberNames kUpdateDynamicTexture{
    "d", "()V", "updateDynamicTexture", "()V", "func_110564_a", "()V"};
constexpr bytecode::ExtraMemberNames kIsCustomColors{
    "isCustomColors", "()Z", "isCustomColors", "()Z", "isCustomColors", "()Z"};
constexpr bytecode::ExtraMemberNames kClearColor{
    "a", "(FFFF)V", "clearColor", "(FFFF)V", "func_179082_a", "(FFFF)V"};
constexpr bytecode::ExtraMemberNames kShadersSetClearColor{
    "setClearColor", "(FFFF)V", "setClearColor", "(FFFF)V", "setClearColor", "(FFFF)V"};
constexpr bytecode::ExtraMemberNames kEnableFog{"m", "()V", "enableFog", "()V", "func_179127_m", "()V"};
constexpr bytecode::ExtraMemberNames kWorldInfoGetWorldTime{
    "g", "()J", "getWorldTime", "()J", "func_76073_f", "()J"};
constexpr bytecode::ExtraMemberNames kWorldGetWorldTime{"L", "()J", "getWorldTime", "()J", "func_72820_D", "()J"};

// ---- Motion Blur ----------------------------------------------------------------------------------------------

const ExtraHookSpec kHookRenderWorld{
    "bfk", kEntityRendererNamed, kUpdateCameraAndRender,
    "bfk", kEntityRendererNamed, kRenderWorld,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxRenderWorld", "(Ljava/lang/Object;FJ)V",
    reinterpret_cast<void*>(&native_render_world),
};

const ExtraHookSpec kHookSetupCameraTransform{
    "bfk", kEntityRendererNamed, kRenderWorldPass,
    "bfk", kEntityRendererNamed, kSetupCameraTransform,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxSetupCameraTransform", "(Ljava/lang/Object;FI)V",
    reinterpret_cast<void*>(&native_setup_camera_transform),
};

const ExtraHookSpec kHookRenderEntitySimple{
    "bfr", kRenderGlobalNamed, kRenderEntities,
    "biu", kRenderManagerNamed, kRenderEntitySimple,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxRenderEntitySimple", "(Ljava/lang/Object;Ljava/lang/Object;F)Z",
    reinterpret_cast<void*>(&native_render_entity_simple),
};

// ---- Atmosphere: lightmap -------------------------------------------------------------------------------------

const ExtraHookSpec kHookGamma{
    "bfk", kEntityRendererNamed, kUpdateLightmap,
    "avh", kGameSettingsNamed, kGammaSetting,
    HookInsnKind::GetField,
    "adjustWorldFxGamma", "(Ljava/lang/Object;)F",
    reinterpret_cast<void*>(&native_gamma),
};

// this.mc.thePlayer.isPotionActive(Potion.nightVision): javac emits the receiver's static type (EntityPlayerSP)
// as owner; the EntityLivingBase variant covers a cast receiver.
const ExtraHookSpec kHookLightmapPotionSP{
    "bfk", kEntityRendererNamed, kUpdateLightmap,
    "bew", kEntityPlayerSPNamed, kIsPotionActive,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxIsPotionActive", "(Ljava/lang/Object;Ljava/lang/Object;)Z",
    reinterpret_cast<void*>(&native_is_potion_active),
};

const ExtraHookSpec kHookLightmapPotionLiving{
    "bfk", kEntityRendererNamed, kUpdateLightmap,
    "pr", kEntityLivingNamed, kIsPotionActive,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxIsPotionActive", "(Ljava/lang/Object;Ljava/lang/Object;)Z",
    reinterpret_cast<void*>(&native_is_potion_active),
};

const ExtraHookSpec kHookLightmapTexture{
    "bfk", kEntityRendererNamed, kUpdateLightmap,
    "blz", kDynamicTextureNamed, kUpdateDynamicTexture,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxUpdateLightmapTexture", "(Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_update_lightmap_texture),
};

// OptiFine only (no match on vanilla / Forge bytecode).
const ExtraHookSpec kHookLightmapCustomColors{
    "bfk", kEntityRendererNamed, kUpdateLightmap,
    "Config", "Config", kIsCustomColors,
    HookInsnKind::InvokeStatic,
    "adjustWorldFxIsCustomColors", "()Z",
    reinterpret_cast<void*>(&native_is_custom_colors),
};

// ---- Atmosphere: fog colour -----------------------------------------------------------------------------------

const ExtraHookSpec kHookFogColorPotion{
    "bfk", kEntityRendererNamed, kUpdateFogColor,
    "pr", kEntityLivingNamed, kIsPotionActive,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxIsPotionActive", "(Ljava/lang/Object;Ljava/lang/Object;)Z",
    reinterpret_cast<void*>(&native_is_potion_active),
};

const ExtraHookSpec kHookFogClearColor{
    "bfk", kEntityRendererNamed, kUpdateFogColor,
    "bfl", kGlStateNamed, kClearColor,
    HookInsnKind::InvokeStatic,
    "adjustWorldFxFogClearColor", "(FFFF)V",
    reinterpret_cast<void*>(&native_fog_clear_color),
};

// OptiFine only.
const ExtraHookSpec kHookFogClearColorShaders{
    "bfk", kEntityRendererNamed, kUpdateFogColor,
    kShadersNamed, kShadersNamed, kShadersSetClearColor,
    HookInsnKind::InvokeStatic,
    "adjustWorldFxFogClearColorShaders", "(FFFF)V",
    reinterpret_cast<void*>(&native_fog_clear_color_shaders),
};

// ---- Atmosphere: fog parameters -------------------------------------------------------------------------------

const ExtraHookSpec kHookSetupFogEnd{
    "bfk", kEntityRendererNamed, kSetupFog,
    "bfl", kGlStateNamed, kEnableFog,
    HookInsnKind::InvokeStatic,
    "adjustWorldFxSetupFogEnd", "()V",
    reinterpret_cast<void*>(&native_setup_fog_end),
};

// ---- Atmosphere: world time -----------------------------------------------------------------------------------

const ExtraHookSpec kHookCelestialAngleTime{
    "adm", kWorldNamed, kGetCelestialAngle,
    "ato", kWorldInfoNamed, kWorldInfoGetWorldTime,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxWorldInfoTime", "(Ljava/lang/Object;)J",
    reinterpret_cast<void*>(&native_world_info_time),
};

const ExtraHookSpec kHookMoonPhaseTime{
    "adm", kWorldNamed, kGetMoonPhase,
    "ato", kWorldInfoNamed, kWorldInfoGetWorldTime,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxWorldInfoTime", "(Ljava/lang/Object;)J",
    reinterpret_cast<void*>(&native_world_info_time),
};

const ExtraHookSpec kHookMoonPhaseFactorTime{
    "adm", kWorldNamed, kGetCurrentMoonPhaseFactor,
    "ato", kWorldInfoNamed, kWorldInfoGetWorldTime,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxWorldInfoTime", "(Ljava/lang/Object;)J",
    reinterpret_cast<void*>(&native_world_info_time),
};

// Forge: getCelestialAngle / getMoonPhase call this.getWorldTime() (no match on vanilla bytecode).
const ExtraHookSpec kHookCelestialAngleTimeForge{
    "adm", kWorldNamed, kGetCelestialAngle,
    "adm", kWorldNamed, kWorldGetWorldTime,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxWorldTime", "(Ljava/lang/Object;)J",
    reinterpret_cast<void*>(&native_world_time),
};

const ExtraHookSpec kHookMoonPhaseTimeForge{
    "adm", kWorldNamed, kGetMoonPhase,
    "adm", kWorldNamed, kWorldGetWorldTime,
    HookInsnKind::InvokeInstance,
    "adjustWorldFxWorldTime", "(Ljava/lang/Object;)J",
    reinterpret_cast<void*>(&native_world_time),
};

} // namespace

const bytecode::ExtraHookSpec* const* world_fx_hooks() noexcept {
    static const ExtraHookSpec* const s_hooks[] = {
        &kHookRenderWorld,
        &kHookSetupCameraTransform,
        &kHookRenderEntitySimple,
        &kHookGamma,
        &kHookLightmapPotionSP,
        &kHookLightmapPotionLiving,
        &kHookLightmapTexture,
        &kHookLightmapCustomColors,
        &kHookFogColorPotion,
        &kHookFogClearColor,
        &kHookFogClearColorShaders,
        &kHookSetupFogEnd,
        &kHookCelestialAngleTime,
        &kHookMoonPhaseTime,
        &kHookMoonPhaseFactorTime,
        &kHookCelestialAngleTimeForge,
        &kHookMoonPhaseTimeForge,
        nullptr,
    };
    return s_hooks;
}

} // namespace slinky::adjust::hooks
