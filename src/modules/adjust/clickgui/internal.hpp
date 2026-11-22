#pragma once

// Internal declarations of the Adjust ClickGUI port (see adjust_click_gui.hpp). Class names follow the Java sources:
//
//   ClickGuiScreen            -> Screen                ClickGuiTab / CategoryTab / ConfigurationTab -> Tab / ...
//   ClickGuiComponent         -> Component             Modifier / ModifierPhase                     -> Modifier / Phase
//   frame.HoverEffect         -> HoverEffect           frame.HoverBackground                        -> HoverBackground
//   frame.ButtonBar           -> ButtonBar             frame.HeaderButton / ExpandButton            -> HeaderButton / ...
//   frame.Tooltip             -> Tooltip               IconButtonModifier                           -> IconButtonModifier
//   frame.TabHeader           -> TabHeader             frame.ModuleEntry                            -> ModuleEntry
//   overlay.BindOverlay       -> BindOverlay           SearchBar                                    -> SearchBar
//   ui.dialog.AdjustButton    -> Button                ui.dialog.DialogBuilder / DialogScreen       -> Dialog
//   render.font.TextInput(Box)-> TextInput(Box)        net.minecraft GuiTextField                   -> TextField
//   event.Render2DEvent       -> DrawQueue (post tasks) ScaledResolution                            -> Window
//
// Java "keyTyped(char, keyCode)" is split by OpenSlinky++'s input into key events (ch = 0, key = input::KeyCode)
// and character events (ch = code point, key = kNoKey). Mouse buttons use Java numbering (0 left, 1 right, 2 middle).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adjust/kit.hpp"
#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "events/module.hpp"
#include "modules/adjust/hud/tabgui/setting_capture.hpp"

namespace slinky::events {
class ModuleManager;
}
namespace slinky::config {
struct ProfileRegistry;
}

namespace slinky::modules::adjust_clickgui::detail {

using adjust::Argb;
namespace capture = slinky::modules::adjust_hud::tabgui;

// net.minecraft.util.ChatColor
#define ACG_BOLD ADJUST_FMT "l"
#define ACG_ITALIC ADJUST_FMT "o"
#define ACG_WHITE ADJUST_FMT "f"
#define ACG_RESET ADJUST_FMT "r"

inline constexpr int kNoKey = -1; // key code of a pure character event

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// com.adjustclient.ui.screen.AdjustComponent
struct Box {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool contains(Vec2 p) const noexcept { return p.x >= x && p.y >= y && x + width > p.x && y + height > p.y; }
};

// ScaledResolution of the Click GUI frame
struct Window {
    float width = 0.0f;
    float height = 0.0f;
    int scaled_width = 0;
    int scaled_height = 0;
};

// Render2DEvent.postTasks
struct DrawQueue {
    std::vector<std::function<void()>> post_tasks;
    void run();
};

// =============================================================================================================
// Environment (input state, game pointers)
// =============================================================================================================

struct Env {
    events::ModuleManager* modules = nullptr;
    config::ProfileRegistry* profiles = nullptr;
    const uint8_t* key_states = nullptr; // GameContext::key_states (stable runtime table)
    Window window{};                     // AdjustScreen.screenResolution of the last frame
    float scale = 2.0f;                  // pixels per HUD unit of the last frame
    Vec2 cursor_px{};                    // window pixels
    bool keys[5 + 104]{};                // fallback key state tracked from the events (index = input key code)
};
Env& env() noexcept;

bool key_down(int key_code) noexcept;
bool ctrl_down() noexcept;     // GuiScreen.isCtrlKeyDown
bool shift_down() noexcept;    // GuiScreen.isShiftKeyDown
bool lcontrol_down() noexcept; // Keyboard.isKeyDown(KEY_LCONTROL)
bool mouse_button_down(int java_button) noexcept; // Mouse.isButtonDown
Vec2 mouse_hud() noexcept;     // cursor in HUD units

void mark_dirty() noexcept; // config::mark_profile_dirty(true)
int clamp_int(int v, int lo, int hi) noexcept;
float clampf(float v, float lo, float hi) noexcept;
float pixel_line_width(bool rounded = false) noexcept; // NvgContext pixel unit * 0.5 (inside push_even_scale)
const char* safe(const char* s) noexcept;

std::string get_clipboard();
void set_clipboard(const std::string& text) noexcept;

// =============================================================================================================
// Text input (GuiTextField / TextInput / TextInputBox)
// =============================================================================================================

class TextField {
public:
    std::string text;
    int max_length = 32;
    int cursor = 0;
    int selection_end = 0;
    bool focused = false;
    int64_t focus_ms = 0;

