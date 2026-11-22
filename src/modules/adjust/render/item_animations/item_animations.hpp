#pragma once

// Adjust "Item Animations" (com.adjustclient.module.impl.render.ItemAnimationsModule + SwordAnimation +
// ItemAnimationsGlintCondition).
//
// The settings and ItemAnimationsModule's methods live here; the Minecraft call-ins (Adjust's patches of
// ItemRenderer, RenderItem, ModelBiped, EntityLivingBase) are bytecode hooks in src/adjust/hooks/item_hooks.cpp,
// which call the functions below. "Swing during use" (Adjust's Minecraft.runTick patch) runs on kEventGameTick.

#include <cstdint>
#include <jni.h>

#include "adjust/kit.hpp"
#include "events/events.hpp"
#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_item_animations_module() noexcept;

namespace item_animations {

// SwordAnimation (enum order = setting option index)
enum class SwordAnimation : int32_t {
    None = 0,
    Vanilla,
    Bounce,
    Stasis,
    Horizontal,
    Dig,
    Shove,
    Tap,
    Slide,
    Attract,
    Detract,
    Swank,
    Punch,
};

// module_is_running(Item Animations)
bool running() noexcept;

// ---- settings (valid whether or not the module runs) ----
bool old_transform() noexcept;            // "Old transform"
bool sword_enabled() noexcept;            // Sword > Enabled
SwordAnimation animation() noexcept;      // Sword > Type
float equip_animation() noexcept;         // Sword > Equip animation
float position_scale() noexcept;          // Position > Scale
bool sword_only() noexcept;               // Position > Sword only

// isEnabled() && hideSwinging
bool should_hide_swinging() noexcept;

// isEnabled() && enchantmentGlintEnabled: writes the glint colour (ColorSetting.getColor().getRGB()).
bool custom_glint_color(adjust::Argb* out) noexcept;

// EntityLivingBase.getArmSwingAnimationEnd() patch: `vanilla` is the original result for `living`; returns the
// patched value (unchanged unless the module runs and `living` is the local player, Adjust's isUser()).
int32_t patch_arm_swing_animation_end(events::JniContext* jni, jobject living, int32_t vanilla) noexcept;

// ---- ItemAnimationsModule methods (GlStateManager transforms; caller checks running()) ----
void transform_blocking(events::JniContext* jni, bool reverse) noexcept;          // transformBlocking
void apply_position(events::JniContext* jni, jobject stack) noexcept;             // applyPosition
bool transform_item(events::JniContext* jni, jobject stack) noexcept;             // transformItem
void transform_bow(events::JniContext* jni) noexcept;                             // transformBow

// Helpers shared with the hooks.
bool is_sword_stack(events::JniContext* jni, jobject stack) noexcept;             // stack.getItem() instanceof ItemSword
int32_t use_action_ordinal(events::JniContext* jni, jobject stack) noexcept;      // stack.getItemUseAction().ordinal(), -1 on error
int32_t local_item_in_use_count(events::JniContext* jni) noexcept;                // mc.thePlayer.getItemInUseCount(), 0 if none

// EnumAction ordinals (1.8.9)
inline constexpr int32_t kActionNone = 0;
inline constexpr int32_t kActionEat = 1;
inline constexpr int32_t kActionDrink = 2;
inline constexpr int32_t kActionBlock = 3;
inline constexpr int32_t kActionBow = 4;

} // namespace item_animations

} // namespace slinky::modules
