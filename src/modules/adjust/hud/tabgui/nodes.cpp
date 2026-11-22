#include "modules/adjust/hud/tabgui/nodes.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "config/profile_manager.hpp"
#include "events/events.hpp"
#include "events/module_manager.hpp"
#include "minecraft/game_helpers.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/adjust/hud/tabgui/setting_capture.hpp"
#include "util/keycodes.hpp"

namespace slinky::modules::adjust_hud::tabgui {

namespace {

using adjust::Animation;
using adjust::Argb;
using adjust::Easing;

constexpr const char* kBold = ADJUST_FMT "l"; // ChatColor.BOLD
constexpr const adjust::Font& kFont = adjust::kTahoma10; // Fonts.TAHOMA_10

// NodeKey kinds of generated nodes
enum : int32_t {
    kKeyNone = 0,
    kKeyCategory,
    kKeyModule,
    kKeyKeybind,
    kKeyKeybindAdd,
    kKeySection,
    kKeyBool,
    kKeyFloat,
    kKeyRange,
    kKeyMode,
    kKeyColor,
    kKeyColorOption,
    kKeyList,
};

constinit TreeContext g_tree{};

bool key_down(int code) noexcept {
    const events::GameContext* game = g_tree.game;
    return game != nullptr && game->key_states != nullptr && code > 0 && code < input::KEY_COUNT &&
           (game->key_states[code] & 1u) != 0;
}

events::ModuleManager* manager() noexcept {
    return g_tree.game != nullptr ? g_tree.game->module_manager : nullptr;
}

void mark_dirty() noexcept {
    config::mark_profile_dirty(true);
}

void client_chat(const std::string& message) noexcept {
    events::GameContext* game = g_tree.game;
    if (game == nullptr || game->jni == nullptr || game->jni->env == nullptr) {
        return;
    }
    minecraft::print_client_chat(game->jni, "%s", message.c_str());
    if (game->jni->env->ExceptionCheck()) {
        game->jni->env->ExceptionClear();
    }
}

const char* safe(const char* s) noexcept {
    return s != nullptr ? s : "";
}

std::string printf_value(const char* format, float value) {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), format, static_cast<double>(value));
    return buf;
}

// ---- clipboard (ClipboardUtil) ---------------------------------------------------------------------------------

void set_clipboard(const std::string& text) noexcept {
    if (OpenClipboard(nullptr) == 0) {
        return;
    }
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (mem != nullptr) {
        void* dst = GlobalLock(mem);
        if (dst != nullptr) {
            std::memcpy(dst, text.c_str(), text.size() + 1);
            GlobalUnlock(mem);
            if (SetClipboardData(CF_TEXT, mem) == nullptr) {
                GlobalFree(mem);
            }
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

std::string get_clipboard() {
    std::string out;
    if (OpenClipboard(nullptr) == 0) {
        return out;
    }
    HANDLE data = GetClipboardData(CF_TEXT);
    if (data != nullptr) {
        const char* text = static_cast<const char*>(GlobalLock(data));
        if (text != nullptr) {
            out = text;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return out;
}

// Long.parseLong(text, 16): optional sign, hex digits, 64-bit range. Returns false on a NumberFormatException.
bool parse_hex_long(const std::string& text, long long& out) noexcept {
    size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
        negative = text[i] == '-';
        ++i;
    }
    if (i >= text.size()) {
        return false;
    }
    unsigned long long value = 0;
    const unsigned long long limit = negative ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A' + 10);
        } else {
            return false;
        }
        if (value > (limit - digit) / 16u) {
            return false;
        }
        value = value * 16u + digit;
    }
    out = negative ? static_cast<long long>(0ull - value) : static_cast<long long>(value);
    return true;
}

std::string trim(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') {
        ++a;
    }
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') {
        --b;
    }
    return s.substr(a, b - a);
}

// ---- sync helpers ----------------------------------------------------------------------------------------------

void pool_nodes(NodeList& list, NodeList& pool) {
    for (auto& node : list) {
        if (node) {
            pool.push_back(std::move(node));
        }
    }
    list.clear();
}

std::unique_ptr<GuiNode> take(NodeList& pool, NodeKey key) noexcept {
    for (auto& node : pool) {
        if (node && node->key == key) {
            return std::move(node);
        }
    }
    return nullptr;
}

struct SelectionMemo {
    NodeKey key{};
    bool valid = false;
};

SelectionMemo remember_selection(GuiNode& container) noexcept {
    SelectionMemo memo{};
    NodeList& list = container.raw_children();
    const int index = container.state.selected_index;
    if (index >= 0 && static_cast<size_t>(index) < list.size() && list[static_cast<size_t>(index)]) {
        memo.key = list[static_cast<size_t>(index)]->key;
        memo.valid = true;
    }
    return memo;
}

// Keeps the cursor on the same node after a re-sync; if it vanished, falls back to the first row exactly like
// TabGui.layoutNode does for a child that became hidden.
void restore_selection(GuiNode& container, const SelectionMemo& memo) {
    if (!memo.valid) {
        return;
    }
    NodeList& list = container.raw_children();
    NodeState& st = container.state;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i]->key == memo.key) {
            st.selected_index = static_cast<int>(i);
            return;
        }
    }
    st.selected_index = 0;
    if (st.expanded) {
        st.expanded = !list.empty() && !list[0]->children().empty();
    } else {
        st.expanded = false;
    }
}

} // namespace

TreeContext& tree_context() noexcept {
    return g_tree;
}

bool shift_down() noexcept {
    return key_down(input::KEY_LSHIFT) || key_down(input::KEY_RSHIFT);
}

bool lcontrol_down() noexcept {
    return key_down(input::KEY_LCONTROL);
}

// =============================================================================================================
// NodeState
// =============================================================================================================

