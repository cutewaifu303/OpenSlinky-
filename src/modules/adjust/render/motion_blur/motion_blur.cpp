// Port of com.adjustclient.module.impl.render.motionblur (MotionBlurModule, MotionBlurMode, MotionBlurRenderer,
// MonkeyBlurShader, DepthCapture, MoulberryBlur) and its shaders
// (assets/minecraft/shaders/{post,program}/adjust_phosphor_motion_blur.*, adjust_monkey_blur.*).
//
// Modes:
//   * Phosphor Blur  - feedback blend of the current frame with the previous output (weighted max, Mode = 0).
//   * Monkey Blur    - per-pixel camera motion blur: world position reconstructed from the depth buffer with the
//                      inverse matrices of this frame, reprojected with the previous frame's matrices; pixels of
//                      protected entities (entity mask) are neither blurred nor sampled.
//   * Moulberry Blur - accumulation buffer: previous accumulation blended over the current frame.
//
// Differences to Adjust (documented, intentional):
//   * Adjust runs the passes through Minecraft's ShaderGroup (post JSON + "sobel" vertex shader + "blit" pass).
//     This port uses its own GLSL 1.20 programs, two ping-pong colour targets and glBlitFramebuffer for the blit
//     passes; the fragment shaders are Adjust's. Every GL state it changes is saved and restored, so
//     GlStateManager's cache stays valid.
//   * Monkey entity mask: Adjust protects the local player and every entity with Adjust-Chams target settings
//     (FrozenPlayerRenderer.ENTITY_SETTINGS). The Adjust Chams port exposes no per-entity query, so only the local
//     player is protected.
//   * Adjust attaches the mask texture to Minecraft's framebuffer once and keeps it attached; this port attaches it
//     only while a protected entity renders (no foreign attachment left on Minecraft's framebuffer).
//   * Adjust's EntityRenderer.onResourceManagerReload destroy and the shutdown-hook onDisable are not needed (the
//     resources do not depend on resource packs; GL objects die with the context).

#include "modules/adjust/render/motion_blur/motion_blur.hpp"

#include <glad/glad.h>

#include <chrono>
#include <cmath>
#include <cstdint>

#include "adjust/jni_lookup.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"
#include "minecraft/mc_wrappers.hpp"

namespace slinky::modules {

namespace {

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

// MotionBlurMode
constexpr const char* kModes[3] = {"Phosphor Blur", "Monkey Blur", "Moulberry Blur"};
enum class Mode : int32_t { Phosphor = 0, Monkey = 1, Moulberry = 2 };

struct MotionBlurState {
    events::Module base{};

    config::ModeSetting mode{{}, "Mode", 3, 0, kModes, 0, 0, false, {}};
    config::FloatSetting strength{{}, "Strength", "%.1f", 0.1f, 0.0f, 1.0f, 10.0f, 1.0f, 1.0f};

    MotionBlurState() noexcept {
        base.name = "Motion Blur";
        base.description = "Blurs the world while the camera moves.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagHiddenFromHud; // Adjust default: hidden
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Motion Blur");
    }
};

MotionBlurState g_state{};

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_state.mode.render(gui, renderer);
    g_state.strength.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.mode.visit(archive, "mode");
    g_state.strength.visit(archive, "strength");
}

// ---------------------------------------------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------------------------------------------

// Replaces Minecraft's "sobel" vertex shader + ortho ProjMat: a unit quad mapped to the full viewport.
constexpr const char* kVertexShader = R"(#version 120
attribute vec2 Position;
varying vec2 texCoord;
void main() {
    texCoord = Position;
    gl_Position = vec4(Position * 2.0 - 1.0, 0.0, 1.0);
}
)";

// adjust_phosphor_motion_blur.fsh
constexpr const char* kPhosphorFragment = R"(#version 120
uniform sampler2D DiffuseSampler;
uniform sampler2D PrevSampler;
varying vec2 texCoord;
uniform float BlendFactor;
uniform float Mode;
void main() {
    // blur by TheKodeToad in Sol Client
    vec4 curr = texture2D(DiffuseSampler, texCoord);
    vec4 prev = texture2D(PrevSampler, texCoord);
    float feedback = clamp(BlendFactor, 0.0, 0.95);
    vec3 blended;
    if (Mode < 0.5) {
        // weighted max
        blended = max(prev.rgb * feedback, curr.rgb);
    } else if (Mode < 1.5) {
        // linear mix
        blended = mix(curr.rgb, prev.rgb, feedback);
    } else {
        // alpha decay
        float a = max(0.0, min(prev.a - 0.325, prev.a * feedback * 0.95));
        blended = prev.rgb * a + curr.rgb * (1.0 - a);
    }
    // shrink the residual by one 8-bit step so static pixels converge instead of sticking
    vec3 delta = blended - curr.rgb;
    gl_FragColor = vec4(curr.rgb + sign(delta) * max(abs(delta) - 1.0 / 255.0, 0.0), 1.0);
}
)";

