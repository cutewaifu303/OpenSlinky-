#include "modules/adjust/hud/tabgui/tab_gui.hpp"

#include <algorithm>

#include "events/events.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "util/keycodes.hpp"

namespace slinky::modules::adjust_hud::tabgui {

namespace {

using adjust::Animation;
using adjust::Argb;

// gameSettings.keyBindUseItem / Attack / Forward / Back / Left / Right .isKeyDown(), in Adjust's order.
bool any_action_key_down(events::GameContext* game) noexcept {
    if (game == nullptr || game->jni == nullptr || game->jni->env == nullptr || game->minecraft == nullptr) {
        return false;
    }
    events::JniContext* jni = game->jni;
    JNIEnv* env = jni->env;
    jobject settings = minecraft::mc_get_game_settings(jni, game->minecraft);
    if (settings == nullptr) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return false;
    }
    using KeyGetter = jobject (*)(events::JniContext*, jobject) noexcept;
    static constexpr KeyGetter kGetters[] = {
        minecraft::settings_get_key_use_item, minecraft::settings_get_key_attack,
        minecraft::settings_get_key_forward,  minecraft::settings_get_key_back,
        minecraft::settings_get_key_left,     minecraft::settings_get_key_right,
    };
    bool down = false;
    for (KeyGetter getter : kGetters) {
        jobject binding = getter(jni, settings);
        if (binding != nullptr) {
            down = minecraft::key_binding_is_pressed(jni, binding);
            env->DeleteLocalRef(binding);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            down = false;
        }
        if (down) {
            break;
        }
    }
    env->DeleteLocalRef(settings);
    return down;
}

} // namespace

void TabGui::ensure_root() {
    if (!root_) {
        root_ = make_root();
    }
}

void TabGui::reset() noexcept {
    root_.reset();
    column_widths_.clear();
    scratch_.clear();
    idle_ticks_ = 0;
    move_grace_ = 0;
}

GuiNode* TabGui::selected_of(NodeState& state) {
    NodeList& children = state.node->children();
    if (children.empty()) {
        return nullptr;
    }
    const int index = state.get_selected_index();
    if (index < 0 || static_cast<size_t>(index) >= children.size()) {
        return nullptr;
    }
    return children[static_cast<size_t>(index)].get();
}

// The state that holds the cursor: walk down while the state is expanded.
NodeState& TabGui::focused_state() {
    NodeState* state = &root_->state;
    while (state->expanded) {
        GuiNode* next = selected_of(*state);
        if (next == nullptr || &next->state == state) {
            break;
        }
        state = &next->state;
    }
    return *state;
}

// ---- input -----------------------------------------------------------------------------------------------------

bool TabGui::on_key(int key) noexcept {
    try {
        ensure_root();
        if (handle_key(key)) {
            idle_ticks_ = 0;
            return true;
        }
    } catch (...) {
    }
    return false;
}

bool TabGui::on_mouse(bool pressed, int button) noexcept {
    try {
        ensure_root();
        GuiNode* selected = selected_of(focused_state());
        return selected != nullptr && selected->mouse_button(pressed, button);
    } catch (...) {
        return false;
    }
}

bool TabGui::selected_is_slider() noexcept {
    try {
        if (!root_) {
            return false;
        }
        GuiNode* selected = selected_of(focused_state());
        return selected != nullptr && selected->is_slider();
    } catch (...) {
        return false;
    }
}

bool TabGui::handle_key(int key) {
    NodeState& focus = focused_state();
    GuiNode* selected = selected_of(focus);
    if (selected == nullptr) {
        // empty column (e.g. a category without modules of the active style): only Left works
        if (key == input::KEY_LEFT && focus.parent != nullptr) {
            focus.parent->state.expanded = false;
            return true;
        }
        return false;
    }
    if (selected->key_pressed(key)) {
        return true;
    }
    // while a keybind entry waits for a key it swallows the navigation keys
    if (selected->is_listening()) {
        return false;
    }
    if (key == input::KEY_LEFT) {
        GuiNode* parent = focus.parent;
        if (parent == nullptr) {
            return false;
        }
        parent->state.expanded = false;
        return true;
    }
    if (key == input::KEY_RIGHT) {
        const bool was_expanded = focus.expanded;
        focus.expanded = !selected->children().empty();
        return was_expanded != focus.expanded;
    }
    if (key == input::KEY_DOWN || key == input::KEY_UP) {
        const int delta = key == input::KEY_DOWN ? 1 : -1;
        const size_t limit = focus.node->children().size();
        size_t guard = 0;
        do {
            focus.expand_anim().value = 0.0f; // restart the submenu animation
            focus.selected_index = focus.get_selected_index() + delta;
            focus.selected_index = focus.get_selected_index();
            selected = selected_of(focus);
        } while (selected != nullptr && selected->is_hidden() && ++guard <= limit);
        return true;
    }
    return false;
}