Animation& NodeState::expand_anim() noexcept {
    if (!has_expand_animation) {
        expand_animation = Animation(Easing::EaseOutExpo, 0);
        has_expand_animation = true;
    }
    return expand_animation;
}

float NodeState::get_row_offset(int target) noexcept {
    if (!has_row_animation) {
        if (target == 0) {
            return 0.0f;
        }
        row_animation = Animation(Easing::EaseOutExpo, 115);
        has_row_animation = true;
    }
    return row_animation.update(static_cast<float>(target));
}

int NodeState::get_selected_index() noexcept {
    const int size = static_cast<int>(node->children().size());
    int index = selected_index;
    if (index < 0) {
        index = size - 1;
        selected_index = index;
    }
    if (size <= index) {
        selected_index = 0;
        return 0;
    }
    return index;
}

float NodeState::get_expand_progress(bool fast) noexcept {
    int64_t duration = fast ? 150 : 210;
    Animation& anim = expand_anim();
    if (expanded) {
        duration = 380;
    }
    anim.duration = duration;
    return anim.update(expanded ? 1.0f : 0.0f);
}

// =============================================================================================================
// GuiNode / LabelNode
// =============================================================================================================

void GuiNode::add_child(std::unique_ptr<GuiNode> child) {
    if (!child) {
        return;
    }
    child->state.parent = this;
    children_.push_back(std::move(child));
}

Argb LabelNode::fixed_target_color(float target) noexcept {
    const int64_t now = adjust::now_ms();
    if (!has_hover_animation_ || now - hover_animation_.last_call >= 501) {
        hover_animation_ = Animation(Easing::Linear, 100);
        hover_animation_.value = target;
        hover_animation_.restart();
        has_hover_animation_ = true;
    }
    const int c = static_cast<int>(120.0f * hover_animation_.update(target) + 120.0f);
    return adjust::argb(c, c, c);
}

Argb LabelNode::get_text_color(bool hovered, bool active) noexcept {
    if (g_tree.laying_out_nodes) {
        return 0xFFFFFFFFu;
    }
    return fixed_target_color(active ? 1.0f : (hovered ? 0.56f : 0.0f));
}

void LabelNode::render(DrawList& dl, const TabGuiSettings& /*settings*/, bool hovered, float x, float y,
                       float /*width*/) {
    dl.set_color(get_text_color(hovered, false));
    dl.text(kFont, get_label(), x + 2.0f, y + 3.0f);
}

// =============================================================================================================
// NumberSource (NumberSetting increment / decrement / format)
// =============================================================================================================

float NumberSource::rounding_step() const noexcept {
    const float s = step();
    if (s > 0.0f) {
        return s;
    }
    const float range = max() - min();
    return range > 0.0f ? range / 100.0f : 1.0f; // continuous OpenSlinky++ slider (step 0): 1 % of the range
}

void NumberSource::set_value(float v) noexcept {
    v = std::clamp(v, min(), max());
    if (v != value()) {
        set(v);
        mark_dirty();
    }
}

void NumberSource::apply_step(float next, float old) noexcept {
    const double st = static_cast<double>(rounding_step());
    double rounded = static_cast<double>(static_cast<int>(std::floor(static_cast<double>(next) / st + 0.5))) * st;
    if (rounded >= static_cast<double>(max())) {
        rounded = static_cast<double>(max());
    }
    if (!(static_cast<double>(min()) < rounded)) {
        rounded = static_cast<double>(min());
    }
    if (static_cast<float>(rounded) != old) {
        set(static_cast<float>(rounded));
        mark_dirty();
    }
}

void NumberSource::increment() noexcept {
    const float old = value();
    const float effective = rounding_step() * (shift_down() ? 10.0f : 1.0f); // getEffectiveStep
    apply_step(std::min(old + effective, max()), old);
    if (wrap() && old == value()) {
        set_value(min());
    }
}

void NumberSource::decrement() noexcept {
    const float old = value();
    const float effective = rounding_step() * (shift_down() ? 10.0f : 1.0f);
    apply_step(std::max(old - effective, min()), old);
    if (wrap() && old == value()) {
        set_value(max());
    }
}

std::string format_decimal(double value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0.0 ? "\xE2\x88\x9E" : "-\xE2\x88\x9E";
    }
    // DecimalFormat("#.##"): HALF_EVEN to two decimals, no trailing zeros, no integer digit when it is zero
    const auto cents_d = std::nearbyint(value * 100.0);
    const bool negative = cents_d < 0.0;
    const auto cents = static_cast<unsigned long long>(negative ? -cents_d : cents_d);
    const unsigned long long whole = cents / 100u;
    const auto frac = static_cast<unsigned>(cents % 100u);
    std::string out = negative ? "-" : "";
    if (whole != 0 || frac == 0) {
        out += std::to_string(whole);
    }
    if (frac != 0) {
        out += '.';
        out += static_cast<char>('0' + frac / 10u);
        if (frac % 10u != 0) {
            out += static_cast<char>('0' + frac % 10u);
        }
    }
    return out;
}