// adjust_monkey_blur.fsh
constexpr const char* kMonkeyFragment = R"(#version 120
uniform sampler2D DiffuseSampler;
uniform sampler2D depthtex;
uniform sampler2D EntityMaskSampler;
uniform mat4 modelViewInverse;
uniform mat4 projectionInverse;
uniform mat4 previousProjection;
uniform mat4 previousModelView;
uniform vec3 cameraPosition;
uniform vec3 previousCameraPosition;
uniform float strength;
varying vec2 texCoord;
bool isProtectedEntity(vec2 coord) {
    return texture2D(EntityMaskSampler, coord).a > 0.001;
}
void main() {
    vec3 color = texture2D(DiffuseSampler, texCoord).rgb;
    float depth = texture2D(depthtex, texCoord).x;
    if (isProtectedEntity(texCoord)) {
        gl_FragColor = vec4(color, 1.0);
        return;
    }
    vec4 H = vec4(texCoord, depth, 1.0) * 2.0 - 1.0;
    vec4 worldpos = projectionInverse * H;
    worldpos = modelViewInverse * worldpos;
    worldpos /= worldpos.w;
    worldpos.xyz += cameraPosition;
    vec4 prevpos = worldpos;
    prevpos.xyz -= previousCameraPosition;
    prevpos = previousModelView * prevpos;
    prevpos = previousProjection * prevpos;
    prevpos /= prevpos.w;
    vec2 vel = (H - prevpos).st;
    vel = vel / (1.0 + length(vel)) * strength * 0.01;
    vec2 coord = texCoord + vel;
    float count = 1.0;
    for (int i = 0; i < 30; ++i, coord += vel) {
        if (coord.s > 1.0 || coord.t > 1.0 || coord.s < 0.0 || coord.t < 0.0) break;
        if (isProtectedEntity(coord)) continue;
        color += texture2D(DiffuseSampler, coord).xyz;
        count += 1.0;
    }
    color /= count;
    gl_FragColor = vec4(color.rgb, 1.0);
}
)";

// MoulberryBlur.draw: POSITION_TEX quad, texture modulated by GlStateManager.color(1, 1, 1, alpha).
constexpr const char* kTexturedFragment = R"(#version 120
uniform sampler2D DiffuseSampler;
uniform float Alpha;
varying vec2 texCoord;
void main() {
    gl_FragColor = texture2D(DiffuseSampler, texCoord) * vec4(1.0, 1.0, 1.0, Alpha);
}
)";

// ---------------------------------------------------------------------------------------------------------------
// GL helpers
// ---------------------------------------------------------------------------------------------------------------

bool gl_ready() noexcept {
    return GLAD_GL_VERSION_3_0 != 0 && glad_glBlitFramebuffer != nullptr && glad_glGenVertexArrays != nullptr &&
           glad_glColorMaski != nullptr && glad_glGetBooleani_v != nullptr;
}

void set_enabled(GLenum cap, GLboolean enabled) noexcept {
    if (enabled != GL_FALSE) {
        glEnable(cap);
    } else {
        glDisable(cap);
    }
}

// Saves every piece of GL state the passes change and restores it exactly (GlStateManager keeps its own cache of
// several of these, so they must come back unchanged).
struct GlStateBackup {
    GLint draw_fbo = 0;
    GLint read_fbo = 0;
    GLint viewport[4]{};
    GLint program = 0;
    GLint active_texture = GL_TEXTURE0;
    GLint texture_binding[3]{};
    GLint array_buffer = 0;
    GLint vertex_array = 0;
    GLboolean blend = GL_FALSE;
    GLboolean depth_test = GL_FALSE;
    GLboolean cull_face = GL_FALSE;
    GLboolean scissor_test = GL_FALSE;
    GLboolean stencil_test = GL_FALSE;
    GLboolean alpha_test = GL_FALSE;
    GLboolean color_mask[4]{};
    GLboolean depth_mask = GL_TRUE;
    GLint blend_src_rgb = GL_ONE;
    GLint blend_dst_rgb = GL_ZERO;
    GLint blend_src_alpha = GL_ONE;
    GLint blend_dst_alpha = GL_ZERO;
    GLint blend_eq_rgb = GL_FUNC_ADD;
    GLint blend_eq_alpha = GL_FUNC_ADD;

    void save() noexcept {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        for (GLint unit = 0; unit < 3; ++unit) {
            glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_binding[unit]);
        }
        glActiveTexture(static_cast<GLenum>(active_texture));
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
        blend = glIsEnabled(GL_BLEND);
        depth_test = glIsEnabled(GL_DEPTH_TEST);
        cull_face = glIsEnabled(GL_CULL_FACE);
        scissor_test = glIsEnabled(GL_SCISSOR_TEST);
        stencil_test = glIsEnabled(GL_STENCIL_TEST);
        alpha_test = glIsEnabled(GL_ALPHA_TEST);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq_rgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_eq_alpha);
    }

    // State used by every pass: no depth/cull/scissor/stencil/alpha test, no blending, full colour writes.
    static void apply_pass_defaults() noexcept {
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_ALPHA_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
    }

    void restore() const noexcept {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glUseProgram(static_cast<GLuint>(program));
        for (GLint unit = 0; unit < 3; ++unit) {
            glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_binding[unit]));
        }
        glActiveTexture(static_cast<GLenum>(active_texture));
        glBindVertexArray(static_cast<GLuint>(vertex_array));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer));
        set_enabled(GL_BLEND, blend);
        set_enabled(GL_DEPTH_TEST, depth_test);
        set_enabled(GL_CULL_FACE, cull_face);
        set_enabled(GL_SCISSOR_TEST, scissor_test);
        set_enabled(GL_STENCIL_TEST, stencil_test);
        set_enabled(GL_ALPHA_TEST, alpha_test);
        glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
        glDepthMask(depth_mask);
        glBlendFuncSeparate(static_cast<GLenum>(blend_src_rgb), static_cast<GLenum>(blend_dst_rgb),
                            static_cast<GLenum>(blend_src_alpha), static_cast<GLenum>(blend_dst_alpha));
        glBlendEquationSeparate(static_cast<GLenum>(blend_eq_rgb), static_cast<GLenum>(blend_eq_alpha));
    }
};

