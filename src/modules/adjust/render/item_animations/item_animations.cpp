// Port of com.adjustclient.module.impl.render.ItemAnimationsModule (+ SwordAnimation, ItemAnimationsGlintCondition).
//
// Where Adjust's code runs:
//   * ItemRenderer.renderItemInFirstPerson / transformFirstPersonItem / doBowTransformations (first-person
//     position, scale, sword block animations, hide swinging, 1.7 item transform) -> item_hooks.cpp
//   * RenderItem.renderEffect (glint colour), RenderItem.renderItemModelForEntity (third-person 1.7 block pose)
//     -> item_hooks.cpp
//   * ModelBiped.setRotationAngles (1.7 blocking arm), EntityLivingBase.getArmSwingAnimationEnd (swing speed)
//     -> item_hooks.cpp
//   * Minecraft.runTick "Swing during use" -> on_game_tick below (no hook needed: runs every client tick, after
//     objectMouseOver was updated, before entities are ticked).
//
// The 1.7 transforms use GlStateManager (Adjust calls GL11 directly; GlStateManager's matrix functions are plain
// GL11 forwards, so the result is identical).

#include "modules/adjust/render/item_animations/item_animations.hpp"

#include <cmath>

#include "adjust/jni_lookup.hpp"
#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/item_animations/item_gl.hpp"

namespace slinky::modules {

namespace {

namespace gl = adjust::item_gl;

constexpr const char* kSwordAnimationNames[13] = {
    "None", "Vanilla", "Bounce", "Stasis", "Horizontal", "Dig", "Shove",
    "Tap", "Slide", "Attract", "Detract", "Swank", "Punch",
};

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;
void on_game_tick(void* event) noexcept;

struct ItemAnimationsState {
    events::Module base{};

    // Enchantment Glint
    config::BoolSetting glint_enabled{{}, "Enabled", false, false, {}};
    adjust::ColorOption glint_color{"Color", "Color mode", adjust::argb(67, 90, 209), adjust::ColorMode::Plain};

    // Swing Speed
    config::FloatSetting swing_speed_using{{}, "Using", "%.0f", 1.0f, 0.0f, -15.0f, 4.0f, 0.0f, 0.0f};
    config::FloatSetting swing_speed_normal{{}, "Normal", "%.0f", 1.0f, 0.0f, -15.0f, 4.0f, 0.0f, 0.0f};
    config::BoolSetting ignore_effects{{}, "Ignore effects", false, false, {}};

    // Position > Using / Normal, Scale, Sword only
    config::FloatSetting position_using_x{{}, "X", "%.2f", 0.01f, 0.0f, -0.8f, 0.5f, 0.0f, 0.0f};
    config::FloatSetting position_using_y{{}, "Y", "%.2f", 0.01f, 0.0f, -0.3f, 0.5f, 0.0f, 0.0f};
    config::FloatSetting position_normal_x{{}, "X", "%.2f", 0.01f, 0.0f, -0.8f, 0.5f, 0.0f, 0.0f};
    config::FloatSetting position_normal_y{{}, "Y", "%.2f", 0.01f, 0.0f, -0.3f, 0.5f, 0.0f, 0.0f};
    config::FloatSetting scale{{}, "Scale", "%.2f", 0.01f, 0.0f, 0.5f, 1.0f, 1.0f, 1.0f};
    config::BoolSetting sword_only{{}, "Sword only", false, false, {}};

    // Sword
    config::BoolSetting sword_enabled{{}, "Enabled", true, true, {}};
    config::ModeSetting animation{{}, "Type", 13, 0, kSwordAnimationNames,
                                  static_cast<int32_t>(item_animations::SwordAnimation::Vanilla),
                                  static_cast<int32_t>(item_animations::SwordAnimation::Vanilla), false, {}};
    config::FloatSetting equip_animation{{}, "Equip animation", "%.2f", 0.05f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f};
    config::BoolSetting accurate{{}, "Accurate", true, true, {}};

    config::BoolSetting swing_during_use{{}, "Swing during use", true, true, {}};
    config::BoolSetting old_transform{{}, "Old transform", true, true, {}};
    config::BoolSetting hide_swinging{{}, "Hide swinging", false, false, {}};

