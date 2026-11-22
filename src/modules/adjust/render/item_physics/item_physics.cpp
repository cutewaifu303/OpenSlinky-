// Port of com.adjustclient.module.impl.render.ItemPhysicsModule.
//
// Hook: RenderEntityItem.doRender(Entity, ...) (the generic bridge) calls doRender(EntityItem, ...); that call is
// replaced by adjustItemPhysicsDoRender (src/adjust/hooks/item_hooks.cpp), which runs render_item() while the
// module is running and the original doRender otherwise - exactly where Adjust's patched doRender branches.
//
// Matrix operations use GlStateManager (Adjust calls GL11.glPushMatrix/glTranslatef/... directly; GlStateManager's
// matrix functions forward to the same GL11 calls).

#include "modules/adjust/render/item_physics/item_physics.hpp"

#include <cstdint>

#include "adjust/jni_lookup.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/item_animations/item_gl.hpp"

namespace slinky::modules {

namespace {

namespace gl = adjust::item_gl;

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

struct ItemPhysicsState {
    events::Module base{};

    config::FloatSetting rotation_speed{{}, "Rotation speed", "%.1f", 0.5f, 0.0f, 1.0f, 3.0f, 1.5f, 1.5f};

    ItemPhysicsState() noexcept {
        base.name = "Item Physics";
        base.description = "Dropped items lie flat on the ground and spin while falling.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagHiddenFromHud; // Adjust default: hidden
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Item Physics");
    }
};

ItemPhysicsState g_state{};

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_state.rotation_speed.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.rotation_speed.visit(archive, "rotation_speed");
}

// java.util.Random (ItemPhysicsModule.random, shared by every item; reseeded per item)
struct JavaRandom {
    int64_t seed = 0;