    void set_text(std::string_view value);
    void set_focused(bool value) noexcept;
    int cursor_counter() const noexcept; // updateCursorCounter ticks since focus
    bool key_typed(int ch, int key);     // textboxKeyTyped
    void write_text(std::string_view value);
    void delete_from_cursor(int amount);
    void delete_words(int count);
    int nth_word(int n, int pos) const noexcept;
    void set_cursor(int pos) noexcept;
    void move_cursor_by(int n) noexcept { set_cursor(selection_end + n); }
    void set_selection(int pos) noexcept;
    std::string selected_text() const;

    static bool allowed(int ch) noexcept; // ChatAllowedCharacters (ASCII printable)
};

class TextInput {
public:
    TextInput(std::string placeholder, adjust::Font font, int max_length);

    TextField field;
    adjust::Animation animation{adjust::Easing::Linear, 125};
    std::string placeholder;
    bool has_placeholder = true;
    adjust::Font font;
    std::vector<std::string> lines;
    bool focused = false;
    bool hovered = false;

    void set_width(int width); // wraps the text at width - 8
    void draw(float x, float y);
};

class TextInputBox {
public:
    explicit TextInputBox(TextInput in) : input(std::move(in)) {}

    TextInput input;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool key_typed(int ch, int key);
    void draw(Vec2 mouse, float at_x, float at_y);
};

// =============================================================================================================
// AdjustButton
// =============================================================================================================

struct Button {
    Button() = default;
    Button(int id_, std::string label_, int w, int h, int base_, bool alpha75_, adjust::Font font_)
        : id(id_), label(std::move(label_)), width(w), height(h), base(base_), alpha75(alpha75_), font(font_) {}

    int id = 0;
    std::string label;
    float x = 0.0f;
    float y = 0.0f;
    int width = 0;
    int height = 0;
    int base = 26;
    bool alpha75 = false; // AlphaModifier alpha * 0.75 (else identity)
    adjust::Font font = adjust::kTahoma12;
    adjust::Animation hover{adjust::Easing::Linear, 111};
    float label_offset_x = 0.0f;
    float label_offset_y = 0.0f;
    bool enabled = true;
    bool visible = true;
    bool hovered = false;

    void draw(float mx, float my);
    bool is_hovered(float mx, float my) const noexcept;
};

// =============================================================================================================
// Dialog (DialogBuilder + DialogScreen)
// =============================================================================================================

// util.ChatMessage: `italic` applies to the next append only.
struct Message {
    struct Part {
        std::string text;
        bool italic = false;
    };
    std::vector<Part> parts;
    bool italic = false;

    Message& append(std::string_view text) {
        parts.push_back(Part{std::string(text), italic});
        italic = false;
        return *this;
    }
};

class Dialog {
public:
    using Action = std::function<void(Dialog&)>;
    using Validator = std::function<bool(Dialog&)>;

    explicit Dialog(std::string title_) : title(std::move(title_)) {}

    std::string title;
    Message message;
    int bottom_inset = 0;

