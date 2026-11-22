#include "modules/adjust/hud/target/target_textures.hpp"

#include <utility>
#include <vector>

#include <glad/glad.h>
#include "nanovg.h"
#define NANOVG_GL3
extern "C" {
#include "nanovg_gl.h"
}

#include "adjust/jni_lookup.hpp"
#include "minecraft/mc_wrappers.hpp"

namespace slinky::modules::adjust_hud::target {

namespace {

namespace jl = adjust::jni;

// ---- JNI ids (1.8.9) ---------------------------------------------------------------------------------------------

struct SkinIds {
    bool ok = false;
    jmethodID get_player_info = nullptr;   // NetHandlerPlayClient.getPlayerInfo(UUID)
    jmethodID get_location_skin = nullptr; // NetworkPlayerInfo.getLocationSkin()
    jclass default_skin_cls = nullptr;     // DefaultPlayerSkin
    jmethodID get_default_skin = nullptr;  // DefaultPlayerSkin.getDefaultSkin(UUID)
};

struct TextureIds {
    bool ok = false;
    jmethodID bind_texture = nullptr;      // TextureManager.bindTexture(ResourceLocation)
    jmethodID get_texture = nullptr;       // TextureManager.getTexture(ResourceLocation)
    jmethodID get_gl_texture_id = nullptr; // ITextureObject.getGlTextureId()
};

struct ItemIds {
    bool ok = false;
    jmethodID get_item_model_mesher = nullptr; // RenderItem.getItemModelMesher()
    jmethodID get_item_model = nullptr;        // ItemModelMesher.getItemModel(ItemStack)
    jmethodID get_particle_texture = nullptr;  // IBakedModel.getParticleTexture()
    jmethodID min_u = nullptr, max_u = nullptr, min_v = nullptr, max_v = nullptr; // TextureAtlasSprite
    jmethodID get_color = nullptr;             // Item.getColorFromItemStack(ItemStack, int)
    jobject blocks_atlas = nullptr;            // TextureMap.locationBlocksTexture (global ref)
};

const TextureIds& texture_ids(events::JniContext* jni) noexcept {
    static const TextureIds s_ids = [&]() noexcept {
        TextureIds ids{};
        const jclass tm = jl::find_class(jni, "bmj", "net/minecraft/client/renderer/texture/TextureManager");
        const jclass tex = jl::find_class(jni, "bmk", "net/minecraft/client/renderer/texture/ITextureObject");
        ids.bind_texture = jl::method(jni, tm, {"a", "bindTexture", "func_110577_a"}, "(Ljy;)V",
                                      "(Lnet/minecraft/util/ResourceLocation;)V");
        ids.get_texture = jl::method(jni, tm, {"b", "getTexture", "func_110581_b"}, "(Ljy;)Lbmk;",
                                     "(Lnet/minecraft/util/ResourceLocation;)"
                                     "Lnet/minecraft/client/renderer/texture/ITextureObject;");
        ids.get_gl_texture_id = jl::method(jni, tex, {"b", "getGlTextureId", "func_110552_b"}, "()I", "()I");
        ids.ok = ids.bind_texture != nullptr && ids.get_texture != nullptr && ids.get_gl_texture_id != nullptr;
        return ids;
    }();
    return s_ids;
}

const SkinIds& skin_ids(events::JniContext* jni) noexcept {
    static const SkinIds s_ids = [&]() noexcept {
        SkinIds ids{};
        const jclass handler = jl::find_class(jni, "bcy", "net/minecraft/client/network/NetHandlerPlayClient");
        const jclass info = jl::find_class(jni, "bdc", "net/minecraft/client/network/NetworkPlayerInfo");
        ids.default_skin_cls = jl::find_class(jni, "bmz", "net/minecraft/client/resources/DefaultPlayerSkin");
        ids.get_player_info = jl::method(jni, handler, {"a", "getPlayerInfo", "func_175102_a"},
                                         "(Ljava/util/UUID;)Lbdc;",
                                         "(Ljava/util/UUID;)Lnet/minecraft/client/network/NetworkPlayerInfo;");
        ids.get_location_skin = jl::method(jni, info, {"g", "getLocationSkin", "func_178837_g"}, "()Ljy;",
                                           "()Lnet/minecraft/util/ResourceLocation;");
        ids.get_default_skin = jl::static_method(jni, ids.default_skin_cls,
                                                 {"a", "getDefaultSkin", "func_177334_a"},
                                                 "(Ljava/util/UUID;)Ljy;",
                                                 "(Ljava/util/UUID;)Lnet/minecraft/util/ResourceLocation;");
        ids.ok = ids.get_player_info != nullptr && ids.get_location_skin != nullptr &&
                 ids.default_skin_cls != nullptr && ids.get_default_skin != nullptr;
        return ids;
    }();
    return s_ids;
}

const ItemIds& item_ids(events::JniContext* jni) noexcept {
    static const ItemIds s_ids = [&]() noexcept {
        ItemIds ids{};
        const jclass render_item = jl::find_class(jni, "bjh", "net/minecraft/client/renderer/entity/RenderItem");
        const jclass mesher = jl::find_class(jni, "bfo", "net/minecraft/client/renderer/ItemModelMesher");
        const jclass model = jl::find_class(jni, "boq", "net/minecraft/client/resources/model/IBakedModel");
        const jclass sprite = jl::find_class(jni, "bmi", "net/minecraft/client/renderer/texture/TextureAtlasSprite");
        const jclass texture_map = jl::find_class(jni, "bmh", "net/minecraft/client/renderer/texture/TextureMap");
        const jclass item = jl::find_class(jni, "zw", "net/minecraft/item/Item");
        ids.get_item_model_mesher = jl::method(jni, render_item, {"a", "getItemModelMesher", "func_175037_a"},
                                               "()Lbfo;", "()Lnet/minecraft/client/renderer/ItemModelMesher;");
        ids.get_item_model = jl::method(jni, mesher, {"a", "getItemModel", "func_178089_a"}, "(Lzx;)Lboq;",
                                        "(Lnet/minecraft/item/ItemStack;)"
                                        "Lnet/minecraft/client/resources/model/IBakedModel;");
        ids.get_particle_texture = jl::method(jni, model, {"e", "getParticleTexture", "func_177554_e"},
                                              "()Lbmi;", "()Lnet/minecraft/client/renderer/texture/TextureAtlasSprite;");
        ids.min_u = jl::method(jni, sprite, {"e", "getMinU", "func_94209_e"}, "()F", "()F");
        ids.max_u = jl::method(jni, sprite, {"f", "getMaxU", "func_94212_f"}, "()F", "()F");
        ids.min_v = jl::method(jni, sprite, {"g", "getMinV", "func_94206_g"}, "()F", "()F");
        ids.max_v = jl::method(jni, sprite, {"h", "getMaxV", "func_94210_h"}, "()F", "()F");
        ids.get_color = jl::method(jni, item, {"a", "getColorFromItemStack", "func_82790_a"}, "(Lzx;I)I",
                                   "(Lnet/minecraft/item/ItemStack;I)I");
        const jfieldID atlas_fid = jl::static_field(jni, texture_map, {"g", "locationBlocksTexture", "field_110575_b"},
                                                    "Ljy;", "Lnet/minecraft/util/ResourceLocation;");
        if (atlas_fid != nullptr) {
            jobject local = jni->env->GetStaticObjectField(texture_map, atlas_fid);
            if (local != nullptr) {
                ids.blocks_atlas = jni->env->NewGlobalRef(local);
                jni->env->DeleteLocalRef(local);
            }
            jl::clear_exception(jni);
        }
        ids.ok = ids.get_item_model_mesher != nullptr && ids.get_item_model != nullptr &&
                 ids.get_particle_texture != nullptr && ids.min_u != nullptr && ids.max_u != nullptr &&
                 ids.min_v != nullptr && ids.max_v != nullptr && ids.get_color != nullptr &&
                 ids.blocks_atlas != nullptr;
        return ids;
    }();
    return s_ids;
}

// TextureManager.getTexture(location).getGlTextureId(); loads the texture first when it is not registered yet.
uint32_t gl_texture_of(events::JniContext* jni, jobject texture_manager, jobject location, bool load) noexcept {
    const TextureIds& ids = texture_ids(jni);
    if (!ids.ok || texture_manager == nullptr || location == nullptr) {
        return 0;
    }
    JNIEnv* env = jni->env;
    jobject texture = env->CallObjectMethod(texture_manager, ids.get_texture, location);
    if (jl::clear_exception(jni)) {
        return 0;
    }
    if (texture == nullptr && load) {
        env->CallVoidMethod(texture_manager, ids.bind_texture, location);
        if (jl::clear_exception(jni)) {
            return 0;
        }
        texture = env->CallObjectMethod(texture_manager, ids.get_texture, location);
        if (jl::clear_exception(jni)) {
            return 0;
        }
    }
    if (texture == nullptr) {
        return 0;
    }
    const jint id = env->CallIntMethod(texture, ids.get_gl_texture_id);
    if (jl::clear_exception(jni) || id <= 0) {
        return 0;
    }
    return static_cast<uint32_t>(id);
}

// ---- NanoVG image cache ----------------------------------------------------------------------------------------

struct CachedImage {
    uint32_t texture = 0;
    int image = 0;
};

NVGcontext* g_cache_vg = nullptr;
std::vector<CachedImage> g_images;

int image_for(NVGcontext* vg, uint32_t texture) noexcept {
    if (vg != g_cache_vg) {
        // A different context: the old handles belong to it, so they are simply forgotten.
        g_images.clear();
        g_cache_vg = vg;
    }
    for (const CachedImage& c : g_images) {
        if (c.texture == texture) {
            return c.image;
        }
    }
    // Minecraft owns the texture (NODELETE); size only matters for nvgImageSize, patterns use normalized UVs.
    const int image = nvglCreateImageFromHandleGL3(vg, texture, 64, 64, NVG_IMAGE_NODELETE);
    if (image != 0) {
        try {
            g_images.push_back(CachedImage{texture, image});
        } catch (...) {
        }
    }
    return image;
}

// Fills (x, y, w, h) with the (u0, v0)-(u1, v1) region of `texture`, multiplied by `tint`.
void fill_textured(uint32_t texture, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                   NVGcolor tint) noexcept {
    NVGcontext* vg = adjust::vg();
    if (vg == nullptr || texture == 0 || u1 == u0 || v1 == v0) {
        return;
    }
    const int image = image_for(vg, texture);
    if (image == 0) {
        return;
    }
    const float pattern_w = w / (u1 - u0);
    const float pattern_h = h / (v1 - v0);
    NVGpaint paint = nvgImagePattern(vg, x - u0 * pattern_w, y - v0 * pattern_h, pattern_w, pattern_h, 0.0f, image,
                                     1.0f);
    paint.innerColor = tint;
    paint.outerColor = tint;
    nvgSave(vg);
    nvgShapeAntiAlias(vg, 0); // no fringe sampling outside the sprite
    nvgBeginPath(vg);
    nvgRect(vg, x, y, w, h);
    nvgFillPaint(vg, paint);
    nvgFill(vg);
    nvgRestore(vg);
}

} // namespace

uint32_t resolve_skin_texture(events::JniContext* jni, jobject minecraft, jobject player) noexcept {
    if (jni == nullptr || minecraft == nullptr || player == nullptr || !jl::is_1_8(jni)) {
        return 0;
    }
    const SkinIds& ids = skin_ids(jni);
    if (!ids.ok) {
        return 0;
    }
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(16) != 0) {
        jl::clear_exception(jni);
        return 0;
    }
    uint32_t result = 0;
    jobject uuid = minecraft::entity_get_unique_id(jni, player);
    jobject handler = minecraft::mc_get_net_handler(jni, minecraft);
    jl::clear_exception(jni);
    if (uuid != nullptr) {
        // NetworkPlayerInfo.getLocationSkin(), else DefaultPlayerSkin.getDefaultSkin(uuid)
        jobject info = handler != nullptr ? env->CallObjectMethod(handler, ids.get_player_info, uuid) : nullptr;
        jl::clear_exception(jni);
        jobject location = info != nullptr ? env->CallObjectMethod(info, ids.get_location_skin) : nullptr;
        jl::clear_exception(jni);
        if (location == nullptr) {
            location = env->CallStaticObjectMethod(ids.default_skin_cls, ids.get_default_skin, uuid);
            jl::clear_exception(jni);
        }
        jobject texture_manager = minecraft::mc_get_texture_manager(jni, minecraft);
        result = gl_texture_of(jni, texture_manager, location, true);
    }
    env->PopLocalFrame(nullptr);
    return result;
}