    void set_seed(int64_t s) noexcept {
        seed = (s ^ 0x5DEECE66DLL) & ((1LL << 48) - 1);
    }
    int32_t next(int bits) noexcept {
        seed = (seed * 0x5DEECE66DLL + 0xBLL) & ((1LL << 48) - 1);
        return static_cast<int32_t>(static_cast<uint64_t>(seed) >> (48 - bits));
    }
    float next_float() noexcept {
        return static_cast<float>(next(24)) / static_cast<float>(1 << 24);
    }
};

JavaRandom g_random{};

struct Ids {
    jmethodID get_entity_item = nullptr;    // EntityItem.getEntityItem()
    jmethodID stack_get_item = nullptr;     // ItemStack.getItem()
    jmethodID stack_get_metadata = nullptr; // ItemStack.getMetadata()
    jfieldID stack_size = nullptr;          // ItemStack.stackSize
    jclass item = nullptr;
    jmethodID item_get_id = nullptr;        // Item.getIdFromItem(Item) (static)
    jobject blocks_texture = nullptr;       // TextureMap.locationBlocksTexture (global ref)
    jmethodID bind_texture = nullptr;       // Render.bindTexture(ResourceLocation)
    jmethodID get_render_manager = nullptr; // Render.getRenderManager()
    jfieldID render_engine = nullptr;       // RenderManager.renderEngine
    jmethodID get_texture = nullptr;        // TextureManager.getTexture(ResourceLocation)
    jmethodID set_blur_mipmap = nullptr;    // ITextureObject.setBlurMipmap(ZZ)
    jmethodID restore_blur_mipmap = nullptr; // ITextureObject.restoreLastBlurMipmap()
    jclass render_helper = nullptr;
    jmethodID enable_item_lighting = nullptr; // RenderHelper.enableStandardItemLighting() (static)
    jmethodID mc_get_render_item = nullptr; // Minecraft.getRenderItem()
    jmethodID get_item_model_mesher = nullptr; // RenderItem.getItemModelMesher()
    jmethodID get_item_model = nullptr;     // ItemModelMesher.getItemModel(ItemStack)
    jmethodID is_gui_3d = nullptr;          // IBakedModel.isGui3d()
    jmethodID get_camera_transforms = nullptr; // IBakedModel.getItemCameraTransforms()
    jmethodID apply_transform = nullptr;    // ItemCameraTransforms.applyTransform(TransformType)
    jobject transform_ground = nullptr;     // TransformType.GROUND (global ref)
    jmethodID render_item = nullptr;        // RenderItem.renderItem(ItemStack, IBakedModel)
    jfieldID rotation_yaw = nullptr;        // Entity.rotationYaw
    jfieldID rotation_pitch = nullptr;      // Entity.rotationPitch
    jfieldID on_ground = nullptr;           // Entity.onGround
    jfieldID world_obj = nullptr;           // Entity.worldObj
    bool complete = false;
};

jobject static_object_global(events::JniContext* jni, jclass cls, const adjust::jni::Names& names,
                             const char* vanilla_desc, const char* named_desc) noexcept {
    const jfieldID fid = adjust::jni::static_field(jni, cls, names, vanilla_desc, named_desc);
    if (fid == nullptr) {
        return nullptr;
    }
    jobject local = jni->env->GetStaticObjectField(cls, fid);
    if (adjust::jni::clear_exception(jni) || local == nullptr) {
        return nullptr;
    }
    jobject global = jni->env->NewGlobalRef(local);
    jni->env->DeleteLocalRef(local);
    return global;
}

const Ids& ids(events::JniContext* jni) noexcept {
    static const Ids s_ids = [&] {
        namespace J = adjust::jni;
        Ids i{};
        const jclass entity_item = J::find_class(jni, "uz", "net/minecraft/entity/item/EntityItem");
        i.get_entity_item = J::method(jni, entity_item, {"l", "getEntityItem", "func_92059_d"},
                                      "()Lzx;", "()Lnet/minecraft/item/ItemStack;");
        const jclass stack = J::find_class(jni, "zx", "net/minecraft/item/ItemStack");
        i.stack_get_item = J::method(jni, stack, {"b", "getItem", "func_77973_b"},
                                     "()Lzw;", "()Lnet/minecraft/item/Item;");
        i.stack_get_metadata = J::method(jni, stack, {"i", "getMetadata", "func_77960_j"}, "()I", "()I");
        i.stack_size = J::field(jni, stack, {"b", "stackSize", "field_77994_a"}, "I", "I");
        i.item = J::find_class(jni, "zw", "net/minecraft/item/Item");
        i.item_get_id = J::static_method(jni, i.item, {"b", "getIdFromItem", "func_150891_b"},
                                         "(Lzw;)I", "(Lnet/minecraft/item/Item;)I");
        const jclass texture_map = J::find_class(jni, "bmh", "net/minecraft/client/renderer/texture/TextureMap");
        i.blocks_texture = static_object_global(jni, texture_map, {"g", "locationBlocksTexture", "field_110575_b"},
                                                "Ljy;", "Lnet/minecraft/util/ResourceLocation;");
        const jclass render = J::find_class(jni, "biv", "net/minecraft/client/renderer/entity/Render");
        i.bind_texture = J::method(jni, render, {"a", "bindTexture", "func_110776_a"},
                                   "(Ljy;)V", "(Lnet/minecraft/util/ResourceLocation;)V");
        i.get_render_manager = J::method(jni, render, {"d", "getRenderManager", "func_177068_d"},
                                         "()Lbiu;", "()Lnet/minecraft/client/renderer/entity/RenderManager;");
        const jclass render_manager = J::find_class(jni, "biu", "net/minecraft/client/renderer/entity/RenderManager");
        i.render_engine = J::field(jni, render_manager, {"a", "renderEngine", "field_78724_e"},
                                   "Lbmj;", "Lnet/minecraft/client/renderer/texture/TextureManager;");
        const jclass texture_manager =
            J::find_class(jni, "bmj", "net/minecraft/client/renderer/texture/TextureManager");
        i.get_texture = J::method(jni, texture_manager, {"b", "getTexture", "func_110581_b"}, "(Ljy;)Lbmk;",
                                  "(Lnet/minecraft/util/ResourceLocation;)"
                                  "Lnet/minecraft/client/renderer/texture/ITextureObject;");
        const jclass texture_object =
            J::find_class(jni, "bmk", "net/minecraft/client/renderer/texture/ITextureObject");
        i.set_blur_mipmap = J::method(jni, texture_object, {"b", "setBlurMipmap", "func_174936_b"}, "(ZZ)V", "(ZZ)V");
        i.restore_blur_mipmap =
            J::method(jni, texture_object, {"a", "restoreLastBlurMipmap", "func_174935_a"}, "()V", "()V");
        i.render_helper = J::find_class(jni, "avc", "net/minecraft/client/renderer/RenderHelper");
        i.enable_item_lighting = J::static_method(
            jni, i.render_helper, {"b", "enableStandardItemLighting", "func_74519_b"}, "()V", "()V");
        const jclass mc = J::find_class(jni, "ave", "net/minecraft/client/Minecraft");
        i.mc_get_render_item = J::method(jni, mc, {"ag", "getRenderItem", "func_175599_af"},
                                         "()Lbjh;", "()Lnet/minecraft/client/renderer/entity/RenderItem;");
        const jclass render_item = J::find_class(jni, "bjh", "net/minecraft/client/renderer/entity/RenderItem");
        i.get_item_model_mesher = J::method(jni, render_item, {"a", "getItemModelMesher", "func_175037_a"},
                                            "()Lbfo;", "()Lnet/minecraft/client/renderer/ItemModelMesher;");
        i.render_item = J::method(jni, render_item, {"a", "renderItem", "func_180454_a"}, "(Lzx;Lboq;)V",
                                  "(Lnet/minecraft/item/ItemStack;Lnet/minecraft/client/resources/model/IBakedModel;)V");
        const jclass mesher = J::find_class(jni, "bfo", "net/minecraft/client/renderer/ItemModelMesher");
        i.get_item_model = J::method(jni, mesher, {"a", "getItemModel", "func_178089_a"}, "(Lzx;)Lboq;",
                                     "(Lnet/minecraft/item/ItemStack;)"
                                     "Lnet/minecraft/client/resources/model/IBakedModel;");
        const jclass baked_model = J::find_class(jni, "boq", "net/minecraft/client/resources/model/IBakedModel");
        i.is_gui_3d = J::method(jni, baked_model, {"c", "isGui3d", "func_177556_c"}, "()Z", "()Z");
        i.get_camera_transforms =
            J::method(jni, baked_model, {"f", "getItemCameraTransforms", "func_177552_f"}, "()Lbgr;",
                      "()Lnet/minecraft/client/renderer/block/model/ItemCameraTransforms;");
        const jclass camera_transforms =
            J::find_class(jni, "bgr", "net/minecraft/client/renderer/block/model/ItemCameraTransforms");
        i.apply_transform = J::method(
            jni, camera_transforms, {"a", "applyTransform", "func_181689_a"}, "(Lbgr$b;)V",
            "(Lnet/minecraft/client/renderer/block/model/ItemCameraTransforms$TransformType;)V");
        const jclass transform_type = J::find_class(
            jni, "bgr$b", "net/minecraft/client/renderer/block/model/ItemCameraTransforms$TransformType");
        i.transform_ground = static_object_global(
            jni, transform_type, {"f", "GROUND", "GROUND"}, "Lbgr$b;",
            "Lnet/minecraft/client/renderer/block/model/ItemCameraTransforms$TransformType;");
        const jclass entity = J::find_class(jni, "pk", "net/minecraft/entity/Entity");
        i.rotation_yaw = J::field(jni, entity, {"y", "rotationYaw", "field_70177_z"}, "F", "F");
        i.rotation_pitch = J::field(jni, entity, {"z", "rotationPitch", "field_70125_A"}, "F", "F");
        i.on_ground = J::field(jni, entity, {"C", "onGround", "field_70122_E"}, "Z", "Z");
        i.world_obj = J::field(jni, entity, {"o", "worldObj", "field_70170_p"}, "Ladm;", "Lnet/minecraft/world/World;");
        i.complete = i.get_entity_item != nullptr && i.stack_get_item != nullptr && i.stack_get_metadata != nullptr &&
                     i.stack_size != nullptr && i.item_get_id != nullptr && i.blocks_texture != nullptr &&
                     i.bind_texture != nullptr && i.get_render_manager != nullptr && i.render_engine != nullptr &&
                     i.get_texture != nullptr && i.set_blur_mipmap != nullptr && i.restore_blur_mipmap != nullptr &&
                     i.enable_item_lighting != nullptr && i.mc_get_render_item != nullptr &&
                     i.get_item_model_mesher != nullptr && i.get_item_model != nullptr && i.is_gui_3d != nullptr &&
                     i.get_camera_transforms != nullptr && i.apply_transform != nullptr &&
                     i.transform_ground != nullptr && i.render_item != nullptr && i.rotation_yaw != nullptr &&
                     i.rotation_pitch != nullptr && i.on_ground != nullptr && i.world_obj != nullptr;
        return i;
    }();
    return s_ids;
}

// renderer.getRenderManager().renderEngine.getTexture(TextureMap.locationBlocksTexture) (local ref or nullptr)
jobject blocks_texture_object(events::JniContext* jni, const Ids& i, jobject renderer) noexcept {
    JNIEnv* env = jni->env;
    jobject manager = env->CallObjectMethod(renderer, i.get_render_manager);
    if (adjust::jni::clear_exception(jni) || manager == nullptr) {
        return nullptr;
    }
    jobject engine = env->GetObjectField(manager, i.render_engine);
    env->DeleteLocalRef(manager);
    if (engine == nullptr) {
        return nullptr;
    }
    jobject texture = env->CallObjectMethod(engine, i.get_texture, i.blocks_texture);
    env->DeleteLocalRef(engine);
    if (adjust::jni::clear_exception(jni)) {
        return nullptr;
    }
    return texture;
}

// model.getItemCameraTransforms().applyTransform(TransformType.GROUND)
void apply_ground_transform(events::JniContext* jni, const Ids& i, jobject model) noexcept {
    jobject transforms = jni->env->CallObjectMethod(model, i.get_camera_transforms);
    if (adjust::jni::clear_exception(jni) || transforms == nullptr) {
        return;
    }
    jni->env->CallVoidMethod(transforms, i.apply_transform, i.transform_ground);
    adjust::jni::clear_exception(jni);
    jni->env->DeleteLocalRef(transforms);
}

// ItemPhysicsModule.renderItemError: RenderItem.renderItem(stack, model, true); a Throwable is caught and logged.
void render_item_error(events::JniContext* jni, const Ids& i, jobject render_item, jobject stack,
                       jobject model) noexcept {
    jni->env->CallVoidMethod(render_item, i.render_item, stack, model);
    adjust::jni::clear_exception(jni); // "Failed to render item"
}

} // namespace