namespace {

class FloatSource final : public NumberSource {
public:
    explicit FloatSource(config::FloatSetting* s) noexcept : s_(s) {}
    float value() const noexcept override { return s_->value; }
    float min() const noexcept override { return s_->min_val; }
    float max() const noexcept override { return s_->max_val; }
    float step() const noexcept override { return s_->step; }
    void set(float v) noexcept override { s_->value = v; }
    std::string format() const override {
        const float shown = s_->display_scale != 0.0f ? s_->value * s_->display_scale : s_->value;
        return printf_value(s_->format != nullptr ? s_->format : "%0.2f", shown);
    }

private:
    config::FloatSetting* s_;
};

// One end of a config::RangeSetting (no Adjust equivalent): keeps min <= max.
class RangeEndSource final : public NumberSource {
public:
    RangeEndSource(config::RangeSetting* s, bool is_max) noexcept : s_(s), is_max_(is_max) {}
    float value() const noexcept override { return is_max_ ? s_->value.max : s_->value.min; }
    float min() const noexcept override { return s_->min_val; }
    float max() const noexcept override { return s_->max_val; }
    float step() const noexcept override { return s_->step; }
    void set(float v) noexcept override {
        if (is_max_) {
            s_->value.max = std::max(v, s_->value.min);
        } else {
            s_->value.min = std::min(v, s_->value.max);
        }
    }
    std::string format() const override {
        const float v = value();
        const float shown = s_->display_scale != 0.0f ? v * s_->display_scale : v;
        return printf_value(s_->single_format != nullptr ? s_->single_format : "%0.2f", shown);
    }

private:
    config::RangeSetting* s_;
    bool is_max_;
};

// =============================================================================================================
// component.CategoryComponent
// =============================================================================================================

class ModuleComponent;

class CategoryComponent final : public LabelNode {
public:
    CategoryComponent(events::ModuleCategory category, const char* name) : LabelNode(name), category_(category) {
        key = NodeKey{kKeyCategory, nullptr};
    }

    Argb get_text_color(bool hovered, bool /*active*/) noexcept override {
        if (g_tree.laying_out_nodes) {
            return 0xFFFFFFFFu;
        }
        return fixed_target_color(hovered ? 1.0f : 0.0f);
    }

    NodeList& children() override {
        if (synced_ != g_tree.generation) {
            synced_ = g_tree.generation;
            try {
                sync();
            } catch (...) {
            }
        }
        return children_;
    }

private:
    void sync();

    events::ModuleCategory category_;
    uint64_t synced_ = 0;
    std::vector<events::Module*> desired_;
};

// =============================================================================================================
// component.KeybindKeyComponent / KeybindAddComponent / KeybindNode
// =============================================================================================================

void set_module_bind(events::Module* mod, int key_code) noexcept {
    events::ModuleManager* mgr = manager();
    if (mgr == nullptr || mod == nullptr) {
        return;
    }
    mgr->set_module_keybind(mod, static_cast<uint32_t>(key_code));
}

class KeybindKeyComponent final : public LabelNode {
public:
    KeybindKeyComponent(events::Module* mod, int bound_key) : LabelNode(input::get_key_name(bound_key)),
                                                              module_(mod), bound_key_(bound_key) {}

    bool is_listening() const override { return listening_; }

    bool key_pressed(int key) override {
        if (key == input::KEY_ENTER) {
            listening_ = true;
            label_ = "Press any key...";
            return true;
        }
        if (!listening_) {
            return false;
        }
        // bind.removeKey(key); then add the new key unless it is Delete / Backspace (one key per module here)
        if (key != input::KEY_DEL && key != input::KEY_BACKSPACE) {
            set_module_bind(module_, key);
        } else {
            set_module_bind(module_, 0);
        }
        mark_dirty();
        listening_ = false; // the KeybindNode rebuild that follows a bind change recreates this row anyway
        update_label();
        return true;
    }

    bool mouse_button(bool pressed, int button) override {
        if (!pressed || !listening_) {
            return false;
        }
        set_module_bind(module_, button);
        mark_dirty();
        listening_ = false;
        update_label();
        return true;
    }

private:
    void update_label() { label_ = listening_ ? "Press any key..." : input::get_key_name(bound_key_); }

    events::Module* module_;
    int bound_key_;
    bool listening_ = false;
};

class KeybindAddComponent final : public LabelNode {
public:
    explicit KeybindAddComponent(events::Module* mod) : LabelNode("Add keybind..."), module_(mod) {
        key = NodeKey{kKeyKeybindAdd, mod};
    }

    bool is_listening() const override { return listening_; }

    bool key_pressed(int key) override {
        if (key != input::KEY_ENTER) {
            if (!listening_) {
                return false;
            }
            set_module_bind(module_, key); // OpenSlinky++ modules hold one key: the new key replaces the old one
            mark_dirty();
            listening_ = false;
            label_ = "Add keybind...";
            return true;
        }
        const bool was_listening = listening_;
        listening_ = !was_listening;
        label_ = was_listening ? "Add keybind..." : "Press any key...";
        return true;
    }

    bool mouse_button(bool pressed, int button) override {
        if (!pressed || !listening_) {
            return false;
        }
        set_module_bind(module_, button);
        mark_dirty();
        listening_ = false;
        label_ = "Add keybind...";
        return true;
    }

private:
    events::Module* module_;
    bool listening_ = false;
};

class KeybindNode final : public LabelNode {
public:
    explicit KeybindNode(events::Module* mod) : module_(mod) { key = NodeKey{kKeyKeybind, mod}; }

    void render(DrawList& dl, const TabGuiSettings& /*settings*/, bool hovered, float x, float y,
                float /*width*/) override {
        children(); // keeps the text in sync with the bind
        const bool active = module_->keybind != 0;
        dl.set_color(get_text_color(hovered, active));
        dl.text(kFont, label_, x + 2.0f, y + 3.0f);
    }

    // BindChangeEvent -> KeybindNode.onBindChanged: rebuilt whenever the module's key changes.
    NodeList& children() override {
        if (!built_ || module_->keybind != built_key_) {
            try {
                rebuild();
            } catch (...) {
            }
        }
        return children_;
    }

private:
    void rebuild() {
        built_ = true;
        built_key_ = module_->keybind;
        NodeList pool;
        pool_nodes(children_, pool);
        if (built_key_ != 0) {
            add_child(std::make_unique<KeybindKeyComponent>(module_, built_key_));
        }
        std::unique_ptr<GuiNode> add_entry = take(pool, NodeKey{kKeyKeybindAdd, module_});
        if (!add_entry) {
            add_entry = std::make_unique<KeybindAddComponent>(module_);
        }
        add_child(std::move(add_entry));

        const size_t size = children_.size();
        std::string summary;
        if (size == 1) {
            summary = "None";
        } else if (size == 2) {
            summary = input::get_key_name(built_key_);
        } else {
            summary = std::to_string(size - 1) + "...";
        }
        label_ = std::string("Keybind: ") + kBold + summary;
    }