GLuint compile_shader(GLenum type, const char* source) noexcept {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint link_program(const char* fragment_source) noexcept {
    const GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (vs == 0 || fs == 0) {
        if (vs != 0) {
            glDeleteShader(vs);
        }
        if (fs != 0) {
            glDeleteShader(fs);
        }
        return 0;
    }
    const GLuint program = glCreateProgram();
    if (program != 0) {
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glBindAttribLocation(program, 0, "Position");
        glLinkProgram(program);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        glDetachShader(program, vs);
        glDetachShader(program, fs);
        if (ok != GL_TRUE) {
            glDeleteProgram(program);
            glDeleteShader(vs);
            glDeleteShader(fs);
            return 0;
        }
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

// Column-major 4x4 inverse (layout agnostic: inverse(transpose(M)) = transpose(inverse(M))).
bool invert4(const float* m, float* out) noexcept {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f || !std::isfinite(det)) {
        for (int i = 0; i < 16; ++i) {
            out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }
        return false;
    }
    const float inv_det = 1.0f / det;
    for (int i = 0; i < 16; ++i) {
        out[i] = inv[i] * inv_det;
    }
    return true;
}

void copy16(const float* src, float* dst) noexcept {
    for (int i = 0; i < 16; ++i) {
        dst[i] = src[i];
    }
}

// ---------------------------------------------------------------------------------------------------------------
// JNI ids
// ---------------------------------------------------------------------------------------------------------------

struct JniIds {
    jfieldID mc_framebuffer = nullptr;     // Minecraft.framebufferMc
    jfieldID fb_width = nullptr;           // Framebuffer.framebufferWidth
    jfieldID fb_height = nullptr;          // Framebuffer.framebufferHeight
    jfieldID fb_use_depth = nullptr;       // Framebuffer.useDepth
    jfieldID fb_object = nullptr;          // Framebuffer.framebufferObject
    jfieldID fb_texture = nullptr;         // Framebuffer.framebufferTexture
    jfieldID settings_anaglyph = nullptr;  // GameSettings.anaglyph
    jclass shaders = nullptr;              // OptiFine shadersmod.client.Shaders (optional)
    jfieldID shader_pack_loaded = nullptr; // Shaders.shaderPackLoaded (optional)
    bool ok = false;
};

const JniIds& jni_ids(events::JniContext* jni) noexcept {
    static const JniIds s_ids = [&] {
        JniIds ids{};
        namespace aj = adjust::jni;
        const jclass mc = aj::find_class(jni, "ave", "net/minecraft/client/Minecraft");
        const jclass fb = aj::find_class(jni, "bfw", "net/minecraft/client/shader/Framebuffer");
        const jclass settings = aj::find_class(jni, "avh", "net/minecraft/client/settings/GameSettings");
        ids.mc_framebuffer = aj::field(jni, mc, {"aF", "framebufferMc", "field_147124_at"}, "Lbfw;",
                                       "Lnet/minecraft/client/shader/Framebuffer;");
        ids.fb_width = aj::field(jni, fb, {"c", "framebufferWidth", "field_147621_c"}, "I", "I");
        ids.fb_height = aj::field(jni, fb, {"d", "framebufferHeight", "field_147618_d"}, "I", "I");
        ids.fb_use_depth = aj::field(jni, fb, {"e", "useDepth", "field_147619_e"}, "Z", "Z");
        ids.fb_object = aj::field(jni, fb, {"f", "framebufferObject", "field_147616_f"}, "I", "I");
        ids.fb_texture = aj::field(jni, fb, {"g", "framebufferTexture", "field_147617_g"}, "I", "I");
        ids.settings_anaglyph = aj::field(jni, settings, {"e", "anaglyph", "field_74337_g"}, "Z", "Z");
        ids.shaders = aj::find_class(jni, "shadersmod/client/Shaders", "shadersmod/client/Shaders");
        ids.shader_pack_loaded = aj::static_field(jni, ids.shaders, {"shaderPackLoaded", "shaderPackLoaded",
                                                                     "shaderPackLoaded"}, "Z", "Z");
        ids.ok = ids.mc_framebuffer != nullptr && ids.fb_width != nullptr && ids.fb_height != nullptr &&
                 ids.fb_use_depth != nullptr && ids.fb_object != nullptr && ids.fb_texture != nullptr &&
                 ids.settings_anaglyph != nullptr;
        return ids;
    }();
    return s_ids;
}

// ---------------------------------------------------------------------------------------------------------------
// Renderer (MotionBlurRenderer + MonkeyBlurShader + DepthCapture + MoulberryBlur)
// ---------------------------------------------------------------------------------------------------------------

struct Program {
    GLuint id = 0;
    GLint loc[12]{};
};

// Uniform slots
enum PhosphorUniform { kPhBlendFactor = 0, kPhMode };
enum MonkeyUniform {
    kMkStrength = 0,
    kMkCamera,
    kMkPrevCamera,
    kMkProjInv,
    kMkModelViewInv,
    kMkPrevProj,
    kMkPrevModelView
};
enum TexturedUniform { kTxAlpha = 0 };

struct Renderer {
    // MotionBlurRenderer bookkeeping
    jweak last_framebuffer = nullptr;
    jweak last_world = nullptr;
    jweak last_camera = nullptr;
    Mode last_mode = Mode::Phosphor;
    float last_strength = 0.0f;
    int64_t last_time = 0;
    int32_t last_width = 0;
    int32_t last_height = 0;
    int32_t last_perspective = 0;
    bool active = false;
    bool failed = false;
    bool matrix_captured = false;
    bool phosphor_history = false;

    // Minecraft framebuffer this frame
    GLuint mc_fbo = 0;
    GLuint mc_color = 0;
    bool mc_use_depth = false;

    // Own GL resources
    GLuint vao = 0;
    GLuint vbo = 0;
    Program phosphor{};
    Program monkey{};
    Program textured{};
    GLuint color_tex[2]{};
    GLuint color_fbo[2]{};
    int ping = 0;               // index of the target holding the previous output / accumulation
    bool has_previous = false;  // MoulberryBlur.hasPrevious

    // MonkeyBlurShader
    GLuint depth_tex = 0;
    float projection[16]{};
    float model_view[16]{};
    float previous_projection[16]{};
    float previous_model_view[16]{};
    float projection_inverse[16]{};
    float model_view_inverse[16]{};
    double camera[3]{};
    double previous_camera[3]{};
    bool initialized = false;
    bool valid = false;

    // DepthCapture
    GLuint mask_tex = 0;
    GLuint mask_fbo = 0;
    bool mask_prepared = false;
    bool capturing = false;
    GLboolean saved_mask[4]{};
};

Renderer g_r{};

int64_t now_nanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool has_gl_resources() noexcept {
    return g_r.vao != 0 || g_r.vbo != 0 || g_r.phosphor.id != 0 || g_r.monkey.id != 0 || g_r.textured.id != 0 ||
           g_r.color_tex[0] != 0 || g_r.color_tex[1] != 0 || g_r.color_fbo[0] != 0 || g_r.color_fbo[1] != 0 ||
           g_r.depth_tex != 0 || g_r.mask_tex != 0 || g_r.mask_fbo != 0;
}

void delete_program(Program* p) noexcept {
    if (p->id != 0) {
        glDeleteProgram(p->id);
    }
    *p = Program{};
}

// DepthCapture.end()
void end_mask_capture() noexcept {
    if (!g_r.capturing) {
        return;
    }
    g_r.capturing = false;
    GLint bound = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
    if (static_cast<GLuint>(bound) != g_r.mc_fbo) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_r.mc_fbo);
    }
    const GLenum attachments[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, attachments);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
    glColorMaski(1, g_r.saved_mask[0], g_r.saved_mask[1], g_r.saved_mask[2], g_r.saved_mask[3]);
    if (static_cast<GLuint>(bound) != g_r.mc_fbo) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(bound));
    }
}

