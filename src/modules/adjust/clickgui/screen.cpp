// Adjust ClickGUI port - ClickGuiScreen, SearchBar, CategoryTab, ConfigurationTab (+ frame.ConfigurationEntry,
// Delete/Rename/Share buttons, LastUsedTooltip) and the public adjust_clickgui API.
//
// Configuration tab -> OpenSlinky++ profiles (%USERPROFILE%\.slinky\profiles): a "Default" row (reset to defaults)
// followed by the profile files. Left click = load (with Adjust's unsaved-changes dialog), middle click = profile
// keybind, Delete / Rename buttons as in Adjust; the Share button (Adjust cloud) saves the current settings into the
// profile instead (OpenSlinky++ profiles are saved explicitly). The "+" header button creates a profile.
// Skipped: Adjust cloud / global configurations (the header's "f" button), the home button (Adjust home screen).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "config/profile_manager.hpp"
#include "events/events.hpp"
#include "events/module_manager.hpp"
#include "gui/gui_internal.hpp"
#include "gui/gui_renderer.hpp"
#include "modules/adjust/clickgui/adjust_click_gui.hpp"
#include "modules/adjust/clickgui/internal.hpp"
#include "modules/altmanager/settings_popup.hpp"
#include "modules/module_interop.hpp"
#include "util/filesystem.hpp"
#include "util/keycodes.hpp"

namespace slinky::modules::adjust_clickgui::detail {

namespace {

std::vector<std::unique_ptr<Dialog>>& dialogs() {
    static std::vector<std::unique_ptr<Dialog>> list;
    return list;
}

void dispatch_error(int32_t code, const char* message = nullptr) noexcept {
    events::ModuleManager* mgr = env().modules;
    if (mgr == nullptr) {
        return;
    }
    events::ErrorNotificationEvent ev{};
    ev.id = events::kEventErrorNotification;
    ev.error_code = code;
    ev.custom_message = message;
    mgr->dispatch(&ev);
}

void dispatch_profile_action(const char* name, int32_t action) noexcept {
    events::ModuleManager* mgr = env().modules;
    if (mgr == nullptr) {
        return;
    }
    events::ProfileActionEvent ev{};
    ev.id = events::kEventProfileAction;
    ev.profile_name = name;
    ev.action = action;
    mgr->dispatch(&ev);
}

bool is_separator_row(const Component* c) noexcept {
    const RowKind k = c->kind();
    return k == RowKind::Expand || k == RowKind::StringSetting || k == RowKind::Color;
}

} // namespace

void show_dialog(std::unique_ptr<Dialog> dialog, float width) {
    if (!dialog) {
        return;
    }
    dialog->minimum_width = width;
    dialogs().push_back(std::move(dialog));
}

// =============================================================================================================
// CategoryTab
// =============================================================================================================

CategoryTab::CategoryTab(Screen* s, events::ModuleCategory category, const char* name)
    : category_(category), name_(name) {
    screen = s;
    width = 66.0f;
    title = "";
    init_header(0.0f);
    entries.push_back(state);
}

void CategoryTab::sync() {
    events::ModuleManager* mgr = env().modules;
    if (mgr == nullptr) {
        return;
    }
    const auto cat = static_cast<size_t>(category_);
    if (cat >= static_cast<size_t>(events::ModuleCategory::Count)) {
        return;
    }
    const events::ModuleSpan& span = mgr->categories[cat];
    const events::VisualStyle style = events::get_requested_visual_style();
    ++sync_frame_;
    std::vector<ModuleEntry*> desired;
    bool changed = false;
    for (size_t i = 0; i < span.count; ++i) {
        events::Module* mod = span.data != nullptr ? span.data[i] : nullptr;
        if (mod == nullptr || !events::module_matches_style(mod, style)) {
            continue;
        }
        auto& slot = modules_[mod];
        if (!slot) {
            slot = std::make_unique<ModuleEntry>(mod);
            changed = true;
        }
        // re-capture the settings every frame while they are listed (or searched), else every 15th frame
        const bool listed = slot->expanded || (screen != nullptr && screen->search_bar.is_searching());
        if ((listed || !slot->captured() || (sync_frame_ + i) % 15 == 0) && slot->sync()) {
            changed = true;
        }
        desired.push_back(slot.get());
    }
    if (!changed && desired == listed_) {
        return;
    }
    listed_ = desired;
    entries.clear();
    entries.push_back(state);
    for (ModuleEntry* m : listed_) {
        entries.push_back(m);
        m->append_rows(entries);
    }
    for (Component* c : entries) {
        c->owner = this;
        c->width = width;
    }
    if (screen != nullptr) {
        screen->layout_dirty = true;
    }
}

// =============================================================================================================
// ConfigurationTab
// =============================================================================================================

namespace {

// ConfigurationTab.validName (Adjust charset minus the characters Windows file names cannot contain)
bool valid_profile_name(const std::string& name) {
    if (name.empty() || name.size() > 16) {
        return false;
    }
    static constexpr std::string_view kExtra = " !@#$%^&~_-=+,.';";
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) != 0 || kExtra.find(c) != std::string_view::npos)) {
            return false;
        }
    }
    return name.front() != ' ' && name.back() != ' ' && name.back() != '.';
}

bool profile_loaded(const std::string& name, bool is_default) {
    const char* active = config::get_active_profile_name();
    if (is_default) {
        return active == nullptr;
    }
    return active != nullptr && name == active;
}

std::string active_profile_label() {
    const char* active = config::get_active_profile_name();
    return active != nullptr ? active : "Default";
}

bool load_profile(const std::string& name, bool is_default) {
    events::ModuleManager* mgr = env().modules;
    if (mgr == nullptr) {
        return false;
    }
    const char* pname = is_default ? nullptr : name.c_str();
    const int err = config::load_or_reset_profile(pname, mgr);
    if (err == 0) {
        dispatch_profile_action(pname, 0);
        return true;
    }
    dispatch_error(err);
    return false;
}

bool save_profile_as(const std::string& name, bool overwrite) {
    events::ModuleManager* mgr = env().modules;
    if (mgr == nullptr) {
        return false;
    }
    const int err = config::save_profile(name.c_str(), overwrite, mgr);
    if (err != 0) {
        dispatch_error(err, err == 1012 ? "Profile name is taken." : nullptr);
        return false;
    }
    return true;
}