    events::Module* module_;
    bool built_ = false;
    int32_t built_key_ = 0;
};

// =============================================================================================================
// component.BooleanComponent / ExpandComponent
// =============================================================================================================

class BooleanComponent final : public LabelNode {
public:
    explicit BooleanComponent(config::BoolSetting* s) : LabelNode(safe(s->label)), s_(s) {
        key = NodeKey{kKeyBool, s};
    }

    bool key_pressed(int key) override {
        if (key == input::KEY_RIGHT || key == input::KEY_ENTER) {
            s_->value = !s_->value;
            mark_dirty();
            return true;
        }
        return false;
    }

    void render(DrawList& dl, const TabGuiSettings& /*settings*/, bool hovered, float x, float y,
                float /*width*/) override {
        const bool on = s_->value;
        dl.set_color(get_text_color(hovered, on));
        dl.text(kFont, label_ + ": " + kBold + (on ? "On" : "Off"), x + 2.0f, y + 3.0f);
    }

private:
    config::BoolSetting* s_;
};

// A section header (gui::widgets::render_section_header) groups the settings that follow it, like ExpandSetting.
class ExpandComponent final : public LabelNode {
public:
    explicit ExpandComponent(const char* title) : LabelNode(safe(title)) { key = NodeKey{kKeySection, title}; }

    void render(DrawList& dl, const TabGuiSettings& /*settings*/, bool hovered, float x, float y,
                float /*width*/) override {
        dl.set_color(get_text_color(hovered, true)); // enabledPredicate == null -> active
        dl.text(kFont, label_ + "...", x + 2.0f, y + 3.0f);
    }
};

// =============================================================================================================
// component.NumberComponent / SliderComponent (+ the RangeSetting value row)
// =============================================================================================================

class NumberComponent final : public LabelNode {
public:
    NumberComponent(std::string label, NumberSource* source, std::unique_ptr<NumberSource> owned = nullptr,
                    const adjust::ColorOption* hide_unless_plain = nullptr)
        : LabelNode(std::move(label)), source_(source), owned_(std::move(owned)),
          hide_unless_plain_(hide_unless_plain) {}

    bool is_hidden() override {
        return hide_unless_plain_ != nullptr && hide_unless_plain_->current_mode() != adjust::ColorMode::Plain;
    }

    void render(DrawList& dl, const TabGuiSettings& /*settings*/, bool /*hovered*/, float x, float y,
                float /*width*/) override {
        dl.set_color(g_tree.laying_out_nodes ? 0xFFFFFFFFu : fixed_target_color(1.0f));
        dl.text(kFont, label_ + ": " + kBold + source_->format(), x + 2.0f, y + 3.0f);
    }

private:
    NumberSource* source_;
    std::unique_ptr<NumberSource> owned_;
    const adjust::ColorOption* hide_unless_plain_;
};

class SliderComponent final : public LabelNode {
public:
    SliderComponent(std::string label, NumberSource* source) : LabelNode(std::move(label)), source_(source) {}

    bool is_slider() const override { return true; }

    bool key_pressed(int key) override {
        if (key == input::KEY_HOME) {
            source_->set_value(source_->min());
            return true;
        }
        if (key == input::KEY_END) {
            source_->set_value(source_->max());
            return true;
        }
        if (key == input::KEY_DOWN) {
            source_->decrement();
            return true;
        }
        if (key == input::KEY_UP || key == input::KEY_RIGHT) {
            source_->increment();
            return true;
        }
        return key == input::KEY_LSHIFT || key == input::KEY_RSHIFT;
    }

    void render(DrawList& dl, const TabGuiSettings& /*settings*/, bool hovered, float x, float y,
                float width) override {
        const float track_width = width - 3.0f;
        const float value = source_->value();
        const float min = source_->min();
        const float max = source_->max();
        const float fraction = max > min ? (value - min) / (max - min) : 0.0f;
        const float fill = track_width * fraction;

        float shown = fill;
        if (g_tree.laying_out_nodes) {
            shown = fill;
        } else if (!has_fill_animation_) {
            if (last_fill_ != -1.0f && last_fill_ != fill) {
                fill_animation_ = Animation(Easing::EaseOutSine, 110);
                fill_animation_.value = fill;
                fill_animation_.update(fill);
                has_fill_animation_ = true;
                shown = fill_animation_.value;
            }
        } else {
            fill_animation_.update(fill);
            shown = fill_animation_.value;
        }
        last_fill_ = shown;

        dl.set_color(adjust::argb(47, 47, 49, 255));
        dl.rect(x + 1.5f, y + 6.5f, track_width, 0.5f);
        dl.set_color(hud_color()); // HUDModule.animatedColor != null ? animatedColor : colorSetting.getColor()
        dl.rect(x + 1.5f, y + 6.5f, shown, 0.5f);

        dl.set_color(get_text_color(hovered, false));
        dl.text(kFont, source_->format(), x + 2.0f, y + 2.5f);
    }

private:
    NumberSource* source_;
    Animation fill_animation_{};
    bool has_fill_animation_ = false;
    float last_fill_ = -1.0f;
};

// config::RangeSetting value row: "Label: §l<min> - <max>" with a Min and a Max number row.
class RangeComponent final : public LabelNode {
public:
    explicit RangeComponent(config::RangeSetting* s)
        : LabelNode(safe(s->label)), s_(s), min_(s, false), max_(s, true) {
        key = NodeKey{kKeyRange, s};
        auto min_row = std::make_unique<NumberComponent>("Min", &min_);
        min_row->add_child(std::make_unique<SliderComponent>("Min", &min_));
        add_child(std::move(min_row));
        auto max_row = std::make_unique<NumberComponent>("Max", &max_);
        max_row->add_child(std::make_unique<SliderComponent>("Max", &max_));
        add_child(std::move(max_row));
    }

