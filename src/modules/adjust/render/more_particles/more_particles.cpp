// Port of com.adjustclient.module.impl.render.MoreParticlesModule: multiplies the critical / enchantment
// (sharpness) hit particles of your own attacks.
//
// Adjust patches EntityPlayer.attackTargetEntityWithCurrentItem and EntityPlayerSP.onCriticalHit /
// onEnchantmentCritical so they emit `Critical` / `Enchantment` particles instead of one, and (option "Always show
// sharp particles") call onEnchantmentCritical on every attack. Without a hook this port reproduces it around
// Minecraft.clickMouse: before the click it evaluates vanilla's conditions (critical: falling, not on ground /
// ladder / in water, not blind, not riding, living target; enchantment: EnchantmentHelper.getModifierForCreature
// > 0; on the client the attack only "succeeds" against other players), after the click it emits the missing
// particles through mc.effectRenderer.emitParticleAtEntity, exactly like EntityPlayerSP does.

#include "modules/adjust/render/more_particles/more_particles.hpp"

#include <algorithm>

#include "adjust/jni_lookup.hpp"
#include "config/settings.hpp"
#include "gui/widgets.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"

namespace slinky::modules {

namespace {

constexpr int32_t kMopTypeEntity = 2;    // MovingObjectPosition.MovingObjectType.ENTITY
constexpr uint32_t kPotionBlindness = 15;

void on_pre_attack(void* event) noexcept;
void on_post_attack(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

struct MoreParticlesState {
    events::Module base{};

    config::FloatSetting enchantment{{}, "Enchantment", "%.0f", 1.0f, 0.0f, 1.0f, 10.0f, 1.0f, 1.0f};
    config::FloatSetting critical{{}, "Critical", "%.0f", 1.0f, 0.0f, 1.0f, 10.0f, 1.0f, 1.0f};
    config::BoolSetting always_sharp{{}, "Always show sharp particles", true, true, {}};

    // Evaluated before clickMouse, consumed after it.
    bool pending = false;
    bool critical_hit = false;    // onCriticalHit would be called
    bool enchantment_hit = false; // onEnchantmentCritical would be called (f1 > 0)
    bool attack_success = false;  // targetEntity.attackEntityFrom(...) on the client

    MoreParticlesState() noexcept {
        base.name = "More Particles";
        base.description = "Shows more hit particles.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagHiddenFromHud; // Adjust default: hidden
        // Lowest priority: sees whether another module cancelled the click.
        base.events[events::kEventPreAttackOrUseEntity] = {on_pre_attack, -100000, 0};
        base.events[events::kEventPostAttackOrUseEntity] = {on_post_attack, 0, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust More Particles");
    }
};

MoreParticlesState g_state{};

int count_of(const config::FloatSetting& setting) noexcept {
    return std::clamp(static_cast<int>(setting.value), 1, 10);
}

bool call_bool(events::JniContext* jni, jobject obj, jmethodID mid) noexcept {
    if (mid == nullptr || obj == nullptr) {
        return false;
    }
    const bool value = jni->env->CallBooleanMethod(obj, mid) != JNI_FALSE;
    return !adjust::jni::clear_exception(jni) && value;
}

// EnchantmentHelper.getModifierForCreature(player.getHeldItem(), target.getCreatureAttribute() / UNDEFINED)
float enchantment_modifier(events::JniContext* jni, jobject player, jobject living_target) noexcept {
    JNIEnv* env = jni->env;
    static const jmethodID mid_held_item = adjust::jni::method(
        jni, jni->mappings->cls_entity_living_base, {"bA", "getHeldItem", "func_70694_bm"}, "()Lzx;",
        "()Lnet/minecraft/item/ItemStack;");
    static const jmethodID mid_creature_attribute = adjust::jni::method(
        jni, jni->mappings->cls_entity_living_base, {"bz", "getCreatureAttribute", "func_70668_bt"}, "()Lpw;",
        "()Lnet/minecraft/entity/EnumCreatureAttribute;");
    static const jclass cls_attribute =
        adjust::jni::find_class(jni, "pw", "net/minecraft/entity/EnumCreatureAttribute");
    static const jfieldID sfid_undefined = adjust::jni::static_field(
        jni, cls_attribute, {"a", "UNDEFINED", "UNDEFINED"}, "Lpw;", "Lnet/minecraft/entity/EnumCreatureAttribute;");
    static const jclass cls_helper =
        adjust::jni::find_class(jni, "ack", "net/minecraft/enchantment/EnchantmentHelper");
    static const jmethodID mid_modifier = adjust::jni::static_method(
        jni, cls_helper, {"a", "getModifierForCreature", "func_152377_a"}, "(Lzx;Lpw;)F",
        "(Lnet/minecraft/item/ItemStack;Lnet/minecraft/entity/EnumCreatureAttribute;)F");
    if (mid_held_item == nullptr || mid_modifier == nullptr) {
        return 0.0f;
    }

    jobject stack = env->CallObjectMethod(player, mid_held_item);
    if (adjust::jni::clear_exception(jni)) {
        return 0.0f;
    }
    jobject attribute = nullptr;
    if (living_target != nullptr && mid_creature_attribute != nullptr) {
        attribute = env->CallObjectMethod(living_target, mid_creature_attribute);
        adjust::jni::clear_exception(jni);
    } else if (sfid_undefined != nullptr) {
        attribute = env->GetStaticObjectField(cls_attribute, sfid_undefined);
    }
    float modifier = 0.0f;
    if (attribute != nullptr) {
        modifier = env->CallStaticFloatMethod(cls_helper, mid_modifier, stack, attribute);
        if (adjust::jni::clear_exception(jni)) {
            modifier = 0.0f;
        }
    }
    return modifier;
}

// Before Minecraft.clickMouse: will it attack an entity, and which particles would vanilla emit?
void on_pre_attack(void* event) noexcept {
    auto* ev = static_cast<events::AttackOrUseEntityEvent*>(event);
    g_state.pending = false;
    events::GameContext* ctx = ev->ctx;
    if (!ev->is_attack || ev->cancelled || ctx == nullptr || ctx->jni == nullptr || ctx->local_player == nullptr ||
        ctx->minecraft == nullptr || !adjust::jni::is_1_8(ctx->jni)) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    static const jmethodID mid_on_ladder = adjust::jni::method(
        jni, jni->mappings->cls_entity_living_base, {"k_", "isOnLadder", "func_70617_f_"}, "()Z", "()Z");
    static const jmethodID mid_in_water =
        adjust::jni::method(jni, jni->mappings->cls_entity, {"V", "isInWater", "func_70090_H"}, "()Z", "()Z");

    if (minecraft::mc_get_left_click_counter(jni, ctx->minecraft) > 0) {
        return; // clickMouse does nothing
    }
    if (env->PushLocalFrame(32) != 0) {
        adjust::jni::clear_exception(jni);
        return;
    }
    jobject mop = minecraft::mc_get_object_mouse_over(jni, ctx->minecraft);
    jobject target = nullptr;
    if (mop != nullptr && minecraft::mop_get_type_ordinal(jni, mop) == kMopTypeEntity) {
        target = minecraft::mop_get_entity_hit(jni, mop);
    }
    if (target != nullptr) {
        jobject player = ctx->local_player;
        jobject living_target = minecraft::as_entity_living_base(jni, target);
        jobject blindness = minecraft::potion_get_by_id(jni, kPotionBlindness);
        const bool blind =
            blindness != nullptr && minecraft::living_get_active_potion_effect(jni, player, blindness) != nullptr;
        adjust::jni::clear_exception(jni);

        g_state.critical_hit = minecraft::entity_get_fall_distance(jni, player) > 0.0f &&
                               !minecraft::entity_is_on_ground(jni, player) && !call_bool(jni, player, mid_on_ladder) &&
                               !call_bool(jni, player, mid_in_water) && !blind &&
                               minecraft::entity_get_riding_entity(jni, player) == nullptr && living_target != nullptr;
        g_state.enchantment_hit = enchantment_modifier(jni, player, living_target) > 0.0f;
        // EntityLivingBase.attackEntityFrom returns false on the client; EntityOtherPlayerMP returns true.
        g_state.attack_success = minecraft::as_entity_other_player_mp(jni, target) != nullptr;
        g_state.pending = true;
    }
    env->PopLocalFrame(nullptr);
}

void emit(events::JniContext* jni, jobject effect_renderer, jmethodID mid_emit, jobject entity, jobject type,
          int count) noexcept {
    for (int i = 0; i < count; ++i) {
        jni->env->CallVoidMethod(effect_renderer, mid_emit, entity, type);
        if (adjust::jni::clear_exception(jni)) {
            return;
        }
    }
}

// After clickMouse: emit the particles Adjust adds on top of vanilla's single ones.
void on_post_attack(void* event) noexcept {
    auto* ev = static_cast<events::AttackOrUseEntityEvent*>(event);
    if (!ev->is_attack || !g_state.pending) {
        return;
    }
    g_state.pending = false;
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->minecraft == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    static const jfieldID fid_effect_renderer = adjust::jni::field(
        jni, jni->mappings->cls_minecraft, {"j", "effectRenderer", "field_71452_i"}, "Lbec;",
        "Lnet/minecraft/client/particle/EffectRenderer;");
    static const jclass cls_effect_renderer =
        adjust::jni::find_class(jni, "bec", "net/minecraft/client/particle/EffectRenderer");
    static const jmethodID mid_emit = adjust::jni::method(
        jni, cls_effect_renderer, {"a", "emitParticleAtEntity", "func_178926_a"}, "(Lpk;Lcy;)V",
        "(Lnet/minecraft/entity/Entity;Lnet/minecraft/util/EnumParticleTypes;)V");
    static const jclass cls_particle_types = adjust::jni::find_class(jni, "cy", "net/minecraft/util/EnumParticleTypes");
    static const jfieldID sfid_crit = adjust::jni::static_field(jni, cls_particle_types, {"j", "CRIT", "CRIT"},
                                                                "Lcy;", "Lnet/minecraft/util/EnumParticleTypes;");
    static const jfieldID sfid_crit_magic = adjust::jni::static_field(
        jni, cls_particle_types, {"k", "CRIT_MAGIC", "CRIT_MAGIC"}, "Lcy;", "Lnet/minecraft/util/EnumParticleTypes;");
    if (fid_effect_renderer == nullptr || mid_emit == nullptr || sfid_crit == nullptr || sfid_crit_magic == nullptr) {
        return;
    }

    const int critical = count_of(g_state.critical);
    const int enchantment = count_of(g_state.enchantment);
    int extra_crit = 0;
    int extra_magic = 0;
    if (g_state.attack_success && g_state.critical_hit) {
        extra_crit += critical - 1; // onCriticalHit: `critical` particles instead of 1
    }
    if (g_state.attack_success && g_state.enchantment_hit) {
        extra_magic += enchantment - 1; // onEnchantmentCritical: `enchantment` particles instead of 1
    }
    if (g_state.always_sharp.value) {
        extra_magic += enchantment; // the additional unconditional onEnchantmentCritical call
    }
    if (extra_crit <= 0 && extra_magic <= 0) {
        return;
    }

    if (env->PushLocalFrame(16) != 0) {
        adjust::jni::clear_exception(jni);
        return;
    }
    jobject mop = minecraft::mc_get_object_mouse_over(jni, ctx->minecraft);
    jobject target = (mop != nullptr && minecraft::mop_get_type_ordinal(jni, mop) == kMopTypeEntity)
                         ? minecraft::mop_get_entity_hit(jni, mop)
                         : nullptr;
    jobject effect_renderer = env->GetObjectField(ctx->minecraft, fid_effect_renderer);
    if (target != nullptr && effect_renderer != nullptr) {
        if (extra_crit > 0) {
            emit(jni, effect_renderer, mid_emit, target, env->GetStaticObjectField(cls_particle_types, sfid_crit),
                 extra_crit);
        }
        if (extra_magic > 0) {
            emit(jni, effect_renderer, mid_emit, target,
                 env->GetStaticObjectField(cls_particle_types, sfid_crit_magic), extra_magic);
        }
    }
    env->PopLocalFrame(nullptr);
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_state.enchantment.render(gui, renderer);
    g_state.critical.render(gui, renderer);
    g_state.always_sharp.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.enchantment.visit(archive, "enchantment");
    g_state.critical.visit(archive, "critical");
    g_state.always_sharp.visit(archive, "always_show_sharp_particles");
}

} // namespace

events::Module* get_adjust_more_particles_module() noexcept {
    return &g_state.base;
}

} // namespace slinky::modules
