#include "modules/altmanager/alt_manager.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/branding.hpp"
#include "events/events.hpp"
#include "gui/gui_internal.hpp"
#include "gui/gui_renderer.hpp"
#include "modules/altmanager/account_store.hpp"
#include "modules/altmanager/msa_auth.hpp"
#include "modules/altmanager/session_inject.hpp"
#include "util/keycodes.hpp"
#include "util/math.hpp"

namespace slinky::altmanager {

namespace {

namespace g = slinky::gui;

enum class Tab : int { Microsoft = 0, Refresh = 1, Session = 2, Cookie = 3 };

enum class HitKind : int {
    None = 0,
    Close,
    Tab,
    Action,
    AccountSelect,
    AccountDelete,
    Field,
};

struct HotRect {
    math::Vec4f rect{};
    HitKind kind = HitKind::None;
    int index = 0;
};

struct AltState {
    events::Module base{};
    bool open = false;
    bool in_menu = true;

    Tab tab = Tab::Microsoft;
    std::string refresh_buf;
    std::string session_buf;
    std::string cookie_buf;
    std::string status; // bottom status line

    math::Vec2f cursor_px{};
    float scale = 1.0f;
    bool ctrl_held = false;
    float list_scroll = 0.0f;

    std::vector<HotRect> hot;

    // Login worker.
    std::unique_ptr<LoginJob> job;
    std::thread job_thread;