Sprite resolve_item_sprite(events::JniContext* jni, jobject minecraft, jobject stack) noexcept {
    Sprite sprite{};
    if (jni == nullptr || minecraft == nullptr || stack == nullptr || !jl::is_1_8(jni)) {
        return sprite;
    }
    const ItemIds& ids = item_ids(jni);
    if (!ids.ok) {
        return sprite;
    }
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(16) != 0) {
        jl::clear_exception(jni);
        return sprite;
    }
    do {
        jobject render_item = minecraft::mc_get_render_item(jni, minecraft);
        if (jl::clear_exception(jni) || render_item == nullptr) break;
        jobject mesher = env->CallObjectMethod(render_item, ids.get_item_model_mesher);
        if (jl::clear_exception(jni) || mesher == nullptr) break;
        jobject model = env->CallObjectMethod(mesher, ids.get_item_model, stack);
        if (jl::clear_exception(jni) || model == nullptr) break;
        jobject atlas_sprite = env->CallObjectMethod(model, ids.get_particle_texture);
        if (jl::clear_exception(jni) || atlas_sprite == nullptr) break;
        const float u0 = env->CallFloatMethod(atlas_sprite, ids.min_u);
        const float u1 = env->CallFloatMethod(atlas_sprite, ids.max_u);
        const float v0 = env->CallFloatMethod(atlas_sprite, ids.min_v);
        const float v1 = env->CallFloatMethod(atlas_sprite, ids.max_v);
        if (jl::clear_exception(jni)) break;
        int color = 0xFFFFFF;
        jobject item = minecraft::item_stack_get_item(jni, stack);
        if (!jl::clear_exception(jni) && item != nullptr) {
            color = env->CallIntMethod(item, ids.get_color, stack, 0);
            if (jl::clear_exception(jni)) color = 0xFFFFFF;
        }
        jobject texture_manager = minecraft::mc_get_texture_manager(jni, minecraft);
        const uint32_t texture = gl_texture_of(jni, texture_manager, ids.blocks_atlas, false);
        if (texture == 0) break;
        sprite.texture = texture;
        sprite.u0 = u0;
        sprite.v0 = v0;
        sprite.u1 = u1;
        sprite.v1 = v1;
        sprite.tint = 0xFF000000u | (static_cast<uint32_t>(color) & 0x00FFFFFFu);
    } while (false);
    env->PopLocalFrame(nullptr);
    return sprite;
}