    void render(DrawList& dl, const TabGuiSettings& /*settings*/, bool /*hovered*/, float x, float y,
                float /*width*/) override {
        dl.set_color(g_tree.laying_out_nodes ? 0xFFFFFFFFu : fixed_target_color(1.0f));
        dl.text(kFont, label_ + ": " + kBold + format(), x + 2.0f, y + 3.0f);
    }

private:
    std::string format() const {
        const float scale = s_->display_scale != 0.0f ? s_->display_scale : 1.0f;
        char buf[96]{};
        if (s_->value.min == s_->value.max && s_->single_format != nullptr) {
            std::snprintf(buf, sizeof(buf), s_->single_format, static_cast<double>(s_->value.min * scale));
        } else {
            std::snprintf(buf, sizeof(buf), s_->range_format != nullptr ? s_->range_format : "%0.2f - %0.2f",
                          static_cast<double>(s_->value.min * scale), static_cast<double>(s_->value.max * scale));
        }
        return buf;
    }

    config::RangeSetting* s_;
    RangeEndSource min_;
    RangeEndSource max_;
};

// =============================================================================================================
// component.ModeComponent / ModeOptionComponent
// =============================================================================================================

class ModeOptionComponent final : public LabelNode {
public:
    ModeOptionComponent(const char* label, config::ModeSetting* s, int32_t index)
        : LabelNode(safe(label)), s_(s), index_(index) {}

    bool key_pressed(int key) override {
        if (key == input::KEY_RIGHT || key == input::KEY_ENTER) {
            if (s_->value != index_) {
                s_->value = index_;
                mark_dirty();
            }
            return true;
        }
        return false;
    }

    void render(DrawList& dl, const TabGuiSettings& /*settings*/, bool hovered, float x, float y,
                float /*width*/) override {
        dl.set_color(get_text_color(hovered, s_->value == index_));
        dl.text(kFont, label_, x + 2.0f, y + 3.0f);
    }

private:
    config::ModeSetting* s_;
    int32_t index_;
};

class ModeComponent final : public LabelNode {
public:
    ModeComponent(std::string label, config::ModeSetting* s) : LabelNode(std::move(label)), s_(s) {
        key = NodeKey{kKeyMode, s};
        if (s->options != nullptr) {
            for (uint32_t i = 0; i < s->option_count; ++i) {
                add_child(std::make_unique<ModeOptionComponent>(s->options[i], s, static_cast<int32_t>(i)));
            }
        }
    }

    bool key_pressed(int key) override {
        if (key != input::KEY_ENTER) {
            return false;
        }
        const auto count = static_cast<int32_t>(s_->option_count);
        if (count <= 0) {
            return true;
        }
        const int32_t index = (s_->value >= 0 && s_->value < count) ? s_->value : -1; // options.indexOf(value)
        s_->value = index == count - 1 ? 0 : index + 1;
        mark_dirty();
        return true;
    }

    void render(DrawList& dl, const TabGuiSettings& /*settings*/, bool hovered, float x, float y,
                float /*width*/) override {
        dl.set_color(get_text_color(hovered, true));
        const char* name = "";
        if (s_->options != nullptr && s_->value >= 0 && static_cast<uint32_t>(s_->value) < s_->option_count) {
            name = safe(s_->options[s_->value]);
        }
        dl.text(kFont, label_ + ": " + kBold + name, x + 2.0f, y + 3.0f);
    }

private:
    config::ModeSetting* s_;
};

// =============================================================================================================
// component.ColorComponent / ColorChannel / ColorSliderComponent / ColorModeOptionComponent
// =============================================================================================================

class ColorComponent;

class ChannelSource final : public NumberSource {
public:
    ChannelSource() = default;
    void bind(ColorComponent* owner, int channel) noexcept {
        owner_ = owner;
        channel_ = channel;
    }
    float value() const noexcept override;
    float min() const noexcept override { return 0.0f; }
    float max() const noexcept override { return 1.0f; }
    float step() const noexcept override { return 0.01f; }
    bool wrap() const noexcept override { return true; } // setting.wrap = true
    void set(float v) noexcept override;
    std::string format() const override { return format_decimal(static_cast<double>(value())); }

private:
    ColorComponent* owner_ = nullptr;
    int channel_ = 0;
};

class ColorComponent final : public LabelNode {
public:
    // `option` != null: an adjust::ColorOption (colour + Plain/Rainbow/Main Color mode), shown with the Type row.
    ColorComponent(config::ColorSetting* color, adjust::ColorOption* option)
        : LabelNode(safe(color->label)), color_(color), option_(option) {
        key = option != nullptr ? NodeKey{kKeyColorOption, color} : NodeKey{kKeyColor, color};
        resync();
        if (option != nullptr) {
            add_child(std::make_unique<ModeComponent>("Type", &option->mode));
        }
        static constexpr const char* kChannelNames[3] = {"Hue", "Saturation", "Brightness"};
        for (int i = 0; i < 3; ++i) {
            sources_[i].bind(this, i);
            auto row = std::make_unique<NumberComponent>(kChannelNames[i], &sources_[i], nullptr, option);
            row->add_child(std::make_unique<SliderComponent>(kChannelNames[i], &sources_[i]));
            add_child(std::move(row));
        }
    }

    float channel(int i) const noexcept { return hsb_[i]; }