    Dialog& add_button(std::string_view text, Action action, Validator enabled = nullptr);
    Dialog& add_text_input(std::string_view label, std::string_view text, int max_length, int width);
    std::string input_text(int index) const; // findTextInput(index).box.input.field.getText() ("" when absent)

    // Screen side
    void render(const Window& window, Vec2 mouse);
    bool key_typed(int ch, int key);
    bool mouse_clicked(Vec2 mouse, int button, bool pressed, const Window& window);
    void close() noexcept;
    bool finished() const noexcept { return finished_; }
    bool has_focused_input() const noexcept;

    float minimum_width = 70.0f;

private:
    struct InputRow {
        TextInputBox box;
        int index = 0;
        int width = 0;
    };
    struct ButtonRow {
        Button button;
        Action action;
        Validator enabled;
    };
    struct Segment {
        std::string text;
        Argb color = 0;
    };

    bool compute_width(float width) const;
    void update_size();
    void layout();
    void draw_inputs(Vec2 mouse, float x, float y);
    void draw_buttons(Vec2 mouse, float x, float y);
    float message_width() const;

    std::vector<InputRow> inputs_;
    std::vector<ButtonRow> buttons_;
    std::vector<std::vector<Segment>> lines_;
    bool laid_out_ = false;
    Vec2 size_{};
    adjust::Animation dim_{adjust::Easing::Linear, 150};
    adjust::Animation transition_{adjust::Easing::EaseOutExpo, 220};
    bool closing_ = false;
    bool finished_ = false;
};

// DialogBuilder.build(width): focuses the first input and puts the dialog on top of the Click GUI.
void show_dialog(std::unique_ptr<Dialog> dialog, float width = 70.0f);

// NotificationBuilder("Invalid Value") etc. (Adjust HUD notifications, only shown while the HUD runs)
void notify(std::string_view title, std::initializer_list<std::pair<std::string_view, bool>> parts);

// =============================================================================================================
// Components and modifiers
// =============================================================================================================

class Tab;
class Screen;
class Modifier;
class Tooltip;

enum class Phase : uint8_t { Pre, Post, Both };

enum class RowKind : uint8_t {
    Generic,
    Header,
    Module,
    Setting,
    Expand,      // ExpandSettingComponent (separator row type)
    Color,       // ColorSettingComponent (separator row type)
    ColorPicker, // no inset
    StringSetting, // separator row type
    StringEditor,
    Separator,
    Configuration,
};

class Component {
public:
    Component() = default;
    virtual ~Component();
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;

    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::vector<std::unique_ptr<Modifier>> modifiers;
    Component* parent = nullptr;
    Box bounds{};
    Tab* owner = nullptr;
    std::vector<Component*> children;
    Argb background_color = 0;
    int visibility_stamp = -1;
    bool hovered = false;
    bool expanded = false;
    bool shown = false;
    bool refresh_pending = false;
    bool cached_visible = false;

    float get_x() const noexcept { return x; }
    float get_y() const noexcept { return y; }

    template <typename T>
    T* add_modifier(std::unique_ptr<T> m) {
        T* raw = m.get();
        modifiers.push_back(std::move(m));
        return raw;
    }

    virtual RowKind kind() const noexcept { return RowKind::Generic; }
    virtual void on_screen_closed() {}
    virtual void render_content(DrawQueue& queue, Vec2 mouse) {}
    virtual bool search_text(std::string& out) const { return false; } // false = null
    virtual bool compute_visible(int stamp) { return is_visible(stamp); }
    virtual bool is_visible(int stamp);
    virtual bool key_typed(int ch, int key);
    virtual bool on_scroll(Vec2 mouse, float direction) { return false; }
    virtual bool mouse_clicked(Vec2 mouse, int button, bool pressed);
    virtual bool on_mouse_pressed(Vec2 mouse, int button, bool pressed) { return false; }
    virtual void render(DrawQueue& queue, DrawQueue& top, Vec2 mouse, const Window& window);
    virtual void set_expanded(bool value);
    virtual void on_click_outside(Vec2 mouse, int button, bool pressed) {}
    virtual bool is_hovered() const { return hovered; }
    virtual void expand_self();
    void toggle_expanded();
    virtual void on_update() {}
    virtual bool is_header() const { return false; }
    virtual float get_height() { return 0.0f; }
    virtual float measure_height() { return get_height(); }
    virtual void on_collapsed_hidden() {} // ExpandSettingComponent: expandAnimation reset in setExpanded(false)
    virtual bool wants_keyboard() const { return false; } // a text input of the row is focused
    void expand_ancestors();

