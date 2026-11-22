#include "modules/visual/stats_overlay.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "adjust/kit.hpp"
#include "cJSON.h"
#include "config/profile_manager.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/gui_renderer.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/module_interop.hpp"
#include "nanovg.h"
#include "util/https_client.hpp"

// Optional built-in Hypixel API key: src/secrets/hypixel_key.hpp is a local, gitignored file holding the key in
// obfuscated form. Without it the client builds normally and only the key from the settings is used.
#if __has_include("secrets/hypixel_key.hpp")
#include "secrets/hypixel_key.hpp"
#define SLINKY_HAS_EMBEDDED_HYPIXEL_KEY 1
#endif

namespace slinky::modules {

namespace {

constexpr const char* kTriggerText = "Protect your bed and destroy the enemy beds";
constexpr const char* kOnlinePrefix = "ONLINE: ";
constexpr int kWhoDelayTicks = 20;        // send /who one second after the start message
constexpr int kRefreshIntervalTicks = 10; // re-resolve UUIDs / teams twice a second
constexpr int kMaxResolveAttempts = 40;   // ~20 s, then the player is shown without stats

void on_game_tick(void* event) noexcept;
void on_incoming_packet(void* event) noexcept;
void on_gui_frame_poll(void* event) noexcept;
void on_gui_render_2d(void* event) noexcept;
void on_module_state_changed(void* event) noexcept;
void on_client_shutdown(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

// =============================================================================================================
// Settings
// =============================================================================================================

void notify_api_key_changed() noexcept;

// Key used when none is entered in the settings.
std::string embedded_api_key() {
#ifdef SLINKY_HAS_EMBEDDED_HYPIXEL_KEY
    return secrets::hypixel_api_key();
#else
    return {};
#endif
}

// Settings key first, otherwise the built-in one.
std::string effective_api_key();

// The API key lives in a one-entry string list (OpenSlinky++ has no plain text setting).
std::vector<std::string> g_api_keys;

std::string effective_api_key() {
    return g_api_keys.empty() ? embedded_api_key() : g_api_keys.front();
}

size_t validate_api_key_input(char* buffer, char ch) noexcept {
    const size_t len = std::strlen(buffer);
    if (len < 36u && (std::isxdigit(static_cast<unsigned char>(ch)) != 0 || ch == '-')) {
        buffer[len] = ch;
    }
    return len;
}

size_t api_key_count(void* /*ctx*/) noexcept {
    return g_api_keys.size();
}

const char* api_key_name(void* /*ctx*/, size_t index) noexcept {
    return index < g_api_keys.size() ? g_api_keys[index].c_str() : "";
}

bool api_key_add(void* /*ctx*/, const char* name, const math::Color3f* /*color*/) noexcept {
    if (name == nullptr || *name == '\0') {
        return false;
    }
    g_api_keys.assign(1, std::string(name)); // only one key
    notify_api_key_changed();
    return true;
}

void api_key_remove(void* /*ctx*/, size_t index) noexcept {
    if (index < g_api_keys.size()) {
        g_api_keys.erase(g_api_keys.begin() + static_cast<std::ptrdiff_t>(index));
        notify_api_key_changed();
    }
}

void api_key_clear(void* /*ctx*/) noexcept {
    g_api_keys.clear();
    notify_api_key_changed();
}

config::DynamicListSetting s_api_key{
    "Hypixel API key",
    "Enter your Hypixel API key",
    validate_api_key_input,
    config::DynamicListItemKind::StringOnly,
    0,
    nullptr,
    api_key_count,
    api_key_name,
    nullptr,
    api_key_add,
    api_key_remove,
    api_key_clear,
    nullptr,
    false,
    {},
};
config::BoolSetting s_auto_who{{}, "Auto /who", true, true, {}};
config::BoolSetting s_show_own_team{{}, "Show own team", true, true, {}};
config::FloatSetting s_max_players{{}, "Max players", "%.0f", 1.0f, 0.0f, 1.0f, 16.0f, 16.0f, 16.0f};
// Position as a fraction of the screen (set by dragging while the chat is open; not shown in the GUI).
config::FloatSetting s_pos_x{{}, "X", "%.3f", 0.0f, 0.0f, 0.0f, 1.0f, 0.01f, 0.01f};
config::FloatSetting s_pos_y{{}, "Y", "%.3f", 0.0f, 0.0f, 0.0f, 1.0f, 0.25f, 0.25f};

events::Module create_module() noexcept {
    events::Module mod{};
    mod.name = "Overlay";
    mod.description = "Bed Wars stats overlay: sends /who and lists FKDR / WR by team. Drag it while the chat is open.";
    mod.category = events::ModuleCategory::Visual;
    // Untagged: available in both visual styles. Global settings so the API key and position are not part of
    // shared profiles.
    mod.flags = events::kModuleFlagGlobalSettings;
    mod.events[events::kEventGameTick] = {&on_game_tick, 0};
    mod.events[events::kEventIncomingPacket] = {&on_incoming_packet, 0};
    mod.events[events::kEventGuiFramePoll] = {&on_gui_frame_poll, 0};
    mod.events[events::kEventGuiRender2D] = {&on_gui_render_2d, 90};
    mod.events[events::kEventModuleStateChanged] = {&on_module_state_changed, 0};
    mod.events[events::kEventClientShutdown] = {&on_client_shutdown, 0};
    mod.render_settings = &render_settings;
    mod.visit_config = &visit_config;
    return mod;
}

// =============================================================================================================
// Stats worker (Hypixel API, background thread)
// =============================================================================================================

enum class StatKind : uint8_t { Loaded, Nicked, Failed };

struct StatResult {
    StatKind kind = StatKind::Failed;
    int stars = 0;
    double fkdr = 0.0;
    double wr = 0.0; // percent
    int finals = 0;
};

enum ApiState : int { kApiOk = 0, kApiNoKey = 1, kApiInvalidKey = 2, kApiRateLimited = 3, kApiNetworkError = 4 };

std::mutex g_mtx;
std::condition_variable g_cv;
std::deque<std::string> g_jobs;
std::unordered_set<std::string> g_requested;
std::unordered_map<std::string, StatResult> g_results;
std::string g_worker_key;
std::atomic<int> g_api_state{kApiNoKey};
std::atomic<bool> g_stop{false};
std::thread g_worker;

double json_number(const cJSON* obj, const char* key) noexcept {
    const cJSON* item = obj != nullptr ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    return cJSON_IsNumber(item) ? cJSON_GetNumberValue(item) : 0.0;
}

StatResult parse_player(const std::string& body) noexcept {
    StatResult r{};
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return r;
    }
    const cJSON* player = cJSON_GetObjectItemCaseSensitive(root, "player");
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "success"))) {
        r.kind = StatKind::Failed;
    } else if (player == nullptr || cJSON_IsNull(player)) {
        r.kind = StatKind::Nicked; // unknown UUID on Hypixel = nicked player
    } else {
        const cJSON* stats = cJSON_GetObjectItemCaseSensitive(player, "stats");
        const cJSON* bw = stats != nullptr ? cJSON_GetObjectItemCaseSensitive(stats, "Bedwars") : nullptr;
        const double fk = json_number(bw, "final_kills_bedwars");
        const double fd = json_number(bw, "final_deaths_bedwars");
        const double wins = json_number(bw, "wins_bedwars");
        const double losses = json_number(bw, "losses_bedwars");
        r.kind = StatKind::Loaded;
        r.stars = static_cast<int>(json_number(cJSON_GetObjectItemCaseSensitive(player, "achievements"), "bedwars_level"));
        r.fkdr = fd > 0.0 ? fk / fd : fk;
        r.wr = (wins + losses) > 0.0 ? wins * 100.0 / (wins + losses) : 0.0;
        r.finals = static_cast<int>(fk);
    }
    cJSON_Delete(root);
    return r;
}