    ItemAnimationsState() noexcept {
        base.name = "Item Animations";
        base.description = "Customizes the first-person item animations.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust; // ItemAnimationsModule.setHidden(false)
        base.events[events::kEventGameTick] = {on_game_tick, 0, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Item Animations");
    }
};

ItemAnimationsState g_state{};

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Enchantment Glint");
    g_state.glint_enabled.render(gui, renderer);
    g_state.glint_color.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Swing Speed");
    g_state.swing_speed_using.render(gui, renderer);
    g_state.swing_speed_normal.render(gui, renderer);
    g_state.ignore_effects.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Position - Using");
    g_state.position_using_x.render(gui, renderer);
    g_state.position_using_y.render(gui, renderer);
    gui::widgets::render_section_header(gui, renderer, "Position - Normal");
    g_state.position_normal_x.render(gui, renderer);
    g_state.position_normal_y.render(gui, renderer);
    gui::widgets::render_section_header(gui, renderer, "Position");
    g_state.scale.render(gui, renderer);
    g_state.sword_only.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Sword");
    g_state.sword_enabled.render(gui, renderer);
    g_state.animation.render(gui, renderer);
    g_state.equip_animation.render(gui, renderer);
    if (item_animations::animation() != item_animations::SwordAnimation::Vanilla) { // hideIf animation == VANILLA
        g_state.accurate.render(gui, renderer);
    }