    static bool is_fully_expanded(const Component* c) noexcept;
    bool is_fully_expanded() const noexcept { return is_fully_expanded(this); }
};

class Modifier {
public:
    virtual ~Modifier() = default;
    virtual Phase phase() const noexcept { return Phase::Pre; }
    virtual bool on_scroll(float direction) { return false; }
    virtual bool can_stack() const noexcept { return true; }
    virtual bool key_typed(int ch, int key) { return false; }
    virtual bool mouse_clicked(Vec2 mouse, int button, bool pressed) { return false; }
    virtual void render(const Window& window, DrawQueue& queue, DrawQueue& top, Vec2 mouse, bool first_pass) = 0;
    virtual void reset() {}
    // ClickGuiComponent.setExpanded(false) of the owner while its parent is collapsed
    virtual void on_owner_collapsed() {}
    virtual Tooltip* as_tooltip() noexcept { return nullptr; }

    bool removed = false; // overlay removed from the screen (purged after the current dispatch)
};

std::function<bool()> hovered_of(const Component* c);

// frame.HoverEffect
class HoverEffect final : public Modifier {
public:
    HoverEffect(const Component* owner, std::function<bool()> active, std::function<bool()> hovered,
                float hover_value = 1.0f, float active_value = 0.8f);

    void render(const Window& window, DrawQueue& queue, DrawQueue& top, Vec2 mouse, bool first_pass) override;
    void reset() override { has_animation_ = false; }
    void on_owner_collapsed() override { reset(); }

    adjust::Animation& k();
    Argb color(int base);
    Argb with_alpha(Argb base);
    Argb tint(int red);
    Argb accent(int max_alpha);

private:
    void step();

    const Component* owner_;
    std::function<bool()> active_;
    std::function<bool()> hovered_;
    float hover_value_;
    float active_value_;
    adjust::Animation animation_{};
    bool has_animation_ = false;
};

// frame.HoverBackground
class HoverBackground final : public Modifier {
public:
    explicit HoverBackground(const Component* owner) : owner_(owner) {}
    void render(const Window& window, DrawQueue& queue, DrawQueue& top, Vec2 mouse, bool first_pass) override;
    void reset() override { has_animation_ = false; }
    void on_owner_collapsed() override { reset(); }

private:
    const Component* owner_;
    adjust::Animation animation_{};
    bool has_animation_ = false;
};

// frame.HeaderButton
class HeaderButton {
public:
    virtual ~HeaderButton() = default;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool draw_immediately = false;
    virtual bool is_shown() { return true; }
    virtual bool on_click() = 0;
    virtual void render() = 0;
    virtual void on_owner_collapsed() {}
};

// frame.ButtonBar
class ButtonBar final : public Modifier {
public:
    ButtonBar(Component* owner, std::vector<std::unique_ptr<HeaderButton>> buttons)
        : owner_(owner), buttons(std::move(buttons)) {}