// Sleeps up to `ms`, returns early when stopping.
void interruptible_sleep(int ms) noexcept {
    std::unique_lock<std::mutex> lock(g_mtx);
    g_cv.wait_for(lock, std::chrono::milliseconds(ms), [] { return g_stop.load(); });
}

void worker_main() noexcept {
    while (!g_stop.load()) {
        std::string uuid;
        std::string key;
        {
            std::unique_lock<std::mutex> lock(g_mtx);
            g_cv.wait_for(lock, std::chrono::milliseconds(500), [] {
                return g_stop.load() ||
                       (!g_jobs.empty() && !g_worker_key.empty() && g_api_state.load() != kApiInvalidKey);
            });
            if (g_stop.load()) {
                break;
            }
            if (g_jobs.empty() || g_worker_key.empty() || g_api_state.load() == kApiInvalidKey) {
                continue;
            }
            uuid = g_jobs.front();
            g_jobs.pop_front();
            key = g_worker_key;
        }

        const net::HttpResponse resp =
            net::https_get("api.hypixel.net", "/v2/player?uuid=" + uuid, {{"API-Key", key}}, 5000);

        if (!resp.transport_ok) {
            g_api_state = kApiNetworkError;
            std::lock_guard<std::mutex> lock(g_mtx);
            g_jobs.push_back(uuid); // retry later
        } else if (resp.status == 403) {
            g_api_state = kApiInvalidKey;
            std::lock_guard<std::mutex> lock(g_mtx);
            g_jobs.push_front(uuid);
            continue;
        } else if (resp.status == 429) {
            g_api_state = kApiRateLimited;
            {
                std::lock_guard<std::mutex> lock(g_mtx);
                g_jobs.push_front(uuid);
            }
            interruptible_sleep(15000);
            continue;
        } else {
            StatResult r = resp.status == 200 ? parse_player(resp.body) : StatResult{};
            g_api_state = kApiOk;
            std::lock_guard<std::mutex> lock(g_mtx);
            g_results[uuid] = r;
        }
        interruptible_sleep(150); // pacing between requests
    }
}

void ensure_worker() noexcept {
    if (!g_worker.joinable()) {
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            if (g_worker_key.empty()) {
                g_worker_key = effective_api_key();
                if (!g_worker_key.empty()) {
                    g_api_state = kApiOk;
                }
            }
        }
        g_stop = false;
        try {
            g_worker = std::thread(worker_main);
        } catch (...) {
        }
    }
}