// MotionBlurRenderer.destroy() (+ MonkeyBlurShader.delete, DepthCapture.delete, MoulberryBlur.delete)
void destroy() noexcept {
    if (!has_gl_resources()) {
        g_r.capturing = false;
        g_r.phosphor_history = false;
        g_r.has_previous = false;
        g_r.initialized = false;
        g_r.valid = false;
        g_r.mask_prepared = false;
        return;
    }
    if (gl_ready()) {
        end_mask_capture();
        delete_program(&g_r.phosphor);
        delete_program(&g_r.monkey);
        delete_program(&g_r.textured);
        if (g_r.vbo != 0) {
            glDeleteBuffers(1, &g_r.vbo);
        }
        if (g_r.vao != 0) {
            glDeleteVertexArrays(1, &g_r.vao);
        }
        for (int i = 0; i < 2; ++i) {
            if (g_r.color_fbo[i] != 0) {
                glDeleteFramebuffers(1, &g_r.color_fbo[i]);
            }
            if (g_r.color_tex[i] != 0) {
                glDeleteTextures(1, &g_r.color_tex[i]);
            }
        }
        if (g_r.mask_fbo != 0) {
            glDeleteFramebuffers(1, &g_r.mask_fbo);
        }
        if (g_r.mask_tex != 0) {
            glDeleteTextures(1, &g_r.mask_tex);
        }
        if (g_r.depth_tex != 0) {
            glDeleteTextures(1, &g_r.depth_tex);
        }
    }
    g_r.vao = 0;
    g_r.vbo = 0;
    g_r.phosphor = Program{};
    g_r.monkey = Program{};
    g_r.textured = Program{};
    g_r.color_tex[0] = g_r.color_tex[1] = 0;
    g_r.color_fbo[0] = g_r.color_fbo[1] = 0;
    g_r.mask_fbo = 0;
    g_r.mask_tex = 0;
    g_r.depth_tex = 0;
    g_r.ping = 0;
    g_r.capturing = false;
    g_r.phosphor_history = false;
    g_r.has_previous = false;
    g_r.initialized = false;
    g_r.valid = false;
    g_r.mask_prepared = false;
}

