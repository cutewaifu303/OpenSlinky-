// Adjust ClickGUI port - component base (ClickGuiComponent), modifiers (frame.HoverEffect, HoverBackground,
// ButtonBar, ExpandButton, VisibilityButton, Tooltip, IconButtonModifier, overlay.BindOverlay), frame.TabHeader,
// frame.ModuleEntry (rows; its setting generation lives in settings.cpp) and ClickGuiTab.

#include <algorithm>
#include <cmath>

#include "events/module_manager.hpp"
#include "modules/adjust/clickgui/internal.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "util/keycodes.hpp"

namespace slinky::modules::adjust_clickgui::detail {

namespace hud = slinky::modules::adjust_hud;

// =============================================================================================================
// ClickGuiComponent
// =============================================================================================================

Component::~Component() = default;

bool Component::is_visible(int stamp) {
    if (stamp == visibility_stamp) {
        return cached_visible;
    }
    visibility_stamp = stamp;
    cached_visible = parent != nullptr ? parent->compute_visible(stamp) : true;
    return cached_visible;
}

bool Component::key_typed(int ch, int key) {
    if (modifiers.empty()) {
        return false;
    }
    std::vector<Modifier*> snapshot;
    snapshot.reserve(modifiers.size());
    for (auto& m : modifiers) {
        snapshot.push_back(m.get());
    }
    for (Modifier* m : snapshot) {
        if (m->key_typed(ch, key)) {
            return true;
        }
    }
    return false;
}

bool Component::mouse_clicked(Vec2 mouse, int button, bool pressed) {
    std::vector<Modifier*> snapshot;
    snapshot.reserve(modifiers.size());
    for (auto& m : modifiers) {
        snapshot.push_back(m.get());
    }
    for (Modifier* m : snapshot) {
        if (m->mouse_clicked(mouse, button, pressed)) {
            return true;
        }
    }
    if (on_mouse_pressed(mouse, button, pressed)) {
        return true;
    }
    if (button == 1) {
        if (pressed) {
            toggle_expanded();
            return true;
        }
        return false;
    }
    return false;
}

void Component::render(DrawQueue& queue, DrawQueue& top, Vec2 mouse, const Window& window) {
    std::vector<Modifier*> snapshot;
    snapshot.reserve(modifiers.size());
    for (auto& m : modifiers) {
        snapshot.push_back(m.get());
    }
    std::vector<Modifier*> deferred;
    for (Modifier* m : snapshot) {
        const Phase phase = m->phase();
        if (phase == Phase::Pre) {
            m->render(window, queue, top, mouse, true);
        } else {
            deferred.push_back(m);
            if (phase != Phase::Post) { // Both
                m->render(window, queue, top, mouse, true);
            }
        }
    }
    render_content(queue, mouse);
    for (Modifier* m : deferred) {
        m->render(window, queue, top, mouse, false);
    }
}

void Component::set_expanded(bool value) {
    if (expanded == value) {
        return;
    }
    screen().layout_dirty = true;
    expanded = value;
    refresh_pending = false;
    if (value) {
        return;
    }
    if (parent == nullptr || parent->is_fully_expanded()) {
        return;
    }
    for (auto& m : modifiers) {
        m->on_owner_collapsed();
    }
    on_collapsed_hidden();
}

void Component::expand_self() {
    if (expanded) {
        return;
    }
    set_expanded(true);
    refresh_pending = true;
}

void Component::toggle_expanded() {
    const bool target = !expanded;
    Tab* tab = owner;
    if (tab == nullptr || tab->screen == nullptr) {
        set_expanded(target);
        return;
    }
    SearchBar& search = tab->screen->search_bar;
    if (!search.is_searching()) {
        set_expanded(target);
        return;
    }
    tab->screen->layout_dirty = true;
    if (refresh_pending) {
        set_expanded(false);
        return;
    }
    search.collect(this);
    const std::vector<Component*> kids = children;
    for (Component* c : kids) {
        c->expand_ancestors();
        const std::vector<Component*> grand = c->children;
        for (Component* g : grand) {
            g->expand_ancestors();
        }
    }
    expand_ancestors();
}

void Component::expand_ancestors() {
    Component* p = this;
    while (p->parent != nullptr) {
        p->parent->expand_self();
        p = p->parent;
    }
    if (kind() == RowKind::Expand && !expanded) {
        set_expanded(true);
        refresh_pending = true;
    }
    if (owner != nullptr) {
        Component* state = owner->get_state();
        if (state != nullptr && !state->expanded) {
            state->set_expanded(true);
            refresh_pending = true;
        }
    }
}

bool Component::is_fully_expanded(const Component* c) noexcept {
    for (const Component* p = c; p != nullptr; p = p->parent) {
        if (!p->expanded) {
            return false;
        }
        if (p->parent == nullptr) {
            return true;
        }
    }
    return false;
}

// =============================================================================================================
// HoverEffect
// =============================================================================================================

std::function<bool()> hovered_of(const Component* c) {
    return [c] { return c->is_hovered(); };
}

HoverEffect::HoverEffect(const Component* owner, std::function<bool()> active, std::function<bool()> hovered,
                         float hover_value, float active_value)
    : owner_(owner), active_(std::move(active)), hovered_(std::move(hovered)), hover_value_(hover_value),
      active_value_(active_value) {}

void HoverEffect::step() {
    const float delta = std::max(0.0f, active_value_ - hover_value_);
    float target = (hovered_ && hovered_()) ? hover_value_ : 0.0f;
    if (active_ && active_()) {
        target = delta + target;
    }
    if (!has_animation_) {
        animation_ = adjust::Animation(adjust::Easing::Linear, 95);
        has_animation_ = true;
        animation_.duration = 0; // snap on creation
        step();
        animation_.duration = 95;
    }
    animation_.update(target);
}

void HoverEffect::render(const Window&, DrawQueue&, DrawQueue&, Vec2, bool) {
    step();
}

adjust::Animation& HoverEffect::k() {
    if (!has_animation_) {
        step();
    }
    return animation_;
}

Argb HoverEffect::color(int base) {
    const float v = k().value;
    const int channel = clamp_int(static_cast<int>(110.0f * v + static_cast<float>(base)), 0, 255);
    const int alpha = clamp_int(static_cast<int>(48.0f * v + static_cast<float>(base + 10)), 0, 255);
    return adjust::argb(channel, channel, channel, alpha);
}

Argb HoverEffect::with_alpha(Argb base) {
    const float v = k().value;
    const int alpha = clamp_int(static_cast<int>(255.0f * v), 0, 255);
    return alpha == adjust::alpha_of(base) ? base : ((static_cast<Argb>(alpha) << 24) | (base & 0xFFFFFFu));
}

Argb HoverEffect::tint(int red) {
    const float v = k().value;
    const int channels[3] = {red, 155, 155};
    int out[3]{};
    for (int i = 0; i < 3; ++i) {
        out[i] = clamp_int(static_cast<int>(110.0f * v + static_cast<float>(channels[i])), 0, 255);
    }
    const int alpha = clamp_int(static_cast<int>(48.0f * v + 165.0f), 0, 255);
    return adjust::argb(out[0], out[1], out[2], alpha);
}

Argb HoverEffect::accent(int max_alpha) {
    const Argb accent_color = hud::hud_color();
    const float v = k().value;
    const int alpha = clamp_int(static_cast<int>(static_cast<float>(max_alpha) * v), 0, 255);
    return alpha == adjust::alpha_of(accent_color) ? accent_color
                                                   : ((static_cast<Argb>(alpha) << 24) | (accent_color & 0xFFFFFFu));
}

// =============================================================================================================
// HoverBackground
// =============================================================================================================

void HoverBackground::render(const Window&, DrawQueue&, DrawQueue&, Vec2, bool) {
    const bool is_hov = owner_->is_hovered();
    if (is_hov && !has_animation_) {
        animation_ = adjust::Animation(adjust::Easing::Linear, 0);
        has_animation_ = true;
    }
    if (!is_hov && !has_animation_) {
        return;
    }
    animation_.duration = is_hov ? 0 : 60; // instant in, 60 ms out
    const float value = animation_.update(owner_->is_hovered() ? 1.0f : 0.0f);
    const Box& b = owner_->bounds;
    const float x1 = b.x;
    const float y1 = owner_->get_y();
    const float x2 = b.x + b.width;
    const float y2 = y1 + owner_->height;
    const int alpha = clamp_int(static_cast<int>(value * 69.0f), 0, 255);
    adjust::fill_rect(adjust::argb(53, 53, 53, alpha), x1, y1, x2, y2);
    if (animation_.value != 0.0f) {
        return;
    }
    if (!is_hov) {
        has_animation_ = false;
    }
}

// =============================================================================================================
// ButtonBar / ExpandButton
// =============================================================================================================

bool ButtonBar::mouse_clicked(Vec2 pos, int button, bool pressed) {
    if (button != 0 || !pressed) {
        return false;
    }
    for (auto& b : buttons) {
        if (pos.x < b->x || pos.y < b->y || b->x + b->width <= pos.x) {
            continue;
        }
        if (b->y + b->height > pos.y) {
            b->on_click();
            return true;
        }
    }
    return false;
}

void ButtonBar::render(const Window& window, DrawQueue& queue, DrawQueue&, Vec2, bool layout_pass) {
    if (layout_pass) {
        Component* p = owner_;
        const float right = p->bounds.x + p->bounds.width;
        const float parent_y = p->get_y();
        std::vector<HeaderButton*> deferred;
        float cursor = right - 0.5f;
        float total_width = 0.0f;
        for (auto& b : buttons) {
            if (!b->is_shown()) {
                continue;
            }
            b->x = cursor - b->width;
            b->y = (11.0f - b->height) * 0.5f + parent_y;
            if (b->draw_immediately) {
                b->render();
            } else {
                deferred.push_back(b.get());
            }
            total_width += b->width;
            cursor = b->x;
        }
        if (!deferred.empty()) {
            queue.post_tasks.push_back([deferred] {
                for (HeaderButton* b : deferred) {
                    b->render();
                }
            });
        }
        if (total_width > 0.0f) {
            content_clipped_ = true;
            // keep the owner's own content out of the area covered by the buttons
            adjust::push_scissor(p->x, 0.0f, p->width - total_width, static_cast<float>(window.scaled_height));
        }
    } else if (content_clipped_) {
        adjust::pop_scissor();
        content_clipped_ = false;
    }
}

void ButtonBar::on_owner_collapsed() {
    for (auto& b : buttons) {
        b->on_owner_collapsed();
    }
}

ExpandButton::ExpandButton(Component* parent_, std::function<bool()> shown_if)
    : parent(parent_), shown_if_(std::move(shown_if)) {
    draw_immediately = true;
    width = 6.0f;
    height = 8.0f;
    hover_ = parent->add_modifier(std::make_unique<HoverEffect>(
        parent, [] { return false; }, hovered_of(parent), 1.0f, 0.8f));
    update_fade();
}

bool ExpandButton::on_click() {
    parent->toggle_expanded();
    return true;
}

bool ExpandButton::parent_open() const {
    const Component* p = parent;
    if (p->owner != nullptr && p->owner->screen != nullptr) {
        const SearchBar& search = p->owner->screen->search_bar;
        if (search.is_searching()) {
            return search.has_matching_child(const_cast<Component*>(p));
        }
    }
    return p->is_fully_expanded();
}

float ExpandButton::update_fade() {
    return fade_.update(parent_open() ? 1.0f : 0.0f);
}

void ExpandButton::on_owner_collapsed() {
    fade_.value = parent_open() ? 1.0f : 0.0f;
    fade_.restart();
}

void ExpandButton::render() {
    const float bx = x + 0.5f;
    const float by = y + 2.0f;
    const Argb text_color = hover_->color(120);
    const float fade_value = update_fade();
    const bool fading = !fade_.finished;
    if (fading) {
        // the clip shrinks from 4.0 to 0.5 high and moves down 1.5 as the parent opens: "+" -> "-"
        adjust::push_scissor(bx, by + 1.5f * fade_value, width, 4.0f - fade_value * 3.5f);
    }
    const bool open = parent_open();
    if (!open || fading) {
        adjust::kTahoma13.draw("+", bx, by, text_color);
    }
    if (open || fading) {
        const int bar_alpha = static_cast<int>(static_cast<float>(text_color >> 24) * fade_.value);
        if (bar_alpha >= 26) {
            adjust::push_even_scale();
            const math::Vec4f p = adjust::to_pixels(bx + 0.5f, by + 1.5f, (bx + width) - 2.0f, by + 2.0f);
            const float t = pixel_line_width(true);
            const Argb rgb = text_color & 0xFFFFFFu;
            const Argb bar = (static_cast<Argb>(bar_alpha) << 24) | rgb;
            const Argb shadow = (text_color & 0xFF000000u) | ((rgb & 0xFCFCFCu) >> 2);
            adjust::fill_rect(shadow, p.x + t, p.y + t, p.w + t, p.h + t);
            adjust::fill_rect(bar, p.x, p.y, p.w, p.h);
            adjust::pop_even_scale();
        }
    }
    if (fading) {
        adjust::pop_scissor();
    }
}

void add_expand_button_bar(Component* owner) {
    std::vector<std::unique_ptr<HeaderButton>> buttons;
    buttons.push_back(std::make_unique<ExpandButton>(owner));
    owner->add_modifier(std::make_unique<ButtonBar>(owner, std::move(buttons)));
}

// =============================================================================================================
// Tooltip
// =============================================================================================================

Tooltip::Tooltip(const Component* owner, int64_t delay, std::string text, std::string extended, bool has_extended,
                 std::function<bool()> hovered)
    : owner_(owner), text_(std::move(text)), extended_(std::move(extended)), has_extended_(has_extended),
      hovered_(hovered ? std::move(hovered) : hovered_of(owner)), delay_(delay) {}

void Tooltip::reset() {
    has_hover_timer_ = false;
    has_fade_ = false;
    previous_ = nullptr;
}

void Tooltip::render(const Window&, DrawQueue&, DrawQueue& top, Vec2 mouse, bool) {
    const Vec2 anchor = mouse;
    top.post_tasks.push_back([this, anchor] { draw_deferred(anchor); });
}

void Tooltip::draw_deferred(Vec2 anchor) {
    const std::string title = get_text();
    const std::string* ext = (is_last_used() || !has_extended_) ? nullptr : &extended_;
    draw(title, ext, hovered_ && hovered_(), anchor);
}

Tooltip* Tooltip::find_previous() {
    Tooltip* last = nullptr;
    for (auto& m : owner_->modifiers) {
        Tooltip* t = m->as_tooltip();
        if (t == nullptr) {
            continue;
        }
        if (t->has_fade_ && t->fade_.value != 0.0f && !t->text_.empty()) {
            if (t == this) {
                return last;
            }
            last = t;
        }
    }
    return nullptr;
}

void Tooltip::draw(const std::string& title, const std::string* extended, bool is_hovered, Vec2 anchor) {
    const int64_t now = adjust::now_ms();
    if (!is_hovered || text_.empty()) {
        if (has_hover_timer_) {
            hover_timer_ = now; // keep the timer reset while not hovered
        }
        if (has_fade_) {
            fade_.update(0.0f);
            if (fade_.finished) {
                has_hover_timer_ = false;
                has_fade_ = false;
                previous_ = nullptr;
            }
        }
    } else {
        if (!has_hover_timer_) {
            hover_timer_ = now;
            has_hover_timer_ = true;
        }
        if (hover_timer_ + delay_ <= now) {
            if (!has_fade_) {
                fade_ = adjust::Animation(adjust::Easing::Linear, 90);
                has_fade_ = true;
            }
            fade_.update(1.0f);
        }
    }
    if (!has_fade_ || fade_.value == 0.0f || text_.empty()) {
        return;
    }
    if (previous_ == nullptr) {
        previous_ = find_previous();
    }
    const int alpha = clamp_int(static_cast<int>(fade_.value * 170.0f), 0, 255);
    static const std::string kHint = "Hold " ACG_WHITE "LCONTROL" ACG_RESET " to view extended description";
    const float stack_offset = previous_ != nullptr ? previous_->bottom_ + 2.0f : 0.0f;
    const float top = (anchor.y - 6.0f) + stack_offset;
    const float x0 = anchor.x + 5.0f;
    float width = adjust::kTahoma10.width(title);
    float line_height = 6.5f;
    bool ctrl = false;
    if (extended == nullptr) {
        line_height = 6.5f;
    } else if (lcontrol_down()) {
        ctrl = true;
        width = std::max(width, adjust::kTahoma10.width(*extended));
        line_height = 14.0f;
    } else {
        width = std::max(width, adjust::kTahoma8.width(kHint));
        line_height = 13.0f;
    }
    bottom_ = line_height + stack_offset;
    const float box_width = width + 2.0f;
    float left = static_cast<float>(env().window.scaled_width) - box_width - 2.0f;
    if (left >= x0) {
        left = x0;
    }
    const float right = left + box_width;
    const float bottom_edge = top + line_height;
    adjust::fill_rect(adjust::argb(25, 25, 28, alpha), left, top, right, bottom_edge);
    adjust::stroke_rect(true, adjust::argb(35, 35, 38, alpha), left, top, right, bottom_edge, 0.5f);
    const Argb text_color = adjust::argb(233, 235, 234, alpha);
    adjust::kTahoma10.draw(title, left + 1.0f, top + 2.5f, text_color);
    if (extended != nullptr) {
        if (ctrl) {
            adjust::kTahoma10.draw(*extended, left + 1.0f, top + 9.5f, text_color);
        } else {
            adjust::kTahoma8.draw(kHint, left + 1.0f, top + 9.5f, text_color);
        }
    }
}

// =============================================================================================================
// IconButtonModifier
// =============================================================================================================

bool IconButtonModifier::mouse_clicked(Vec2 mouse, int button, bool pressed) {
    if (!pressed || button != 0) {
        return false;
    }
    if (!button_.is_hovered(mouse.x, mouse.y)) {
        return false;
    }
    if (action_) {
        action_();
    }
    return true;
}

void IconButtonModifier::render(const Window&, DrawQueue&, DrawQueue&, Vec2 mouse, bool) {
    Component* tab = anchor_;
    button_.x = ((tab->x + tab->width) - static_cast<float>(button_.width)) - right_offset_ - 2.0f;
    button_.y = (tab->y + 2.0f) + 0.5f;
    button_.draw(mouse.x, mouse.y);
}

// =============================================================================================================
// BindOverlay
// =============================================================================================================

BindOverlay::BindOverlay(std::string name, std::function<void(int)> callback, int current_key)
    : name_(std::move(name)), callback_(std::move(callback)), debounce_(adjust::now_ms()) {
    if (current_key > 0) {
        pressed_text_ = key_name(current_key); // BindOverlay.formatKeys(bind.keys)
    }
}

bool BindOverlay::on_scroll(float) {
    return true; // OpenSlinky++ keybinds cannot be mouse wheel directions (Adjust ScrollKey): ignored
}

void BindOverlay::add_key(int key, bool has_key) {
    if (has_key) {
        if (debounce_ + 180 > adjust::now_ms()) {
            return; // still inside the 180 ms grace period
        }
        if (callback_) {
            callback_(key);
        }
        mark_dirty();
    }
    screen().remove_overlay(this);
}

bool BindOverlay::key_typed(int ch, int key) {
    if (key == kNoKey || key <= 0) {
        return false;
    }
    if (key == input::KEY_ESC) {
        add_key(0, false);
    } else {
        add_key(key, true);
    }
    return true;
}

bool BindOverlay::mouse_clicked(Vec2, int button, bool pressed) {
    if (!pressed) {
        return false;
    }
    if (button >= 0 && button < 5) {
        add_key(input::KEY_MOUSE1 + button, true);
    }
    return true;
}

void BindOverlay::render(const Window& window, DrawQueue&, DrawQueue&, Vec2, bool) {
    fade_.update(1.0f);
    const auto w = static_cast<float>(window.scaled_width);
    const auto h = static_cast<float>(window.scaled_height);
    const Argb bg = (static_cast<Argb>(clamp_int(static_cast<int>(220.0f * fade_.value), 0, 255)) << 24) | 0x151515u;
    adjust::fill_rect(bg, 0.0f, 0.0f, w, h);
    const std::string text = "Press any key to bind " ACG_WHITE + name_;
    const float text_fade = pulse_.value();
    const Argb text_color =
        (static_cast<Argb>(clamp_int(static_cast<int>(text_fade * 60.0f) + 170, 0, 255)) << 24) | 0x969696u;
    adjust::kTahoma20.draw(text, w / 2.0f - adjust::kTahoma20.width(text) * 0.5f, h / 2.17f, text_color);
    if (!pressed_text_.empty()) {
        adjust::kTahoma20.draw_centered(pressed_text_, w / 2.0f, h / 2.17f + 12.0f, 0xC8E9E9E9u);
    }
    // the pulse restarts once its fall has finished
    if (pulse_.has_fade_out && pulse_.fade_out.finished) {
        pulse_ = adjust::TimedFade(1050, 500, 1000);
    }
}

// =============================================================================================================
// TabHeader
// =============================================================================================================

void TabHeader::render_content(DrawQueue&, Vec2 mouse) {
    if (tab_ == nullptr) {
        return;
    }
    float a = 0.08f;
    float b = 0.2f;
    float dim = 0.0f;
    if (tab_->hovered) {
        const float f = ((mouse.x - x) / width) * 0.5f;
        if (f < 0.0f) {
            a = 0.08f;
            b = 0.2f;
            dim = 0.0f;
        } else if (f > 1.0f) {
            a = 0.16f;
            b = 0.5f;
            dim = 0.3f;
        } else {
            dim = 0.3f * f;
            a = f * 0.08f + 0.08f;
            b = 0.2f + 0.3f * f;
        }
    }
    const float v = gradient_hover->k().value;
    const int level_alpha = clamp_int(static_cast<int>((v - dim) * 30.0f + 210.0f), 0, 255);
    const Argb accent = hud::hud_color();
    const Argb c1 = (accent & 0xFFFFFFu) | (static_cast<Argb>(level_alpha) << 24);
    const Argb start = adjust::brighter(c1, 0.5f - a * v);
    const Argb themed = hud::secondary_for(c1);
    const float d = 1.0f - b * gradient_hover->k().value;
    const Argb end = adjust::argb(clamp_int(static_cast<int>(static_cast<float>(adjust::red_of(themed)) * d), 0, 255),
                                  clamp_int(static_cast<int>(static_cast<float>(adjust::green_of(themed)) * d), 0, 255),
                                  clamp_int(static_cast<int>(static_cast<float>(adjust::blue_of(themed)) * d), 0, 255),
                                  adjust::alpha_of(themed));
    adjust::push_even_scale();
    const math::Vec4f p = adjust::to_pixels(x, y, x + width, y + height);
    adjust::fill_gradient_rect(start, end, p.x, p.y, p.w, p.h, p.x, p.y, p.w, p.y); // horizontal, left = start
    adjust::stroke_rect(false, 0x64232323u, p.x, p.y, p.w, p.h, pixel_line_width());
    adjust::pop_even_scale();
    const std::string text = ACG_BOLD + tab_->display_title();
    const float cx = ((width - title_reserve) / 2.0f + x + 1.0f) - title_shift;
    const float ty = y + 4.5f;
    const Argb color = title_hover->color(220);
    adjust::kTahoma12.draw(text, cx - adjust::kTahoma12.width(text) * 0.5f, ty, color);
}

bool TabHeader::on_mouse_pressed(Vec2 mouse, int button, bool pressed) {
    if (pressed && button == 0) {
        Screen* s = tab_->screen;
        s->dragged_tab = tab_;
        s->focused_tab = tab_;
        s->grab_x = mouse.x - tab_->get_x();
        s->grab_y = mouse.y - tab_->get_y();
        return true;
    }
    if (button == 1 && pressed) {
        toggle_expanded();
    }
    return true;
}

bool TabHeader::is_hovered() const {
    return tab_->hovered;
}

// =============================================================================================================
// ModuleEntry (rows)
// =============================================================================================================

namespace {

// frame.VisibilityButton: Adjust's "visible in the array list" flag = OpenSlinky++ !kModuleFlagHiddenFromHud.
class VisibilityButton final : public HeaderButton {
public:
    VisibilityButton(HoverEffect* hover, ModuleEntry* entry) : hover_(hover), entry_(entry) {
        width = 9.5f;
        height = 11.0f;
        draw_immediately = true;
    }

