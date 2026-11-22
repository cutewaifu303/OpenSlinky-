#pragma once

// Port of Adjust's Tab GUI node tree: com.adjustclient.ui.tabgui.{GuiNode, NodeState, LabelNode, SettingNode,
// RootNode, KeybindNode} and com.adjustclient.ui.tabgui.component.* (Category, Module, Boolean, Number, Slider, Mode,
// ModeOption, Expand, Color, ColorChannel, ColorSlider, ColorModeOption, KeybindAdd, KeybindKey, StringList,
// StringListEditor).
//
// Mapping onto OpenSlinky++:
//   Category            -> events::ModuleCategory (modules of the active visual style only)
//   Module              -> events::Module (toggle = ModuleManager::request_set_enabled)
//   Bind (multi key)    -> Module::keybind (one key; keyboard or mouse button, OpenSlinky++ key codes)
//   BooleanSetting      -> config::BoolSetting            ("On" / "Off")
//   NumberSetting       -> config::FloatSetting            (value text = the setting's printf format)
//   (no equivalent)     <- config::RangeSetting            (value row with "Min" / "Max" number rows)
//   ModeSetting         -> config::ModeSetting
//   ExpandSetting       -> gui::widgets::render_section_header (group of the settings that follow it)
//   ColorSetting        -> config::ColorSetting / adjust::ColorOption (Type row only for ColorOption, no Alpha)
//   StringSetting       -> config::DynamicListSetting       (entries are shown, not editable: no text input hook)
//   Button/Items/Hotbar/EspPreview/BedDefense/Keybind settings: no OpenSlinky++ equivalent (not generated)

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "adjust/kit.hpp"
#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "events/module.hpp"
#include "modules/adjust/hud/tabgui/draw_list.hpp"

namespace slinky::events {
struct GameContext;
class ModuleManager;
}

namespace slinky::modules::adjust_hud::tabgui {

// com.adjustclient.ui.hud.tabgui.TabGuiSettings: HUD > Tab GUI (Enabled / Gradient / Auto Close), column width 49,
// row height 8, background colour (19, 19, 19).
struct TabGuiSettings {
    config::BoolSetting enabled{{}, "Enabled", true, true, {}};
    config::BoolSetting gradient{{}, "Gradient", true, true, {}};
    config::BoolSetting auto_close{{}, "Auto Close", true, true, {}};
    int width = 49;
    int row_height = 8;
    adjust::Argb owner = adjust::argb(19, 19, 19, 255);
};

// Shared state of the tree (Java statics + what the nodes need from the game).
struct TreeContext {
    events::GameContext* game = nullptr; // set before every build / key dispatch
    uint64_t generation = 1;             // bumped once per frame / key poll: dynamic children re-sync once per value
    bool laying_out_nodes = false;       // TabGui.layingOutNodes (measure pass: no animations, colour -1)
};
TreeContext& tree_context() noexcept;

bool shift_down() noexcept;    // Keyboard.isKeyDown(KEY_LSHIFT) || KEY_RSHIFT
bool lcontrol_down() noexcept; // Keyboard.isKeyDown(KEY_LCONTROL)

class GuiNode;
using NodeList = std::vector<std::unique_ptr<GuiNode>>;

// Identity of a generated node, so that re-synced children keep their state (selection, animations).
struct NodeKey {
    int32_t kind = 0;
    const void* ptr = nullptr;
    bool operator==(const NodeKey& o) const noexcept { return kind == o.kind && ptr == o.ptr; }
};

// com.adjustclient.ui.tabgui.NodeState
struct NodeState {
    explicit NodeState(GuiNode* owner) noexcept : node(owner) {}

    GuiNode* node = nullptr;
    GuiNode* parent = nullptr;      // set by GuiNode::add_child (null for the root)
    adjust::Animation row_animation{};
    bool has_row_animation = false; // rowAnimation != null
    adjust::Animation expand_animation{};
    bool has_expand_animation = false; // expandAnimation != null
    int selected_index = 0;
    bool expanded = false;

    adjust::Animation& expand_anim() noexcept; // `if (expandAnimation == null) expandAnimation = new Animation(EASE_OUT_EXPO, 0)`
    float get_row_offset(int target) noexcept;
    int get_selected_index() noexcept;
    float get_expand_progress(bool fast) noexcept;
};

// com.adjustclient.ui.tabgui.GuiNode (+ the KeyPressHandler / MouseInputHandler interfaces as virtuals)
class GuiNode {
public:
    GuiNode() noexcept : state(this) {}
    virtual ~GuiNode() = default;
    GuiNode(const GuiNode&) = delete;
    GuiNode& operator=(const GuiNode&) = delete;

    void add_child(std::unique_ptr<GuiNode> child);

    virtual void render(DrawList& dl, const TabGuiSettings& settings, bool hovered, float x, float y, float width) = 0;
    virtual bool is_hidden() { return false; }
    virtual NodeList& children() { return children_; }

    virtual bool key_pressed(int /*key*/) { return false; }                    // KeyPressHandler
    virtual bool mouse_button(bool /*pressed*/, int /*button*/) { return false; } // MouseInputHandler
    virtual bool is_listening() const { return false; } // KeybindAdd/KeybindKey waiting for a key
    virtual bool is_slider() const { return false; }    // instanceof SliderComponent

    // The child list without re-syncing dynamic children (the Java `children` field).
    NodeList& raw_children() noexcept { return children_; }

    NodeState state;
    NodeKey key{};

protected:
    NodeList children_;
};

// com.adjustclient.ui.tabgui.LabelNode
class LabelNode : public GuiNode {
public:
    LabelNode() = default;
    explicit LabelNode(std::string label) : label_(std::move(label)) {}

    virtual std::string_view get_label() { return label_; }
    virtual adjust::Argb get_text_color(bool hovered, bool active) noexcept;
    void render(DrawList& dl, const TabGuiSettings& settings, bool hovered, float x, float y, float width) override;

protected:
    adjust::Argb fixed_target_color(float target) noexcept; // shared body of the hover-animation colour

    std::string label_;
    adjust::Animation hover_animation_{};
    bool has_hover_animation_ = false;
};

class RootNode final : public GuiNode {
public:
    void render(DrawList&, const TabGuiSettings&, bool, float, float, float) override {}
};

// ---- numbers -------------------------------------------------------------------------------------------------

// The NumberSetting part a NumberComponent / SliderComponent works on.
class NumberSource {
public:
    virtual ~NumberSource() = default;
    virtual float value() const noexcept = 0;
    virtual float min() const noexcept = 0;
    virtual float max() const noexcept = 0;
    virtual float step() const noexcept = 0;
    virtual bool wrap() const noexcept { return false; }
    virtual void set(float v) noexcept = 0;
    virtual std::string format() const = 0; // NumberSetting formatter / FORMAT

    void set_value(float v) noexcept; // setValue (+ marks the profile dirty)
    void increment() noexcept;        // NumberSetting.increment
    void decrement() noexcept;        // NumberSetting.decrement

private:
    float rounding_step() const noexcept;
    void apply_step(float next, float old) noexcept;
};

std::string format_decimal(double value); // NumberSetting.FORMAT = new DecimalFormat("#.##")

// ---- tree construction ---------------------------------------------------------------------------------------

std::unique_ptr<GuiNode> make_root();

} // namespace slinky::modules::adjust_hud::tabgui