    // ColorSliderComponent listener: HSB sliders -> colour.
    void set_channel(int i, float v) noexcept {
        hsb_[i] = v;
        const Argb rgb = adjust::hsb_to_rgb(hsb_[0], hsb_[1], hsb_[2]);
        color_->value = adjust::to_color3f(rgb);
        synced_ = adjust::from_color3f(color_->value);
        has_synced_ = true;
    }

    NodeList& children() override {
        resync();
        return children_;
    }

    bool key_pressed(int key) override {
        if (key == input::KEY_C) {
            if (lcontrol_down()) {
                set_clipboard(hex());
                client_chat(std::string("Copied " ADJUST_FMT "f") + label_ + ADJUST_FMT "7 to your clipboard.");
                return true;
            }
        } else if (key == input::KEY_V) {
            if (lcontrol_down()) {
                std::string pasted = get_clipboard();
                pasted.erase(std::remove(pasted.begin(), pasted.end(), '#'), pasted.end());
                pasted = trim(pasted);
                if (pasted == hex()) {
                    return true;
                }
                long long value = 0;
                if (!parse_hex_long(pasted, value)) {
                    client_chat("Invalid hex value.");
                    return true;
                }
                // new Color((int) value, hasAlpha = false)
                const auto rgb = static_cast<Argb>(static_cast<uint32_t>(value) & 0x00FFFFFFu) | 0xFF000000u;
                color_->value = adjust::to_color3f(rgb);
                mark_dirty();
                client_chat(std::string("Pasted " ADJUST_FMT "f") + pasted + ADJUST_FMT "7 into " ADJUST_FMT "f" +
                            label_ + ADJUST_FMT "7.");
                return true;
            }
        }
        return false;
    }

    void render(DrawList& dl, const TabGuiSettings& /*settings*/, bool /*hovered*/, float x, float y,
                float /*width*/) override {
        resync();
        dl.set_color(get_text_color(false, true));
        const float text_x = x + 2.0f;
        const float text_y = y + 3.0f;
        const std::string text = label_ + ": ";
        dl.text(kFont, text, text_x, text_y);
        const float swatch_x = text_x + kFont.width(text);
        const Argb shown = option_ != nullptr ? option_->get() : adjust::from_color3f(color_->value);
        dl.set_color((shown & 0x00FFFFFFu) | 0xFF000000u);
        dl.rect(swatch_x, y + 2.0f, 4.0f, 4.0f);
    }

private:
    // Integer.toHexString(setting.getValue().getRGB())
    std::string hex() const {
        const Argb raw = option_ != nullptr ? option_->plain() : adjust::from_color3f(color_->value);
        char buf[16]{};
        std::snprintf(buf, sizeof(buf), "%x", static_cast<unsigned>(raw));
        return buf;
    }

    // ColorComponent.syncSliders: colour changed elsewhere -> HSB sliders.
    void resync() noexcept {
        const Argb now = adjust::from_color3f(color_->value);
        if (!has_synced_ || now != synced_) {
            adjust::rgb_to_hsb(now, hsb_);
            synced_ = now;
            has_synced_ = true;
        }
    }

    config::ColorSetting* color_;
    adjust::ColorOption* option_;
    float hsb_[3]{};
    Argb synced_ = 0;
    bool has_synced_ = false;
    ChannelSource sources_[3];
};

float ChannelSource::value() const noexcept {
    return owner_ != nullptr ? owner_->channel(channel_) : 0.0f;
}

void ChannelSource::set(float v) noexcept {
    if (owner_ != nullptr) {
        owner_->set_channel(channel_, v);
    }
}

// adjust::ColorOption that owns `mode`, if `mode` is the Plain/Rainbow/Main Color dropdown of one.
adjust::ColorOption* color_option_of(config::ModeSetting* mode) noexcept {
    if (mode == nullptr || mode->options != adjust::kColorModeOptions || mode->option_count != 3) {
        return nullptr;
    }
    return reinterpret_cast<adjust::ColorOption*>(reinterpret_cast<char*>(mode) -
                                                  offsetof(adjust::ColorOption, mode));
}

// =============================================================================================================
// component.StringListComponent / StringListEditorComponent (config::DynamicListSetting)
// =============================================================================================================

size_t list_count(const config::DynamicListSetting* s) noexcept {
    return s->get_count != nullptr ? s->get_count(s->ctx) : 0;
}

const char* list_name(const config::DynamicListSetting* s, size_t i) noexcept {
    return s->get_name != nullptr ? safe(s->get_name(s->ctx, i)) : "";
}

// TextInput.setWidth: wraps the entry at (90 - 8) units (character wrap).
std::vector<std::string> wrap_entry(const char* text) {
    std::vector<std::string> lines;
    constexpr float kWrap = 90.0f - 8.0f;
    std::string line;
    const size_t len = std::strlen(text);
    for (size_t i = 0; i < len;) {
        // keep UTF-8 sequences together
        size_t n = 1;
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead >= 0xF0) {
            n = 4;
        } else if (lead >= 0xE0) {
            n = 3;
        } else if (lead >= 0xC0) {
            n = 2;
        }
        n = std::min(n, len - i);
        std::string candidate = line;
        candidate.append(text + i, n);
        if (!line.empty() && kFont.width(candidate) > kWrap) {
            lines.push_back(line);
            line.assign(text + i, n);
        } else {
            line = std::move(candidate);
        }
        i += n;
    }
    lines.push_back(line);
    return lines;
}

class StringListEditorComponent final : public LabelNode {
public:
    explicit StringListEditorComponent(config::DynamicListSetting* s)
        : LabelNode(std::string(safe(s->label)) + ":"), s_(s), scroll_(Easing::EaseOutExpo, 150) {}

    // Focusing a text field needs a text-input hook (see report): the row only shows the entries.
    bool key_pressed(int key) override { return key == input::KEY_RIGHT || key == input::KEY_ENTER; }