int rename_profile_file(const std::string& from, const std::string& to) {
    fs::WidePath a{};
    fs::WidePath b{};
    int err = fs::init_user_profile_path(&a);
    if (err == 0) err = fs::append_path(&a, ".slinky");
    if (err == 0) err = fs::append_path(&a, "profiles");
    if (err == 0) err = fs::init_user_profile_path(&b);
    if (err == 0) err = fs::append_path(&b, ".slinky");
    if (err == 0) err = fs::append_path(&b, "profiles");
    if (err == 0) err = fs::append_path(&a, from.c_str());
    if (err == 0) err = fs::append_path(&b, to.c_str());
    if (err == 0 && MoveFileExW(a.ptr, b.ptr, 0) == 0) {
        err = 1;
    }
    fs::free_path(&a);
    fs::free_path(&b);
    return err;
}

class ConfigurationEntry;

// frame.LastUsedTooltip: "Last used ..." -> the profile's modification label
class LastUsedTooltip final : public Tooltip {
public:
    LastUsedTooltip(const Component* owner, std::string name, bool is_default)
        : Tooltip(owner, 250, std::string(), std::string(), true, hovered_of(owner)), name_(std::move(name)),
          is_default_(is_default) {
        text_ = " "; // non-empty: Tooltip skips rows with an empty text field
    }

    std::string get_text() override {
        if (is_default_) {
            return ACG_ITALIC "Contains the default configuration.";
        }
        config::ProfileRegistry* reg = env().profiles;
        const config::ProfileEntry* entry = reg != nullptr ? reg->find_by_name(name_.c_str()) : nullptr;
        if (entry == nullptr || entry->age_seconds < 0) {
            return "Never used";
        }
        return std::string(ACG_ITALIC) + entry->modified_label;
    }
    bool is_last_used() const override { return true; }

private:
    std::string name_;
    bool is_default_;
};

class ConfigButton : public HeaderButton {
public:
    ConfigButton(HoverEffect* hover, ConfigurationEntry* owner) : hover_(hover), owner_(owner) {
        width = 9.5f;
        height = 11.0f;
        draw_immediately = true;
    }
    bool is_shown() override;

protected:
    HoverEffect* hover_;
    ConfigurationEntry* owner_;
};

class ConfigurationEntry final : public Component {
public:
    ConfigurationEntry(std::string name, bool is_default);

    RowKind kind() const noexcept override { return RowKind::Configuration; }
    const std::string& name() const noexcept { return name_; }
    bool is_default() const noexcept { return is_default_; }

    void render_content(DrawQueue&, Vec2) override {
        adjust::kTahoma12.draw(name_, x + 2.0f, y + 4.0f, label_hover_->color(190));
    }

    bool search_text(std::string& out) const override {
        out = name_;
        return true;
    }

    float get_height() override { return 11.0f; }

    bool on_mouse_pressed(Vec2, int button, bool pressed) override {
        if (button == 0 && pressed) {
            if (!config::is_profile_dirty()) {
                if (!profile_loaded(name_, is_default_)) {
                    load_profile(name_, is_default_);
                }
                return true;
            }
            auto dialog = std::make_unique<Dialog>("Are you sure?");
            dialog->message.append("You have unsaved changes to ");
            dialog->message.italic = true;
            dialog->message.append(active_profile_label());
            dialog->message.append(" and loading a different configuration will overwrite your current changes.");
            const std::string name = name_;
            const bool def = is_default_;
            if (config::get_active_profile_name() != nullptr) {
                dialog->add_button("Save", [name, def](Dialog&) {
                    const char* active = config::get_active_profile_name();
                    if (active != nullptr) {
                        const std::string current = active;
                        if (save_profile_as(current, true)) {
                            dispatch_profile_action(current.c_str(), 1);
                        }
                    }
                    load_profile(name, def);
                });
            }
            dialog->add_button("Discard", [name, def](Dialog&) { load_profile(name, def); });
            dialog->add_button("Cancel", nullptr);
            show_dialog(std::move(dialog));
            return true;
        }
        if (button != 2 || !pressed || is_default_) {
            return false;
        }
        config::ProfileRegistry* reg = env().profiles;
        config::ProfileEntry* entry = reg != nullptr ? reg->find_by_name(name_.c_str()) : nullptr;
        if (entry == nullptr) {
            return false;
        }
        const std::string name = name_;
        screen().add_overlay(std::make_unique<BindOverlay>(
            name_,
            [name](int key) {
                config::ProfileRegistry* r = env().profiles;
                config::ProfileEntry* e = r != nullptr ? r->find_by_name(name.c_str()) : nullptr;
                if (e == nullptr) {
                    return;
                }
                const auto next = static_cast<uint32_t>(e->keybind == static_cast<uint32_t>(key) ? 0 : key);
                if (r->bind_callback() != nullptr) {
                    r->bind_callback()(e, next);
                } else {
                    e->keybind = next;
                }
            },
            static_cast<int>(entry->keybind)));
        return false;
    }

private:
    std::string name_;
    bool is_default_;
    HoverEffect* label_hover_ = nullptr;
};

bool ConfigButton::is_shown() {
    return !owner_->is_default() && owner_->is_hovered();
}

void force_profile_refresh();

// frame.DeleteConfigButton
class DeleteConfigButton final : public ConfigButton {
public:
    using ConfigButton::ConfigButton;

    bool on_click() override {
        const std::string name = owner_->name();
        if (profile_loaded(name, false)) {
            return true; // the loaded configuration cannot be deleted
        }
        if (shift_down()) {
            delete_now(name);
            return true;
        }
        auto dialog = std::make_unique<Dialog>("Delete configuration file '" + name + "'?");
        dialog->message.append("Are you sure you want to delete ");
        dialog->message.italic = true;
        dialog->message.append(name);
        dialog->message.append("? This action cannot be undone.");
        dialog->add_button("Delete", [name](Dialog&) { delete_now(name); });
        dialog->add_button("Cancel", nullptr);
        show_dialog(std::move(dialog), 70.0f);
        return true;
    }

