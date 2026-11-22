#pragma once

// Minecraft 1.8.9 members used by the Adjust render ports (ESP, Chams, Skeleton) that the central Mappings
// table does not cover. Resolved lazily through adjust::jni (cached in function-local statics) and shared by
// src/modules/adjust/render/{esp,chams,skeleton}. Every accessor degrades gracefully: on 1.7.10, or when a
// member cannot be resolved, it returns a neutral value (0 / false / nullptr) and never throws into Java.
//
// Local references returned by the jobject accessors belong to the caller (delete them in loops).

#include <jni.h>

#include <string>

#include "events/events.hpp"
#include "util/math.hpp"

namespace slinky::modules::adjust_render {

// ---- generic helpers ----------------------------------------------------------------------------------------

bool available(events::JniContext* jni) noexcept; // 1.8.9 with a usable JNI context

// java.lang.String -> UTF-8 (modified UTF-8 for BMP characters is identical). Does not delete `text`.
std::string to_utf8(events::JniContext* jni, jstring text);

// Object.toString() of any object (e.g. an EnumChatFormatting -> "§c"). Empty on failure.
std::string object_to_string(events::JniContext* jni, jobject obj);

// Deletes a local reference if non-null.
inline void drop(events::JniContext* jni, jobject obj) noexcept {
    if (obj != nullptr) {
        jni->env->DeleteLocalRef(obj);
    }
}

// ---- class checks -------------------------------------------------------------------------------------------

enum class Cls : int {
    EntityLivingBase,
    EntityLiving,
    EntityPlayer,
    EntityPlayerSP,
    EntityItem,
    EntityTNTPrimed,
    EntityMob,
    EntityIronGolem,
    EntityArmorStand,
    EntityZombie,
    ItemSword,
    ItemTool,
    Count
};

bool is_instance(events::JniContext* jni, jobject obj, Cls cls) noexcept;

// ---- Entity -------------------------------------------------------------------------------------------------

math::Vec3d entity_last_tick_pos(events::JniContext* jni, jobject entity) noexcept;   // lastTickPosX/Y/Z
float entity_width(events::JniContext* jni, jobject entity) noexcept;
float entity_height(events::JniContext* jni, jobject entity) noexcept;
bool entity_is_invisible(events::JniContext* jni, jobject entity) noexcept;           // isInvisible()
bool entity_is_riding(events::JniContext* jni, jobject entity) noexcept;              // isRiding()
bool entity_ignore_frustum_check(events::JniContext* jni, jobject entity) noexcept;
float entity_pitch(events::JniContext* jni, jobject entity) noexcept;                 // rotationPitch
float entity_prev_pitch(events::JniContext* jni, jobject entity) noexcept;            // prevRotationPitch
std::string entity_name(events::JniContext* jni, jobject entity);                     // getName()
std::string entity_formatted_display_name(events::JniContext* jni, jobject entity);   // getDisplayName().getFormattedText()

// ---- EntityLivingBase ---------------------------------------------------------------------------------------

float living_render_yaw_offset(events::JniContext* jni, jobject living) noexcept;
float living_prev_render_yaw_offset(events::JniContext* jni, jobject living) noexcept;
float living_head_yaw(events::JniContext* jni, jobject living) noexcept;              // rotationYawHead
float living_prev_head_yaw(events::JniContext* jni, jobject living) noexcept;         // prevRotationYawHead
int living_death_time(events::JniContext* jni, jobject living) noexcept;
float living_limb_swing(events::JniContext* jni, jobject living) noexcept;
float living_limb_swing_amount(events::JniContext* jni, jobject living) noexcept;
float living_prev_limb_swing_amount(events::JniContext* jni, jobject living) noexcept;
bool living_is_child(events::JniContext* jni, jobject living) noexcept;               // isChild()
float living_swing_progress(events::JniContext* jni, jobject living, float partial_ticks) noexcept;
int living_total_armor(events::JniContext* jni, jobject living) noexcept;             // getTotalArmorValue()
float living_absorption(events::JniContext* jni, jobject living) noexcept;            // getAbsorptionAmount()
jobject living_held_item(events::JniContext* jni, jobject living) noexcept;           // getHeldItem()
jobject living_current_armor(events::JniContext* jni, jobject living, int slot) noexcept; // getCurrentArmor(0..3)

struct PotionIcon {
    int icon_index = -1;  // Potion.getStatusIconIndex()
    int amplifier = 0;
};
// getActivePotionEffects() -> icon index + amplifier for each effect with an icon (count written to *out_count).
int living_potion_icons(events::JniContext* jni, jobject living, PotionIcon* out, int max_count) noexcept;

// ---- EntityPlayer -------------------------------------------------------------------------------------------

bool player_is_blocking(events::JniContext* jni, jobject player) noexcept;
bool player_is_creative(events::JniContext* jni, jobject player) noexcept;            // capabilities.isCreativeMode
bool player_is_spectator(events::JniContext* jni, jobject player) noexcept;

// ---- EntityItem / EntityTNTPrimed ---------------------------------------------------------------------------

float item_entity_hover_start(events::JniContext* jni, jobject item_entity) noexcept;
int item_entity_age(events::JniContext* jni, jobject item_entity) noexcept;
int tnt_fuse(events::JniContext* jni, jobject tnt) noexcept;

// ---- ItemStack / Item ---------------------------------------------------------------------------------------

int stack_metadata(events::JniContext* jni, jobject stack) noexcept;                  // getMetadata()
int stack_item_damage(events::JniContext* jni, jobject stack) noexcept;               // getItemDamage()
int stack_max_damage(events::JniContext* jni, jobject stack) noexcept;                // getMaxDamage()
std::string stack_display_name(events::JniContext* jni, jobject stack);               // getDisplayName()
std::string stack_rarity_color(events::JniContext* jni, jobject stack);               // getRarity().rarityColor ("§f")
bool stack_has_display_tag(events::JniContext* jni, jobject stack) noexcept;          // tag != null && hasKey("display", 10)
std::string stack_custom_name(events::JniContext* jni, jobject stack, bool* found);   // tag.display.Name (type 8)
bool item_has_subtypes(events::JniContext* jni, jobject item) noexcept;
std::string item_stack_display_name(events::JniContext* jni, jobject item, jobject stack); // Item.getItemStackDisplayName
int item_tool_material_ordinal(events::JniContext* jni, jobject tool_item) noexcept;  // ItemTool.getToolMaterial().ordinal()

// ---- Rendering ----------------------------------------------------------------------------------------------

math::Vec3d render_manager_viewer_pos(events::JniContext* jni, jobject render_manager) noexcept;
jobject renderer_main_model(events::JniContext* jni, jobject living_renderer) noexcept; // RendererLivingEntity.getMainModel()

// TextureManager.bindTexture(GuiContainer.inventoryBackground) (the potion icon sheet). False if unresolved.
bool bind_inventory_background(events::JniContext* jni, jobject texture_manager) noexcept;

} // namespace slinky::modules::adjust_render