    Phase phase() const noexcept override { return Phase::Both; }
    bool mouse_clicked(Vec2 mouse, int button, bool pressed) override;
    void render(const Window& window, DrawQueue& queue, DrawQueue& top, Vec2 mouse, bool first_pass) override;
    void on_owner_collapsed() override;

private:
    Component* owner_;

public:
    std::vector<std::unique_ptr<HeaderButton>> buttons;

private:
    bool content_clipped_ = false;
};

// frame.ExpandButton (adds its own HoverEffect to the parent's modifiers)
class ExpandButton final : public HeaderButton {
public:
    explicit ExpandButton(Component* parent, std::function<bool()> shown_if = nullptr);
    bool is_shown() override { return !shown_if_ || shown_if_(); }
    bool on_click() override;
    void render() override;
    void on_owner_collapsed() override;

    Component* parent;

private:
    bool parent_open() const;
    float update_fade();

    HoverEffect* hover_ = nullptr;
    adjust::Animation fade_{adjust::Easing::Linear, 60};
    std::function<bool()> shown_if_;
};

// ButtonBar(owner, ExpandButton(owner)) - the "+"/"-" of groups, colours, lists
void add_expand_button_bar(Component* owner);

// frame.Tooltip
class Tooltip : public Modifier {
public:
    Tooltip(const Component* owner, int64_t delay, std::string text, std::string extended, bool has_extended,
            std::function<bool()> hovered);

    Phase phase() const noexcept override { return Phase::Post; }
    void render(const Window& window, DrawQueue& queue, DrawQueue& top, Vec2 mouse, bool first_pass) override;
    void reset() override;
    Tooltip* as_tooltip() noexcept override { return this; }

    virtual std::string get_text() { return text_; }
    virtual bool is_last_used() const { return false; }
    const std::string& raw_text() const noexcept { return text_; }
    bool fading_visible() const noexcept { return has_fade_ && fade_.value != 0.0f; }

protected:
    void draw_deferred(Vec2 anchor);
    Tooltip* find_previous();
    void draw(const std::string& title, const std::string* extended, bool is_hovered, Vec2 anchor);

    const Component* owner_;
    std::string text_;
    std::string extended_;
    bool has_extended_;
    std::function<bool()> hovered_;
    adjust::Animation fade_{};
    bool has_fade_ = false;
    int64_t hover_timer_ = 0;
    bool has_hover_timer_ = false;
    Tooltip* previous_ = nullptr;
    int64_t delay_ = 250;
    float bottom_ = 0.0f;
};

// IconButtonModifier
class IconButtonModifier final : public Modifier {
public:
    IconButtonModifier(Component* anchor, Button button, std::function<void()> action, float right_offset)
        : anchor_(anchor), button_(std::move(button)), action_(std::move(action)), right_offset_(right_offset) {}
    Phase phase() const noexcept override { return Phase::Post; }
    bool mouse_clicked(Vec2 mouse, int button, bool pressed) override;
    void render(const Window& window, DrawQueue& queue, DrawQueue& top, Vec2 mouse, bool first_pass) override;

private:
    Component* anchor_;
    Button button_;
    std::function<void()> action_;
    float right_offset_;
};

// overlay.BindOverlay. The callback receives an OpenSlinky++ key code (keyboard key or mouse button).
class BindOverlay final : public Modifier {
public:
    BindOverlay(std::string name, std::function<void(int)> callback, int current_key);

    bool can_stack() const noexcept override { return false; }
    bool on_scroll(float direction) override;
    bool key_typed(int ch, int key) override;
    bool mouse_clicked(Vec2 mouse, int button, bool pressed) override;
    void render(const Window& window, DrawQueue& queue, DrawQueue& top, Vec2 mouse, bool first_pass) override;

private:
    void add_key(int key, bool has_key);

    std::string name_;
    std::function<void(int)> callback_;
    std::string pressed_text_;
    adjust::Animation fade_{adjust::Easing::Linear, 150};
    adjust::TimedFade pulse_{1050, 500, 1000};
    int64_t debounce_ = 0;
};

std::string key_name(int key_code); // KeyboardKey / MouseKey names

// =============================================================================================================
// Rows
// =============================================================================================================

// frame.TabHeader
class TabHeader final : public Component {
public:
    explicit TabHeader(Tab* tab) : tab_(tab) {}