    // Pending live injection (applied on the render thread which has the JNI context).
    bool has_pending_inject = false;
    Account pending_inject;
};

AltState& st() {
    static AltState s;
    return s;
}

bool job_running() {
    AltState& s = st();
    if (!s.job) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s.job->mutex);
    return !s.job->done;
}

std::string* active_field() {
    AltState& s = st();
    switch (s.tab) {
        case Tab::Refresh: return &s.refresh_buf;
        case Tab::Session: return &s.session_buf;
        case Tab::Cookie: return &s.cookie_buf;
        default: return nullptr;
    }
}

std::string clipboard_text() {
    std::string out;
    if (!OpenClipboard(nullptr)) {
        return out;
    }
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h != nullptr) {
        auto* w = static_cast<const wchar_t*>(GlobalLock(h));
        if (w != nullptr) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                out.resize(static_cast<size_t>(len));
                WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
                if (!out.empty() && out.back() == '\0') {
                    out.pop_back();
                }
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void start_job(void (*fn)(LoginJob*)) {
    AltState& s = st();
    if (job_running()) {
        return;
    }
    if (s.job_thread.joinable()) {
        s.job_thread.join();
    }
    s.job = std::make_unique<LoginJob>();
    LoginJob* job = s.job.get();
    s.status = "Working...";
    s.job_thread = std::thread([fn, job]() { fn(job); });
}

void start_refresh_job(const std::string& token) {
    AltState& s = st();
    if (job_running()) {
        return;
    }
    if (s.job_thread.joinable()) {
        s.job_thread.join();
    }
    s.job = std::make_unique<LoginJob>();
    LoginJob* job = s.job.get();
    const std::string copy = token;
    s.status = "Working...";
    s.job_thread = std::thread([copy, job]() { run_refresh_login(job, copy); });
}

void start_cookie_job(const std::string& cookies) {
    AltState& s = st();
    if (job_running()) {
        return;
    }
    if (s.job_thread.joinable()) {
        s.job_thread.join();
    }
    s.job = std::make_unique<LoginJob>();
    LoginJob* job = s.job.get();
    const std::string copy = cookies;
    s.status = "Working...";
    s.job_thread = std::thread([copy, job]() { run_cookie_login(job, copy); });
}

// Parses "name", "name:uuid" or "name:uuid:token" into an offline/mojang Account.
Account parse_session_account(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == ':') {
            parts.push_back(current);
            current.clear();
        } else if (c != ' ' && c != '\r' && c != '\n') {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    Account a{};
    a.name = parts.empty() ? "" : parts[0];
    a.uuid = parts.size() > 1 ? parts[1] : "";
    a.token = parts.size() > 2 ? parts[2] : "";
    a.type = a.token.empty() ? AccountType::Offline : AccountType::Mojang;
    a.tag = a.name;
    return a;
}

void queue_inject(const Account& account) {
    AltState& s = st();
    s.has_pending_inject = true;
    s.pending_inject = account;
    s.status = "Switching to " + account.name + "...";
}

void do_action() {
    AltState& s = st();
    switch (s.tab) {
        case Tab::Microsoft:
            start_job(&run_microsoft_login);
            break;
        case Tab::Refresh:
            if (s.refresh_buf.empty()) {
                s.status = "Paste a refresh token first";
            } else {
                start_refresh_job(s.refresh_buf);
            }
            break;
        case Tab::Session: {
            if (s.session_buf.empty()) {
                s.status = "Enter a name (or name:uuid:token)";
                break;
            }
            const Account a = parse_session_account(s.session_buf);
            if (a.name.empty()) {
                s.status = "Invalid session format";
                break;
            }
            upsert(a);
            save();
            queue_inject(a);
            s.session_buf.clear();
            break;
        }
        case Tab::Cookie:
            if (s.cookie_buf.empty()) {
                s.status = "Paste your login cookies first";
            } else {
                start_cookie_job(s.cookie_buf);
            }
            break;
    }
}

void poll_job() {
    AltState& s = st();
    if (!s.job) {
        return;
    }
    bool done = false;
    bool ok = false;
    std::string progress;
    std::string error;
    Account account;
    {
        std::lock_guard<std::mutex> lock(s.job->mutex);
        done = s.job->done;
        ok = s.job->ok;
        progress = s.job->progress;
        error = s.job->error;
        account = s.job->account;
    }
    if (!done) {
        if (!progress.empty()) {
            s.status = progress;
        }
        return;
    }
    if (s.job_thread.joinable()) {
        s.job_thread.join();
    }
    s.job.reset();
    if (ok) {
        upsert(account);
        save();
        queue_inject(account);
        s.status = "Added " + account.name;
    } else {
        s.status = error.empty() ? "Login failed" : error;
    }
}

// ---- rendering --------------------------------------------------------------------------------------------
void add_hot(const math::Vec4f& rect, HitKind kind, int index) {
    st().hot.push_back(HotRect{rect, kind, index});
}

bool hovered(const math::Vec4f& rect) {
    AltState& s = st();
    const math::Vec2f c = math::make_vec2(s.cursor_px.x / s.scale, s.cursor_px.y / s.scale);
    return math::rect_contains(rect, c);
}

const char* action_label(Tab t) {
    switch (t) {
        case Tab::Microsoft: return "Sign in with Microsoft";
        case Tab::Refresh: return "Login with refresh token";
        case Tab::Session: return "Add session account";
        case Tab::Cookie: return "Login with cookies";
    }
    return "";
}

void draw_avatar(g::GuiRenderer* renderer, const math::Vec4f& rect, const std::string& name) {
    NVGcontext* vg = renderer->vg;
    // Deterministic accent-ish color from the name.
    uint32_t h = 2166136261u;
    for (char c : name) {
        h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    const float r = 0.35f + ((h >> 16) & 0x3F) / 160.0f;
    const float gr = 0.35f + ((h >> 8) & 0x3F) / 160.0f;
    const float b = 0.35f + (h & 0x3F) / 160.0f;
    nvgFillColor(vg, nvgRGBf(r, gr, b));
    renderer->fill_rounded_rect(rect, 4.0f);
    char letter[2] = {name.empty() ? '?' : name[0], '\0'};
    if (letter[0] >= 'a' && letter[0] <= 'z') {
        letter[0] = static_cast<char>(letter[0] - 'a' + 'A');
    }
    nvgFontSize(vg, rect.h * 0.55f);
    nvgFillColor(vg, g::detail::color_text_hover());
    renderer->draw_text(rect, g::kTextAlignCenterMiddle, g::kFontBold, letter);
}

void render_overlay(g::GuiRenderer* renderer) {
    AltState& s = st();
    s.hot.clear();
    s.scale = renderer->scale;

    NVGcontext* vg = renderer->vg;
    renderer->draw_fullscreen_dim(0.65f);

    const float lw = renderer->logical_width;
    const float lh = renderer->logical_height;
    const float card_w = std::fmin(600.0f, lw - 24.0f);
    const float card_h = std::fmin(380.0f, lh - 24.0f);
    const math::Vec4f card = math::make_rect((lw - card_w) * 0.5f, (lh - card_h) * 0.5f, card_w, card_h);

    nvgFillColor(vg, g::detail::color_card_bg());
    renderer->fill_rounded_rect(card, 8.0f);
    nvgStrokeColor(vg, g::detail::color_border());
    renderer->stroke_rounded_rect(card, 8.0f);

    const NVGcolor accent = nvgRGBf(0.953f, 0.365f, 0.071f);

    // Header.
    const float pad = 14.0f;
    const math::Vec4f title_rect = math::make_rect(card.x + pad, card.y + 10.0f, card.w - pad * 2.0f, 20.0f);
    nvgFontSize(vg, 16.0f);
    nvgFillColor(vg, g::detail::color_text_primary());
    renderer->draw_text(title_rect, g::kTextAlignLeftMiddle, g::kFontBold, "Alt Manager");

    const math::Vec4f close_rect = math::make_rect(card.x + card.w - 28.0f, card.y + 8.0f, 20.0f, 20.0f);
    nvgFontSize(vg, 16.0f);
    nvgFillColor(vg, hovered(close_rect) ? g::detail::color_danger_hover() : g::detail::color_text_muted());
    renderer->draw_text(close_rect, g::kTextAlignCenterMiddle, g::kFontBold, "X");
    add_hot(close_rect, HitKind::Close, 0);

    const float body_top = card.y + 38.0f;
    const float body_h = card.h - 38.0f - 28.0f;

    // Left: account list.
    const float left_w = card.w * 0.46f;
    const math::Vec4f left = math::make_rect(card.x + pad, body_top, left_w - pad, body_h);
    nvgFontSize(vg, 11.0f);
    nvgFillColor(vg, g::detail::color_text_muted());
    renderer->draw_text(math::make_rect(left.x, left.y, left.w, 14.0f), g::kTextAlignLeftMiddle, g::kFontBold,
                        "Accounts");

    const float row_h = 30.0f;
    const float list_top = left.y + 18.0f;
    const float list_h = left.h - 18.0f;
    nvgSave(vg);
    nvgScissor(vg, left.x, list_top, left.w, list_h);
    const std::vector<Account>& accs = accounts();
    float y = list_top - s.list_scroll;
    for (size_t i = 0; i < accs.size(); ++i) {
        const math::Vec4f row = math::make_rect(left.x, y, left.w, row_h - 4.0f);
        if (y + row_h >= list_top && y <= list_top + list_h) {
            nvgFillColor(vg, hovered(row) ? g::detail::color_widget_hover() : g::detail::color_widget_bg());
            renderer->fill_rounded_rect(row, 4.0f);
            draw_avatar(renderer, math::make_rect(row.x + 3.0f, row.y + 3.0f, row.h - 6.0f, row.h - 6.0f),
                        accs[i].name);
            const math::Vec4f name_rect =
                math::make_rect(row.x + row.h + 2.0f, row.y, row.w - row.h - 36.0f, row.h);
            nvgFontSize(vg, 12.0f);
            nvgFillColor(vg, g::detail::color_text_primary());
            renderer->draw_text(name_rect, g::kTextAlignLeftMiddle, g::kFontRegular,
                                accs[i].tag.empty() ? accs[i].name.c_str() : accs[i].tag.c_str());
            const char* badge = accs[i].type == AccountType::MSA
                                    ? "MSA"
                                    : (accs[i].type == AccountType::Mojang ? "SES" : "OFF");
            nvgFontSize(vg, 9.0f);
            nvgFillColor(vg, accent);
            renderer->draw_text(math::make_rect(row.x + row.h + 2.0f, row.y + row.h - 12.0f, 60.0f, 10.0f),
                                g::kTextAlignLeftMiddle, g::kFontBold, badge);
            const math::Vec4f del = math::make_rect(row.x + row.w - 20.0f, row.y, 18.0f, row.h);
            nvgFontSize(vg, 13.0f);
            nvgFillColor(vg,
                         hovered(del) ? g::detail::color_danger_hover() : g::detail::color_text_muted());
            renderer->draw_text(del, g::kTextAlignCenterMiddle, g::kFontRegular, "x");
            add_hot(del, HitKind::AccountDelete, static_cast<int>(i));
            add_hot(math::make_rect(row.x, row.y, row.w - 22.0f, row.h), HitKind::AccountSelect,
                    static_cast<int>(i));
        }
        y += row_h;
    }
    if (accs.empty()) {
        nvgFontSize(vg, 11.0f);
        nvgFillColor(vg, g::detail::color_text_muted());
        renderer->draw_text(math::make_rect(left.x, list_top + 10.0f, left.w, 14.0f),
                            g::kTextAlignCenterMiddle, g::kFontRegular, "No accounts yet");
    }
    nvgRestore(vg);

    // Right: login tabs.
    const float right_x = card.x + left_w + 6.0f;
    const float right_w = card.x + card.w - pad - right_x;
    const math::Vec4f right = math::make_rect(right_x, body_top, right_w, body_h);

    const float tab_w = right.w / 4.0f;
    for (int i = 0; i < 4; ++i) {
        const Tab t = static_cast<Tab>(i);
        const math::Vec4f tr = math::make_rect(right.x + tab_w * i, right.y, tab_w - 2.0f, 18.0f);
        const bool active = s.tab == t;
        nvgFillColor(vg, active ? accent : g::detail::color_widget_bg());
        renderer->fill_rounded_rect(tr, 3.0f);
        nvgFontSize(vg, 9.5f);
        nvgFillColor(vg, active ? g::detail::color_text_hover() : g::detail::color_text_muted());
        // Short tab captions.
        static const char* kShort[4] = {"Microsoft", "Refresh", "Session", "Cookie"};
        renderer->draw_text(tr, g::kTextAlignCenterMiddle, g::kFontBold, kShort[i]);
        add_hot(tr, HitKind::Tab, i);
    }

    // Description + input.
    const float input_y = right.y + 28.0f;
    nvgFontSize(vg, 11.0f);
    nvgFillColor(vg, g::detail::color_text_muted());
    const char* desc = "";
    switch (s.tab) {
        case Tab::Microsoft:
            desc = "Opens your browser to sign in to a Microsoft account.";
            break;
        case Tab::Refresh:
            desc = "Paste an MSA refresh token (Ctrl+V).";
            break;
        case Tab::Session:
            desc = "Enter name, or name:uuid:token for a session.";
            break;
        case Tab::Cookie:
            desc = "Paste login.live.com cookies incl. __Host-MSAAUTHP.";
            break;
    }
    renderer->draw_text(math::make_rect(right.x, input_y, right.w, 14.0f), g::kTextAlignLeftMiddle,
                        g::kFontRegular, desc);

    std::string* field = active_field();
    if (field != nullptr) {
        const math::Vec4f box = math::make_rect(right.x, input_y + 20.0f, right.w, 24.0f);
        nvgFillColor(vg, g::detail::color_widget_bg());
        renderer->fill_rounded_rect(box, 4.0f);
        nvgStrokeColor(vg, accent);
        renderer->stroke_rounded_rect(box, 4.0f);
        // Show the tail of the buffer so long tokens stay visible, masked for secrets.
        std::string shown = *field;
        if (s.tab != Tab::Session && shown.size() > 28) {
            shown = "..." + shown.substr(shown.size() - 25);
        } else if (shown.size() > 40) {
            shown = shown.substr(shown.size() - 40);
        }
        if (shown.empty()) {
            shown = "(paste here)";
            nvgFillColor(vg, g::detail::color_text_muted());
        } else {
            nvgFillColor(vg, g::detail::color_text_primary());
        }
        nvgFontSize(vg, 11.0f);
        renderer->draw_text(math::make_rect(box.x + 6.0f, box.y, box.w - 12.0f, box.h),
                            g::kTextAlignLeftMiddle, g::kFontRegular, shown.c_str());
        add_hot(box, HitKind::Field, 0);
    }

    // Action button.
    const math::Vec4f action = math::make_rect(right.x, right.y + right.h - 28.0f, right.w, 24.0f);
    const bool busy = job_running();
    nvgFillColor(vg, busy ? g::detail::color_widget_bg()
                          : (hovered(action) ? g::detail::color_danger_hover() : accent));
    renderer->fill_rounded_rect(action, 5.0f);
    nvgFontSize(vg, 12.0f);
    nvgFillColor(vg, g::detail::color_text_hover());
    renderer->draw_text(action, g::kTextAlignCenterMiddle, g::kFontBold,
                        busy ? "Please wait..." : action_label(s.tab));
    if (!busy) {
        add_hot(action, HitKind::Action, 0);
    }

    // Status line.
    if (!s.status.empty()) {
        nvgFontSize(vg, 10.5f);
        nvgFillColor(vg, g::detail::color_text_muted());
        renderer->draw_text(math::make_rect(card.x + pad, card.y + card.h - 22.0f, card.w - pad * 2.0f, 14.0f),
                            g::kTextAlignLeftMiddle, g::kFontRegular, s.status.c_str());
    }
}

// ---- hit handling -----------------------------------------------------------------------------------------
void on_left_press() {
    AltState& s = st();
    const math::Vec2f c = math::make_vec2(s.cursor_px.x / s.scale, s.cursor_px.y / s.scale);
    for (const HotRect& hr : s.hot) {
        if (!math::rect_contains(hr.rect, c)) {
            continue;
        }
        switch (hr.kind) {
            case HitKind::Close:
                close();
                return;
            case HitKind::Tab:
                s.tab = static_cast<Tab>(hr.index);
                return;
            case HitKind::Action:
                do_action();
                return;
            case HitKind::Field:
                return; // field is always "focused" for its tab
            case HitKind::AccountDelete:
                if (static_cast<size_t>(hr.index) < accounts().size()) {
                    remove(static_cast<size_t>(hr.index));
                    save();
                }
                return;
            case HitKind::AccountSelect:
                if (static_cast<size_t>(hr.index) < accounts().size()) {
                    queue_inject(accounts()[static_cast<size_t>(hr.index)]);
                }
                return;
            default:
                break;
        }
    }
}

void type_char(const char* utf8) {
    std::string* field = active_field();
    if (field == nullptr || utf8 == nullptr || utf8[0] == '\0') {
        return;
    }
    const unsigned char first = static_cast<unsigned char>(utf8[0]);
    if (first < 0x20) {
        return; // ignore control chars
    }
    for (int i = 0; i < 4 && utf8[i] != '\0'; ++i) {
        field->push_back(utf8[i]);
    }
}

// ---- module callbacks -------------------------------------------------------------------------------------
void on_frame_poll(void* raw_event) noexcept {
    auto* ev = static_cast<events::GuiFramePollEvent*>(raw_event);
    AltState& s = st();
    s.in_menu = (ev->ctx == nullptr) || (ev->ctx->world == nullptr);
    if (s.open) {
        ev->render_flags |= 3u; // visible + needs redraw
    }
}

void on_render(void* raw_event) noexcept {
    auto* ev = static_cast<events::GuiRender2DEvent*>(raw_event);
    AltState& s = st();
    poll_job();
    // Apply any pending live session switch now that we hold a JNI context.
    if (s.has_pending_inject && ev->ctx != nullptr) {
        const bool ok = inject_session(ev->ctx->jni, ev->ctx->minecraft, s.pending_inject);
        s.status = ok ? ("Now playing as " + s.pending_inject.name)
                      : ("Saved " + s.pending_inject.name + " (switch in-game)");
        s.has_pending_inject = false;
    }
    if (!s.open || ev->renderer == nullptr) {
        return;
    }
    render_overlay(ev->renderer);
}

void on_shutdown(void* raw_event) noexcept {
    (void)raw_event;
    AltState& s = st();
    if (s.job_thread.joinable()) {
        s.job_thread.detach(); // the worker only touches its own LoginJob + network
    }
}

void render_settings(g::ClickGui* gui, g::GuiRenderer* renderer) noexcept {
    // The Adjust Tab/Click GUI calls this with null pointers to "capture" a module's settings list; this module
    // exposes no captured settings, so bail out before touching the (null) gui/renderer.
    if (gui == nullptr || renderer == nullptr) {
        return;
    }
    const math::Vec4f cell = gui->next_cell(22.0f);
    if (g::detail::render_action_button(gui, renderer, "Open Alt Manager", true, cell)) {
        open();
    }
}

events::Module create_module() noexcept {
    events::Module mod{};
    mod.name = "Alt Manager";
    mod.description = "Manage and switch Minecraft accounts.";
    mod.category = events::ModuleCategory::Utility;
    mod.enabled = true; // always enabled so its render/poll callbacks run; visibility is the `open` flag
    mod.flags = events::kModuleFlagNoToggleSwitch | events::kModuleFlagHiddenFromHud |
                events::kModuleFlagNoKeybind | events::kModuleFlagGlobalSettings;
    mod.events[events::kEventGuiFramePoll] = {&on_frame_poll, 0};
    mod.events[events::kEventGuiRender2D] = {&on_render, -100}; // draw last (on top of everything)
    mod.events[events::kEventClientShutdown] = {&on_shutdown, 0};
    mod.render_settings = &render_settings;
    return mod;
}

} // namespace

events::Module* get_alt_manager_module() noexcept {
    static AltState& s = st();
    static bool initialized = false;
    if (!initialized) {
        s.base = create_module();
        load(); // load stored accounts once
        initialized = true;
    }
    return &s.base;
}

void open() noexcept {
    AltState& s = st();
    s.open = true;
    s.status.clear();
}

void close() noexcept {
    st().open = false;
}

bool is_open() noexcept {
    return st().open;
}

bool is_in_menu() noexcept {
    return st().in_menu;
}

bool handle_input(const input::InputEvent* ev, double now) noexcept {
    (void)now;
    AltState& s = st();
    if (!s.open || ev == nullptr) {
        return false;
    }
    switch (ev->kind) {
        case input::InputEventKind::MouseMove:
            s.cursor_px = math::make_vec2(static_cast<float>(ev->move.x), static_cast<float>(ev->move.y));
            return true;
        case input::InputEventKind::MouseScroll:
            s.list_scroll = std::fmax(0.0f, s.list_scroll - ev->scroll.delta * 18.0f);
            return true;
        case input::InputEventKind::KeyOrButton: {
            const int key = ev->key.key_code;
            if (ev->pressed == 0) {
                if (key == input::KEY_LCONTROL || key == input::KEY_RCONTROL) {
                    s.ctrl_held = false;
                }
                return true;
            }
            // pressed
            if (key == input::KEY_LCONTROL || key == input::KEY_RCONTROL) {
                s.ctrl_held = true;
                return true;
            }
            if (key == input::KEY_MOUSE1) {
                on_left_press();
                return true;
            }
            if (key == input::KEY_ESC) {
                close();
                return true;
            }
            if (key == input::KEY_RSHIFT) {
                return true; // Right Shift toggle is handled in gui_handle_window_input (repeat-safe)
            }
            if (key == input::KEY_BACKSPACE) {
                std::string* field = active_field();
                if (field != nullptr && !field->empty()) {
                    field->pop_back();
                }
                return true;
            }
            if (key == input::KEY_ENTER) {
                do_action();
                return true;
            }
            if (s.ctrl_held && key == input::KEY_V) {
                std::string* field = active_field();
                if (field != nullptr) {
                    const std::string clip = clipboard_text();
                    for (char ch : clip) {
                        if (static_cast<unsigned char>(ch) >= 0x20 || static_cast<unsigned char>(ch) >= 0x80) {
                            field->push_back(ch);
                        }
                    }
                }
                return true;
            }
            if (key == 0 && ev->key.utf8_text[0] != '\0') {
                type_char(ev->key.utf8_text); // WM_CHAR text event
            }
            return true;
        }
        default:
            return true; // swallow everything else while open
    }
}

} // namespace slinky::altmanager