void release_weak(JNIEnv* env, jweak* ref) noexcept {
    if (*ref != nullptr && env != nullptr) {
        env->DeleteWeakGlobalRef(*ref);
    }
    *ref = nullptr;
}

// destroy(); lastFramebuffer = lastWorld = lastCamera = null; lastTime = 0; active = failed = false;
void reset(JNIEnv* env) noexcept {
    destroy();
    release_weak(env, &g_r.last_framebuffer);
    release_weak(env, &g_r.last_world);
    release_weak(env, &g_r.last_camera);
    g_r.last_time = 0;
    g_r.active = false;
    g_r.failed = false;
}

// MotionBlurRenderer.fail(): resources dropped, renderer disabled until the next state change.
void fail() noexcept {
    destroy();
    g_r.active = false;
    g_r.matrix_captured = false;
    g_r.failed = true;
}

GLuint create_texture(GLint internal_format, GLenum format, GLenum type, int32_t w, int32_t h) noexcept {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, format, type, nullptr);
    return tex;
}

// Colour texture + FBO, cleared to transparent black. Call inside a saved state block.
bool create_color_target(int32_t w, int32_t h, GLuint* tex, GLuint* fbo) noexcept {
    glActiveTexture(GL_TEXTURE0);
    *tex = create_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h);
    glGenFramebuffers(1, fbo);
    if (*tex == 0 || *fbo == 0) {
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return false;
    }
    GLfloat clear[4]{};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    return true;
}

// Lazily creates the quad, the programs and the two ping-pong targets. Call inside a saved state block.
bool ensure_common_resources() noexcept {
    if (g_r.vao == 0) {
        glGenVertexArrays(1, &g_r.vao);
        glGenBuffers(1, &g_r.vbo);
        if (g_r.vao == 0 || g_r.vbo == 0) {
            return false;
        }
        static constexpr GLfloat kQuad[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
        glBindVertexArray(g_r.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_r.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    if (g_r.color_tex[0] == 0) {
        for (int i = 0; i < 2; ++i) {
            if (!create_color_target(g_r.last_width, g_r.last_height, &g_r.color_tex[i], &g_r.color_fbo[i])) {
                return false;
            }
        }
    }
    return true;
}

bool ensure_phosphor_program() noexcept {
    if (g_r.phosphor.id != 0) {
        return true;
    }
    g_r.phosphor.id = link_program(kPhosphorFragment);
    if (g_r.phosphor.id == 0) {
        return false;
    }
    glUseProgram(g_r.phosphor.id);
    glUniform1i(glGetUniformLocation(g_r.phosphor.id, "DiffuseSampler"), 0);
    glUniform1i(glGetUniformLocation(g_r.phosphor.id, "PrevSampler"), 1);
    g_r.phosphor.loc[kPhBlendFactor] = glGetUniformLocation(g_r.phosphor.id, "BlendFactor");
    g_r.phosphor.loc[kPhMode] = glGetUniformLocation(g_r.phosphor.id, "Mode");
    return true;
}

bool ensure_monkey_program() noexcept {
    if (g_r.monkey.id != 0) {
        return true;
    }
    g_r.monkey.id = link_program(kMonkeyFragment);
    if (g_r.monkey.id == 0) {
        return false;
    }
    const GLuint p = g_r.monkey.id;
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "DiffuseSampler"), 0);
    glUniform1i(glGetUniformLocation(p, "depthtex"), 1);
    glUniform1i(glGetUniformLocation(p, "EntityMaskSampler"), 2);
    g_r.monkey.loc[kMkStrength] = glGetUniformLocation(p, "strength");
    g_r.monkey.loc[kMkCamera] = glGetUniformLocation(p, "cameraPosition");
    g_r.monkey.loc[kMkPrevCamera] = glGetUniformLocation(p, "previousCameraPosition");
    g_r.monkey.loc[kMkProjInv] = glGetUniformLocation(p, "projectionInverse");
    g_r.monkey.loc[kMkModelViewInv] = glGetUniformLocation(p, "modelViewInverse");
    g_r.monkey.loc[kMkPrevProj] = glGetUniformLocation(p, "previousProjection");
    g_r.monkey.loc[kMkPrevModelView] = glGetUniformLocation(p, "previousModelView");
    return true;
}

bool ensure_textured_program() noexcept {
    if (g_r.textured.id != 0) {
        return true;
    }
    g_r.textured.id = link_program(kTexturedFragment);
    if (g_r.textured.id == 0) {
        return false;
    }
    glUseProgram(g_r.textured.id);
    glUniform1i(glGetUniformLocation(g_r.textured.id, "DiffuseSampler"), 0);
    g_r.textured.loc[kTxAlpha] = glGetUniformLocation(g_r.textured.id, "Alpha");
    return true;
}

void draw_quad() noexcept {
    glBindVertexArray(g_r.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void bind_texture(GLint unit, GLuint texture) noexcept {
    glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
    glBindTexture(GL_TEXTURE_2D, texture);
}

void blit_color(GLuint from_fbo, GLuint to_fbo) noexcept {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, from_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to_fbo);
    glBlitFramebuffer(0, 0, g_r.last_width, g_r.last_height, 0, 0, g_r.last_width, g_r.last_height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

// DepthCapture.prepare(): mask texture (+ its own FBO for clearing) created once, cleared once per frame.
bool prepare_mask() noexcept {
    if (g_r.mask_prepared) {
        return true;
    }
    GLint draw_fbo = 0;
    GLint active_texture = GL_TEXTURE0;
    GLint texture = 0;
    GLfloat clear[4]{};
    GLboolean color_mask[4]{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

    bool ok = true;
    if (g_r.mask_tex == 0) {
        g_r.mask_tex = create_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, g_r.last_width, g_r.last_height);
        glGenFramebuffers(1, &g_r.mask_fbo);
        ok = g_r.mask_tex != 0 && g_r.mask_fbo != 0;
        if (ok) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_r.mask_fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_r.mask_tex, 0);
            ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
    }
    if (ok) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_r.mask_fbo);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // mask.setFramebufferColor(0, 0, 0, 0)
        glClear(GL_COLOR_BUFFER_BIT);
        g_r.mask_prepared = true;
    }

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
    glActiveTexture(static_cast<GLenum>(active_texture));
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    set_enabled(GL_SCISSOR_TEST, scissor);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo));
    return ok;
}