    RowKind kind() const noexcept override { return RowKind::Header; }
    void render_content(DrawQueue& queue, Vec2 mouse) override;
    bool on_mouse_pressed(Vec2 mouse, int button, bool pressed) override;
    bool is_hovered() const override;
    float get_height() override { return 11.0f; }
    bool is_header() const override { return true; }

    HoverEffect* title_hover = nullptr;
    HoverEffect* gradient_hover = nullptr;
    float title_reserve = 0.0f;
    float title_shift = 0.0f;

private:
    Tab* tab_;
};

// frame.ModuleEntry. Its setting rows are (re)generated from the module's captured settings (settings.cpp).
class ModuleEntry final : public Component {
public:
    explicit ModuleEntry(events::Module* mod);
    ~ModuleEntry() override;

    RowKind kind() const noexcept override { return RowKind::Module; }
    void render_content(DrawQueue& queue, Vec2 mouse) override;
    bool search_text(std::string& out) const override;
    void set_expanded(bool value) override;
    bool on_mouse_pressed(Vec2 mouse, int button, bool pressed) override;
    float get_height() override { return 11.0f; }

    // Re-captures the module's settings; rebuilds the child rows when they changed. True = rows changed.
    bool sync();
    // Pre-order list of the setting rows below this entry (ClickGuiTab entries order).
    void append_rows(std::vector<Component*>& out) const;
    bool has_settings() const noexcept { return !children.empty(); }
    bool captured() const noexcept { return captured_once_; }

    events::Module* module;

private:
    void collapse_all(const std::vector<Component*>& list);
    void reset_module();

    HoverEffect* label_hover_ = nullptr;
    bool default_hidden_ = false; // kModuleFlagHiddenFromHud when first listed (reset target of the visible state)
    std::vector<capture::CapturedItem> captured_;
    bool captured_once_ = false;
    std::map<std::pair<int, const void*>, std::unique_ptr<Component>> cache_; // setting rows by (kind, setting)
};

// =============================================================================================================
// Tabs
// =============================================================================================================

class Tab {
public:
    virtual ~Tab() = default;

    float x = 0.0f;
    float y = 0.0f;
    float width = 66.0f;
    float height = 0.0f;
    std::string title;
    Screen* screen = nullptr;
    std::vector<Component*> entries;
    std::vector<Component*> render_list;
    std::vector<Component*> row_list;
    std::vector<Component*> layout_list;
    adjust::Animation scroll_animation{adjust::Easing::EaseOutExpo, 135};
    bool has_anchor = false;
    Vec2 anchor{};
    TabHeader* state = nullptr;
    float scroll = 0.0f;
    float content_height = 0.0f;
    bool hovered = false;

    bool contains(Vec2 mouse) const noexcept;
    virtual TabHeader* get_state() { return state; }
    virtual std::vector<Component*>& get_entries() { return entries; }
    virtual void sync() {} // per frame: regenerate rows from the game state
    virtual std::string display_title() const { return title; }
    void scroll_by(float delta) noexcept;
    float get_x() const noexcept { return has_anchor ? anchor.x : x; }
    float get_y() const noexcept { return has_anchor ? anchor.y : y; }
    float clamp_x() const noexcept;
    float clamp_y() const noexcept;

protected:
    void init_header(float title_shift);
    std::unique_ptr<TabHeader> header_;
};

class CategoryTab final : public Tab {
public:
    CategoryTab(Screen* screen, events::ModuleCategory category, const char* name);
    void sync() override;
    std::string display_title() const override { return name_; }

private:
    events::ModuleCategory category_;
    const char* name_;
    std::map<events::Module*, std::unique_ptr<ModuleEntry>> modules_;
    std::vector<ModuleEntry*> listed_;
    uint32_t sync_frame_ = 0;
};

std::unique_ptr<Tab> make_configuration_tab(Screen* screen);

// =============================================================================================================
// SearchBar / Screen
// =============================================================================================================

class SearchBar {
public:
    explicit SearchBar(Screen* screen);