    void render() override {
        adjust::kIcons14.draw_no_shadow("c", x + 1.75f, y + 4.0f, hover_->tint(shift_down() ? 255 : 155));
    }

private:
    static void delete_now(const std::string& name) {
        const int err = config::delete_profile_file(name.c_str());
        if (err != 0) {
            dispatch_error(err);
        } else {
            dispatch_profile_action(name.c_str(), 3);
        }
        force_profile_refresh();
    }
};

// frame.RenameConfigButton
class RenameConfigButton final : public ConfigButton {
public:
    using ConfigButton::ConfigButton;

    bool on_click() override {
        const std::string name = owner_->name();
        auto dialog = std::make_unique<Dialog>("Rename configuration file '" + name + "'");
        dialog->message.append("Enter a new name for ");
        dialog->message.italic = true;
        dialog->message.append(name);
        dialog->message.append(".");
        dialog->add_text_input("Name", "", 16, 128);
        dialog->add_button("Rename", [name](Dialog& d) { rename(name, d.input_text(0)); },
                           [](Dialog& d) { return valid_profile_name(d.input_text(0)); });
        dialog->add_button("Cancel", nullptr);
        show_dialog(std::move(dialog), 133.0f);
        return true;
    }

    void render() override { adjust::kIcons14.draw_no_shadow("b", x + 2.0f, y + 4.0f, hover_->color(190)); }

private:
    static void rename(const std::string& from, const std::string& to) {
        config::ProfileRegistry* reg = env().profiles;
        uint32_t bind = 0;
        if (reg != nullptr) {
            if (const config::ProfileEntry* e = reg->find_by_name(from.c_str())) {
                bind = e->keybind;
            }
        }
        const bool was_active = profile_loaded(from, false);
        if (to == from || rename_profile_file(from, to) != 0) {
            notify("Configuration", {{"Failed to rename configuration ", false}, {from, true}, {".", false}});
            return;
        }
        if (was_active) {
            config::set_active_profile_name(to.c_str());
        }
        if (reg != nullptr) {
            reg->refresh_from_disk();
            if (bind != 0) {
                if (config::ProfileEntry* e = reg->find_by_name(to.c_str())) {
                    if (reg->bind_callback() != nullptr) {
                        reg->bind_callback()(e, bind);
                    } else {
                        e->keybind = bind;
                    }
                }
            }
        }
        force_profile_refresh();
    }
};

// frame.ShareConfigButton slot: saves the current settings into the profile (Adjust: share to another user)
class SaveConfigButton final : public ConfigButton {
public:
    using ConfigButton::ConfigButton;

    bool on_click() override {
        const std::string name = owner_->name();
        if (profile_loaded(name, false)) {
            save_now(name);
            return true;
        }
        auto dialog = std::make_unique<Dialog>("Save configuration file '" + name + "'?");
        dialog->message.append("The current settings will overwrite ");
        dialog->message.italic = true;
        dialog->message.append(name);
        dialog->message.append(" if you continue.");
        dialog->add_button("Save", [name](Dialog&) { save_now(name); });
        dialog->add_button("Cancel", nullptr);
        show_dialog(std::move(dialog), 70.0f);
        return true;
    }

    void render() override { adjust::kIcons14.draw_no_shadow("a", x + 1.5f, y + 4.0f, hover_->color(190)); }

private:
    static void save_now(const std::string& name) {
        if (save_profile_as(name, true)) {
            dispatch_profile_action(name.c_str(), 1);
        }
        force_profile_refresh();
    }
};

ConfigurationEntry::ConfigurationEntry(std::string name, bool is_default)
    : name_(std::move(name)), is_default_(is_default) {
    add_modifier(std::make_unique<HoverBackground>(this));
    const std::string n = name_;
    const bool def = is_default_;
    label_hover_ = add_modifier(std::make_unique<HoverEffect>(
        this, [n, def] { return profile_loaded(n, def); }, hovered_of(this), 0.2f, 1.0f));
    add_modifier(std::make_unique<LastUsedTooltip>(this, name_, is_default_));
    HoverEffect* delete_hover = add_modifier(std::make_unique<HoverEffect>(
        this, [] { return false; }, hovered_of(this), 0.05f, 1.0f));
    HoverEffect* rename_hover = add_modifier(std::make_unique<HoverEffect>(
        this, [] { return false; }, hovered_of(this), 0.05f, 1.0f));
    HoverEffect* share_hover = add_modifier(std::make_unique<HoverEffect>(
        this, [] { return false; }, hovered_of(this), 0.05f, 1.0f));
    // left to right in the list = right to left on screen
    std::vector<std::unique_ptr<HeaderButton>> buttons;
    buttons.push_back(std::make_unique<DeleteConfigButton>(delete_hover, this));
    buttons.push_back(std::make_unique<RenameConfigButton>(rename_hover, this));
    buttons.push_back(std::make_unique<SaveConfigButton>(share_hover, this));
    add_modifier(std::make_unique<ButtonBar>(this, std::move(buttons)));
}

class ConfigurationTab final : public Tab {
public:
    explicit ConfigurationTab(Screen* s) {
        screen = s;
        title = "Configuration";
        width = 66.0f;
        init_header(4.0f); // Adjust: 8 (room for two header buttons; the global configurations button is skipped)
        Button plus(0, "+", 6, 6, 26, true, adjust::kTahoma12);
        state->add_modifier(std::make_unique<IconButtonModifier>(state, std::move(plus), open_create_dialog, 0.0f));
        entries.push_back(state);
    }

    void sync() override {
        const int64_t now = adjust::now_ms();
        config::ProfileRegistry* reg = env().profiles;
        if (reg != nullptr && (refresh_requested() || now - last_refresh_ >= 2000)) {
            last_refresh_ = now;
            refresh_requested() = false;
            const int err = reg->refresh_from_disk();
            if (err != 0) {
                dispatch_error(err);
            }
        }
        std::vector<std::string> names;
        if (reg != nullptr) {
            size_t count = 0;
            const config::ProfileEntry* list = reg->get_entries(&count);
            for (size_t i = 0; list != nullptr && i < count; ++i) {
                if (list[i].name != nullptr) {
                    names.emplace_back(list[i].name);
                }
            }
        }
        if (built_ && names == names_) {
            return;
        }
        built_ = true;
        names_ = names;
        entries.clear();
        entries.push_back(state);
        entries.push_back(row(std::string("Default"), true));
        for (const std::string& n : names_) {
            entries.push_back(row(n, false));
        }
        for (Component* c : entries) {
            c->owner = this;
            c->width = width;
        }
        if (screen != nullptr) {
            screen->layout_dirty = true;
        }
    }