// MonkeyBlurShader.render(target, strength)
bool render_monkey(float strength) noexcept {
    if (!g_r.valid || !g_r.mc_use_depth) {
        return true;
    }
    if (!prepare_mask()) {
        return false;
    }
    GlStateBackup saved{};
    saved.save();
    bool ok = ensure_common_resources() && ensure_monkey_program();
    if (ok && g_r.depth_tex == 0) {
        glActiveTexture(GL_TEXTURE0);
        g_r.depth_tex = create_texture(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT, g_r.last_width,
                                       g_r.last_height);
        ok = g_r.depth_tex != 0;
    }
    if (ok) {
        // copyDepth(target, depthTexture)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_r.mc_fbo);
        bind_texture(0, g_r.depth_tex);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, g_r.last_width, g_r.last_height);

        GlStateBackup::apply_pass_defaults();
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_r.color_fbo[0]);
        glViewport(0, 0, g_r.last_width, g_r.last_height);
        glUseProgram(g_r.monkey.id);
        const Program& p = g_r.monkey;
        glUniform1f(p.loc[kMkStrength], strength);
        glUniform3f(p.loc[kMkCamera], static_cast<float>(g_r.camera[0]), static_cast<float>(g_r.camera[1]),
                    static_cast<float>(g_r.camera[2]));
        glUniform3f(p.loc[kMkPrevCamera], static_cast<float>(g_r.previous_camera[0]),
                    static_cast<float>(g_r.previous_camera[1]), static_cast<float>(g_r.previous_camera[2]));
        glUniformMatrix4fv(p.loc[kMkProjInv], 1, GL_FALSE, g_r.projection_inverse);
        glUniformMatrix4fv(p.loc[kMkModelViewInv], 1, GL_FALSE, g_r.model_view_inverse);
        glUniformMatrix4fv(p.loc[kMkPrevProj], 1, GL_FALSE, g_r.previous_projection);
        glUniformMatrix4fv(p.loc[kMkPrevModelView], 1, GL_FALSE, g_r.previous_model_view);
        bind_texture(0, g_r.mc_color);
        bind_texture(1, g_r.depth_tex);
        bind_texture(2, g_r.mask_tex);
        draw_quad();
        // "blit" pass: swap -> minecraft:main
        blit_color(g_r.color_fbo[0], g_r.mc_fbo);
    }
    saved.restore();
    return ok;
}

// ShaderGroup "adjust_phosphor_motion_blur": main + previous -> swap; swap -> previous; swap -> main.
bool render_phosphor(float strength) noexcept {
    GlStateBackup saved{};
    saved.save();
    const bool ok = ensure_common_resources() && ensure_phosphor_program();
    if (ok) {
        const int prev = g_r.ping;
        const int swap = 1 - prev;
        const float blend = g_r.phosphor_history ? strength / 100.0f * 3.0f + 0.69999999f - 0.0099999998f : 0.0f;
        GlStateBackup::apply_pass_defaults();
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_r.color_fbo[swap]);
        glViewport(0, 0, g_r.last_width, g_r.last_height);
        glUseProgram(g_r.phosphor.id);
        glUniform1f(g_r.phosphor.loc[kPhBlendFactor], blend);
        glUniform1f(g_r.phosphor.loc[kPhMode], 0.0f);
        bind_texture(0, g_r.mc_color);
        bind_texture(1, g_r.color_tex[prev]);
        draw_quad();
        blit_color(g_r.color_fbo[swap], g_r.mc_fbo);
        g_r.ping = swap; // the "previous" target now holds this frame's output
        g_r.phosphor_history = true;
    }
    saved.restore();
    return ok;
}

