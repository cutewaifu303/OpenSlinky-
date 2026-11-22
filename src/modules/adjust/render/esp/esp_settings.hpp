#pragma once

// Settings of the Adjust ESP port (com.adjustclient.module.impl.render.esp.settings.*).
//
// Adjust builds every target group ("Players", "Friendly", "Hostile", "Passive", "Items", "TNT") from a class
// hierarchy: EspTargetSettings -> EntityEspSettings -> BorderedEspSettings -> LivingEspSettings -> MobEspSettings
// -> PlayerEspSettings, plus ItemEspSettings / TntEspSettings. Here one Group struct holds the union of all those
// settings and `kinds` says which layers a group has. Names, defaults and ranges are Adjust's.
//
// Not ported (no OpenSlinky++ equivalent): the "Enemy" group (Adjust TARGET player tag), the nametag "Bind"
// keybind (nametags are always shown), "Adjust name" (Adjust user backend), the "Hacking" icon (Adjust hacker
// detector), the "Outline" shader, the "Validate" item check (Adjust inventory manager), the "Preview" widget and
// the Targeted / Hacker / Murderer / Detective / Auto block / Talking player flags.

#include <cstdint>
#include <string>
#include <vector>

#include "adjust/settings.hpp"
#include "config/settings.hpp"

namespace slinky::gui {
struct ClickGui;
struct GuiRenderer;
}

namespace slinky::modules::adjust_esp {

enum GroupKind : uint32_t {
    kKindEntity = 1u << 0,   // EntityEspSettings
    kKindBordered = 1u << 1, // BorderedEspSettings
    kKindLiving = 1u << 2,   // LivingEspSettings
    kKindMob = 1u << 3,      // MobEspSettings
    kKindPlayer = 1u << 4,   // PlayerEspSettings
    kKindItem = 1u << 5,     // ItemEspSettings
    kKindTnt = 1u << 6,      // TntEspSettings
};

enum class Theme : int32_t { Minecraft = 0, Pixel = 1, Adjust = 2 };        // EspTheme
enum class RectLayout : int32_t { Together = 0, Separated = 1 };            // EspRectangleLayout
enum class BorderMode : int32_t { Disabled = 0, Cornered = 1, Full = 2 };   // EspBorderMode
enum class BarPosition : int32_t { Bottom = 0, Right = 1 };                 // EspBarPosition
enum class ItemFilter : int32_t { Blacklist = 0, Whitelist = 1 };           // EspItemFilter

// EspPlayerFlag entries that OpenSlinky++ can evaluate.
enum class PlayerFlag : int32_t { Whitelisted = 0, CreativeMode, Blocking, Sneaking, Count };

struct FlagGroup {                 // EspPlayerFlagGroup
    const char* label = nullptr;   // "FRD", "GMC", ...
    config::BoolSetting enabled;
    adjust::ColorOption color;
    std::string color_label;

    FlagGroup(const char* name, const char* tag, adjust::Argb color_default, bool enabled_default) noexcept;
};

struct ItemIdList {                // ItemsSetting value (sorted item IDs)
    std::vector<int32_t> ids;
    bool contains(int32_t id) const noexcept;
};

struct Group {
    const char* name = nullptr;
    std::string key_prefix;
    uint32_t kinds = 0;

    // EspTargetSettings
    config::BoolSetting enabled;
    config::FloatSetting max_alpha;     // Alpha > Max. alpha (70..255)
    config::BoolSetting use_fov;        // Alpha > Use FOV
    config::BoolSetting background;     // Information > Background
    adjust::ColorOption nametag_color;  // Nametags > Color
    config::FloatSetting nametag_scale; // Nametags > Scale (1..4)
    config::BoolSetting distance;       // Nametags > Distance
    // EntityEspSettings
    config::BoolSetting dormant;
    config::BoolSetting invisible;
    config::BoolSetting nametags_enabled;
    config::ModeSetting rectangles;
    config::BoolSetting bold_text;
    // BorderedEspSettings
    config::ModeSetting border_mode;
    adjust::ColorOption border_color;
    config::BoolSetting border_team_color; // player
    // LivingEspSettings
    config::BoolSetting custom_name;
    config::BoolSetting nametag_health_enabled;
    config::BoolSetting nametag_health_colored;
    config::BoolSetting health_enabled;
    config::BoolSetting health_thinner;
    config::BoolSetting health_custom_color;
    adjust::ColorOption health_color;
    // MobEspSettings
    config::BoolSetting armor_enabled;
    config::BoolSetting armor_thinner;
    config::ModeSetting armor_position;
    adjust::ColorOption armor_color;
    config::BoolSetting equipment;
    config::BoolSetting potions;
    config::BoolSetting fov_alpha;
    config::BoolSetting item_name_enabled;
    config::BoolSetting item_name_custom_only;
    config::FloatSetting item_name_scale;
    // PlayerEspSettings
    config::BoolSetting team_color;
    config::BoolSetting flags_enabled;
    FlagGroup flags[static_cast<int>(PlayerFlag::Count)];
    // ItemEspSettings
    config::ModeSetting filter;
    ItemIdList selected;
    config::DynamicListSetting selected_items;
    config::BoolSetting durability_enabled;
    config::BoolSetting durability_thinner;
    adjust::ColorOption durability_color;
    // TntEspSettings
    config::BoolSetting countdown_enabled;
    config::BoolSetting countdown_thinner;
    config::ModeSetting countdown_position;
    adjust::ColorOption countdown_color;

    // Section header labels ("Players - Nametags", ...)
    std::string h_alpha, h_nametags, h_item_name, h_border, h_health, h_armor, h_icons, h_flags, h_durability,
        h_countdown;

    Group(const char* group_name, const char* prefix, uint32_t group_kinds, bool enabled_default,
          bool dormant_default) noexcept;

    bool has(uint32_t kind) const noexcept { return (kinds & kind) != 0; }

    float nametag_scale_value() const noexcept { return nametag_scale.value; } // getNametagScale()
    bool accepts_item(int32_t item_id) const noexcept;                      // ItemEspSettings.acceptsItem

    void render(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
    void visit(config::ConfigArchive* ar) noexcept;
};

struct Settings {
    config::ModeSetting theme; // "Theme": Minecraft / Pixel / Adjust
    Group players;
    Group friendly;
    Group hostile;
    Group passive;
    Group items;
    Group tnt;

    Settings() noexcept;

    Theme theme_value() const noexcept { return static_cast<Theme>(theme.value); }
    void render(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
    void visit(config::ConfigArchive* ar) noexcept;
};

Settings& settings() noexcept;

} // namespace slinky::modules::adjust_esp