events::Module* get_adjust_item_physics_module() noexcept {
    return &g_state.base;
}

namespace item_physics {

bool running() noexcept {
    return events::module_is_running(&g_state.base);
}

void render_item(events::JniContext* jni, jobject renderer, jobject entity_item, double x, double y,
                 double z) noexcept {
    if (jni == nullptr || jni->env == nullptr || renderer == nullptr || entity_item == nullptr ||
        !gl::available(jni)) {
        return;
    }
    const Ids& i = ids(jni);
    if (!i.complete) {
        return;
    }
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(32) != 0) {
        env->ExceptionClear();
        return;
    }

    jobject stack = env->CallObjectMethod(entity_item, i.get_entity_item);
    jobject item = nullptr;
    if (!adjust::jni::clear_exception(jni) && stack != nullptr) {
        item = env->CallObjectMethod(stack, i.stack_get_item);
        adjust::jni::clear_exception(jni);
    }
    if (item == nullptr) {
        env->PopLocalFrame(nullptr);
        return;
    }

    // random.setSeed(Item.getIdFromItem(item) + stack.getMetadata())
    const jint id = env->CallStaticIntMethod(i.item, i.item_get_id, item);
    const jint meta = env->CallIntMethod(stack, i.stack_get_metadata);
    adjust::jni::clear_exception(jni);
    g_random.set_seed(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(id) + static_cast<uint32_t>(meta))));

    env->CallVoidMethod(renderer, i.bind_texture, i.blocks_texture);
    adjust::jni::clear_exception(jni);
    if (jobject texture = blocks_texture_object(jni, i, renderer); texture != nullptr) {
        env->CallVoidMethod(texture, i.set_blur_mipmap, JNI_FALSE, JNI_FALSE);
        adjust::jni::clear_exception(jni);
    }
    gl::enable_rescale_normal(jni);
    gl::alpha_func(jni, 516, 0.1f);
    gl::enable_blend(jni);
    env->CallStaticVoidMethod(i.render_helper, i.enable_item_lighting);
    adjust::jni::clear_exception(jni);
    gl::try_blend_func_separate(jni, 770, 771, 1, 0);
    gl::push_matrix(jni);

    jobject mc = minecraft::mc_get_instance(jni);
    jobject render_item = (mc != nullptr) ? env->CallObjectMethod(mc, i.mc_get_render_item) : nullptr;
    jobject mesher = nullptr;
    jobject model = nullptr;
    if (!adjust::jni::clear_exception(jni) && render_item != nullptr) {
        mesher = env->CallObjectMethod(render_item, i.get_item_model_mesher);
        if (!adjust::jni::clear_exception(jni) && mesher != nullptr) {
            model = env->CallObjectMethod(mesher, i.get_item_model, stack);
            adjust::jni::clear_exception(jni);
        }
    }

    if (model != nullptr) {
        const bool gui3d = env->CallBooleanMethod(model, i.is_gui_3d) != JNI_FALSE;
        adjust::jni::clear_exception(jni);
        const bool physical3d = gui3d;
        const jint stack_size = env->GetIntField(stack, i.stack_size);
        const int count = stack_size >= 49 ? 5 : stack_size >= 33 ? 4 : stack_size >= 17 ? 3 : stack_size >= 2 ? 2 : 1;

        gl::translate(jni, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        if (gui3d) {
            gl::scale(jni, 0.5f, 0.5f, 0.5f);
        }
        gl::rotate(jni, 90.0f, 1.0f, 0.0f, 0.0f);
        gl::rotate(jni, env->GetFloatField(entity_item, i.rotation_yaw), 0.0f, 0.0f, 1.0f);
        gl::translate(jni, 0.0f, 0.0f, physical3d ? -0.079999998f : -0.039999999f);

        // `physical3d || mc.gameSettings != null` is always true
        const float speed = g_state.rotation_speed.value;
        const bool on_ground = env->GetBooleanField(entity_item, i.on_ground) != JNI_FALSE;
        float pitch = env->GetFloatField(entity_item, i.rotation_pitch);
        if (physical3d) {
            if (!on_ground) {
                pitch += speed;
            }
        } else {
            jobject world = env->GetObjectField(entity_item, i.world_obj);
            if (world != nullptr) {
                pitch = on_ground ? 0.0f : pitch + speed;
                env->DeleteLocalRef(world);
            }
        }
        env->SetFloatField(entity_item, i.rotation_pitch, pitch);
        gl::rotate(jni, pitch, 1.0f, 0.0f, 0.0f);
        gl::color(jni, 1.0f, 1.0f, 1.0f, 1.0f);

        gl::push_matrix(jni);
        apply_ground_transform(jni, i, model);
        render_item_error(jni, i, render_item, stack, model);
        gl::pop_matrix(jni);
        if (!gui3d) {
            gl::translate(jni, 0.0f, 0.0f, 0.053750001f);
        }
        for (int n = 1; n < count; ++n) {
            gl::push_matrix(jni);
            if (gui3d) {
                const float offset_x = (g_random.next_float() * 2.0f - 1.0f) * 0.15000001f;
                const float offset_y = (g_random.next_float() * 2.0f - 1.0f) * 0.15000001f;
                const float offset_z = (g_random.next_float() * 2.0f - 1.0f) * 0.15000001f;
                gl::translate(jni, offset_x, offset_y, offset_z);
            }
            apply_ground_transform(jni, i, model);
            render_item_error(jni, i, render_item, stack, model);
            gl::pop_matrix(jni);
            if (!gui3d) {
                gl::translate(jni, 0.0f, 0.0f, 0.053750001f);
            }
        }
    }

    gl::pop_matrix(jni);
    gl::disable_rescale_normal(jni);
    gl::disable_blend(jni);
    env->CallVoidMethod(renderer, i.bind_texture, i.blocks_texture);
    adjust::jni::clear_exception(jni);
    if (jobject texture = blocks_texture_object(jni, i, renderer); texture != nullptr) {
        env->CallVoidMethod(texture, i.restore_blur_mipmap);
        adjust::jni::clear_exception(jni);
    }
    env->PopLocalFrame(nullptr);
}

} // namespace item_physics

} // namespace slinky::modules