// MoulberryBlur.render(target, strength)
bool render_moulberry(float strength) noexcept {
    GlStateBackup saved{};
    saved.save();
    const bool ok = ensure_common_resources() && ensure_textured_program();
    if (ok) {
        const int previous = g_r.ping;
        const int current = 1 - previous;
        GlStateBackup::apply_pass_defaults();
        // current = target (nearest copy, no blending)
        blit_color(g_r.mc_fbo, g_r.color_fbo[current]);
        if (g_r.has_previous) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_r.color_fbo[current]);
            glViewport(0, 0, g_r.last_width, g_r.last_height);
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glUseProgram(g_r.textured.id);
            const float alpha = strength * 0.1f + 0.1f < 0.89999998f ? strength * 0.1f + 0.1f : 0.89999998f;
            glUniform1f(g_r.textured.loc[kTxAlpha], alpha);
            bind_texture(0, g_r.color_tex[previous]);
            draw_quad();
            glDisable(GL_BLEND);
        }
        // target = current
        blit_color(g_r.color_fbo[current], g_r.mc_fbo);
        g_r.ping = current; // swap previous / current
        g_r.has_previous = true;
    }
    saved.restore();
    return ok;
}

} // namespace

events::Module* get_adjust_motion_blur_module() noexcept {
    return &g_state.base;
}

// MotionBlurRenderer.beginFrame()
void adjust_motion_blur_begin_frame(events::JniContext* jni) noexcept {
    g_r.active = false;
    g_r.matrix_captured = false;
    JNIEnv* env = (jni != nullptr) ? jni->env : nullptr;
    const bool running = events::module_is_running(&g_state.base);
    if (!running || env == nullptr || jni->mappings == nullptr || !gl_ready()) {
        if (g_r.last_world != nullptr || g_r.last_camera != nullptr || g_r.last_framebuffer != nullptr ||
            has_gl_resources() || g_r.failed) {
            reset(env);
        }
        return;
    }
    const JniIds& ids = jni_ids(jni);
    if (!ids.ok) {
        reset(env);
        return;
    }

    jobject mc = minecraft::mc_get_instance(jni);
    jobject world = (mc != nullptr) ? minecraft::mc_get_world(jni, mc) : nullptr;
    jobject camera = (mc != nullptr) ? minecraft::mc_get_render_view_entity(jni, mc) : nullptr;
    jobject settings = (mc != nullptr) ? minecraft::mc_get_game_settings(jni, mc) : nullptr;
    jobject framebuffer = (mc != nullptr) ? env->GetObjectField(mc, ids.mc_framebuffer) : nullptr;

    const bool anaglyph = settings != nullptr && env->GetBooleanField(settings, ids.settings_anaglyph) != JNI_FALSE;
    const int32_t perspective = (settings != nullptr) ? minecraft::settings_get_third_person_view(jni, settings) : 0;
    const bool shader_pack = ids.shaders != nullptr && ids.shader_pack_loaded != nullptr &&
                             env->GetStaticBooleanField(ids.shaders, ids.shader_pack_loaded) != JNI_FALSE;
    int32_t fb_object = -1;
    int32_t width = 0;
    int32_t height = 0;
    if (framebuffer != nullptr) {
        fb_object = env->GetIntField(framebuffer, ids.fb_object);
        width = env->GetIntField(framebuffer, ids.fb_width);
        height = env->GetIntField(framebuffer, ids.fb_height);
        g_r.mc_color = static_cast<GLuint>(env->GetIntField(framebuffer, ids.fb_texture));
        g_r.mc_use_depth = env->GetBooleanField(framebuffer, ids.fb_use_depth) != JNI_FALSE;
    }

    const auto release_locals = [&]() noexcept {
        if (framebuffer != nullptr) {
            env->DeleteLocalRef(framebuffer);
        }
        if (settings != nullptr) {
            env->DeleteLocalRef(settings);
        }
        if (camera != nullptr) {
            env->DeleteLocalRef(camera);
        }
        if (world != nullptr) {
            env->DeleteLocalRef(world);
        }
        if (mc != nullptr) {
            env->DeleteLocalRef(mc);
        }
    };

    // !isFramebufferEnabled() -> framebufferObject < 0; Shaders.shaderPackLoaded; gameSettings.anaglyph
    if (world == nullptr || camera == nullptr || framebuffer == nullptr || fb_object <= 0 || shader_pack ||
        anaglyph || width < 1 || height < 1) {
        reset(env);
        release_locals();
        return;
    }

    const int64_t now = now_nanos();
    const Mode mode = static_cast<Mode>(g_state.mode.value);
    const float strength = g_state.strength.value;
    if (env->IsSameObject(g_r.last_framebuffer, framebuffer) == JNI_FALSE || width != g_r.last_width ||
        height != g_r.last_height || env->IsSameObject(g_r.last_world, world) == JNI_FALSE ||
        env->IsSameObject(g_r.last_camera, camera) == JNI_FALSE || perspective != g_r.last_perspective ||
        mode != g_r.last_mode || strength != g_r.last_strength ||
        (g_r.last_time != 0 && now - g_r.last_time >= 100000001LL)) {
        reset(env);
    }
    if (g_r.last_framebuffer == nullptr) {
        g_r.last_framebuffer = env->NewWeakGlobalRef(framebuffer);
    }
    if (g_r.last_world == nullptr) {
        g_r.last_world = env->NewWeakGlobalRef(world);
    }
    if (g_r.last_camera == nullptr) {
        g_r.last_camera = env->NewWeakGlobalRef(camera);
    }
    g_r.mc_fbo = static_cast<GLuint>(fb_object);
    g_r.last_width = width;
    g_r.last_height = height;
    g_r.last_perspective = perspective;
    g_r.last_mode = mode;
    g_r.last_strength = strength;
    g_r.last_time = now;
    g_r.active = !g_r.failed; // OpenGlHelper.shadersSupported: GL 3.0 (checked by gl_ready)
    g_r.valid = false;          // monkey.valid = false
    g_r.mask_prepared = false;  // monkey.depthCapture.prepared = false
    release_locals();
}