    gui::widgets::render_section_header(gui, renderer, "General");
    g_state.swing_during_use.render(gui, renderer);
    g_state.old_transform.render(gui, renderer);
    g_state.hide_swinging.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.glint_enabled.visit(archive, "glint_enabled");
    g_state.glint_color.visit(archive, "glint_color", "glint_color_mode");
    g_state.swing_speed_using.visit(archive, "swing_speed_using");
    g_state.swing_speed_normal.visit(archive, "swing_speed_normal");
    g_state.ignore_effects.visit(archive, "ignore_effects");
    g_state.position_using_x.visit(archive, "position_using_x");
    g_state.position_using_y.visit(archive, "position_using_y");
    g_state.position_normal_x.visit(archive, "position_normal_x");
    g_state.position_normal_y.visit(archive, "position_normal_y");
    g_state.scale.visit(archive, "scale");
    g_state.sword_only.visit(archive, "sword_only");
    g_state.sword_enabled.visit(archive, "sword_enabled");
    g_state.animation.visit(archive, "animation");
    g_state.equip_animation.visit(archive, "equip_animation");
    g_state.accurate.visit(archive, "accurate");
    g_state.swing_during_use.visit(archive, "swing_during_use");
    g_state.old_transform.visit(archive, "old_transform");
    g_state.hide_swinging.visit(archive, "hide_swinging");
}

// NumberSetting.getInt() of an integer slider
int32_t setting_int(const config::FloatSetting& s) noexcept {
    return static_cast<int32_t>(std::lround(s.value));
}

// ---------------------------------------------------------------------------------------------------------------
// JNI ids
// ---------------------------------------------------------------------------------------------------------------

struct Ids {
    jclass living = nullptr;           // EntityLivingBase
    jclass player_sp = nullptr;        // EntityPlayerSP
    jclass item_sword = nullptr;       // ItemSword
    jmethodID arm_swing_end = nullptr; // EntityLivingBase.getArmSwingAnimationEnd()I (private)
    jfieldID is_swing_in_progress = nullptr;
    jfieldID swing_progress_int = nullptr;
    jmethodID stack_get_item = nullptr;
    jmethodID stack_use_action = nullptr;
    jmethodID item_rotate_around = nullptr;   // Item.shouldRotateAroundWhenRendering()Z
    jmethodID mc_get_render_item = nullptr;   // Minecraft.getRenderItem()
    jmethodID render_item_in_3d = nullptr;    // RenderItem.shouldRenderItemIn3D(ItemStack)Z
};

const Ids& ids(events::JniContext* jni) noexcept {
    static const Ids s_ids = [&] {
        Ids i{};
        i.living = adjust::jni::find_class(jni, "pr", "net/minecraft/entity/EntityLivingBase");
        i.player_sp = adjust::jni::find_class(jni, "bew", "net/minecraft/client/entity/EntityPlayerSP");
        i.item_sword = adjust::jni::find_class(jni, "aay", "net/minecraft/item/ItemSword");
        i.arm_swing_end = adjust::jni::method(jni, i.living, {"n", "getArmSwingAnimationEnd", "func_82166_i"},
                                              "()I", "()I");
        i.is_swing_in_progress =
            adjust::jni::field(jni, i.living, {"ar", "isSwingInProgress", "field_82175_bq"}, "Z", "Z");
        i.swing_progress_int =
            adjust::jni::field(jni, i.living, {"as", "swingProgressInt", "field_110158_av"}, "I", "I");
        const jclass stack = adjust::jni::find_class(jni, "zx", "net/minecraft/item/ItemStack");
        i.stack_get_item = adjust::jni::method(jni, stack, {"b", "getItem", "func_77973_b"},
                                               "()Lzw;", "()Lnet/minecraft/item/Item;");
        i.stack_use_action = adjust::jni::method(jni, stack, {"m", "getItemUseAction", "func_77975_n"},
                                                 "()Laba;", "()Lnet/minecraft/item/EnumAction;");
        const jclass item = adjust::jni::find_class(jni, "zw", "net/minecraft/item/Item");
        i.item_rotate_around = adjust::jni::method(
            jni, item, {"e", "shouldRotateAroundWhenRendering", "func_77629_n_"}, "()Z", "()Z");
        const jclass mc = adjust::jni::find_class(jni, "ave", "net/minecraft/client/Minecraft");
        i.mc_get_render_item = adjust::jni::method(jni, mc, {"ag", "getRenderItem", "func_175599_af"},
                                                   "()Lbjh;", "()Lnet/minecraft/client/renderer/entity/RenderItem;");
        const jclass render_item = adjust::jni::find_class(jni, "bjh", "net/minecraft/client/renderer/entity/RenderItem");
        i.render_item_in_3d = adjust::jni::method(jni, render_item, {"a", "shouldRenderItemIn3D", "func_175050_a"},
                                                  "(Lzx;)Z", "(Lnet/minecraft/item/ItemStack;)Z");
        return i;
    }();
    return s_ids;
}

bool usable(events::JniContext* jni) noexcept {
    return jni != nullptr && jni->env != nullptr && jni->mappings != nullptr && adjust::jni::is_1_8(jni);
}

// Original EntityLivingBase.getArmSwingAnimationEnd() through JNI (private -> non-virtual call).
int32_t vanilla_arm_swing_end(events::JniContext* jni, jobject living) noexcept {
    const Ids& i = ids(jni);
    if (i.living == nullptr || i.arm_swing_end == nullptr) {
        return 6;
    }
    const jint v = jni->env->CallNonvirtualIntMethod(living, i.living, i.arm_swing_end);
    if (jni->env->ExceptionCheck()) {
        jni->env->ExceptionClear();
        return 6;
    }
    return v;
}

// Minecraft.runTick (Adjust): while the player uses an item, holding attack on a block keeps the swing animation
// going client-side.
void on_game_tick(void* event) noexcept {
    auto* ev = static_cast<events::ContextEvent*>(event);
    // ItemAnimationsModule.shouldSwingDuringUse()
    if (ev == nullptr || ev->ctx == nullptr || !g_state.swing_during_use.value ||
        !events::module_is_running(&g_state.base)) {
        return;
    }
    events::GameContext* ctx = ev->ctx;
    events::JniContext* jni = ctx->jni;
    if (!usable(jni) || ctx->local_player == nullptr || ctx->minecraft == nullptr || ctx->key_states == nullptr ||
        !ctx->in_game_focus) {
        return;
    }
    const Ids& i = ids(jni);
    if (i.is_swing_in_progress == nullptr || i.swing_progress_int == nullptr) {
        return;
    }
    JNIEnv* env = jni->env;
    jobject player = ctx->local_player;
    if (!minecraft::player_is_using_item(jni, player) || adjust::jni::clear_exception(jni)) {
        return;
    }
    if (!minecraft::is_attack_key_down(ctx)) { // Adjust.mouseClickHelper.left.isClicked(false)
        return;
    }
    jobject mop = minecraft::mc_get_object_mouse_over(jni, ctx->minecraft);
    if (mop == nullptr) {
        return;
    }
    const int32_t type = minecraft::mop_get_type_ordinal(jni, mop); // MovingObjectType: MISS, BLOCK, ENTITY
    env->DeleteLocalRef(mop);
    if (adjust::jni::clear_exception(jni) || type != 1) {
        return;
    }
    const bool in_progress = env->GetBooleanField(player, i.is_swing_in_progress) != JNI_FALSE;
    const jint progress = env->GetIntField(player, i.swing_progress_int);
    const int32_t end = item_animations::patch_arm_swing_animation_end(jni, player, vanilla_arm_swing_end(jni, player));
    if (!in_progress || progress >= end / 2 || progress < 0) {
        env->SetIntField(player, i.swing_progress_int, -1);
        env->SetBooleanField(player, i.is_swing_in_progress, JNI_TRUE);
    }
}

} // namespace

events::Module* get_adjust_item_animations_module() noexcept {
    return &g_state.base;
}

namespace item_animations {

bool running() noexcept {
    return events::module_is_running(&g_state.base);
}

bool old_transform() noexcept { return g_state.old_transform.value; }
bool sword_enabled() noexcept { return g_state.sword_enabled.value; }

SwordAnimation animation() noexcept {
    const int32_t v = g_state.animation.value;
    if (v < 0 || v >= 13) {
        return SwordAnimation::Vanilla;
    }
    return static_cast<SwordAnimation>(v);
}

float equip_animation() noexcept { return g_state.equip_animation.value; }
float position_scale() noexcept { return g_state.scale.value; }
bool sword_only() noexcept { return g_state.sword_only.value; }

bool should_hide_swinging() noexcept {
    return running() && g_state.hide_swinging.value;
}

bool custom_glint_color(adjust::Argb* out) noexcept {
    if (out == nullptr || !running() || !g_state.glint_enabled.value) {
        return false;
    }
    *out = g_state.glint_color.get();
    return true;
}

bool is_sword_stack(events::JniContext* jni, jobject stack) noexcept {
    if (stack == nullptr || !usable(jni)) {
        return false;
    }
    const Ids& i = ids(jni);
    if (i.stack_get_item == nullptr || i.item_sword == nullptr) {
        return false;
    }
    jobject item = jni->env->CallObjectMethod(stack, i.stack_get_item);
    if (adjust::jni::clear_exception(jni) || item == nullptr) {
        return false;
    }
    const bool sword = jni->env->IsInstanceOf(item, i.item_sword) != JNI_FALSE;
    jni->env->DeleteLocalRef(item);
    return sword;
}

int32_t use_action_ordinal(events::JniContext* jni, jobject stack) noexcept {
    if (stack == nullptr || !usable(jni)) {
        return -1;
    }
    const Ids& i = ids(jni);
    if (i.stack_use_action == nullptr) {
        return -1;
    }
    jobject action = jni->env->CallObjectMethod(stack, i.stack_use_action);
    if (adjust::jni::clear_exception(jni) || action == nullptr) {
        return -1;
    }
    const int32_t ordinal = minecraft::enum_get_ordinal(jni, action);
    jni->env->DeleteLocalRef(action);
    if (adjust::jni::clear_exception(jni)) {
        return -1;
    }
    return ordinal;
}

int32_t local_item_in_use_count(events::JniContext* jni) noexcept {
    if (!usable(jni)) {
        return 0;
    }
    jobject mc = minecraft::mc_get_instance(jni);
    if (mc == nullptr) {
        return 0;
    }
    jobject player = minecraft::mc_get_player(jni, mc);
    jni->env->DeleteLocalRef(mc);
    if (player == nullptr) {
        return 0;
    }
    const int32_t count = minecraft::player_get_item_in_use_count(jni, player);
    jni->env->DeleteLocalRef(player);
    return count;
}

int32_t patch_arm_swing_animation_end(events::JniContext* jni, jobject living, int32_t vanilla) noexcept {
    if (!running() || living == nullptr || !usable(jni)) {
        return vanilla;
    }
    const Ids& i = ids(jni);
    if (i.player_sp == nullptr || jni->env->IsInstanceOf(living, i.player_sp) == JNI_FALSE) {
        return vanilla; // Entity.isUser() is only true for EntityPlayerSP
    }
    int32_t value = vanilla;
    if (g_state.ignore_effects.value) {
        value = 6;
    }
    // mc.thePlayer != null ? mc.thePlayer.getItemInUseCount() >= 1 : this.getItemInUseCount() >= 1
    int32_t count = 0;
    jobject mc = minecraft::mc_get_instance(jni);
    jobject player = (mc != nullptr) ? minecraft::mc_get_player(jni, mc) : nullptr;
    count = minecraft::player_get_item_in_use_count(jni, player != nullptr ? player : living);
    if (player != nullptr) {
        jni->env->DeleteLocalRef(player);
    }
    if (mc != nullptr) {
        jni->env->DeleteLocalRef(mc);
    }
    const bool using_item = count >= 1;
    value -= setting_int(using_item ? g_state.swing_speed_using : g_state.swing_speed_normal);
    return value;
}

void transform_blocking(events::JniContext* jni, bool reverse) noexcept {
    if (!g_state.old_transform.value) {
        return;
    }
    if (reverse) {
        gl::rotate(jni, -335.0f, 0.0f, 0.0f, 1.0f);
        gl::rotate(jni, -50.0f, 0.0f, 1.0f, 0.0f);
        gl::translate(jni, 0.0f, 0.5f, 0.0f);
    } else {
        gl::translate(jni, 0.0f, -0.5f, 0.0f);
        gl::rotate(jni, 50.0f, 0.0f, 1.0f, 0.0f);
        gl::rotate(jni, 335.0f, 0.0f, 0.0f, 1.0f);
    }
}

void apply_position(events::JniContext* jni, jobject stack) noexcept {
    if (!(stack != nullptr && is_sword_stack(jni, stack)) && g_state.sword_only.value) {
        return;
    }
    gl::translate(jni, 0.0f, (1.0f - g_state.scale.value) * 0.5f, 0.0f);
    if (local_item_in_use_count(jni) > 0) {
        gl::translate(jni, g_state.position_using_x.value, g_state.position_using_y.value, 0.0f);
    } else {
        gl::translate(jni, g_state.position_normal_x.value, g_state.position_normal_y.value, 0.0f);
    }
}

bool transform_item(events::JniContext* jni, jobject stack) noexcept {
    if (!g_state.old_transform.value) {
        return true;
    }
    if (stack == nullptr || !usable(jni)) {
        return true;
    }
    if (local_item_in_use_count(jni) > 0 && use_action_ordinal(jni, stack) == kActionBlock &&
        g_state.sword_enabled.value && !(animation() == SwordAnimation::Vanilla && g_state.accurate.value)) {
        return true;
    }
    const Ids& i = ids(jni);
    JNIEnv* env = jni->env;

    bool rotate_around = false;
    if (i.stack_get_item != nullptr && i.item_rotate_around != nullptr) {
        jobject item = env->CallObjectMethod(stack, i.stack_get_item);
        if (!adjust::jni::clear_exception(jni) && item != nullptr) {
            rotate_around = env->CallBooleanMethod(item, i.item_rotate_around) != JNI_FALSE;
            adjust::jni::clear_exception(jni);
            env->DeleteLocalRef(item);
        }
    }
    if (rotate_around) {
        gl::rotate(jni, 180.0f, 0.0f, 1.0f, 0.0f);
    }
    gl::translate(jni, 0.58800083f, 0.36999986f, -0.77000016f);
    gl::translate(jni, 0.0f, -0.30000001f, 0.0f);
    gl::scale(jni, 1.5f, 1.5f, 1.5f);
    gl::rotate(jni, 50.0f, 0.0f, 1.0f, 0.0f);
    gl::rotate(jni, 335.0f, 0.0f, 0.0f, 1.0f);
    gl::translate(jni, -0.9375f, -0.0625f, 0.0f);
    gl::scale(jni, -2.0f, 2.0f, -2.0f);

    bool in_3d = false;
    if (i.mc_get_render_item != nullptr && i.render_item_in_3d != nullptr) {
        jobject mc = minecraft::mc_get_instance(jni);
        jobject render_item = (mc != nullptr) ? env->CallObjectMethod(mc, i.mc_get_render_item) : nullptr;
        if (!adjust::jni::clear_exception(jni) && render_item != nullptr) {
            in_3d = env->CallBooleanMethod(render_item, i.render_item_in_3d, stack) != JNI_FALSE;
            adjust::jni::clear_exception(jni);
        }
        if (render_item != nullptr) {
            env->DeleteLocalRef(render_item);
        }
        if (mc != nullptr) {
            env->DeleteLocalRef(mc);
        }
    }
    if (in_3d) {
        gl::scale(jni, 0.58823526f, 0.58823526f, 0.58823526f);
        gl::rotate(jni, -25.0f, 0.0f, 0.0f, 1.0f);
        gl::rotate(jni, 0.0f, 1.0f, 0.0f, 0.0f);
        gl::rotate(jni, 135.0f, 0.0f, 1.0f, 0.0f);
        gl::translate(jni, 0.0f, -0.25f, -0.125f);
        gl::scale(jni, 0.5f, 0.5f, 0.5f);
        return true;
    }
    gl::scale(jni, 0.5f, 0.5f, 0.5f);
    return false;
}

void transform_bow(events::JniContext* jni) noexcept {
    if (!g_state.old_transform.value) {
        return;
    }
    gl::translate(jni, -0.15000001f, -0.2f, 0.0f);
    gl::rotate(jni, 70.0f, 1.0f, 0.0f, 0.0f);
    gl::translate(jni, 0.119f, 0.2f, -0.024f);
}

} // namespace item_animations

} // namespace slinky::modules