    void render(DrawList& dl, const TabGuiSettings& settings, bool /*hovered*/, float x, float y,
                float width) override {
        const size_t count = list_count(s_);
        std::vector<std::vector<std::string>> fields;
        fields.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            fields.push_back(wrap_entry(list_name(s_, i)));
        }

        const int first = std::max(selected_ - 4, 0);
        constexpr float kRowBase = 8.5f;
        float hidden_above = 0.0f;
        float visible = 0.0f;
        for (size_t i = 0; i < fields.size(); ++i) {
            const size_t lines = fields[i].size();
            const float row_height =
                lines >= 2 ? static_cast<float>(lines - 1) * 7.5f + kRowBase : kRowBase;
            if (static_cast<size_t>(first) > i) {
                hidden_above += row_height;
            } else {
                visible += row_height;
            }
            if (static_cast<int>(i) - first >= 8) {
                break;
            }
        }
        const float panel_height = visible + static_cast<float>(settings.row_height);
        if (!g_tree.laying_out_nodes) {
            dl.set_color(settings.owner);
            dl.rect(x, y, width, panel_height);
        }
        dl.set_color(0xFFFFFFFFu);
        dl.text(kFont, label_, x + 2.0f, y + 3.0f);
        if (!g_tree.laying_out_nodes) {
            dl.push_clip(x, y + 8.0f, width, panel_height - kRowBase);
            scroll_.update(hidden_above);
            draw_fields(dl, fields, first, x + 2.0f, (y + 11.0f) - scroll_.value);
            dl.pop_clip();
        } else {
            draw_fields(dl, fields, first, x + 2.0f, (y + 11.0f) - scroll_.value);
        }
    }

private:
    // TextInput.draw of unfocused fields: "..." placeholder (grey 130) or the text in (149, 149, 149).
    void draw_fields(DrawList& dl, const std::vector<std::vector<std::string>>& fields, int first, float x,
                     float y) const {
        for (size_t i = 0; i < fields.size(); ++i) {
            const std::vector<std::string>& lines = fields[i];
            if (static_cast<int>(i) >= first - 1) {
                const bool empty = lines.size() == 1 && lines[0].empty();
                if (empty) {
                    dl.set_color(adjust::argb(130, 130, 130));
                    dl.text(kFont, "...", x, y);
                } else {
                    dl.set_color(adjust::argb(149, 149, 149));
                    for (size_t l = 0; l < lines.size(); ++l) {
                        dl.text(kFont, lines[l], x, y + static_cast<float>(l) * 7.5f);
                    }
                }
            }
            if (static_cast<size_t>(std::max(selected_, 4) + 4) <= i) {
                return;
            }
            const size_t n = lines.size();
            y += n >= 2 ? static_cast<float>(n - 1) * 7.5f + 8.5f : 8.5f;
        }
    }

    config::DynamicListSetting* s_;
    Animation scroll_;
    int selected_ = 0;
};

class StringListComponent final : public LabelNode {
public:
    explicit StringListComponent(config::DynamicListSetting* s) : LabelNode(safe(s->label)), s_(s) {
        key = NodeKey{kKeyList, s};
        add_child(std::make_unique<StringListEditorComponent>(s));
    }

    void render(DrawList& dl, const TabGuiSettings& /*settings*/, bool /*hovered*/, float x, float y,
                float /*width*/) override {
        dl.set_color(get_text_color(false, true));
        // StringSetting shows " (size/max)"; DynamicListSetting has no maximum, so only the size is shown
        dl.text(kFont, label_ + kBold + " (" + std::to_string(list_count(s_)) + ")", x + 2.0f, y + 3.0f);
    }

private:
    config::DynamicListSetting* s_;
};

// =============================================================================================================
// component.ModuleComponent (+ TabGui.buildSettingNodes on captured settings)
// =============================================================================================================

class ModuleComponent final : public LabelNode {
public:
    explicit ModuleComponent(events::Module* mod) : LabelNode(safe(mod->name)), module_(mod) {
        key = NodeKey{kKeyModule, mod};
    }

    bool key_pressed(int key) override {
        if (key != input::KEY_ENTER) {
            return false;
        }
        if ((module_->flags & events::kModuleFlagNoToggleSwitch) != 0u) {
            return false; // module without an on/off state in OpenSlinky++
        }
        events::ModuleManager* mgr = manager();
        if (mgr == nullptr) {
            return false;
        }
        mgr->request_set_enabled(module_, !module_->enabled);
        mark_dirty();
        return true;
    }

    void render(DrawList& dl, const TabGuiSettings& /*settings*/, bool hovered, float x, float y,
                float /*width*/) override {
        dl.set_color(get_text_color(hovered, module_->enabled));
        dl.text(kFont, label_, x + 2.0f, y + 3.0f);
    }

    NodeList& children() override {
        if (synced_ != g_tree.generation) {
            synced_ = g_tree.generation;
            try {
                sync();
            } catch (...) {
            }
        }
        return children_;
    }

private:
    void sync();

    events::Module* module_;
    uint64_t synced_ = 0;
    std::vector<CapturedItem> captured_;
};

template <typename T, typename... Args>
std::unique_ptr<GuiNode> take_or_make(NodeList& pool, NodeKey key, Args&&... args) {
    std::unique_ptr<GuiNode> node = take(pool, key);
    if (!node) {
        node = std::make_unique<T>(std::forward<Args>(args)...);
    }
    return node;
}