// MotionBlurRenderer.renderMonkey() + renderPost()
void adjust_motion_blur_end_frame() noexcept {
    g_r.matrix_captured = false;
    if (!g_r.active || !gl_ready()) {
        return;
    }
    end_mask_capture();
    bool ok = true;
    switch (g_r.last_mode) {
        case Mode::Monkey:
            ok = render_monkey(g_r.last_strength);
            break;
        case Mode::Moulberry:
            ok = render_moulberry(g_r.last_strength);
            break;
        case Mode::Phosphor:
            ok = render_phosphor(g_r.last_strength);
            break;
    }
    if (!ok) {
        fail();
    }
}

bool adjust_motion_blur_wants_matrices() noexcept {
    return g_r.active && g_r.last_mode == Mode::Monkey;
}

// MotionBlurRenderer.captureMatrices(partialTicks) -> MonkeyBlurShader.updateMatrices(lastCamera, partialTicks)
void adjust_motion_blur_capture_matrices(events::JniContext* jni, float partial_ticks) noexcept {
    if (!adjust_motion_blur_wants_matrices() || jni == nullptr || jni->env == nullptr || jni->mappings == nullptr) {
        return;
    }
    JNIEnv* env = jni->env;
    jobject camera = (g_r.last_camera != nullptr) ? env->NewLocalRef(g_r.last_camera) : nullptr;
    if (camera == nullptr) {
        return;
    }
    const math::Vec3d pos = minecraft::entity_get_position(jni, camera);
    const math::Vec3d prev = minecraft::entity_get_prev_position(jni, camera);
    env->DeleteLocalRef(camera);

    copy16(g_r.projection, g_r.previous_projection);
    copy16(g_r.model_view, g_r.previous_model_view);
    g_r.previous_camera[0] = g_r.camera[0];
    g_r.previous_camera[1] = g_r.camera[1];
    g_r.previous_camera[2] = g_r.camera[2];
    const double pt = static_cast<double>(partial_ticks);
    g_r.camera[0] = prev.x + (pos.x - prev.x) * pt;
    g_r.camera[1] = prev.y + (pos.y - prev.y) * pt;
    g_r.camera[2] = prev.z + (pos.z - prev.z) * pt;
    glGetFloatv(GL_PROJECTION_MATRIX, g_r.projection);
    glGetFloatv(GL_MODELVIEW_MATRIX, g_r.model_view);
    if (!g_r.initialized || std::fabs(g_r.camera[0] - g_r.previous_camera[0]) > 16.0 ||
        std::fabs(g_r.camera[1] - g_r.previous_camera[1]) > 16.0 ||
        std::fabs(g_r.camera[2] - g_r.previous_camera[2]) > 16.0) {
        copy16(g_r.projection, g_r.previous_projection);
        copy16(g_r.model_view, g_r.previous_model_view);
        g_r.previous_camera[0] = g_r.camera[0];
        g_r.previous_camera[1] = g_r.camera[1];
        g_r.previous_camera[2] = g_r.camera[2];
    }
    invert4(g_r.projection, g_r.projection_inverse);
    invert4(g_r.model_view, g_r.model_view_inverse);
    g_r.initialized = true;
    g_r.valid = true;
    g_r.matrix_captured = true;
}

bool adjust_motion_blur_mask_possible() noexcept {
    return g_r.active && g_r.matrix_captured && g_r.last_mode == Mode::Monkey && !g_r.capturing;
}

// MotionBlurRenderer.beginEntityMask(entity) -> DepthCapture.begin(lastFramebuffer)
bool adjust_motion_blur_begin_entity_mask(events::JniContext* jni, jobject entity) noexcept {
    if (!adjust_motion_blur_mask_possible() || jni == nullptr || jni->env == nullptr || jni->mappings == nullptr ||
        entity == nullptr || !gl_ready()) {
        return false;
    }
    GLint bound = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
    if (static_cast<GLuint>(bound) != g_r.mc_fbo) {
        return false;
    }
    // Protected: the local player (Adjust also protects entities with Adjust-Chams target settings).
    JNIEnv* env = jni->env;
    jobject mc = minecraft::mc_get_instance(jni);
    jobject player = (mc != nullptr) ? minecraft::mc_get_player(jni, mc) : nullptr;
    const bool protect = player != nullptr && env->IsSameObject(player, entity) != JNI_FALSE;
    if (player != nullptr) {
        env->DeleteLocalRef(player);
    }
    if (mc != nullptr) {
        env->DeleteLocalRef(mc);
    }
    if (!protect) {
        return false;
    }
    if (!prepare_mask()) {
        fail();
        return false;
    }
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_r.mask_tex, 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        return false;
    }
    glGetBooleani_v(GL_COLOR_WRITEMASK, 1, g_r.saved_mask);
    const GLenum attachments[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, attachments);
    glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g_r.capturing = true;
    return true;
}

void adjust_motion_blur_end_entity_mask() noexcept {
    if (gl_ready()) {
        end_mask_capture();
    }
}

} // namespace slinky::modules