    static bool& refresh_requested() {
        static bool flag = true;
        return flag;
    }

private:
    Component* row(const std::string& name, bool is_default) {
        auto& slot = rows_[std::make_pair(name, is_default)];
        if (!slot) {
            slot = std::make_unique<ConfigurationEntry>(name, is_default);
        }
        return slot.get();
    }

    static void open_create_dialog() {
        auto dialog = std::make_unique<Dialog>("Create a new configuration file");
        dialog->message.append("Enter a name for your new configuration:");
        dialog->add_text_input("Name", "", 16, 128);
        const auto valid = [](Dialog& d) { return valid_profile_name(d.input_text(0)); };
        dialog->add_button("Create", [](Dialog& d) { create(d.input_text(0), false); }, valid);
        dialog->add_button("Create Empty", [](Dialog& d) { create(d.input_text(0), true); }, valid);
        dialog->add_button("Cancel", nullptr);
        dialog->bottom_inset = 7;
        show_dialog(std::move(dialog), 133.0f);
    }

    // "Create": a profile with the current settings; "Create Empty": with the default settings (this resets the
    // current settings first - OpenSlinky++ profiles are snapshots of the live settings).
    static void create(const std::string& name, bool empty) {
        events::ModuleManager* mgr = env().modules;
        if (mgr == nullptr) {
            return;
        }
        if (empty) {
            config::load_or_reset_profile(nullptr, mgr);
        }
        if (save_profile_as(name, false)) {
            notify("Configuration", {{"Created configuration ", false}, {name, true}, {".", false}});
        } else {
            notify("Configuration", {{"Could not create configuration ", false}, {name, true}, {".", false}});
        }
        force_profile_refresh();
    }

    std::map<std::pair<std::string, bool>, std::unique_ptr<ConfigurationEntry>> rows_;
    std::vector<std::string> names_;
    bool built_ = false;
    int64_t last_refresh_ = 0;
};

void force_profile_refresh() {
    ConfigurationTab::refresh_requested() = true;
}

} // namespace

std::unique_ptr<Tab> make_configuration_tab(Screen* s) {
    return std::make_unique<ConfigurationTab>(s);
}

// =============================================================================================================
// SearchBar
// =============================================================================================================

SearchBar::SearchBar(Screen* s) : text_input(TextInput("", adjust::kTahoma12, 32)), screen(s) {
    text_input.input.set_width(158);
    text_input.width = 158.0f;
    compile_pattern();
}