    bool on_click() override {
        entry_->module->flags ^= events::kModuleFlagHiddenFromHud;
        mark_dirty();
        return true;
    }

    void render() override {
        const bool visible = (entry_->module->flags & events::kModuleFlagHiddenFromHud) == 0u;
        cross_fade_.value = visible ? 1.0f : 0.0f;
        const float bx = x + 1.5f;
        const float v = cross_fade_.value;
        const Argb base = hover_->color(190);
        const int alpha = adjust::alpha_of(base);
        const Argb rgb = base & 0xFFFFFFu;
        const int d_alpha = static_cast<int>(v * static_cast<float>(alpha));
        if (d_alpha >= 26) {
            adjust::kIcons14.draw_no_shadow("d", bx, y + 3.75f, (static_cast<Argb>(d_alpha) << 24) | rgb);
        }
        const int e_alpha = static_cast<int>((1.0f - v) * static_cast<float>(alpha));
        if (e_alpha >= 26) {
            adjust::kIcons14.draw_no_shadow("e", bx, y + 3.5f, (static_cast<Argb>(e_alpha) << 24) | rgb);
        }
    }

    bool is_shown() override { return entry_->is_hovered(); }

private:
    HoverEffect* hover_;
    ModuleEntry* entry_;
    adjust::Animation cross_fade_{adjust::Easing::Linear, 60};
};

} // namespace

ModuleEntry::ModuleEntry(events::Module* mod) : module(mod) {
    default_hidden_ = (mod->flags & events::kModuleFlagHiddenFromHud) != 0u;
    add_modifier(std::make_unique<HoverBackground>(this));
    events::Module* m = mod;
    label_hover_ = add_modifier(std::make_unique<HoverEffect>(
        this, [m] { return m->enabled; }, hovered_of(this), 0.333f, 1.0f));
    if (mod->description != nullptr && mod->description[0] != '\0') {
        add_modifier(std::make_unique<Tooltip>(this, 300, mod->description, std::string(), false, nullptr));
    }
    std::vector<std::unique_ptr<HeaderButton>> buttons;
    // Adjust adds the expand button only for modules with settings; the settings are captured live here
    buttons.push_back(std::make_unique<ExpandButton>(this, [this] { return has_settings(); }));
    HoverEffect* visibility_hover = add_modifier(std::make_unique<HoverEffect>(
        this, [m] { return (m->flags & events::kModuleFlagHiddenFromHud) == 0u; }, hovered_of(this), 0.05f, 1.0f));
    buttons.push_back(std::make_unique<VisibilityButton>(visibility_hover, this));
    add_modifier(std::make_unique<ButtonBar>(this, std::move(buttons)));
}

ModuleEntry::~ModuleEntry() = default;

void ModuleEntry::render_content(DrawQueue&, Vec2) {
    adjust::kTahoma12.draw(safe(module->name), x + 2.5f, y + 4.5f, label_hover_->color(190));
}

bool ModuleEntry::search_text(std::string& out) const {
    out = safe(module->name);
    return true;
}

void ModuleEntry::set_expanded(bool value) {
    const bool collapsing = !value && expanded;
    Component::set_expanded(value);
    if (collapsing) {
        collapse_all(children);
    }
}

void ModuleEntry::collapse_all(const std::vector<Component*>& list) {
    const std::vector<Component*> copy = list;
    for (Component* c : copy) {
        c->set_expanded(false);
        collapse_all(c->children);
    }
}

bool ModuleEntry::on_mouse_pressed(Vec2, int button, bool pressed) {
    if (button == 0 && pressed) {
        if (!ctrl_down()) {
            if ((module->flags & events::kModuleFlagNoToggleSwitch) != 0u) {
                toggle_expanded(); // OpenSlinky++ module without an on/off state: a click opens its settings
                return true;
            }
            events::ModuleManager* mgr = env().modules;
            if (mgr != nullptr) {
                mgr->request_set_enabled(module, !module->enabled);
                mark_dirty();
            }
            return true;
        }
        const std::string name = safe(module->name);
        auto dialog = std::make_unique<Dialog>("Reset module " + name + " to default?");
        dialog->message.append("The settings along with enabled and visible states of ");
        dialog->message.italic = true;
        dialog->message.append(name);
        dialog->message.append(" will be reset to their defaults if you continue.");
        dialog->add_button("Cancel", nullptr);
        dialog->add_button("Reset Module", [this](Dialog&) { reset_module(); });
        show_dialog(std::move(dialog));
        return true;
    }
    if (button == 2 && pressed) {
        if ((module->flags & events::kModuleFlagNoKeybind) != 0u) {
            return false;
        }
        events::Module* m = module;
        screen().add_overlay(std::make_unique<BindOverlay>(
            safe(module->name),
            [m](int key) {
                events::ModuleManager* mgr = env().modules;
                // Bind.addKey: the key that is already bound removes it
                const auto next = static_cast<uint32_t>(m->keybind == key ? 0 : key);
                if (mgr != nullptr) {
                    mgr->set_module_keybind(m, next);
                } else {
                    m->keybind = static_cast<int32_t>(next);
                }
            },
            module->keybind));
        return true;
    }
    return false;
}

void ModuleEntry::reset_module() {
    if (default_hidden_) {
        module->flags |= events::kModuleFlagHiddenFromHud;
    } else {
        module->flags &= ~static_cast<uint32_t>(events::kModuleFlagHiddenFromHud);
    }
    if ((module->flags & events::kModuleFlagNoToggleSwitch) == 0u && module->enabled && env().modules != nullptr) {
        env().modules->request_set_enabled(module, false); // OpenSlinky++ default state: off
    }
    if (module->visit_config != nullptr) {
        config::ConfigArchive ar{nullptr, config::ConfigAction::ResetDefaults, false, {}};
        module->visit_config(&ar);
    }
    mark_dirty();
}

// =============================================================================================================
// ClickGuiTab
// =============================================================================================================

bool Tab::contains(Vec2 mouse) const noexcept {
    const float ox = has_anchor ? anchor.x : x;
    const float oy = has_anchor ? anchor.y : y;
    return mouse.x >= ox && mouse.y >= oy && (ox + width) > mouse.x && (oy + height) > mouse.y;
}

void Tab::scroll_by(float delta) noexcept {
    const float a = scroll - delta;
    const float b = content_height - height;
    scroll = std::max(0.0f, std::min(a, b));
}

float Tab::clamp_x() const noexcept {
    const Window& w = env().window;
    if (w.scaled_width == 0) {
        return std::max(0.0f, x);
    }
    const float limit = std::max(0.0f, static_cast<float>(w.scaled_width) - width);
    return std::max(0.0f, std::min(x, limit));
}

float Tab::clamp_y() const noexcept {
    const Window& w = env().window;
    if (w.scaled_height == 0) {
        return std::max(0.0f, y);
    }
    const float limit = std::max(0.0f, static_cast<float>(w.scaled_height) - height);
    return std::max(0.0f, std::min(y, limit));
}

void Tab::init_header(float title_shift) {
    header_ = std::make_unique<TabHeader>(this);
    TabHeader* h = header_.get();
    state = h;
    h->expanded = true;
    // fade modifier 1: alpha 0.4 -> 0.7 while this tab is dragged; 2: gradient 0.5 -> 1.0 while hovered
    h->title_hover = h->add_modifier(std::make_unique<HoverEffect>(
        h, [this] { return screen != nullptr && screen->dragged_tab == this; }, hovered_of(h), 0.4f, 0.7f));
    h->gradient_hover = h->add_modifier(std::make_unique<HoverEffect>(
        h, [this] { return hovered; }, hovered_of(h), 0.5f, 1.0f));
    h->title_shift = title_shift;
    h->owner = this;
    h->width = width;
}

} // namespace slinky::modules::adjust_clickgui::detail