void ModuleComponent::sync() {
    capture_module_settings(module_, captured_);

    // remember the cursor of the module row and of every group before re-generating
    struct GroupMemo {
        NodeKey key;
        SelectionMemo memo;
    };
    std::vector<GroupMemo> group_memos;
    const SelectionMemo top_memo = remember_selection(*this);
    NodeList pool;
    for (auto& child : children_) {
        if (child && child->key.kind == kKeySection) {
            group_memos.push_back(GroupMemo{child->key, remember_selection(*child)});
            pool_nodes(child->raw_children(), pool);
        }
    }
    pool_nodes(children_, pool);

    // colours of adjust::ColorOption are shown inside the ColorOption's own row
    std::vector<const void*> merged_colors;
    for (const CapturedItem& item : captured_) {
        if (item.kind == CaptureKind::Mode) {
            auto* mode = static_cast<config::ModeSetting*>(const_cast<void*>(item.item));
            if (adjust::ColorOption* option = color_option_of(mode)) {
                merged_colors.push_back(&option->color);
            }
        }
    }

    if ((module_->flags & events::kModuleFlagNoKeybind) == 0u) {
        add_child(take_or_make<KeybindNode>(pool, NodeKey{kKeyKeybind, module_}, module_));
    }

    GuiNode* container = this;
    std::vector<GuiNode*> groups;
    for (const CapturedItem& item : captured_) {
        void* ptr = const_cast<void*>(item.item);
        std::unique_ptr<GuiNode> node;
        switch (item.kind) {
            case CaptureKind::Section: {
                auto* title = static_cast<const char*>(item.item);
                std::unique_ptr<GuiNode> group = take_or_make<ExpandComponent>(pool, NodeKey{kKeySection, title}, title);
                container = group.get();
                groups.push_back(group.get());
                add_child(std::move(group));
                continue;
            }
            case CaptureKind::Bool: {
                auto* s = static_cast<config::BoolSetting*>(ptr);
                node = take_or_make<BooleanComponent>(pool, NodeKey{kKeyBool, s}, s);
                break;
            }
            case CaptureKind::Float: {
                auto* s = static_cast<config::FloatSetting*>(ptr);
                node = take(pool, NodeKey{kKeyFloat, s});
                if (!node) {
                    auto source = std::make_unique<FloatSource>(s);
                    NumberSource* raw = source.get();
                    auto row = std::make_unique<NumberComponent>(safe(s->label), raw, std::move(source));
                    row->add_child(std::make_unique<SliderComponent>(safe(s->label), raw));
                    row->key = NodeKey{kKeyFloat, s};
                    node = std::move(row);
                }
                break;
            }
            case CaptureKind::Range: {
                auto* s = static_cast<config::RangeSetting*>(ptr);
                node = take_or_make<RangeComponent>(pool, NodeKey{kKeyRange, s}, s);
                break;
            }
            case CaptureKind::Mode: {
                auto* s = static_cast<config::ModeSetting*>(ptr);
                if (adjust::ColorOption* option = color_option_of(s)) {
                    node = take_or_make<ColorComponent>(pool, NodeKey{kKeyColorOption, &option->color},
                                                        &option->color, option);
                } else {
                    node = take_or_make<ModeComponent>(pool, NodeKey{kKeyMode, s}, safe(s->label), s);
                }
                break;
            }
            case CaptureKind::Color: {
                if (std::find(merged_colors.begin(), merged_colors.end(), item.item) != merged_colors.end()) {
                    continue;
                }
                auto* s = static_cast<config::ColorSetting*>(ptr);
                node = take_or_make<ColorComponent>(pool, NodeKey{kKeyColor, s}, s, nullptr);
                break;
            }
            case CaptureKind::DynamicList: {
                auto* s = static_cast<config::DynamicListSetting*>(ptr);
                node = take_or_make<StringListComponent>(pool, NodeKey{kKeyList, s}, s);
                break;
            }
        }
        if (node) {
            container->add_child(std::move(node));
        }
    }

    restore_selection(*this, top_memo);
    for (GuiNode* group : groups) {
        for (const GroupMemo& gm : group_memos) {
            if (gm.key == group->key) {
                restore_selection(*group, gm.memo);
                break;
            }
        }
    }
}

// =============================================================================================================
// CategoryComponent::sync (TabGui.buildTree: category.modules)
// =============================================================================================================

void CategoryComponent::sync() {
    events::ModuleManager* mgr = manager();
    if (mgr == nullptr) {
        return; // keep the last list (no context)
    }
    const auto cat = static_cast<size_t>(category_);
    if (cat >= static_cast<size_t>(events::ModuleCategory::Count)) {
        return;
    }
    const events::ModuleSpan& span = mgr->categories[cat];
    const events::VisualStyle style = events::get_active_visual_style();
    desired_.clear();
    for (size_t i = 0; i < span.count; ++i) {
        events::Module* mod = span.data != nullptr ? span.data[i] : nullptr;
        if (mod != nullptr && events::module_matches_style(mod, style)) {
            desired_.push_back(mod);
        }
    }

    bool same = desired_.size() == children_.size();
    for (size_t i = 0; same && i < desired_.size(); ++i) {
        same = children_[i] && children_[i]->key.ptr == desired_[i];
    }
    if (same) {
        return;
    }

    const SelectionMemo memo = remember_selection(*this);
    NodeList pool;
    pool_nodes(children_, pool);
    for (events::Module* mod : desired_) {
        add_child(take_or_make<ModuleComponent>(pool, NodeKey{kKeyModule, mod}, mod));
    }
    restore_selection(*this, memo);
}

} // namespace

// =============================================================================================================
// TabGui.buildTree
// =============================================================================================================

std::unique_ptr<GuiNode> make_root() {
    static constexpr const char* kCategoryNames[static_cast<size_t>(events::ModuleCategory::Count)] = {
        "Combat", "Move", "Visual", "Block", "Utility", // ClickGUI tab names
    };
    auto root = std::make_unique<RootNode>();
    for (size_t i = 0; i < static_cast<size_t>(events::ModuleCategory::Count); ++i) {
        root->add_child(std::make_unique<CategoryComponent>(static_cast<events::ModuleCategory>(i), kCategoryNames[i]));
    }
    return root;
}

} // namespace slinky::modules::adjust_hud::tabgui
