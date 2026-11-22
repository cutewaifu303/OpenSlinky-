#include "modules/adjust/render/esp/esp_settings.hpp"

#include <algorithm>
#include <cstring>

#include "gui/widgets.hpp"
#include "minecraft/item_database.hpp"

namespace slinky::modules::adjust_esp {

namespace {

constexpr const char* kThemeOptions[3] = {"Minecraft", "Pixel", "Adjust"};
constexpr const char* kRectangleOptions[2] = {"Together", "Separated"};
constexpr const char* kBorderOptions[3] = {"Disabled", "Cornered", "Full"};
constexpr const char* kBarPositionOptions[2] = {"Bottom", "Right"};
constexpr const char* kFilterOptions[2] = {"Blacklist", "Whitelist"};

constexpr adjust::Argb kNametagColor = adjust::argb(233, 235, 234);

config::BoolSetting make_bool(const char* label, bool value) noexcept {
    return config::BoolSetting{{}, label, value, value, {}};
}

config::FloatSetting make_float(const char* label, const char* format, float step, float min_value, float max_value,
                                float value) noexcept {
    return config::FloatSetting{{}, label, format, step, 0.0f, min_value, max_value, value, value};
}

config::ModeSetting make_mode(const char* label, const char* const* options, uint32_t count, int32_t value) noexcept {
    return config::ModeSetting{{}, label, count, 0, options, value, value, false, {}};
}

// ---- "Selected items" list (Adjust ItemsSetting) ----------------------------------------------------------------

size_t validate_item_char(char* buffer, char ch) noexcept {
    const size_t len = std::strlen(buffer);
    if (len > 99) {
        return len;
    }
    const auto upper = static_cast<uint8_t>((static_cast<uint8_t>(ch) & 0xDFu) - 'A');
    const auto digit = static_cast<uint8_t>(static_cast<uint8_t>(ch) - '0');
    if (upper < 26u || digit < 10u || ch == ' ') {
        buffer[len] = ch;
    }
    return len;
}

size_t list_count(void* ctx) noexcept {
    return static_cast<const ItemIdList*>(ctx)->ids.size();
}

const char* list_name(void* ctx, size_t index) noexcept {
    const char* name = minecraft::find_item_name_by_id(static_cast<const ItemIdList*>(ctx)->ids[index]);
    return name != nullptr ? name : "?";
}

math::Color3f* list_color(void* /*ctx*/, size_t /*index*/) noexcept {
    static math::Color3f s_white{1.0f, 1.0f, 1.0f};
    return &s_white;
}

bool list_add(void* ctx, const char* name, const math::Color3f* /*color*/) noexcept {
    auto* list = static_cast<ItemIdList*>(ctx);
    const int32_t id = minecraft::find_item_id_by_name(name);
    if (id == -1 || list->contains(id)) {
        return false;
    }
    list->ids.insert(std::upper_bound(list->ids.begin(), list->ids.end(), id), id);
    return true;
}

void list_remove(void* ctx, size_t index) noexcept {
    auto* list = static_cast<ItemIdList*>(ctx);
    if (index < list->ids.size()) {
        list->ids.erase(list->ids.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void list_clear(void* ctx) noexcept {
    static_cast<ItemIdList*>(ctx)->ids.clear();
}

void list_options(const char* const** out_items, size_t* out_count) noexcept {
    minecraft::get_all_item_names(out_items, out_count);
}

std::string join(const char* a, const char* b) {
    return std::string(a) + " - " + b;
}

void header(gui::ClickGui* gui, gui::GuiRenderer* renderer, const std::string& title) noexcept {
    gui::widgets::render_section_header(gui, renderer, title.c_str());
}

} // namespace

// ---- FlagGroup / ItemIdList ---------------------------------------------------------------------------------------

FlagGroup::FlagGroup(const char* name, const char* tag, adjust::Argb color_default, bool enabled_default) noexcept
    : label(tag),
      enabled(make_bool(name, enabled_default)),
      color("Color", "Color mode", color_default),
      color_label(std::string(name) + " color") {
    color.color.label = color_label.c_str();
}

bool ItemIdList::contains(int32_t id) const noexcept {
    return std::binary_search(ids.begin(), ids.end(), id);
}

// ---- Group --------------------------------------------------------------------------------------------------------

Group::Group(const char* group_name, const char* prefix, uint32_t group_kinds, bool enabled_default,
             bool dormant_default) noexcept
    : name(group_name),
      key_prefix(prefix),
      kinds(group_kinds),
      enabled(make_bool("Enabled", enabled_default)),
      max_alpha(make_float("Max. alpha", "%0.0f", 1.0f, 70.0f, 255.0f, 255.0f)),
      use_fov(make_bool("Use FOV", false)),
      background(make_bool("Background", true)),
      nametag_color("Color", "Color mode", kNametagColor),
      nametag_scale(make_float("Scale", "%0.2f", 0.05f, 1.0f, 4.0f, 1.0f)),
      distance(make_bool("Distance", false)),
      dormant(make_bool("Dormant", dormant_default)),
      invisible(make_bool("Invisible", true)),
      nametags_enabled(make_bool("Enabled", true)),
      rectangles(make_mode("Rectangles", kRectangleOptions, 2, static_cast<int32_t>(RectLayout::Together))),
      bold_text(make_bool("Bold text", false)),
      border_mode(make_mode("Mode", kBorderOptions, 3, static_cast<int32_t>(BorderMode::Disabled))),
      border_color("Color", "Color mode", adjust::argb(170, 170, 170)),
      border_team_color(make_bool("Team color", false)),
      custom_name(make_bool("Custom name", true)),
      nametag_health_enabled(make_bool("Health", true)),
      nametag_health_colored(make_bool("Health colored", false)),
      health_enabled(make_bool("Enabled", true)),
      health_thinner(make_bool("Thinner", false)),
      health_custom_color(make_bool("Custom color", true)),
      health_color("Color", "Color mode", adjust::argb(67, 90, 209)),
      armor_enabled(make_bool("Enabled", true)),
      armor_thinner(make_bool("Thinner", false)),
      armor_position(make_mode("Position", kBarPositionOptions, 2, static_cast<int32_t>(BarPosition::Bottom))),
      armor_color("Color", "Color mode", adjust::argb(77, 88, 199)),
      equipment(make_bool("Equipment", true)),
      potions(make_bool("Potions", true)),
      fov_alpha(make_bool("FOV alpha", true)),
      item_name_enabled(make_bool("Enabled", true)),
      item_name_custom_only(make_bool("Custom names only", false)),
      item_name_scale(make_float("Scale", "%0.2f", 0.05f, 1.0f, 1.5f, 1.0f)),
      team_color(make_bool("Team color", true)),
      flags_enabled(make_bool("Enabled", true)),
      flags{
          FlagGroup("Whitelisted", "FRD", adjust::argb(70, 70, 200), true),
          FlagGroup("Creative Mode", "GMC", adjust::argb(149, 59, 190), true),
          FlagGroup("Blocking", "BLK", adjust::argb(233, 235, 234), false),
          FlagGroup("Sneaking", "SNK", adjust::argb(233, 235, 234), false),
      },
      filter(make_mode("Filter", kFilterOptions, 2, static_cast<int32_t>(ItemFilter::Blacklist))),
      selected(),
      selected_items{"Selected items", "Search item", &validate_item_char, config::DynamicListItemKind::StringOnly, 0,
                     &selected, &list_count, &list_name, &list_color, &list_add, &list_remove, &list_clear,
                     &list_options, false, {}},
      durability_enabled(make_bool("Enabled", true)),
      durability_thinner(make_bool("Thinner", false)),
      durability_color("Color", "Color mode", adjust::argb(77, 88, 199)),
      countdown_enabled(make_bool("Enabled", true)),
      countdown_thinner(make_bool("Thinner", false)),
      countdown_position(make_mode("Position", kBarPositionOptions, 2, static_cast<int32_t>(BarPosition::Bottom))),
      countdown_color("Color", "Color mode", adjust::argb(199, 88, 77)),
      h_alpha(join(group_name, "Alpha")),
      h_nametags(join(group_name, "Nametags")),
      h_item_name(join(group_name, "Item name")),
      h_border(join(group_name, "Border")),
      h_health(join(group_name, "Health")),
      h_armor(join(group_name, "Armor")),
      h_icons(join(group_name, "Icons")),
      h_flags(join(group_name, "Player flags")),
      h_durability(join(group_name, "Durability")),
      h_countdown(join(group_name, "Countdown")) {}

bool Group::accepts_item(int32_t item_id) const noexcept {
    return selected.contains(item_id) == (static_cast<ItemFilter>(filter.value) == ItemFilter::Whitelist);
}

void Group::render(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, name);
    enabled.render(gui, renderer);
    if (!enabled.value) {
        return;
    }
    if (has(kKindItem)) {
        filter.render(gui, renderer);
        selected_items.render(gui, renderer);
    }
    dormant.render(gui, renderer);
    invisible.render(gui, renderer);

    header(gui, renderer, h_alpha);
    max_alpha.render(gui, renderer);
    use_fov.render(gui, renderer);

    // Information > Nametags
    header(gui, renderer, h_nametags);
    nametags_enabled.render(gui, renderer);
    if (nametags_enabled.value) {
        const bool color_hidden = has(kKindItem) || (has(kKindPlayer) && team_color.value);
        if (!color_hidden) {
            nametag_color.render(gui, renderer);
        }
        nametag_scale.render(gui, renderer);
        distance.render(gui, renderer);
        if (background.value) {
            rectangles.render(gui, renderer);
        }
        bold_text.render(gui, renderer);
        if (has(kKindLiving)) {
            custom_name.render(gui, renderer);
            nametag_health_enabled.render(gui, renderer);
            if (nametag_health_enabled.value) {
                nametag_health_colored.render(gui, renderer);
            }
        }
        if (has(kKindPlayer) && custom_name.value) {
            team_color.render(gui, renderer);
        }
    }
    // Information > Item name
    if (has(kKindMob)) {
        header(gui, renderer, h_item_name);
        item_name_enabled.render(gui, renderer);
        if (item_name_enabled.value) {
            item_name_custom_only.render(gui, renderer);
            item_name_scale.render(gui, renderer);
        }
    }
    // Information > Background
    background.render(gui, renderer);

    if (has(kKindBordered)) {
        header(gui, renderer, h_border);
        border_mode.render(gui, renderer);
        if (static_cast<BorderMode>(border_mode.value) != BorderMode::Disabled) {
            if (!(has(kKindPlayer) && border_team_color.value)) {
                border_color.render(gui, renderer);
            }
            if (has(kKindPlayer)) {
                border_team_color.render(gui, renderer);
            }
        }
    }
    if (has(kKindLiving)) {
        header(gui, renderer, h_health);
        health_enabled.render(gui, renderer);
        if (health_enabled.value) {
            health_thinner.render(gui, renderer);
            health_custom_color.render(gui, renderer);
            if (health_custom_color.value) {
                health_color.render(gui, renderer);
            }
        }
    }
    if (has(kKindMob)) {
        header(gui, renderer, h_armor);
        armor_enabled.render(gui, renderer);
        if (armor_enabled.value) {
            armor_thinner.render(gui, renderer);
            armor_position.render(gui, renderer);
            armor_color.render(gui, renderer);
        }
        header(gui, renderer, h_icons);
        equipment.render(gui, renderer);
        potions.render(gui, renderer);
        fov_alpha.render(gui, renderer);
    }
    if (has(kKindPlayer)) {
        header(gui, renderer, h_flags);
        flags_enabled.render(gui, renderer);
        if (flags_enabled.value) {
            for (FlagGroup& flag : flags) {
                flag.enabled.render(gui, renderer);
                if (flag.enabled.value) {
                    flag.color.render(gui, renderer);
                }
            }
        }
    }
    if (has(kKindItem)) {
        header(gui, renderer, h_durability);
        durability_enabled.render(gui, renderer);
        if (durability_enabled.value) {
            durability_thinner.render(gui, renderer);
            durability_color.render(gui, renderer);
        }
    }
    if (has(kKindTnt)) {
        header(gui, renderer, h_countdown);
        countdown_enabled.render(gui, renderer);
        if (countdown_enabled.value) {
            countdown_thinner.render(gui, renderer);
            countdown_position.render(gui, renderer);
            countdown_color.render(gui, renderer);
        }
    }
}

void Group::visit(config::ConfigArchive* ar) noexcept {
    const auto key = [this](const char* suffix) { return key_prefix + "_" + suffix; };
    const auto visit_bool = [&](config::BoolSetting& s, const char* suffix) { s.visit(ar, key(suffix).c_str()); };
    const auto visit_float = [&](config::FloatSetting& s, const char* suffix) { s.visit(ar, key(suffix).c_str()); };
    const auto visit_mode = [&](config::ModeSetting& s, const char* suffix) { s.visit(ar, key(suffix).c_str()); };
    const auto visit_color = [&](adjust::ColorOption& s, const char* suffix) {
        const std::string base = key(suffix);
        s.visit(ar, base.c_str(), (base + "_mode").c_str());
    };

    visit_bool(enabled, "enabled");
    visit_float(max_alpha, "max_alpha");
    visit_bool(use_fov, "use_fov");
    visit_bool(background, "background");
    visit_color(nametag_color, "nametag_color");
    visit_float(nametag_scale, "nametag_scale");
    visit_bool(distance, "distance");
    visit_bool(dormant, "dormant");
    visit_bool(invisible, "invisible");
    visit_bool(nametags_enabled, "nametags");
    visit_mode(rectangles, "rectangles");
    visit_bool(bold_text, "bold_text");
    if (has(kKindBordered)) {
        visit_mode(border_mode, "border_mode");
        visit_color(border_color, "border_color");
        if (has(kKindPlayer)) {
            visit_bool(border_team_color, "border_team_color");
        }
    }
    if (has(kKindLiving)) {
        visit_bool(custom_name, "custom_name");
        visit_bool(nametag_health_enabled, "nametag_health");
        visit_bool(nametag_health_colored, "nametag_health_colored");
        visit_bool(health_enabled, "health");
        visit_bool(health_thinner, "health_thinner");
        visit_bool(health_custom_color, "health_custom_color");
        visit_color(health_color, "health_color");
    }
    if (has(kKindMob)) {
        visit_bool(armor_enabled, "armor");
        visit_bool(armor_thinner, "armor_thinner");
        visit_mode(armor_position, "armor_position");
        visit_color(armor_color, "armor_color");
        visit_bool(equipment, "equipment");
        visit_bool(potions, "potions");
        visit_bool(fov_alpha, "fov_alpha");
        visit_bool(item_name_enabled, "item_name");
        visit_bool(item_name_custom_only, "item_name_custom_only");
        visit_float(item_name_scale, "item_name_scale");
    }
    if (has(kKindPlayer)) {
        visit_bool(team_color, "team_color");
        visit_bool(flags_enabled, "flags");
        static constexpr const char* kFlagKeys[static_cast<int>(PlayerFlag::Count)] = {
            "flag_whitelisted", "flag_creative", "flag_blocking", "flag_sneaking"};
        for (int i = 0; i < static_cast<int>(PlayerFlag::Count); ++i) {
            visit_bool(flags[i].enabled, kFlagKeys[i]);
            visit_color(flags[i].color, (std::string(kFlagKeys[i]) + "_color").c_str());
        }
    }
    if (has(kKindItem)) {
        visit_mode(filter, "filter");
        selected_items.visit(ar, key("selected_items").c_str());
        visit_bool(durability_enabled, "durability");
        visit_bool(durability_thinner, "durability_thinner");
        visit_color(durability_color, "durability_color");
    }
    if (has(kKindTnt)) {
        visit_bool(countdown_enabled, "countdown");
        visit_bool(countdown_thinner, "countdown_thinner");
        visit_mode(countdown_position, "countdown_position");
        visit_color(countdown_color, "countdown_color");
    }
}

// ---- Settings -----------------------------------------------------------------------------------------------------

namespace {
constexpr uint32_t kPlayerKinds = kKindEntity | kKindBordered | kKindLiving | kKindMob | kKindPlayer;
constexpr uint32_t kMobKinds = kKindEntity | kKindBordered | kKindLiving | kKindMob;
constexpr uint32_t kLivingKinds = kKindEntity | kKindBordered | kKindLiving;
} // namespace

Settings::Settings() noexcept
    : theme(make_mode("Theme", kThemeOptions, 3, static_cast<int32_t>(Theme::Minecraft))),
      players("Players", "players", kPlayerKinds, true, true),
      friendly("Friendly", "friendly", kPlayerKinds, true, true),
      hostile("Hostile", "hostile", kMobKinds, false, false),
      passive("Passive", "passive", kLivingKinds, false, false),
      items("Items", "items", kKindEntity | kKindItem, true, true),
      tnt("TNT", "tnt", kKindEntity | kKindBordered | kKindTnt, true, false) {}

void Settings::render(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    theme.render(gui, renderer);
    players.render(gui, renderer);
    friendly.render(gui, renderer);
    hostile.render(gui, renderer);
    passive.render(gui, renderer);
    items.render(gui, renderer);
    tnt.render(gui, renderer);
}

void Settings::visit(config::ConfigArchive* ar) noexcept {
    theme.visit(ar, "theme");
    players.visit(ar);
    friendly.visit(ar);
    hostile.visit(ar);
    passive.visit(ar);
    items.visit(ar);
    tnt.visit(ar);
}

Settings& settings() noexcept {
    static Settings s_settings;
    return s_settings;
}

} // namespace slinky::modules::adjust_esp