// ---- auto close ------------------------------------------------------------------------------------------------

void TabGui::update() noexcept {
    if (!settings_.enabled.value) {
        return;
    }
    try {
        if (idle_ticks_ != -1) {
            update_auto_close();
        }
    } catch (...) {
    }
    // StringListEditorComponent.e() (text field cursor blink) is not needed: the list rows are read-only
}

// While the player keeps an action key held, idleTicks grows by one per tick (during the 8 ticks after a press,
// see moveGrace); it decays by 3 after more than 9 ticks without one. At 201 every open column is closed.
void TabGui::update_auto_close() {
    if (!settings_.auto_close.value) {
        return;
    }
    int idle = idle_ticks_;
    const int grace = move_grace_;
    if (grace < 1 && any_action_key_down(tree_context().game)) {
        move_grace_ = 9;
    } else {
        if (grace < -8) {
            idle_ticks_ = std::max(idle - 3, 0);
            move_grace_ = -1;
            return;
        }
        move_grace_ = grace - 1;
        if (grace < 2) {
            return;
        }
    }
    idle = idle + 1;
    idle_ticks_ = idle;
    if (idle < 201) {
        return;
    }
    idle_ticks_ = -1;
    if (!root_) {
        return;
    }
    // close every column: repeatedly collapse the parent of the focused state
    while (true) {
        NodeState& focus = focused_state();
        GuiNode* parent = focus.parent;
        if (parent == nullptr || !parent->state.expanded) {
            break;
        }
        parent->state.expanded = false;
    }
}

// ---- layout / drawing ------------------------------------------------------------------------------------------

float TabGui::top_margin() noexcept {
    // below the watermark: 9.5 when HUD > Other > Watermark is on, otherwise 2.0
    return watermark_enabled() ? 9.5f : 2.0f;
}

float TabGui::column_width_of(const GuiNode* node) const noexcept {
    for (const auto& entry : column_widths_) {
        if (entry.first == node) {
            return entry.second;
        }
    }
    return static_cast<float>(settings_.width);
}

void TabGui::build(DrawList& dl) noexcept {
    try {
        ensure_root();
        dl.clear();
        column_widths_.clear();
        layout_node(dl, *root_, -1, 0, 0, static_cast<float>(settings_.width), 0.0f);

        // 1) walk down while the submenu animation of the current state is still running (value != 0)
        NodeState* animated = &root_->state;
        while (true) {
            if (!animated->has_expand_animation) {
                animated->expand_anim();
                break;
            }
            if (0.0f == animated->expand_animation.value) {
                break;
            }
            GuiNode* next = selected_of(*animated);
            if (next == nullptr || &next->state == animated) {
                break;
            }
            animated = &next->state;
        }
        // 2) the state that really holds the cursor (deepest expanded)
        NodeState& focus = focused_state();

        // 3) advance the open/close animations of the animated state and of its parent
        const bool fast = &focus != animated;
        animated->get_expand_progress(fast);
        if (animated->parent == nullptr) {
            return;
        }
        NodeState& parent_state = animated->parent->state;
        parent_state.get_expand_progress(fast);

        // 4) clip everything to a rectangle whose right edge follows the open animation of the last column
        const Rect2f bounds = dl.bounds();
        dl.insertion_index = 0; // insert at the front, i.e. before the drawing commands
        const float width = column_width_of(animated->node);
        const Animation& open = parent_state.expand_anim();
        const float x = bounds.x - 0.5f;
        const float y = bounds.y - 0.5f;
        const float w = (bounds.width - (1.0f - open.value) * width) + 1.0f;
        const float h = 0.5f + bounds.height;
        dl.push_clip(x, y, w, h);
        dl.pop_clip();
    } catch (...) {
        tree_context().laying_out_nodes = false;
    }
}

