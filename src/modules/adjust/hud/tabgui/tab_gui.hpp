#pragma once

// Port of com.adjustclient.ui.tabgui.TabGui: the keyboard-driven category / module / setting menu in the top-left
// corner of the HUD (arrow keys navigate, Enter toggles / cycles, Right enters a column, Left leaves it).

#include <memory>
#include <utility>
#include <vector>

#include "modules/adjust/hud/tabgui/draw_list.hpp"
#include "modules/adjust/hud/tabgui/nodes.hpp"

namespace slinky::modules::adjust_hud::tabgui {

class TabGui {
public:
    explicit TabGui(TabGuiSettings& settings) noexcept : settings_(settings) {}

    // HudElement.onKey: a key press (OpenSlinky++ key code) while in game. Returns true when it was consumed.
    bool on_key(int key) noexcept;
    // HudElement.onMouse: a mouse button press (KEY_MOUSE1..5), forwarded to the keybind capture rows.
    bool on_mouse(bool pressed, int button) noexcept;
    // HudElement.build: lays the tree out into `dl` (absolute HUD coordinates, top-left anchor).
    void build(DrawList& dl) noexcept;
    // HudElement.update (per tick): Auto Close.
    void update() noexcept;

    // The selected row of the focused column is a slider (SliderComponent.keyEvent: arrow key repeat).
    bool selected_is_slider() noexcept;

    // Drops the node tree (rebuilt lazily); used on client shutdown.
    void reset() noexcept;

private:
    void ensure_root();
    bool handle_key(int key);
    NodeState& focused_state();
    GuiNode* selected_of(NodeState& state);
    void update_auto_close();
    void layout_node(DrawList& dl, GuiNode& node, int depth, int index, int row_base, float parent_width, float x);
    float column_width_of(const GuiNode* node) const noexcept;
    static float top_margin() noexcept;

    TabGuiSettings& settings_;
    std::unique_ptr<GuiNode> root_;
    std::vector<std::pair<const GuiNode*, float>> column_widths_;
    DrawList scratch_;
    int idle_ticks_ = 0;
    int move_grace_ = 0;
};

} // namespace slinky::modules::adjust_hud::tabgui