void stop_worker() noexcept {
    g_stop = true;
    g_cv.notify_all();
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

void notify_api_key_changed() noexcept {
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_worker_key = effective_api_key();
        // Retry everything with the new key.
        g_requested.clear();
        g_results.clear();
        g_jobs.clear();
    }
    g_api_state = effective_api_key().empty() ? kApiNoKey : kApiOk;
    g_cv.notify_all();
}

void request_stats(const std::string& uuid) noexcept {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_requested.insert(uuid).second) {
        g_jobs.push_back(uuid);
        g_cv.notify_all();
    }
}

// =============================================================================================================
// Players (main thread)
// =============================================================================================================

struct Entry {
    std::string name;
    std::string uuid;
    uint16_t team = u'f';
    bool self = false;
    bool resolved = false;
    int resolve_attempts = 0;
    bool has_result = false;
    StatResult result{};
};

std::vector<Entry> g_entries;
int g_who_countdown = -1;
int g_refresh_counter = 0;
bool g_trigger_seen = false;
jobject g_last_world = nullptr; // global ref

// Chat lines of interest, handed over from the Netty thread.
std::mutex g_chat_mtx;
std::vector<std::string> g_chat_lines;

std::string strip_formatting(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        // '§' is C2 A7 in UTF-8; skip it and the following code character.
        if (static_cast<unsigned char>(in[i]) == 0xC2 && i + 1 < in.size() &&
            static_cast<unsigned char>(in[i + 1]) == 0xA7) {
            i += 2;
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string jstring_to_utf8(JNIEnv* env, jstring s) {
    if (s == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars != nullptr ? chars : "";
    if (chars != nullptr) {
        env->ReleaseStringUTFChars(s, chars);
    }
    return out;
}

// "ONLINE: a, b, c" -> new player list (results of players already known are kept).
void parse_online_line(const std::string& line) {
    std::vector<Entry> next;
    size_t pos = std::strlen(kOnlinePrefix);
    while (pos < line.size()) {
        size_t comma = line.find(',', pos);
        if (comma == std::string::npos) {
            comma = line.size();
        }
        const std::string name = trim(line.substr(pos, comma - pos));
        pos = comma + 1;
        if (name.empty() || name.size() > 16) {
            continue;
        }
        Entry e{};
        e.name = name;
        for (const Entry& old : g_entries) {
            if (old.name == name) {
                e = old;
            }
        }
        next.push_back(std::move(e));
    }
    if (!next.empty()) {
        g_entries = std::move(next);
        g_refresh_counter = 0; // resolve right away
    }
}

void send_chat(events::GameContext* ctx, const char* message) noexcept {
    events::JniContext* jni = ctx->jni;
    static const jclass cls = adjust::jni::find_class(jni, "bew", "net/minecraft/client/entity/EntityPlayerSP");
    static const jmethodID mid = adjust::jni::method(jni, cls, {"e", "sendChatMessage", "func_71165_d"},
                                                     "(Ljava/lang/String;)V", "(Ljava/lang/String;)V");
    if (mid == nullptr || ctx->local_player == nullptr) {
        return;
    }
    jstring text = jni->env->NewStringUTF(message);
    jni->env->CallVoidMethod(ctx->local_player, mid, text);
    adjust::jni::clear_exception(jni);
    jni->env->DeleteLocalRef(text);
}

// UUID (no dashes) and team colour of `entry` from the tab list.
void resolve_entry(events::GameContext* ctx, Entry& entry, const std::string& self_name) noexcept {
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    static const jclass nh_cls = adjust::jni::find_class(jni, "bcy", "net/minecraft/client/network/NetHandlerPlayClient");
    static const jmethodID get_info = adjust::jni::method(
        jni, nh_cls, {"a", "getPlayerInfo", "func_175104_a"},
        "(Ljava/lang/String;)Lbdc;", "(Ljava/lang/String;)Lnet/minecraft/client/network/NetworkPlayerInfo;");
    static const jclass info_cls = adjust::jni::find_class(jni, "bdc", "net/minecraft/client/network/NetworkPlayerInfo");
    static const jmethodID get_profile = adjust::jni::method(
        jni, info_cls, {"a", "getGameProfile", "func_178845_a"},
        "()Lcom/mojang/authlib/GameProfile;", "()Lcom/mojang/authlib/GameProfile;");
    if (get_info == nullptr || get_profile == nullptr) {
        return;
    }

    entry.self = entry.name == self_name;
    jobject net_handler = minecraft::mc_get_net_handler(jni, ctx->minecraft);
    if (net_handler == nullptr) {
        return;
    }
    jstring jname = env->NewStringUTF(entry.name.c_str());
    jobject info = env->CallObjectMethod(net_handler, get_info, jname);
    adjust::jni::clear_exception(jni);
    env->DeleteLocalRef(jname);
    env->DeleteLocalRef(net_handler);
    if (info == nullptr) {
        return;
    }

    // Team colour (the colour code at the end of the team prefix, e.g. "§c").
    jobject team = minecraft::network_player_info_get_player_team(jni, info);
    if (team != nullptr) {
        entry.team = minecraft::score_player_team_get_color_code(jni, team);
        env->DeleteLocalRef(team);
    }

    if (!entry.resolved) {
        jobject profile = env->CallObjectMethod(info, get_profile);
        adjust::jni::clear_exception(jni);
        if (profile != nullptr) {
            jclass profile_cls = env->GetObjectClass(profile);
            const jmethodID get_id = env->GetMethodID(profile_cls, "getId", "()Ljava/util/UUID;");
            jobject uuid = get_id != nullptr ? env->CallObjectMethod(profile, get_id) : nullptr;
            adjust::jni::clear_exception(jni);
            if (uuid != nullptr) {
                jclass uuid_cls = env->GetObjectClass(uuid);
                const jmethodID to_string = env->GetMethodID(uuid_cls, "toString", "()Ljava/lang/String;");
                auto str = static_cast<jstring>(to_string != nullptr ? env->CallObjectMethod(uuid, to_string) : nullptr);
                adjust::jni::clear_exception(jni);
                std::string text = jstring_to_utf8(env, str);
                text.erase(std::remove(text.begin(), text.end(), '-'), text.end());
                if (text.size() == 32) {
                    entry.uuid = text;
                    entry.resolved = true;
                }
                if (str != nullptr) env->DeleteLocalRef(str);
                env->DeleteLocalRef(uuid_cls);
                env->DeleteLocalRef(uuid);
            }
            env->DeleteLocalRef(profile_cls);
            env->DeleteLocalRef(profile);
        }
    }
    env->DeleteLocalRef(info);
}

std::string local_player_name(events::GameContext* ctx) {
    jstring name = minecraft::entity_get_name(ctx->jni, ctx->local_player);
    std::string out = strip_formatting(jstring_to_utf8(ctx->jni->env, name));
    if (name != nullptr) {
        ctx->jni->env->DeleteLocalRef(name);
    }
    return out;
}

// Netty thread: pick out the start message and the /who answer (cheap: IsInstanceOf + one string read).
void on_incoming_packet(void* event) noexcept {
    auto* ev = static_cast<events::NettyPacketEvent*>(event);
    if (ev->elapsed_ms != 0 || ev->packet == nullptr) {
        return; // re-dispatch of a held packet
    }
    events::JniContext* jni = ev->jni;
    static const jclass s02 = adjust::jni::find_class(jni, "fy", "net/minecraft/network/play/server/S02PacketChat");
    static const jfieldID fid_component = adjust::jni::field(jni, s02, {"a", "chatComponent", "field_148919_a"},
                                                             "Leu;", "Lnet/minecraft/util/IChatComponent;");
    static const jfieldID fid_type = adjust::jni::field(jni, s02, {"b", "type", "field_179842_b"}, "B", "B");
    static const jclass component_cls = adjust::jni::find_class(jni, "eu", "net/minecraft/util/IChatComponent");
    static const jmethodID get_text = adjust::jni::method(jni, component_cls, {"c", "getUnformattedText", "func_150260_c"},
                                                          "()Ljava/lang/String;", "()Ljava/lang/String;");
    if (s02 == nullptr || fid_component == nullptr || get_text == nullptr) {
        return;
    }
    JNIEnv* env = jni->env;
    if (env->IsInstanceOf(ev->packet, s02) != JNI_TRUE) {
        return;
    }
    if (fid_type != nullptr && env->GetByteField(ev->packet, fid_type) == 2) {
        return; // action bar
    }
    jobject component = env->GetObjectField(ev->packet, fid_component);
    if (component == nullptr) {
        return;
    }
    auto text = static_cast<jstring>(env->CallObjectMethod(component, get_text));
    adjust::jni::clear_exception(jni);
    const std::string line = trim(strip_formatting(jstring_to_utf8(env, text)));
    if (text != nullptr) {
        env->DeleteLocalRef(text);
    }
    env->DeleteLocalRef(component);

    if (line.rfind(kTriggerText, 0) == 0 || line.rfind(kOnlinePrefix, 0) == 0) {
        std::lock_guard<std::mutex> lock(g_chat_mtx);
        if (g_chat_lines.size() < 32) {
            g_chat_lines.push_back(line);
        }
    }
}

void reset_game() noexcept {
    g_entries.clear();
    g_who_countdown = -1;
    g_trigger_seen = false;
}

void on_game_tick(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (ctx == nullptr || ctx->world == nullptr || ctx->local_player == nullptr) {
        return;
    }
    JNIEnv* env = ctx->jni->env;
    ensure_worker();

    // New world (new game / lobby): start over.
    if (g_last_world == nullptr || env->IsSameObject(g_last_world, ctx->world) != JNI_TRUE) {
        if (g_last_world != nullptr) {
            env->DeleteGlobalRef(g_last_world);
        }
        g_last_world = env->NewGlobalRef(ctx->world);
        reset_game();
    }

    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_chat_mtx);
        lines.swap(g_chat_lines);
    }
    for (const std::string& line : lines) {
        if (line.rfind(kTriggerText, 0) == 0) {
            if (s_auto_who.value && !g_trigger_seen) {
                g_who_countdown = kWhoDelayTicks;
            }
            g_trigger_seen = true;
        } else if (line.rfind(kOnlinePrefix, 0) == 0) {
            parse_online_line(line);
        }
    }

    if (g_who_countdown > 0 && --g_who_countdown == 0) {
        g_who_countdown = -1;
        send_chat(ctx, "/who");
    }

    if (!g_entries.empty() && g_refresh_counter-- <= 0) {
        g_refresh_counter = kRefreshIntervalTicks;
        const std::string self_name = local_player_name(ctx);
        for (Entry& e : g_entries) {
            if (!e.resolved && e.resolve_attempts++ > kMaxResolveAttempts) {
                continue;
            }
            resolve_entry(ctx, e, self_name); // also refreshes the team (teams are assigned after the start)
            if (e.resolved && !e.self && !e.has_result) {
                request_stats(e.uuid); // never looks up the local player
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_mtx);
    for (Entry& e : g_entries) {
        if (e.resolved && !e.has_result) {
            const auto it = g_results.find(e.uuid);
            if (it != g_results.end()) {
                e.result = it->second;
                e.has_result = true;
            }
        }
    }
}

// =============================================================================================================
// Rendering (Slinky++ and Adjust look) + dragging
// =============================================================================================================

constexpr adjust::Argb kColorCodes[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA, 0xFFAA0000, 0xFFAA00AA, 0xFFFFAA00, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF, 0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF,
};

adjust::Argb team_color(uint16_t code) noexcept {
    if (code >= u'0' && code <= u'9') {
        return kColorCodes[code - u'0'];
    }
    if (code >= u'a' && code <= u'f') {
        return kColorCodes[10 + (code - u'a')];
    }
    return 0xFFFFFFFF;
}

adjust::Argb star_color(int stars) noexcept {
    static constexpr adjust::Argb kPrestige[10] = {
        0xFFAAAAAA, 0xFFFFFFFF, 0xFFFFAA00, 0xFF55FFFF, 0xFF00AA00,
        0xFF00AAAA, 0xFFAA0000, 0xFFFF55FF, 0xFF5555FF, 0xFFAA00AA,
    };
    return stars >= 1000 ? 0xFFFFAA00 : kPrestige[std::clamp(stars / 100, 0, 9)];
}

adjust::Argb fkdr_color(double v) noexcept {
    if (v < 1.0) return 0xFFAAAAAA;
    if (v < 3.0) return 0xFFFFFFFF;
    if (v < 5.0) return 0xFFFFAA00;
    if (v < 10.0) return 0xFFFF5555;
    return 0xFFAA0000;
}

adjust::Argb wr_color(double v) noexcept {
    if (v < 30.0) return 0xFFAAAAAA;
    if (v < 50.0) return 0xFFFFFFFF;
    if (v < 70.0) return 0xFFFFAA00;
    if (v < 85.0) return 0xFFFF5555;
    return 0xFFAA0000;
}

// Sort key: nicked first (unknown = dangerous), then FKDR, then players still loading / without data.
double threat(const Entry& e) noexcept {
    if (e.self) return -2.0;
    if (!e.has_result) return -1.0;
    if (e.result.kind == StatKind::Nicked) return 1e9;
    if (e.result.kind == StatKind::Failed) return -1.0;
    return e.result.fkdr;
}

// Teams ordered by their most dangerous player (own team last), players inside a team by FKDR.
std::vector<const Entry*> sorted_entries() {
    uint16_t own_team = 0;
    for (const Entry& e : g_entries) {
        if (e.self) own_team = e.team;
    }
    std::unordered_map<uint16_t, double> team_threat;
    for (const Entry& e : g_entries) {
        double& t = team_threat.try_emplace(e.team, -3.0).first->second;
        t = std::max(t, threat(e));
    }
    std::vector<const Entry*> out;
    for (const Entry& e : g_entries) {
        if (!s_show_own_team.value && own_team != 0 && e.team == own_team) {
            continue;
        }
        out.push_back(&e);
    }
    std::stable_sort(out.begin(), out.end(), [&](const Entry* a, const Entry* b) {
        if (a->team != b->team) {
            const bool a_own = own_team != 0 && a->team == own_team;
            const bool b_own = own_team != 0 && b->team == own_team;
            if (a_own != b_own) return b_own; // own team at the bottom
            const double ta = team_threat[a->team];
            const double tb = team_threat[b->team];
            if (ta != tb) return ta > tb;
            return a->team < b->team;
        }
        return threat(*a) > threat(*b);
    });
    const auto limit = static_cast<size_t>(s_max_players.value);
    if (out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

struct Cell {
    std::string text;
    adjust::Argb color;
};

struct Row {
    adjust::Argb team;
    Cell star, name, fkdr, wr;
};

std::vector<Row> build_rows() {
    std::vector<Row> rows;
    for (const Entry* e : sorted_entries()) {
        Row r{};
        r.team = team_color(e->team);
        r.name = {e->name, team_color(e->team)};
        char buf[32];
        if (e->self) {
            r.star = {"", 0xFFAAAAAA};
            r.fkdr = {"you", 0xFFAAAAAA};
            r.wr = {"", 0xFFAAAAAA};
        } else if (!e->has_result) {
            r.star = {"", 0xFFAAAAAA};
            r.fkdr = {"...", 0xFFAAAAAA};
            r.wr = {"", 0xFFAAAAAA};
        } else if (e->result.kind == StatKind::Nicked) {
            r.star = {"[?]", 0xFFFF5555};
            r.fkdr = {"NICK", 0xFFFF5555};
            r.wr = {"-", 0xFFAAAAAA};
        } else if (e->result.kind == StatKind::Failed) {
            r.star = {"", 0xFFAAAAAA};
            r.fkdr = {"-", 0xFFAAAAAA};
            r.wr = {"-", 0xFFAAAAAA};
        } else {
            std::snprintf(buf, sizeof(buf), "[%d]", e->result.stars);
            r.star = {buf, star_color(e->result.stars)};
            std::snprintf(buf, sizeof(buf), "%.2f", e->result.fkdr);
            r.fkdr = {buf, fkdr_color(e->result.fkdr)};
            std::snprintf(buf, sizeof(buf), "%.0f%%", e->result.wr);
            r.wr = {buf, wr_color(e->result.wr)};
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

const char* status_text() noexcept {
    if (g_api_keys.empty() && embedded_api_key().empty()) return "Set your Hypixel API key in the Overlay settings";
    switch (g_api_state.load()) {
        case kApiInvalidKey: return "Invalid Hypixel API key";
        case kApiRateLimited: return "Rate limited by Hypixel - retrying";
        case kApiNetworkError: return "Cannot reach api.hypixel.net - retrying";
        default: return nullptr;
    }
}

// Drag state (in the units of the style that drew last frame).
bool g_dragging = false;
float g_drag_dx = 0.0f;
float g_drag_dy = 0.0f;

bool chat_open(events::GameContext* ctx) noexcept {
    events::JniContext* jni = ctx->jni;
    static const jclass gui_chat = adjust::jni::find_class(jni, "awv", "net/minecraft/client/gui/GuiChat");
    if (gui_chat == nullptr || jni->mappings->fid_mc_current_screen == nullptr) {
        return false;
    }
    jobject screen = jni->env->GetObjectField(ctx->minecraft, jni->mappings->fid_mc_current_screen);
    const bool open = screen != nullptr && jni->env->IsInstanceOf(screen, gui_chat) == JNI_TRUE;
    if (screen != nullptr) {
        jni->env->DeleteLocalRef(screen);
    }
    return open;
}

// Mouse position in framebuffer pixels (top-left origin).
bool mouse_fb(events::GameContext* ctx, float fb_height, float* out_x, float* out_y) noexcept {
    events::JniContext* jni = ctx->jni;
    const jclass mouse = jni->mappings->cls_lwjgl_mouse;
    if (mouse == nullptr) {
        return false;
    }
    static const jmethodID get_x = jni->env->GetStaticMethodID(mouse, "getX", "()I");
    static const jmethodID get_y = jni->env->GetStaticMethodID(mouse, "getY", "()I");
    if (get_x == nullptr || get_y == nullptr) {
        adjust::jni::clear_exception(jni);
        return false;
    }
    *out_x = static_cast<float>(jni->env->CallStaticIntMethod(mouse, get_x));
    *out_y = fb_height - static_cast<float>(jni->env->CallStaticIntMethod(mouse, get_y)) - 1.0f;
    return true;
}

// Drags the box while the chat is open; positions are stored as screen fractions and saved on release.
void handle_drag(events::GameContext* ctx, bool chat, float units_per_px, float screen_w, float screen_h,
                 float box_x, float box_y, float box_w, float box_h, float fb_h) noexcept {
    const bool down = chat && minecraft::lwjgl_mouse_is_button_down(ctx->jni, 0);
    float mx = 0.0f;
    float my = 0.0f;
    if (!chat || !mouse_fb(ctx, fb_h, &mx, &my)) {
        if (g_dragging) {
            g_dragging = false;
            config::save_global_settings(ctx->module_manager, ctx->profile_registry);
        }
        return;
    }
    mx *= units_per_px;
    my *= units_per_px;
    if (down && !g_dragging && mx >= box_x && mx <= box_x + box_w && my >= box_y && my <= box_y + box_h) {
        g_dragging = true;
        g_drag_dx = mx - box_x;
        g_drag_dy = my - box_y;
    }
    if (g_dragging && down) {
        const float nx = std::clamp(mx - g_drag_dx, 0.0f, std::max(0.0f, screen_w - box_w));
        const float ny = std::clamp(my - g_drag_dy, 0.0f, std::max(0.0f, screen_h - box_h));
        s_pos_x.value = screen_w > 0.0f ? nx / screen_w : 0.0f;
        s_pos_y.value = screen_h > 0.0f ? ny / screen_h : 0.0f;
    } else if (g_dragging && !down) {
        g_dragging = false;
        config::save_global_settings(ctx->module_manager, ctx->profile_registry);
    }
}

bool should_show(events::GameContext* ctx, bool chat) noexcept {
    return ctx != nullptr && ctx->world != nullptr && ctx->local_player != nullptr &&
           (!g_entries.empty() || chat);
}

void on_gui_frame_poll(void* event) noexcept {
    auto* ev = static_cast<events::GuiFramePollEvent*>(event);
    if (ev->ctx == nullptr || ev->ctx->world == nullptr) {
        return;
    }
    if (!g_entries.empty() || chat_open(ev->ctx)) {
        ev->render_flags |= 3u; // visible; redraw every frame (loading stats, dragging)
    }
}

// ---- Slinky++ look ------------------------------------------------------------------------------------------

void render_slinky(events::GameContext* ctx, gui::GuiRenderer* renderer, bool chat, const std::vector<Row>& rows) {
    NVGcontext* vg = renderer->vg;
    constexpr float kFont = 12.0f;
    constexpr float kRowH = 15.0f;
    constexpr float kPad = 7.0f;
    constexpr float kGap = 10.0f;

    nvgSave(vg);
    nvgFontSize(vg, kFont);
    renderer->select_font(gui::kFontRegular);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    const auto width = [&](const std::string& s) { return nvgTextBounds(vg, 0, 0, s.c_str(), nullptr, nullptr); };

    float w_star = width("[1000]");
    float w_name = width("Player");
    float w_fkdr = width("FKDR");
    float w_wr = width("100%");
    for (const Row& r : rows) {
        w_star = std::max(w_star, width(r.star.text));
        w_name = std::max(w_name, width(r.name.text));
        w_fkdr = std::max(w_fkdr, width(r.fkdr.text));
        w_wr = std::max(w_wr, width(r.wr.text));
    }
    const char* status = status_text();
    const float content_w = w_star + w_name + w_fkdr + w_wr + kGap * 3.0f;
    const float status_w = (status != nullptr && !rows.empty()) ? width(status) : 0.0f;
    const float box_w = std::max(content_w, status_w) + kPad * 2.0f + 4.0f;
    const float rows_h = rows.empty() ? kRowH : static_cast<float>(rows.size()) * kRowH;
    const float box_h = kPad * 2.0f + kRowH /*header*/ + rows_h + ((status != nullptr && !rows.empty()) ? kRowH : 0.0f);

    const float sw = renderer->logical_width;
    const float sh = renderer->logical_height;
    const float bx = std::clamp(s_pos_x.value * sw, 0.0f, std::max(0.0f, sw - box_w));
    const float by = std::clamp(s_pos_y.value * sh, 0.0f, std::max(0.0f, sh - box_h));
    handle_drag(ctx, chat, 1.0f / renderer->scale, sw, sh, bx, by, box_w, box_h, sh * renderer->scale);

    nvgFillColor(vg, nvgRGBA(20, 20, 20, 215));
    renderer->fill_rounded_rect(math::make_rect(bx, by, box_w, box_h), 8.0f);
    if (chat) {
        const math::Color3f acc = gui_get_accent_color();
        nvgStrokeColor(vg, nvgRGBAf(acc.r, acc.g, acc.b, g_dragging ? 1.0f : 0.6f));
        nvgStrokeWidth(vg, 1.0f);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx + 0.5f, by + 0.5f, box_w - 1.0f, box_h - 1.0f, 8.0f);
        nvgStroke(vg);
    }

    const float x0 = bx + kPad + 4.0f;
    const float c_name = x0 + w_star + kGap;
    const float c_fkdr = c_name + w_name + kGap;
    const float c_wr = c_fkdr + w_fkdr + kGap;
    float y = by + kPad + kRowH * 0.5f;

    const math::Color3f acc = gui_get_accent_color();
    renderer->select_font(gui::kFontBold);
    nvgFillColor(vg, nvgRGBf(acc.r, acc.g, acc.b));
    nvgText(vg, x0, y, "Overlay", nullptr);
    renderer->select_font(gui::kFontRegular);
    nvgFillColor(vg, nvgRGBA(153, 153, 153, 255));
    nvgText(vg, c_fkdr, y, "FKDR", nullptr);
    nvgText(vg, c_wr, y, "WR", nullptr);
    y += kRowH;

    const auto cell = [&](float x, const Cell& c) {
        nvgFillColor(vg, adjust::to_nvg(c.color));
        nvgText(vg, x, y, c.text.c_str(), nullptr);
    };
    if (rows.empty()) {
        nvgFillColor(vg, nvgRGBA(153, 153, 153, 255));
        nvgText(vg, x0, y, "Waiting for /who ...", nullptr);
    }
    for (const Row& r : rows) {
        nvgFillColor(vg, adjust::to_nvg(r.team));
        nvgBeginPath(vg);
        nvgRect(vg, bx + kPad - 1.0f, y - kRowH * 0.5f + 2.0f, 2.0f, kRowH - 4.0f);
        nvgFill(vg);
        cell(x0, r.star);
        cell(c_name, r.name);
        cell(c_fkdr, r.fkdr);
        cell(c_wr, r.wr);
        y += kRowH;
    }
    if (status != nullptr && !rows.empty()) {
        nvgFillColor(vg, nvgRGBA(221, 71, 71, 255));
        nvgText(vg, x0, y, status, nullptr);
    }
    nvgRestore(vg);
}

// ---- Adjust look (Tahoma, HudWidget-style panel, HUD colour) -------------------------------------------------

void render_adjust(events::GameContext* ctx, gui::GuiRenderer* renderer, bool chat, const std::vector<Row>& rows) {
    const adjust::HudFrame frame = adjust::begin_hud(renderer, modules::adjust_hud::requested_hud_scale());
    if (!frame.valid) {
        return;
    }
    const adjust::Font font = adjust::kTahoma10;
    constexpr float kRowH = 7.0f;
    constexpr float kPad = 3.0f;
    constexpr float kGap = 5.0f;

    float w_star = font.width("[1000]");
    float w_name = font.width("Player");
    float w_fkdr = font.width("FKDR");
    float w_wr = font.width("100%");
    for (const Row& r : rows) {
        w_star = std::max(w_star, font.width(r.star.text));
        w_name = std::max(w_name, font.width(r.name.text));
        w_fkdr = std::max(w_fkdr, font.width(r.fkdr.text));
        w_wr = std::max(w_wr, font.width(r.wr.text));
    }
    const char* status = status_text();
    const float content_w = w_star + w_name + w_fkdr + w_wr + kGap * 3.0f;
    const float status_w = (status != nullptr && !rows.empty()) ? font.width(status) : 0.0f;
    const float box_w = std::max(content_w, status_w) + kPad * 2.0f + 2.0f;
    const float rows_h = rows.empty() ? kRowH : static_cast<float>(rows.size()) * kRowH;
    const float header_h = kRowH + 2.0f;
    const float box_h = kPad * 2.0f + header_h + rows_h + ((status != nullptr && !rows.empty()) ? kRowH : 0.0f);

    const float bx = std::clamp(s_pos_x.value * frame.width, 0.0f, std::max(0.0f, frame.width - box_w));
    const float by = std::clamp(s_pos_y.value * frame.height, 0.0f, std::max(0.0f, frame.height - box_h));
    handle_drag(ctx, chat, 1.0f / frame.scale, frame.width, frame.height, bx, by, box_w, box_h,
                frame.height * frame.scale);

    const adjust::Argb hud = modules::adjust_hud::hud_color();
    adjust::fill_rect(0x8C131313u, bx, by, bx + box_w, by + box_h);
    adjust::fill_rect(adjust::with_alpha(hud, 0xFF), bx, by, bx + box_w, by + 0.5f); // accent line on top
    if (chat) {
        adjust::stroke_rect(true, adjust::with_alpha(hud, g_dragging ? 0xFF : 0x96), bx, by, bx + box_w, by + box_h, 0.5f);
    }

    const float x0 = bx + kPad + 2.0f;
    const float c_name = x0 + w_star + kGap;
    const float c_fkdr = c_name + w_name + kGap;
    const float c_wr = c_fkdr + w_fkdr + kGap;
    float y = by + kPad;

    adjust::kTahoma12.draw(ADJUST_FMT "lOverlay", x0, y - 0.5f, hud);
    font.draw("FKDR", c_fkdr, y, 0xFF8C8C8Cu);
    font.draw("WR", c_wr, y, 0xFF8C8C8Cu);
    y += header_h;

    if (rows.empty()) {
        font.draw("Waiting for /who ...", x0, y, 0xFF8C8C8Cu);
    }
    for (const Row& r : rows) {
        adjust::fill_rect(r.team, bx + kPad - 0.5f, y + 0.5f, bx + kPad + 0.5f, y + kRowH - 1.0f);
        font.draw(r.star.text, x0, y, r.star.color);
        font.draw(r.name.text, c_name, y, r.name.color);
        font.draw(r.fkdr.text, c_fkdr, y, r.fkdr.color);
        font.draw(r.wr.text, c_wr, y, r.wr.color);
        y += kRowH;
    }
    if (status != nullptr && !rows.empty()) {
        font.draw(status, x0, y, 0xFFDD4747u);
    }
    adjust::end_hud();
}

void on_gui_render_2d(void* event) noexcept {
    auto* ev = static_cast<events::GuiRender2DEvent*>(event);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ev->renderer == nullptr || ctx->world == nullptr) {
        return;
    }
    const bool chat = chat_open(ctx);
    if (!should_show(ctx, chat)) {
        g_dragging = false;
        return;
    }
    const std::vector<Row> rows = build_rows();
    if (events::get_active_visual_style() == events::VisualStyle::Adjust) {
        render_adjust(ctx, ev->renderer, chat, rows);
    } else {
        render_slinky(ctx, ev->renderer, chat, rows);
    }
}

void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    if (ev->module != get_stats_overlay_module()) {
        return;
    }
    if (!ev->enabled) {
        reset_game();
        std::lock_guard<std::mutex> lock(g_mtx);
        g_jobs.clear();
    }
}

void on_client_shutdown(void* event) noexcept {
    stop_worker();
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (g_last_world != nullptr && ctx != nullptr && ctx->jni != nullptr && ctx->jni->env != nullptr) {
        ctx->jni->env->DeleteGlobalRef(g_last_world);
    }
    g_last_world = nullptr;
    reset_game();
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Hypixel");
    s_api_key.render(gui, renderer);
    gui::widgets::render_section_header(gui, renderer, "Main");
    s_auto_who.render(gui, renderer);
    s_show_own_team.render(gui, renderer);
    s_max_players.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    s_api_key.visit(archive, "api_key");
    s_auto_who.visit(archive, "auto_who");
    s_show_own_team.visit(archive, "show_own_team");
    s_max_players.visit(archive, "max_players");
    s_pos_x.visit(archive, "pos_x");
    s_pos_y.visit(archive, "pos_y");
}

} // namespace

events::Module* get_stats_overlay_module() noexcept {
    static events::Module s_module = create_module();
    return &s_module;
}

} // namespace slinky::modules