void TabGui::layout_node(DrawList& dl, GuiNode& node, int depth, int index, int row_base, float parent_width,
                         float x) {
    TreeContext& tree = tree_context();
    NodeState& state = node.state;
    const float top = top_margin();
    const auto row_height = static_cast<float>(settings_.row_height);
    bool selected = false; // node is the cursor row of its parent
    bool open = false;     // its submenu is (being) shown
    if (state.parent == nullptr) {
        open = true; // the root
    } else {
        NodeState& parent_state = state.parent->state;
        if (parent_state.get_selected_index() == index) {
            const Animation& anim = parent_state.expand_anim();
            selected = true;
            open = 0.0f != anim.value;
        }
    }

    if (root_.get() != &node) {
        node.render(dl, settings_, selected, x + 2.0f, row_height * static_cast<float>(row_base) + top, parent_width);
    }
    if (!open) {
        return;
    }

    // ---- children column ---------------------------------------------------------------------------------------
    NodeList& children = node.children();
    const auto min_width = static_cast<float>(settings_.width);
    float child_x = (parent_width + x) + 2.5f; // x of the children column
    float width = min_width;
    if (depth < 0) {
        child_x = 0.0f; // the first column starts at x = 0
        column_widths_.emplace_back(&node, width);
    } else {
        // measure pass
        tree.laying_out_nodes = true;
        scratch_.clear();
        for (auto& child : children) {
            if (!child->is_hidden()) {
                child->render(scratch_, settings_, true, 0.0f, 0.0f, min_width);
            }
        }
        tree.laying_out_nodes = false;
        const float measured = scratch_.width() + 2.5f;
        width = measured == min_width ? min_width : std::max(measured + 2.5f, min_width);
        column_widths_.emplace_back(&node, width);
    }

    // visible row count / selected visible row (-1 = no highlight bar: the slider column of a number setting)
    int hidden = 0;
    int selected_row = 0;
    float column_height = 0.0f;
    for (size_t i = 0; i < children.size(); ++i) {
        GuiNode& child = *children[i];
        if (child.is_hidden()) {
            if (state.get_selected_index() == static_cast<int>(i)) {
                // the selected child vanished (dependency changed): fall back to the first row
                state.selected_index = 0;
                state.selected_index = state.get_selected_index();
                if (state.expanded) {
                    GuiNode* first = selected_of(state);
                    state.expanded = first != nullptr && !first->children().empty();
                } else {
                    state.expanded = false;
                }
            }
            ++hidden;
        } else {
            if (child.is_slider()) {
                selected_row = -1;
            } else if (state.get_selected_index() == static_cast<int>(i)) {
                selected_row = static_cast<int>(i) - hidden;
            }
            column_height += row_height;
        }
    }

    // column background
    dl.set_color(settings_.owner);
    const float column_top = row_height * static_cast<float>(row_base) + top;
    dl.rect(child_x + 2.0f, column_top, width, column_height);

    // selection highlight (animated bar behind the cursor row, colour = current HUD colour)
    if (selected_row != -1) {
        const Argb primary = hud_color();
        dl.set_color(primary);
        const float row_offset = state.get_row_offset(selected_row);
        const float y = row_height * static_cast<float>(row_base) + top + row_height * row_offset;
        const float bar_x = child_x + 2.0f;
        if (settings_.gradient.value) {
            // a 12 unit solid bar, then a gradient from the HUD colour to the secondary colour (or a 65 % variant)
            const Argb secondary = secondary_active() ? secondary_for(primary) : adjust::scale(primary, 0.65f);
            dl.rect(bar_x, y, 12.0f, row_height);
            dl.gradient_rect(bar_x + 12.0f, y, width - 12.0f, row_height, primary, secondary);
        } else {
            dl.rect(bar_x, y, width, row_height);
        }
    }

    // recurse into every visible child
    int hidden_before = 0;
    for (size_t i = 0; i < children.size(); ++i) {
        GuiNode& child = *children[i];
        if (child.is_hidden()) {
            ++hidden_before;
        } else {
            layout_node(dl, child, depth + 1, static_cast<int>(i), row_base + static_cast<int>(i) - hidden_before,
                        width, child_x);
        }
    }
}

} // namespace slinky::modules::adjust_hud::tabgui