    bool matches(Component* c);
    void expand_matches(const std::vector<Component*>& list);
    bool key_typed(int ch, int key);
    void render(Vec2 mouse, const Window& window);
    void collect(Component* c);
    void toggle();
    bool has_matching_child(Component* c) const;
    bool is_searching() const noexcept { return active && !text_input.input.field.text.empty(); }
    void compile_pattern();
    bool pattern_find(const std::string& text) const;

    adjust::Animation backdrop{adjust::Easing::Linear, 100};
    TextInputBox text_input;
    Screen* screen;
    std::string pattern;
    std::vector<Component*> keywords;
    bool active = false;
};

// PopupTransition
struct Popup {
    adjust::Animation animation{adjust::Easing::EaseOutExpo, 220};
    bool closing = false;
    int64_t close_duration = 220;

    void close() noexcept {
        animation.duration = close_duration;
        closing = true;
    }
    // getProgress(): true once the closing animation reached 0 (the screen closes)
    bool progress() noexcept {
        animation.update(closing ? 0.0f : 1.0f);
        if (!closing || animation.value != 0.0f) {
            return false;
        }
        closing = false;
        return true;
    }
};

class Screen {
public:
    Screen();

    bool layout_dirty = true;
    bool initialized = false;
    bool drag_moved = false;
    std::vector<Tab*> tabs;
    adjust::Animation backdrop{adjust::Easing::Linear, 100};
    Popup fade{};
    Tab* dragged_tab = nullptr;
    Tab* focused_tab = nullptr;
    std::vector<std::unique_ptr<Modifier>> overlays;
    adjust::Animation drag_animation{adjust::Easing::Linear, 130};
    int64_t last_activity = 0;
    int64_t timer48 = 0; // last tab scroll (number rows ignore the wheel for 700 ms)
    SearchBar search_bar;
    float grab_x = 0.0f;
    float grab_y = 0.0f;
    int frame = 0;
    int counter = 0;
    int64_t last_tick = 0;
    bool close_requested = false;
    bool closed = false; // closing fade finished; nothing is drawn until the next on_open()
    bool last_separators = true;

    void refresh_tab(Tab* tab);
    void add_overlay(std::unique_ptr<Modifier> overlay);
    void remove_overlay(Modifier* overlay) noexcept;
    bool key_typed(int ch, int key);
    void on_scroll(Vec2 mouse, float direction);
    bool mouse_clicked(Vec2 mouse, int button, bool pressed);
    void update_screen();
    void on_open();
    void on_close();
    void layout_tab(Tab* tab);
    void render_screen(Vec2 mouse, const Window& window); // false when closed
    float tab_alpha(Tab* tab) const;
    void close_all();
    void collapse_temporary_rows();
    void refresh_all_tabs();
    void layout_all_tabs(const Window& window);
    void update_shown_flags();
    std::vector<Modifier*> all_modifiers();
    void purge_overlays();

private:
    std::vector<std::unique_ptr<Tab>> owned_tabs_;
};

Screen& screen() noexcept;

// "Click GUI" module settings (click_gui_module.cpp)
const config::FloatSetting* click_gui_scale_setting() noexcept;
float click_gui_scale() noexcept;        // HUDModule.getClickGuiScale()
bool click_gui_separators() noexcept;    // HUD > Click GUI > Separators

// Shared drawing helpers
void draw_dim(adjust::Animation& anim, const Window& window, float target); // DimTransition.draw
void draw_accent_gradient(float x, float y, float width, float height, float a, float b); // RenderUtil
void draw_check_box(const Box& bounds, float row_y, const std::string& text, Argb text_color, Argb box_fill);

} // namespace slinky::modules::adjust_clickgui::detail