void draw_head(uint32_t skin_texture, float x, float y, adjust::Argb tint) noexcept {
    if (skin_texture == 0) {
        return;
    }
    // drawModalRectWithCustomSizedTexture(x, y, 32, 32, 32, 32, 256, 256) at 0.5 scale: face (8,8)-(16,16) of the
    // 64x64 skin, then the hat layer (40,8)-(48,16), both 16x16 HUD units.
    const NVGcolor color = adjust::to_nvg(tint);
    constexpr float kV0 = 8.0f / 64.0f;
    constexpr float kV1 = 16.0f / 64.0f;
    fill_textured(skin_texture, x, y, 16.0f, 16.0f, 8.0f / 64.0f, kV0, 16.0f / 64.0f, kV1, color);
    fill_textured(skin_texture, x, y, 16.0f, 16.0f, 40.0f / 64.0f, kV0, 48.0f / 64.0f, kV1, color);
}

void draw_item(const Sprite& sprite, float x, float y, float size, int alpha) noexcept {
    if (sprite.texture == 0) {
        return;
    }
    fill_textured(sprite.texture, x, y, size, size, sprite.u0, sprite.v0, sprite.u1, sprite.v1,
                  adjust::to_nvg(adjust::with_alpha(sprite.tint, alpha)));
}

void trim_image_cache() noexcept {
    constexpr size_t kMaxImages = 48;
    if (g_images.size() <= kMaxImages || g_cache_vg == nullptr) {
        return;
    }
    for (const CachedImage& c : g_images) {
        nvgDeleteImage(g_cache_vg, c.image); // NODELETE: only the NanoVG record is freed
    }
    g_images.clear();
}

void reset_image_cache() noexcept {
    g_images.clear();
    g_cache_vg = nullptr;
}

} // namespace slinky::modules::adjust_hud::target