bool SearchBar::pattern_find(const std::string& text) const {
    // Pattern: every query character (quoted, case-insensitive) followed by \s* (except the last one)
    const size_t plen = pattern.size();
    if (plen == 0) {
        return true;
    }
    const size_t len = text.size();
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    for (size_t start = 0; start < len; ++start) {
        size_t i = start;
        bool ok = true;
        for (size_t k = 0; k < plen; ++k) {
            if (i >= len || lower(text[i]) != pattern[k]) {
                ok = false;
                break;
            }
            ++i;
            if (k + 1 < plen && !space(pattern[k + 1])) {
                while (i < len && space(text[i])) {
                    ++i;
                }
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

bool SearchBar::matches(Component* c) {
    if (c->refresh_pending) {
        return true;
    }
    if (std::find(keywords.begin(), keywords.end(), c) != keywords.end()) {
        return true;
    }
    std::string text;
    if (c->search_text(text) && pattern_find(text)) {
        return true;
    }
    if (c->parent != nullptr) {
        return false;
    }
    expand_matches(c->children);
    return c->refresh_pending;
}

void SearchBar::expand_matches(const std::vector<Component*>& list) {
    const std::vector<Component*> copy = list;
    for (Component* c : copy) {
        if (c->shown) {
            std::string text;
            if (c->search_text(text) && pattern_find(text)) {
                c->expand_ancestors();
                if (c->kind() == RowKind::Expand) {
                    const std::vector<Component*> kids = c->children;
                    for (Component* child : kids) {
                        child->expand_self();
                    }
                }
            }
        }
        expand_matches(c->children);
    }
}

bool SearchBar::key_typed(int ch, int key) {
    if (key == input::KEY_ENTER) {
        toggle();
        return true;
    }
    if (key == input::KEY_ESC && active) {
        toggle();
        return true;
    }
    if (!active) {
        return false;
    }
    text_input.input.focused = true;
    if (text_input.key_typed(ch, key)) {
        screen->layout_dirty = true;
        screen->collapse_temporary_rows();
        compile_pattern();
        return true;
    }
    return false;
}

void SearchBar::render(Vec2 mouse, const Window& window) {
    float dim = 0.0f;
    if (active) {
        if (is_searching()) {
            dim = text_input.input.focused ? 40.0f : 20.0f;
        } else {
            dim = 60.0f;
        }
    }
    draw_dim(backdrop, window, dim);
    if (active) {
        text_input.input.has_placeholder = false;
    } else {
        text_input.input.placeholder = "Press " ACG_WHITE "ENTER" ACG_RESET " to search...";
        text_input.input.has_placeholder = true;
    }
    text_input.input.set_width(158);
    text_input.width = 158.0f;
    const float previous = adjust::global_alpha();
    const float dim_value = backdrop.value;
    adjust::set_global_alpha(std::min(previous, std::max(0.7f, dim_value)));
    // centred horizontally (next to the 9 wide home button of Adjust), 13 units from the top
    const float x = (static_cast<float>(window.scaled_width) - text_input.width - 9.0f) * 0.5f;
    text_input.draw(mouse, x, 13.0f);
    adjust::set_global_alpha(previous);
}

void SearchBar::collect(Component* c) {
    const std::vector<Component*> kids = c->children;
    for (Component* child : kids) {
        collect(child);
    }
    keywords.push_back(c);
}

void SearchBar::toggle() {
    screen->layout_dirty = true;
    for (Tab* tab : screen->tabs) {
        const std::vector<Component*> entries = tab->get_entries();
        for (Component* e : entries) {
            if (e->refresh_pending) {
                e->set_expanded(false);
            }
        }
    }
    active = !active;
    text_input.input.field.set_text("");
    text_input.input.focused = active;
    compile_pattern();
}

bool SearchBar::has_matching_child(Component* c) const {
    Tab* tab = c->owner;
    if (tab == nullptr) {
        return false;
    }
    for (Component* child : c->children) {
        if (std::find(tab->render_list.begin(), tab->render_list.end(), child) != tab->render_list.end()) {
            return true;
        }
    }
    return false;
}

void SearchBar::compile_pattern() {
    pattern.clear();
    for (char c : text_input.input.field.text) {
        pattern.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    keywords.clear();
}

// =============================================================================================================
// ClickGuiScreen
// =============================================================================================================

Screen& screen() noexcept {
    static Screen s{};
    return s;
}

Screen::Screen() : search_bar(this) {
    static constexpr const char* kNames[5] = {"Combat", "Move", "Visual", "Block", "Utility"};
    for (size_t i = 0; i < 5; ++i) {
        owned_tabs_.push_back(std::make_unique<CategoryTab>(this, static_cast<events::ModuleCategory>(i), kNames[i]));
    }
    owned_tabs_.push_back(make_configuration_tab(this));
    float x = 0.0f;
    for (auto& t : owned_tabs_) {
        t->x = x;
        t->y = 84.0f;
        x += 68.0f; // 66 wide tabs with a 2 unit gap
        tabs.push_back(t.get());
    }
    last_activity = 0;
}

void Screen::refresh_tab(Tab* tab) {
    tab->render_list.clear();
    tab->row_list.clear();
    tab->layout_list.clear();
    const bool searching = search_bar.is_searching();
    Component* state = tab->get_state();
    if (searching && state != nullptr && !state->expanded) {
        state->set_expanded(true);
        state->refresh_pending = true;
    }
    const std::vector<Component*> entries = tab->get_entries();
    for (Component* e : entries) {
        if (e->is_header()) {
            tab->row_list.push_back(e);
            if (!Component::is_fully_expanded(e)) {
                break;
            }
            continue;
        }
        if (e->parent != nullptr && !Component::is_fully_expanded(e->parent)) {
            continue;
        }
        if (!e->shown) {
            continue;
        }
        if (searching && !search_bar.matches(e)) {
            continue;
        }
        tab->render_list.push_back(e);
    }
    tab->layout_list = tab->row_list;
    tab->layout_list.insert(tab->layout_list.end(), tab->render_list.begin(), tab->render_list.end());
}

void Screen::add_overlay(std::unique_ptr<Modifier> overlay) {
    bool empty = true;
    for (auto& o : overlays) {
        if (!o->removed) {
            empty = false;
            break;
        }
    }
    if (overlay->can_stack() || empty) {
        overlays.push_back(std::move(overlay));
    }
}

void Screen::remove_overlay(Modifier* overlay) noexcept {
    for (auto& o : overlays) {
        if (o.get() == overlay) {
            o->removed = true;
        }
    }
}

void Screen::purge_overlays() {
    overlays.erase(std::remove_if(overlays.begin(), overlays.end(), [](const std::unique_ptr<Modifier>& o) {
                       return o->removed;
                   }),
                   overlays.end());
}

namespace {

std::vector<Modifier*> live_overlays(Screen& s) {
    std::vector<Modifier*> out;
    for (auto& o : s.overlays) {
        if (!o->removed) {
            out.push_back(o.get());
        }
    }
    return out;
}

} // namespace

bool Screen::key_typed(int ch, int key) {
    for (Modifier* o : live_overlays(*this)) {
        if (!o->removed && o->key_typed(ch, key)) {
            return true;
        }
    }
    for (Tab* tab : tabs) {
        const std::vector<Component*> list = tab->layout_list;
        for (Component* c : list) {
            if (c->key_typed(ch, key)) {
                return true;
            }
        }
    }
    if (search_bar.key_typed(ch, key)) {
        return true;
    }
    if (key != input::KEY_ESC) {
        return false;
    }
    fade.close();
    return true;
}

void Screen::on_scroll(Vec2 mouse, float direction) {
    for (Modifier* o : live_overlays(*this)) {
        if (!o->removed && o->on_scroll(direction)) {
            return;
        }
    }
    for (Tab* tab : tabs) {
        const std::vector<Component*> list = tab->layout_list;
        for (Component* c : list) {
            if (mouse.x < c->x || mouse.y < c->y || c->x + c->width <= mouse.x || c->y + c->height <= mouse.y) {
                continue;
            }
            if (c->on_scroll(mouse, direction)) {
                return;
            }
        }
    }
    for (Tab* tab : tabs) {
        const float tx = tab->get_x();
        const float ty = tab->get_y();
        if (mouse.x < tx || mouse.y < ty || tx + tab->width <= mouse.x || ty + tab->height <= mouse.y) {
            continue;
        }
        tab->scroll_by(-direction * 14.0f);
        timer48 = adjust::now_ms();
        layout_dirty = true;
    }
}

bool Screen::mouse_clicked(Vec2 mouse, int button, bool pressed) {
    if (button == 0 && !pressed) {
        dragged_tab = nullptr;
    }
    // search text field: a left press inside focuses it, every other press unfocuses it
    TextInputBox& tf = search_bar.text_input;
    bool inside_field = false;
    if (pressed) {
        if (button == 0 && mouse.x >= tf.x && mouse.y >= tf.y && tf.x + tf.width > mouse.x &&
            tf.y + tf.height > mouse.y) {
            tf.input.focused = true;
            inside_field = true;
        } else {
            tf.input.focused = false;
        }
    }
    // search field focus loss / gain toggles the search
    if (tf.input.focused != search_bar.active) {
        if (tf.input.focused || !search_bar.is_searching()) {
            search_bar.toggle();
            return true;
        }
        if (inside_field) {
            return true;
        }
    } else if (inside_field) {
        return true;
    }
    for (Modifier* o : live_overlays(*this)) {
        if (!o->removed && o->mouse_clicked(mouse, button, pressed)) {
            return true;
        }
    }
    const std::vector<Tab*> tab_copy = tabs; // list order: the last tab is drawn in front
    for (Tab* tab : tab_copy) {
        const float bottom = tab->height + tab->get_y();
        const std::vector<Component*> list = tab->layout_list;
        for (Component* c : list) {
            if (!tab->hovered) {
                c->on_click_outside(mouse, button, pressed);
                continue;
            }
            if (mouse.y >= bottom) {
                break; // rows below the tab body get no events
            }
            const bool inside = mouse.x >= c->x && mouse.y >= c->y && c->x + c->width > mouse.x &&
                                c->y + c->height > mouse.y;
            if (inside) {
                if (c->mouse_clicked(mouse, button, pressed)) {
                    return true;
                }
            } else {
                c->on_click_outside(mouse, button, pressed);
            }
        }
    }
    return false;
}

void Screen::update_screen() {
    for (Tab* tab : tabs) {
        const std::vector<Component*> list = tab->layout_list;
        for (Component* c : list) {
            c->on_update();
        }
    }
    ++counter;
    last_activity = adjust::now_ms();
}

void Screen::on_open() {
    const int64_t now = adjust::now_ms();
    backdrop.value = 0.0f;
    backdrop.restart();
    counter = 0;
    last_tick = now;
    if (last_activity + 60000 <= now) {
        fade.animation.duration = 400; // opened again after > 60 s: slower fade-in
        last_activity = now;
    } else {
        fade.animation.duration = 220;
    }
    // the menu may have been closed without its fade (module disabled directly): always fade in from 0
    fade.closing = false;
    fade.animation.value = 0.0f;
    fade.animation.target = 0.0f;
    fade.animation.restart();
    close_requested = false;
    closed = false;
    layout_dirty = true;
}

void Screen::on_close() {
    for (Modifier* m : all_modifiers()) {
        m->reset();
    }
    close_all();
    dragged_tab = nullptr;
    layout_dirty = true;
    backdrop.value = 0.0f;
    backdrop.restart();
    overlays.clear();
    dialogs().clear();
}

void Screen::layout_tab(Tab* tab) {
    const float ty = tab->get_y();
    const float tx = tab->get_x();
    const float w = tab->width;
    const float scroll_value = tab->scroll_animation.value;
    float running = ty;
    for (Component* c : tab->layout_list) {
        c->width = w;
        const float scrolled = c->is_header() ? 0.0f : scroll_value;
        c->x = tx;
        c->y = running - scrolled;
        const float next = running + c->height;
        int depth = 0;
        if (c->kind() != RowKind::ColorPicker) {
            for (Component* p = c->parent; p != nullptr; p = p->parent) {
                ++depth;
            }
        }
        const int shade = 19 - 3 * std::min(depth, 3); // 19, 16, 13, 10
        c->background_color = 0xAA000000u | (static_cast<Argb>(shade) << 16) | (static_cast<Argb>(shade) << 8) |
                              static_cast<Argb>(shade);
        c->bounds.x = c->get_x() + static_cast<float>(depth);
        c->bounds.y = c->get_y();
        c->bounds.width = (c->width - static_cast<float>(depth)) - static_cast<float>(depth);
        c->bounds.height = c->height;
        running = next;
    }
}

float Screen::tab_alpha(Tab* tab) const {
    float a = 1.0f;
    if (focused_tab != tab) {
        a = 1.0f - 0.65f * drag_animation.value;
    }
    return std::min(a, fade.animation.value);
}

void Screen::close_all() {
    for (Tab* tab : tabs) {
        const std::vector<Component*> entries = tab->get_entries();
        for (Component* e : entries) {
            if (e->refresh_pending) {
                e->set_expanded(false);
            }
            e->on_screen_closed();
        }
    }
}

void Screen::collapse_temporary_rows() {
    for (Tab* tab : tabs) {
        const std::vector<Component*> entries = tab->get_entries();
        for (Component* e : entries) {
            if (e->refresh_pending) {
                e->set_expanded(false);
            }
        }
    }
}

void Screen::refresh_all_tabs() {
    for (Tab* tab : tabs) {
        refresh_tab(tab);
    }
}

void Screen::layout_all_tabs(const Window& window) {
    for (Tab* tab : tabs) {
        tab->has_anchor = false;
        const auto window_height = static_cast<float>(window.scaled_height);
        float running = tab->y;
        for (Component* c : tab->layout_list) {
            const float h = c->measure_height();
            c->height = h;
            running += h;
        }
        tab->content_height = running - tab->get_y();
        const float max_height = window_height / 1.75f;
        tab->height = std::min(tab->content_height, max_height);
        const float over = window_height - (tab->get_y() + tab->height); // free space below the tab
        if (over < 5.0f) {
            tab->height = tab->height - std::fabs(over);
            if (tab->height < 11.0f) {
                tab->height = 11.0f;
            }
        }
        tab->scroll = std::max(0.0f, std::min(tab->scroll, tab->content_height - tab->height));
        tab->anchor = Vec2{tab->clamp_x(), tab->clamp_y()};
        tab->has_anchor = true;
        layout_tab(tab);
    }
}

void Screen::update_shown_flags() {
    ++frame;
    for (Tab* tab : tabs) {
        const std::vector<Component*> entries = tab->get_entries();
        for (Component* c : entries) {
            const bool visible = c->compute_visible(frame);
            if (c->shown != visible) {
                c->shown = visible;
                layout_dirty = true;
            }
        }
    }
}

std::vector<Modifier*> Screen::all_modifiers() {
    std::vector<Modifier*> out;
    out.reserve(128);
    for (Tab* tab : tabs) {
        for (Component* c : tab->get_entries()) {
            for (auto& m : c->modifiers) {
                out.push_back(m.get());
            }
        }
    }
    return out;
}

void Screen::render_screen(Vec2 mouse, const Window& window) {
    if (closed) {
        return;
    }
    if (fade.progress()) {
        close_requested = true; // the closing fade has finished
        closed = true;
        return;
    }
    drag_animation.update(dragged_tab != nullptr ? 1.0f : 0.0f);
    if (!initialized) {
        initialized = true;
        float total = 0.0f;
        for (Tab* t : tabs) {
            total += 2.0f + t->width;
        }
        float x = (static_cast<float>(window.scaled_width) / 2.0f) - (total / 2.0f); // tabs centred, 2 unit gaps
        for (Tab* t : tabs) {
            t->x = x;
            t->y = static_cast<float>(window.scaled_height) / 4.5f; // tab top at 1/4.5 of the window height
            x += 2.0f + t->width;
        }
    }
    if (dragged_tab != nullptr) {
        if (tabs.back() != dragged_tab) { // bring the dragged tab to the front (end of the list = drawn last)
            tabs.erase(std::remove(tabs.begin(), tabs.end(), dragged_tab), tabs.end());
            tabs.push_back(dragged_tab);
        }
        dragged_tab->x = mouse.x - grab_x;
        dragged_tab->y = mouse.y - grab_y;
        drag_moved = true;
        layout_dirty = true;
    }
    update_shown_flags();
    for (Tab* tab : tabs) {
        if (counter == 0) {
            tab->scroll_animation.value = tab->scroll;
            tab->scroll_animation.restart();
        }
        tab->scroll_animation.update(tab->scroll); // 135 ms follow animation
        if (!tab->scroll_animation.finished) {
            layout_dirty = true;
        }
    }
    if (layout_dirty) {
        refresh_all_tabs();
        layout_all_tabs(window);
        layout_dirty = false;
    }
    Tab* hovered_tab = nullptr; // topmost tab under the mouse (list is back to front)
    for (size_t i = tabs.size(); i-- > 0;) {
        if (tabs[i]->contains(mouse)) {
            hovered_tab = tabs[i];
            break;
        }
    }
    const float previous = adjust::global_alpha();
    adjust::set_global_alpha(fade.animation.value);
    draw_dim(backdrop, window, dragged_tab != nullptr ? 95.0f : 45.0f);
    adjust::set_global_alpha(previous);
    DrawQueue top; // frame level queue: drawn after all tabs (tooltips)
    const std::vector<Tab*> tab_copy = tabs;
    for (Tab* tab : tab_copy) {
        adjust::set_global_alpha(tab_alpha(tab));
        tab->hovered = tab == hovered_tab;
        const float tab_x = tab->get_x();
        const float tab_y = tab->get_y();
        const float bottom = tab->height + tab_y;
        const bool mouse_above_bottom = tab == hovered_tab && bottom > mouse.y;
        DrawQueue row_queue; // per-tab queue: drawn inside the tab clip after the rows
        bool header_hovered = false;
        for (Component* c : tab->row_list) {
            if (c->get_y() >= bottom) {
                break;
            }
            c->hovered = mouse_above_bottom && c->bounds.contains(mouse);
            header_hovered |= c->hovered;
            c->render(row_queue, top, mouse, window);
        }
        // clip of the body: below the header
        adjust::push_scissor(tab_x - 0.5f, tab_y + 10.5f, 1.0f + tab->width, tab->height - 10.5f);
        for (Component* c : tab->render_list) {
            if (c->get_y() >= bottom) {
                break;
            }
            const float row_top = c->get_y();
            const float row_bottom = row_top + c->height;
            adjust::fill_rect(c->background_color, c->bounds.x, row_top, c->bounds.width + c->bounds.x, row_bottom);
            if (is_separator_row(c) && Component::is_fully_expanded(c)) {
                adjust::push_even_scale();
                const math::Vec4f r = adjust::to_pixels(c->bounds.x, c->y + c->height - 0.5f,
                                                        c->bounds.x + c->bounds.width, c->y + c->height);
                adjust::fill_rect(0xAA373738u, r.x, r.y, r.w, r.h);
                adjust::pop_even_scale();
            }
        }
        for (Component* c : tab->render_list) {
            if (c->get_y() >= bottom) {
                break;
            }
            c->hovered = !header_hovered && mouse_above_bottom && c->bounds.contains(mouse);
            c->render(row_queue, top, mouse, window);
        }
        if (!row_queue.post_tasks.empty()) {
            adjust::push_scissor(tab_x, tab_y + 11.0f, tab->width, tab->height - 11.0f);
            row_queue.run();
            adjust::pop_scissor();
        }
        adjust::pop_scissor();
    }
    top.run();
    adjust::set_global_alpha(previous);
    // overlays (bind overlay)
    adjust::set_global_alpha(fade.animation.value);
    for (Modifier* o : live_overlays(*this)) {
        DrawQueue q;
        o->render(window, q, q, mouse, false);
        q.run();
    }
    search_bar.render(mouse, window);
    adjust::set_global_alpha(previous);
}

// =============================================================================================================
// Visual-style switch (same as the OpenSlinky++ ClickGUI's render_visual_style_switch)
// =============================================================================================================

namespace {

// Renderer scale captured during the last Settings-button draw (used to map cursor pixels to logical coords
// in the deferred click handler).
float g_settings_popup_scale = 1.0f;

// OpenSlinky++: bottom-right "Settings" button (gear) that opens the style/theme + Alt Manager popup
// (replaces the old "Slinky++ | Adjust" pill). Shared with the Slinky++ ClickGUI via settings_popup.
void render_visual_style_switch(gui::GuiRenderer* renderer, float alpha) {
    NVGcontext* vg = renderer->vg;
    nvgSave(vg);
    nvgGlobalAlpha(vg, alpha);
    g_settings_popup_scale = (renderer->scale > 0.0f) ? renderer->scale : 1.0f;
    const float cx = env().cursor_px.x / g_settings_popup_scale;
    const float cy = env().cursor_px.y / g_settings_popup_scale;
    altmanager::settings_popup_render(renderer, cx, cy);
    nvgRestore(vg);
}

// Left press on the Settings button / popup: true if it consumed the click.
bool style_switch_click() {
    const float scale = (g_settings_popup_scale > 0.0f) ? g_settings_popup_scale : 1.0f;
    const float cx = env().cursor_px.x / scale;
    const float cy = env().cursor_px.y / scale;
    return altmanager::settings_popup_click(cx, cy);
}

int decode_char(const char* utf8) noexcept {
    const auto c0 = static_cast<unsigned char>(utf8[0]);
    return c0 < 0x80 ? static_cast<int>(c0) : 0; // ASCII only (TextField)
}

} // namespace

} // namespace slinky::modules::adjust_clickgui::detail

// =============================================================================================================
// Public API
// =============================================================================================================

namespace slinky::modules::adjust_clickgui {

using namespace detail;

void on_open(double now) noexcept {
    (void)now;
    try {
        screen().on_open();
    } catch (...) {
    }
}

void on_close() noexcept {
    try {
        screen().on_close();
    } catch (...) {
    }
}

void request_close() noexcept {
    screen().fade.close();
}

bool consume_close_request() noexcept {
    Screen& s = screen();
    if (!s.close_requested) {
        return false;
    }
    s.close_requested = false;
    return true;
}

bool wants_keyboard() noexcept {
    if (!dialogs().empty()) {
        return true;
    }
    Screen& s = screen();
    for (auto& o : s.overlays) {
        if (!o->removed) {
            return true;
        }
    }
    if (s.search_bar.active && s.search_bar.text_input.input.focused) {
        return true;
    }
    for (Tab* tab : s.tabs) {
        for (Component* c : tab->layout_list) {
            if (c->wants_keyboard()) {
                return true;
            }
        }
    }
    return false;
}

void render(events::GuiRender2DEvent* ev) noexcept {
    if (ev == nullptr || ev->renderer == nullptr || ev->renderer->vg == nullptr) {
        return;
    }
    try {
        Env& e = env();
        if (ev->ctx != nullptr) {
            e.modules = ev->ctx->module_manager;
            e.profiles = ev->ctx->profile_registry;
            e.key_states = ev->ctx->key_states;
        }
        Screen& s = screen();
        const adjust::HudFrame frame = adjust::begin_hud(ev->renderer, click_gui_scale());
        if (!frame.valid) {
            return;
        }
        const Window window{frame.width, frame.height, frame.scaled_width, frame.scaled_height};
        if (window.scaled_width != e.window.scaled_width || window.scaled_height != e.window.scaled_height) {
            s.layout_dirty = true; // UserConfiguration.secondaryScale listener / resize
        }
        e.window = window;
        e.scale = frame.scale;
        const bool separators = click_gui_separators();
        if (separators != s.last_separators) {
            s.last_separators = separators;
            s.layout_dirty = true; // HUDModule.onSeparatorsChanged
        }
        // updateScreen once per game tick
        const int64_t now = adjust::now_ms();
        if (now - s.last_tick > 1000) {
            s.last_tick = now - 50;
        }
        while (now - s.last_tick >= 50) {
            s.update_screen();
            s.last_tick += 50;
        }
        s.purge_overlays();
        for (Tab* tab : s.tabs) {
            tab->sync();
        }
        const Vec2 mouse = mouse_hud();
        s.render_screen(mouse, window);
        // dialogs (screen stack overlays above the Click GUI)
        auto& list = dialogs();
        for (auto& d : list) {
            d->render(window, mouse);
        }
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [](const std::unique_ptr<Dialog>& d) { return d->finished(); }),
                   list.end());
        adjust::end_hud();
        if (!s.closed) {
            render_visual_style_switch(ev->renderer, s.fade.animation.value);
        }
    } catch (...) {
        adjust::end_hud();
    }
}

bool handle_input(const input::InputEvent* event, double now) noexcept {
    (void)now;
    if (event == nullptr) {
        return false;
    }
    try {
        Env& e = env();
        Screen& s = screen();
        s.purge_overlays();
        switch (event->kind) {
            case input::InputEventKind::MouseMove:
                e.cursor_px = Vec2{static_cast<float>(event->move.x), static_cast<float>(event->move.y)};
                return false;
            case input::InputEventKind::MouseScroll: {
                if (!dialogs().empty()) {
                    return true;
                }
                const float delta = event->scroll.delta;
                if (delta == 0.0f) {
                    return false;
                }
                s.on_scroll(mouse_hud(), delta < 0.0f ? 1.0f : -1.0f); // wheel down = +1 (AdjustScreen.render)
                return true;
            }
            case input::InputEventKind::KeyOrButton: {
                const int code = event->key.key_code;
                const bool pressed = event->pressed != 0;
                if (code > 0 && code < input::KEY_COUNT) {
                    e.keys[code] = pressed;
                }
                if (code >= input::KEY_MOUSE1 && code <= input::KEY_MOUSE5) {
                    const int button = code - input::KEY_MOUSE1;
                    auto& list = dialogs();
                    if (!list.empty()) {
                        list.back()->mouse_clicked(mouse_hud(), button, pressed, e.window);
                        return true;
                    }
                    if (s.fade.closing || s.closed) {
                        return true;
                    }
                    if (pressed && button == 0 && live_overlays(s).empty() && style_switch_click()) {
                        return true;
                    }
                    return s.mouse_clicked(mouse_hud(), button, pressed);
                }
                if (!pressed) {
                    return wants_keyboard();
                }
                const int ch = code == 0 ? decode_char(event->key.utf8_text) : 0;
                const int key = code == 0 ? kNoKey : code;
                if (code == 0 && ch == 0) {
                    return wants_keyboard();
                }
                static bool swallow_char = false; // character of a key the bind overlay just captured
                if (code == 0 && swallow_char) {
                    swallow_char = false;
                    return true;
                }
                swallow_char = false;
                auto& list = dialogs();
                if (!list.empty()) {
                    list.back()->key_typed(ch, key);
                    return true;
                }
                const bool keyboard = wants_keyboard();
                const bool had_overlay = !live_overlays(s).empty();
                const bool consumed = s.key_typed(ch, key);
                if (code != 0 && had_overlay && consumed) {
                    swallow_char = true;
                }
                return consumed || keyboard || key == input::KEY_ESC;
            }
            case input::InputEventKind::RawInput:
            default:
                return false;
        }
    } catch (...) {
        return false;
    }
}

} // namespace slinky::modules::adjust_clickgui
