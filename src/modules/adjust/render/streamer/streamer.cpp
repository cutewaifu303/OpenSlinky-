// Port of com.adjustclient.module.impl.render.streamer.{StreamerModule, StreamerName, StreamerNameType,
// FirstNameSource} and com.adjustclient.util.name.{WordNameGenerator, RandomCharNameGenerator}:
// "Hides player names and game information."
//
// Ported here:
//   - the name map (HashBiMap real name <-> generated name, 1025-entry / 60 s refresh rule, unique suffixes),
//   - both generators with java.util.Random reproduced bit for bit (StreamerNameType seeds: name.hashCode() << 32
//     | Objects.hash(0, 0L ^ 0x20L)), word lists embedded (streamer_words.cpp),
//   - StreamerName (server id masking "mini123AB" -> "mini45K"), maskNamesInText (incl. "§7...§7" truncation),
//     replaceKnownNames, restoreNameInText, scoreboard line masking, chat sender parsing,
//   - keeping the map filled: on enable / world change (S01PacketJoinGame) the session name and every tab-list
//     name are registered (refreshWorldNames); new tab-list entries are registered every 5 ticks (Adjust: on
//     S38PacketPlayerListItem ADD_PLAYER),
//   - onEnable / onDisable re-render the chat history (GuiNewChat.refreshChat).
//
// The Minecraft side (chat, scoreboard, tab list, name tags, signs, chat sending) lives in
// src/adjust/hooks/streamer_hooks.cpp, which calls the functions of streamer.hpp.
//
// Skipped (Adjust backend): the account nickname (UserConfiguration "nick", which keeps the map alive while the
// module is off and names the local player), the party id in the generator seed (no-party value used) and
// Adjust.username in the StreamerName seed (the Minecraft session name is hashed instead). The Hypixel-only
// sender parsing (ServerTracker server type) is approximated by the current server address containing
// "hypixel.net" (streamer_hooks.cpp).

#include "modules/adjust/render/streamer/streamer.hpp"

#include <jni.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "adjust/kit.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/streamer/streamer_words.hpp"
#include "util/random.hpp"

namespace slinky::modules {

namespace {

constexpr const char* kNameTypeOptions[2] = {"Usernames", "First Names"}; // StreamerNameType
enum NameType : int32_t { kNameTypeUsernames = 0, kNameTypeFirstNames = 1 };

constexpr const char* kSection = ADJUST_FMT; // UTF-8 "§"
constexpr size_t kMapRefreshSize = 1025;
constexpr int64_t kMapRefreshMs = 60000;
constexpr int kTabPollTicks = 5;

void on_game_tick(void* event) noexcept;
void on_module_state_changed(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

// ---- java.util.Random -----------------------------------------------------------------------------------------

class JavaRandom {
public:
    explicit JavaRandom(int64_t seed) noexcept
        : seed_((static_cast<uint64_t>(seed) ^ kMultiplier) & kMask) {}

    int32_t next(int bits) noexcept {
        seed_ = (seed_ * kMultiplier + kAddend) & kMask;
        return static_cast<int32_t>(static_cast<uint32_t>(seed_ >> (48 - bits)));
    }
    int32_t next_int() noexcept { return next(32); }
    int32_t next_int(int32_t bound) noexcept {
        if (bound <= 0) {
            return 0; // Java throws; never reached with the bounds used here
        }
        if ((bound & -bound) == bound) {
            return static_cast<int32_t>((static_cast<int64_t>(bound) * static_cast<int64_t>(next(31))) >> 31);
        }
        int32_t bits = 0;
        int32_t value = 0;
        do {
            bits = next(31);
            value = bits % bound;
        } while (static_cast<int64_t>(bits) - value + (bound - 1) > INT32_MAX);
        return value;
    }
    bool next_boolean() noexcept { return next(1) != 0; }
    double next_double() noexcept {
        const int64_t high = static_cast<int64_t>(next(26)) << 27;
        return static_cast<double>(high + next(27)) * (1.0 / static_cast<double>(1ULL << 53));
    }

private:
    static constexpr uint64_t kMultiplier = 0x5DEECE66DULL;
    static constexpr uint64_t kAddend = 0xBULL;
    static constexpr uint64_t kMask = (1ULL << 48) - 1;
    uint64_t seed_;
};

// ---- Java string helpers (ASCII / UTF-8) ------------------------------------------------------------------------

int32_t java_hash(std::string_view s) noexcept { // String.hashCode (ASCII names)
    uint32_t h = 0;
    for (unsigned char c : s) {
        h = 31u * h + c;
    }
    return static_cast<int32_t>(h);
}

int32_t hash_util(std::string_view s) noexcept { // HashUtil.hash(String): UTF-16BE bytes, 33 * hash + b
    uint32_t h = 64206u;
    for (unsigned char c : s) {
        h = 33u * h + 0u;                                         // high byte of the UTF-16 unit
        h = 33u * h + static_cast<uint32_t>(static_cast<int8_t>(c)); // low byte (signed)
    }
    return static_cast<int32_t>(h);
}

bool is_word_char(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_java_space(char c) noexcept { // \s
    return c == ' ' || c == '\t' || c == '\n' || c == '\x0B' || c == '\f' || c == '\r';
}

bool is_word_name(std::string_view s) noexcept { // ^\w{2,16}$
    if (s.size() < 2 || s.size() > 16) {
        return false;
    }
    for (char c : s) {
        if (!is_word_char(c)) {
            return false;
        }
    }
    return true;
}

size_t utf8_len(unsigned char lead) noexcept {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

size_t code_points(std::string_view s) noexcept {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i += utf8_len(static_cast<unsigned char>(s[i]))) {
        ++n;
    }
    return n;
}

// Byte length of the first `count` code points (or nullopt if the string is shorter).
std::optional<size_t> prefix_bytes(std::string_view s, size_t count) noexcept {
    size_t i = 0;
    for (size_t n = 0; n < count; ++n) {
        if (i >= s.size()) {
            return std::nullopt;
        }
        i += utf8_len(static_cast<unsigned char>(s[i]));
    }
    return std::min(i, s.size());
}

bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// String.replace(CharSequence, CharSequence) (literal, all occurrences).
std::string replace_all(std::string_view text, std::string_view from, std::string_view to) {
    std::string out;
    if (from.empty()) {
        return std::string(text);
    }
    size_t pos = 0;
    while (true) {
        const size_t hit = text.find(from, pos);
        if (hit == std::string_view::npos) {
            out.append(text.substr(pos));
            return out;
        }
        out.append(text.substr(pos, hit - pos));
        out.append(to);
        pos = hit + from.size();
    }
}

// String.split(" "): trailing empty strings removed.
std::vector<std::string_view> split_space(std::string_view text) {
    std::vector<std::string_view> parts;
    if (text.find(' ') == std::string_view::npos) {
        parts.push_back(text);
        return parts;
    }
    size_t pos = 0;
    while (true) {
        const size_t hit = text.find(' ', pos);
        if (hit == std::string_view::npos) {
            parts.push_back(text.substr(pos));
            break;
        }
        parts.push_back(text.substr(pos, hit - pos));
        pos = hit + 1;
    }
    while (!parts.empty() && parts.back().empty()) {
        parts.pop_back();
    }
    return parts;
}

std::string keep_alnum(std::string_view s) { // replaceAll("[^a-zA-Z0-9]", "")
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 && static_cast<unsigned char>(c) < 0x80) {
            out.push_back(c);
        }
    }
    return out;
}

// StringUtils.stripControlCodes: removes "§" + [0-9a-fk-or] (case-insensitive).
std::string strip_control_codes(std::string_view s) {
    std::string out;
    const std::string_view section = kSection;
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, section.size(), section) == 0 && i + section.size() < s.size()) {
            const char code = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i + section.size()])));
            if ((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f') || (code >= 'k' && code <= 'o') ||
                code == 'r') {
                i += section.size() + 1;
                continue;
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

std::string capitalize(std::string_view s) { // StringUtils.capitalize (ASCII words)
    std::string out(s);
    if (!out.empty()) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// RandomStringUtils.randomNumeric (commons-lang3 uses its own unseeded Random).
std::string random_numeric(int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out.push_back(static_cast<char>('0' + static_cast<int>(random::next_u32() % 10u)));
    }
    return out;
}

// ---- name generators --------------------------------------------------------------------------------------------

// WordNameGenerator.generate
std::string word_name(JavaRandom& random) {
    const bool use_words = random.next_boolean();
    std::string prefix;
    std::optional<std::string> suffix;
    if (random.next_double() > 0.8) {
        switch (random.next_int(8)) {
            case 0: prefix = "_"; break;
            case 1: prefix = "xx"; if (random.next_double() > 0.4) suffix = "xx"; break;
            case 2: prefix = "ii"; if (random.next_double() > 0.7) suffix = "ii"; break;
            case 3: prefix = "x"; if (random.next_double() > 0.8) suffix = "x"; break;
            case 4: {
                const bool taiwan = random.next_boolean();
                suffix = taiwan ? (random.next_boolean() ? "_TW" : "TW") : (random.next_boolean() ? "_CN" : "CN");
                break;
            }
            case 5: suffix = random.next_boolean() ? "PVP" : "PvP"; break;
            case 6: suffix = "YT"; break;
            case 7: suffix = "Plays"; break;
            default: break;
        }
    }
    std::string result = prefix;
    const std::string adjective = capitalize(
        adjust_streamer::kAdjectives[random.next_int(static_cast<int32_t>(adjust_streamer::kAdjectivesCount) - 1)]);
    const std::string noun =
        capitalize(adjust_streamer::kNouns[random.next_int(static_cast<int32_t>(adjust_streamer::kNounsCount) - 1)]);
    if (!use_words) {
        switch (random.next_int(14)) {
            case 0: result = prefix + noun + "Is" + adjective; break;
            case 1: result = prefix + noun + "May" + adjective; break;
            case 2: result = prefix + adjective + random_numeric(random.next_int(2) + 1) + noun; break;
            case 3: result = prefix + lower(adjective + noun) + random_numeric(random.next_int(2) + 1); break;
            case 4: result = prefix + lower(adjective) + random_numeric(random.next_int(6) + 1); break;
            case 5: {
                const int digits = random.next_int(2) + 1;
                result = prefix + lower(adjective) + random_numeric(digits) + lower(noun);
                break;
            }
            case 6: result = prefix + lower(noun) + random_numeric(random.next_int(7) + 1); break;
            case 7: result = prefix + lower(adjective + noun); break;
            case 8: result = prefix + lower(adjective) + random_numeric(1) + lower(noun); break;
            case 9: result = prefix + lower(adjective) + "_" + random_numeric(random.next_int(6) + 1); break;
            case 10: result = prefix + lower(noun) + "_" + random_numeric(random.next_int(6) + 1); break;
            case 11: result = prefix + adjective + "_" + random_numeric(random.next_int(6) + 1); break;
            case 12: result = prefix + noun + "_" + random_numeric(random.next_int(6) + 1); break;
            case 13: result = prefix + noun + "Not" + adjective; break;
            default: result = prefix; break;
        }
    } else {
        const auto count = static_cast<int32_t>(adjust_streamer::kNamesCount);
        const std::string first = adjust_streamer::kNames[random.next_int(count - 1)];
        const std::string second = adjust_streamer::kNames[random.next_int(count - 1)];
        switch (random.next_int(12)) {
            case 0: result = prefix + first + second + "_" + random_numeric(1); break;
            case 1: result = prefix + first + random_numeric(random.next_int(7) + 1); break;
            case 2: result = prefix + first + "_" + second; break;
            case 3: result = prefix + lower(first + second) + random_numeric(random.next_int(2) + 1); break;
            case 4: {
                const int digits = random.next_int(2) + 1;
                result = prefix + lower(first) + random_numeric(digits) + lower(second);
                break;
            }
            case 5: result = prefix + adjective + "_" + first; break;
            case 6: result = prefix + lower(first) + random_numeric(random.next_int(7) + 1); break;
            case 7: result = prefix + adjective + random_numeric(random.next_int(2) + 1) + first; break;
            case 8: result = prefix + second + adjective; break;
            case 9: result = prefix + first + "da" + (random.next_double() <= 0.5 ? noun : capitalize(noun)); break;
            case 10: result = prefix + first + "The" + capitalize(noun); break;
            case 11: result = prefix + lower(first + second); break;
            default: result = prefix; break;
        }
    }
    if (suffix.has_value()) {
        result += *suffix;
    }
    return result.size() < 17 ? result : result.substr(0, 16);
}

// RandomCharNameGenerator.generate
std::string first_name(JavaRandom& random) {
    const auto count = static_cast<int32_t>(adjust_streamer::kNamesCount);
    return count == 1 ? adjust_streamer::kNames[0] : adjust_streamer::kNames[random.next_int(count)];
}

// ---- StreamerName / FirstNameSource -------------------------------------------------------------------------------

struct NameSource {
    const char* prefix;
    char abbreviation;
};
constexpr NameSource kSources[3] = {{"dynamiclobby", 'L'}, {"mini", 'm'}, {"mega", 'M'}}; // LOBBY, MINI, MEGA
constexpr int kSourceMega = 2;

int source_from_abbreviation(char c) noexcept {
    for (int i = 0; i < 3; ++i) {
        if (kSources[i].abbreviation == c) {
            return i;
        }
    }
    return -1;
}

int source_from_prefix(std::string_view prefix) noexcept {
    for (int i = 0; i < 3; ++i) {
        if (prefix == kSources[i].prefix) {
            return i;
        }
    }
    return -1;
}

struct StreamerName {
    std::string original;    // suffix after the source prefix
    std::string replacement; // generated digits + letter
    int source = 0;

    bool matches(std::string_view name) const noexcept {
        if (name.empty()) {
            return false;
        }
        return source_from_abbreviation(name[0]) == source && original == name.substr(1);
    }

    std::string display(bool abbreviated) const {
        return (abbreviated ? std::string(1, kSources[source].abbreviation) : std::string(kSources[source].prefix)) +
               replacement;
    }
};

// ---- module state -----------------------------------------------------------------------------------------------

struct StreamerState {
    events::Module base{};

    config::ModeSetting name_type{{}, "Name Type", 2, 0, kNameTypeOptions, kNameTypeUsernames, kNameTypeUsernames,
                                  false, {}};

    std::recursive_mutex lock; // nameRefreshLock
    bool map_active = false;   // nameMap != null
    std::vector<std::string> order;                     // keys in insertion order
    std::unordered_map<std::string, std::string> names; // nameMap
    std::unordered_map<std::string, std::string> masks; // reverseNameMap
    int64_t refresh_timer_ms = 0;                       // nameRefreshTimer
    std::optional<StreamerName> server_names;
    int32_t session_hash = 0;
    bool world_refresh_requested = false;

    std::vector<std::string> tab_names; // last tab-list profile names (PlayerNameCompleter source)
    uint64_t generation = 1;            // bumped whenever the name map changes

    jweak last_world = nullptr;
    int tick_counter = 0;

    StreamerState() noexcept {
        base.name = "Streamer";
        base.description = "Hides player names and game information.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagHiddenFromHud; // Adjust default: hidden
        base.events[events::kEventGameTick] = {on_game_tick, 0, 0};
        base.events[events::kEventModuleStateChanged] = {on_module_state_changed, 0, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Streamer");
    }
};

StreamerState g_state{};

bool module_running() noexcept {
    return events::module_is_running(&g_state.base);
}

void reset_map_locked(bool active) {
    ++g_state.generation;
    g_state.map_active = active;
    g_state.order.clear();
    g_state.names.clear();
    g_state.masks.clear();
}

// HashBiMap.forcePut
void force_put_locked(const std::string& name, const std::string& masked) {
    ++g_state.generation;
    auto value_owner = g_state.masks.find(masked);
    if (value_owner != g_state.masks.end() && value_owner->second != name) {
        const std::string other = value_owner->second;
        g_state.names.erase(other);
        g_state.order.erase(std::remove(g_state.order.begin(), g_state.order.end(), other), g_state.order.end());
        g_state.masks.erase(value_owner);
    }
    auto existing = g_state.names.find(name);
    if (existing != g_state.names.end()) {
        g_state.masks.erase(existing->second);
        existing->second = masked;
    } else {
        g_state.names.emplace(name, masked);
        g_state.order.push_back(name);
    }
    g_state.masks[masked] = name;
}

// StreamerNameType.createGenerator(name).generate()
std::string generate_name(std::string_view name) {
    constexpr uint32_t kIdentity = 993; // Objects.hash(0, 0L ^ 0x20L) (no party)
    const auto seed = static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(java_hash(name))) << 32) |
                                           kIdentity);
    JavaRandom random(seed);
    return g_state.name_type.value == kNameTypeFirstNames ? first_name(random) : word_name(random);
}

// new StreamerName(String) - nullopt where Java throws.
std::optional<StreamerName> make_streamer_name(std::string_view name) {
    if (name.size() < 2) {
        return std::nullopt;
    }
    int kind = -1;
    std::string_view suffix;
    if (std::isdigit(static_cast<unsigned char>(name[1])) != 0) {
        kind = source_from_abbreviation(name[0]);
        suffix = name.substr(1);
    } else {
        size_t first_digit = 0;
        while (first_digit < name.size() && std::isdigit(static_cast<unsigned char>(name[first_digit])) == 0) {
            ++first_digit;
        }
        kind = source_from_prefix(name.substr(0, first_digit));
        suffix = name.substr(first_digit);
    }
    if (kind < 0) {
        return std::nullopt; // "Type not found?"
    }
    int64_t numeric = 0;
    bool any_digit = false;
    for (char c : suffix) {
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            numeric = numeric * 10 + (c - '0');
            any_digit = true;
            if (numeric > INT32_MAX) {
                return std::nullopt; // Integer.parseInt overflow
            }
        }
    }
    if (!any_digit) {
        return std::nullopt;
    }
    const auto seed = static_cast<uint32_t>(kind) ^ 0u /* (int) (seedFactor * 1e6), seedFactor = 0 */ ^
                      static_cast<uint32_t>(numeric) ^ static_cast<uint32_t>(g_state.session_hash);
    std::string candidate;
    for (uint32_t candidate_seed = seed;; ++candidate_seed) {
        JavaRandom random(static_cast<int64_t>(static_cast<int32_t>(candidate_seed)));
        const int count = random.next_int(kind == kSourceMega ? 1 : 2) + 1;
        candidate.clear();
        for (int i = 0; i < count; ++i) {
            candidate.push_back("1234567890"[random.next_int(10)]);
        }
        if (kind != kSourceMega) {
            random.next_int(); // the non-MEGA path advances the state once before the suffix letter
        }
        candidate.push_back("ABCDEFGHIJKLMNOPQRSTUVWXYZ"[random.next_int(26)]);
        if (static_cast<int32_t>(candidate_seed + 1u) >= 17 || !equals_ignore_case(candidate, name)) {
            break;
        }
    }
    return StreamerName{std::string(suffix), candidate, kind};
}

// \b(?:dynamiclobby|mini|mega)\d+[A-Z]{1,2}\b on an alphanumeric word (matches()).
bool is_server_id(std::string_view name) noexcept {
    for (const NameSource& source : kSources) {
        const std::string_view prefix = source.prefix;
        if (!starts_with(name, prefix)) {
            continue;
        }
        size_t i = prefix.size();
        const size_t digits_start = i;
        while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])) != 0) {
            ++i;
        }
        if (i == digits_start) {
            continue;
        }
        const size_t letters = name.size() - i;
        if (letters < 1 || letters > 2) {
            continue;
        }
        bool upper = true;
        for (size_t j = i; j < name.size(); ++j) {
            upper = upper && name[j] >= 'A' && name[j] <= 'Z';
        }
        if (upper) {
            return true;
        }
    }
    return false;
}

// maskNamesInText's `Pattern.quote(prefix) + "(.){1,2}§7\\.\\.\\.§7"` replaceAll.
std::string replace_truncated(std::string_view text, std::string_view prefix, std::string_view value) {
    const std::string tail = std::string(kSection) + "7..." + kSection + "7";
    std::string out;
    size_t pos = 0;
    size_t search = 0;
    while (true) {
        const size_t hit = text.find(prefix, search);
        if (hit == std::string_view::npos) {
            break;
        }
        const size_t after = hit + prefix.size();
        size_t match_end = std::string_view::npos;
        // (.){1,2} is greedy: try two characters, then one.
        size_t ends[2]{};
        size_t n = 0;
        size_t i = after;
        for (int k = 0; k < 2 && i < text.size() && text[i] != '\n' && text[i] != '\r'; ++k) {
            i += utf8_len(static_cast<unsigned char>(text[i]));
            ends[n++] = std::min(i, text.size());
        }
        for (size_t k = n; k > 0; --k) {
            if (text.compare(ends[k - 1], tail.size(), tail) == 0) {
                match_end = ends[k - 1] + tail.size();
                break;
            }
        }
        if (match_end == std::string_view::npos) {
            search = hit + 1;
            continue;
        }
        out.append(text.substr(pos, hit - pos));
        out.append(value);
        pos = match_end;
        search = match_end;
    }
    out.append(text.substr(pos));
    return out;
}

// refreshPlayerNames(): new map, timer reset, world names re-registered (on the next tick: needs JNI).
void refresh_player_names_locked() {
    if (g_state.map_active) {
        reset_map_locked(true);
    }
    g_state.refresh_timer_ms = adjust::now_ms();
    g_state.world_refresh_requested = true;
}

// ---- JNI: session + tab list ------------------------------------------------------------------------------------

std::string jstring_to_utf8(JNIEnv* env, jstring s) {
    std::string out;
    if (s == nullptr) {
        return out;
    }
    const char* chars = env->GetStringUTFChars(s, nullptr);
    if (chars != nullptr) {
        out = chars;
        env->ReleaseStringUTFChars(s, chars);
    }
    return out;
}

std::string session_username(events::JniContext* jni, jobject mc) {
    JNIEnv* env = jni->env;
    static const jmethodID mid_get_session = adjust::jni::method(
        jni, jni->mappings->cls_minecraft, {"L", "getSession", "func_110432_I"}, "()Lavm;",
        "()Lnet/minecraft/util/Session;");
    static const jclass cls_session = adjust::jni::find_class(jni, "avm", "net/minecraft/util/Session");
    static const jmethodID mid_username = adjust::jni::method(jni, cls_session, {"c", "getUsername", "func_111285_a"},
                                                              "()Ljava/lang/String;", "()Ljava/lang/String;");
    if (mid_get_session == nullptr || mid_username == nullptr) {
        return {};
    }
    std::string name;
    jobject session = env->CallObjectMethod(mc, mid_get_session);
    if (!adjust::jni::clear_exception(jni) && session != nullptr) {
        auto jname = static_cast<jstring>(env->CallObjectMethod(session, mid_username));
        if (!adjust::jni::clear_exception(jni) && jname != nullptr) {
            name = jstring_to_utf8(env, jname);
            env->DeleteLocalRef(jname);
        }
        env->DeleteLocalRef(session);
    }
    return name;
}

// Profile names of mc.thePlayer.sendQueue.getPlayerInfoMap().
std::vector<std::string> tab_list_names(events::JniContext* jni, jobject mc) {
    std::vector<std::string> names;
    JNIEnv* env = jni->env;
    static const jmethodID mid_info_map = adjust::jni::method(
        jni, jni->mappings->cls_net_handler_play_client, {"d", "getPlayerInfoMap", "func_175106_d"},
        "()Ljava/util/Collection;", "()Ljava/util/Collection;");
    static const jmethodID mid_profile = adjust::jni::method(
        jni, jni->mappings->cls_network_player_info, {"a", "getGameProfile", "func_178845_a"},
        "()Lcom/mojang/authlib/GameProfile;", "()Lcom/mojang/authlib/GameProfile;");
    static const jclass cls_profile =
        adjust::jni::find_class(jni, "com/mojang/authlib/GameProfile", "com/mojang/authlib/GameProfile");
    static const jmethodID mid_profile_name =
        cls_profile != nullptr ? env->GetMethodID(cls_profile, "getName", "()Ljava/lang/String;") : nullptr;
    static const jmethodID mid_to_array = [&]() noexcept -> jmethodID {
        jclass collection = env->FindClass("java/util/Collection");
        if (collection == nullptr) {
            adjust::jni::clear_exception(jni);
            return nullptr;
        }
        jmethodID id = env->GetMethodID(collection, "toArray", "()[Ljava/lang/Object;");
        adjust::jni::clear_exception(jni);
        env->DeleteLocalRef(collection);
        return id;
    }();
    adjust::jni::clear_exception(jni);
    if (mid_info_map == nullptr || mid_profile == nullptr || mid_profile_name == nullptr || mid_to_array == nullptr) {
        return names;
    }
    if (env->PushLocalFrame(16) != 0) {
        adjust::jni::clear_exception(jni);
        return names;
    }
    jobject handler = minecraft::mc_get_net_handler(jni, mc);
    jobject collection = handler != nullptr ? env->CallObjectMethod(handler, mid_info_map) : nullptr;
    auto entries = (!adjust::jni::clear_exception(jni) && collection != nullptr)
                       ? static_cast<jobjectArray>(env->CallObjectMethod(collection, mid_to_array))
                       : nullptr;
    if (!adjust::jni::clear_exception(jni) && entries != nullptr) {
        const jsize count = env->GetArrayLength(entries);
        names.reserve(static_cast<size_t>(count));
        for (jsize i = 0; i < count; ++i) {
            if (env->PushLocalFrame(8) != 0) {
                adjust::jni::clear_exception(jni);
                break;
            }
            jobject info = env->GetObjectArrayElement(entries, i);
            jobject profile = info != nullptr ? env->CallObjectMethod(info, mid_profile) : nullptr;
            if (!adjust::jni::clear_exception(jni) && profile != nullptr) {
                auto jname = static_cast<jstring>(env->CallObjectMethod(profile, mid_profile_name));
                if (!adjust::jni::clear_exception(jni) && jname != nullptr) {
                    names.push_back(jstring_to_utf8(env, jname));
                }
            }
            env->PopLocalFrame(nullptr);
        }
    }
    env->PopLocalFrame(nullptr);
    return names;
}

// S38PacketPlayerListItem ADD_PLAYER -> replaceNames(profile name); the list is kept for restore_typed_names.
void register_tab_names(std::vector<std::string> names) {
    for (const std::string& name : names) {
        adjust_streamer::replace_names(name);
    }
    std::lock_guard<std::recursive_mutex> guard(g_state.lock);
    g_state.tab_names = std::move(names);
}

// refreshWorldNames
void refresh_world_names(events::GameContext* ctx) {
    const std::string session = session_username(ctx->jni, ctx->minecraft);
    {
        std::lock_guard<std::recursive_mutex> guard(g_state.lock);
        g_state.session_hash = hash_util(session);
        g_state.world_refresh_requested = false;
    }
    if (!session.empty()) {
        adjust_streamer::replace_names(session);
    }
    if (ctx->local_player == nullptr) {
        return;
    }
    register_tab_names(tab_list_names(ctx->jni, ctx->minecraft));
}

// GuiNewChat.refreshChat() of mc.ingameGUI.getChatGUI() (Adjust: minecraftChannel.history.refreshChat()).
void refresh_chat(events::GameContext* ctx) {
    if (ctx == nullptr || ctx->jni == nullptr || ctx->jni->env == nullptr || ctx->minecraft == nullptr ||
        !adjust::jni::is_1_8(ctx->jni)) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    static const jfieldID fid_ingame_gui = adjust::jni::field(
        jni, jni->mappings->cls_minecraft, {"q", "ingameGUI", "field_71456_v"}, "Lavo;",
        "Lnet/minecraft/client/gui/GuiIngame;");
    static const jclass cls_ingame = adjust::jni::find_class(jni, "avo", "net/minecraft/client/gui/GuiIngame");
    static const jmethodID mid_chat_gui = adjust::jni::method(
        jni, cls_ingame, {"d", "getChatGUI", "func_146158_b"}, "()Lavt;", "()Lnet/minecraft/client/gui/GuiNewChat;");
    static const jclass cls_chat = adjust::jni::find_class(jni, "avt", "net/minecraft/client/gui/GuiNewChat");
    static const jmethodID mid_refresh =
        adjust::jni::method(jni, cls_chat, {"b", "refreshChat", "func_146245_b"}, "()V", "()V");
    if (fid_ingame_gui == nullptr || mid_chat_gui == nullptr || mid_refresh == nullptr) {
        return;
    }
    jobject ingame = env->GetObjectField(ctx->minecraft, fid_ingame_gui);
    if (adjust::jni::clear_exception(jni) || ingame == nullptr) {
        return;
    }
    jobject chat = env->CallObjectMethod(ingame, mid_chat_gui);
    if (!adjust::jni::clear_exception(jni) && chat != nullptr) {
        env->CallVoidMethod(chat, mid_refresh);
        adjust::jni::clear_exception(jni);
        env->DeleteLocalRef(chat);
    }
    env->DeleteLocalRef(ingame);
}

// ---- events -----------------------------------------------------------------------------------------------------

void on_game_tick(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->minecraft == nullptr || !adjust::jni::is_1_8(ctx->jni)) {
        return;
    }
    JNIEnv* env = ctx->jni->env;

    {
        // Running without an enable notification (e.g. restored from a profile): onEnable.
        std::lock_guard<std::recursive_mutex> guard(g_state.lock);
        if (!g_state.map_active) {
            reset_map_locked(true);
            g_state.server_names.reset();
            g_state.refresh_timer_ms = adjust::now_ms();
            g_state.world_refresh_requested = true;
        }
    }

    // onWorldJoinPacket (S01PacketJoinGame): new map, then onPacketReceive -> refreshWorldNames.
    if (ctx->world != nullptr &&
        (g_state.last_world == nullptr || !env->IsSameObject(g_state.last_world, ctx->world))) {
        if (g_state.last_world != nullptr) {
            env->DeleteWeakGlobalRef(g_state.last_world);
        }
        g_state.last_world = env->NewWeakGlobalRef(ctx->world);
        std::lock_guard<std::recursive_mutex> guard(g_state.lock);
        g_state.server_names.reset();
        if (g_state.map_active) {
            reset_map_locked(true);
        }
        g_state.refresh_timer_ms = adjust::now_ms();
        g_state.world_refresh_requested = true;
    }

    bool full_refresh = false;
    {
        std::lock_guard<std::recursive_mutex> guard(g_state.lock);
        full_refresh = g_state.world_refresh_requested;
    }
    if (full_refresh) {
        refresh_world_names(ctx);
        g_state.tick_counter = 0;
        return;
    }
    // S38PacketPlayerListItem ADD_PLAYER -> replaceNames(profile name)
    if (ctx->local_player != nullptr && ++g_state.tick_counter >= kTabPollTicks) {
        g_state.tick_counter = 0;
        register_tab_names(tab_list_names(ctx->jni, ctx->minecraft));
    }
}

// onEnable / onDisable
void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    if (ev->module != &g_state.base) {
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> guard(g_state.lock);
        reset_map_locked(ev->enabled);
        g_state.server_names.reset();
        g_state.refresh_timer_ms = adjust::now_ms();
        g_state.world_refresh_requested = ev->enabled;
        g_state.tab_names.clear();
        g_state.tick_counter = 0;
    }
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->jni->env == nullptr || ctx->minecraft == nullptr ||
        !adjust::jni::is_1_8(ctx->jni)) {
        return;
    }
    if (ev->enabled) {
        refresh_world_names(ctx); // onEnable: refreshWorldNames() before refreshChat()
        g_state.tick_counter = 0;
    }
    refresh_chat(ctx); // the chat hook masks (enabled) or shows the stored originals (disabled)
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_state.name_type.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.name_type.visit(archive, "name_type");
}

} // namespace

events::Module* get_adjust_streamer_module() noexcept {
    return &g_state.base;
}

namespace adjust_streamer {

bool active() noexcept {
    std::lock_guard<std::recursive_mutex> guard(g_state.lock);
    return g_state.map_active && module_running();
}

// StreamerModule.replaceNames
std::string replace_names(std::string_view name) {
    std::lock_guard<std::recursive_mutex> guard(g_state.lock);
    if (!g_state.map_active || !module_running()) {
        return std::string(name);
    }
    const std::string key(name);
    if (auto it = g_state.names.find(key); it != g_state.names.end()) {
        return it->second;
    }
    if (g_state.names.size() >= kMapRefreshSize && g_state.refresh_timer_ms + kMapRefreshMs <= adjust::now_ms()) {
        refresh_player_names_locked();
    }
    std::string replacement = key;
    if (is_word_name(key)) {
        replacement = generate_name(key);
    }
    std::string unique;
    for (int suffix = 0;; ++suffix) {
        unique = replacement + (suffix == 0 ? std::string() : std::to_string(suffix));
        if (g_state.masks.find(unique) == g_state.masks.end()) {
            break;
        }
    }
    force_put_locked(key, unique);
    return unique;
}

std::string replace_name(std::string_view text, std::string_view name) {
    if (!active()) {
        return std::string(text);
    }
    return replace_all(text, name, replace_names(name));
}

std::string replace_known_names(std::string_view text) {
    std::lock_guard<std::recursive_mutex> guard(g_state.lock);
    std::string result(text);
    if (!g_state.map_active || !module_running()) {
        return result;
    }
    for (const std::string& key : g_state.order) {
        result = replace_all(result, key, g_state.names[key]);
    }
    return result;
}

std::string lookup_masked_name(std::string_view name) {
    std::lock_guard<std::recursive_mutex> guard(g_state.lock);
    if (!g_state.map_active || !module_running()) {
        return std::string(name);
    }
    auto it = g_state.names.find(std::string(name));
    return it != g_state.names.end() ? it->second : std::string(name);
}

std::string mask_name_if_enabled(std::string_view name) {
    return module_running() ? replace_names(name) : std::string(name);
}

std::string restore_name_in_text(std::string_view text, std::string_view masked) {
    std::lock_guard<std::recursive_mutex> guard(g_state.lock);
    if (!g_state.map_active || !module_running()) {
        return std::string(text);
    }
    auto it = g_state.masks.find(std::string(masked));
    const std::string original = it != g_state.masks.end() ? it->second : std::string(masked);
    return replace_all(text, masked, original);
}

// StreamerModule.maskNamesInText
bool mask_names_in_text(std::string_view text, std::string* out) {
    std::lock_guard<std::recursive_mutex> guard(g_state.lock);
    if (!g_state.map_active || !module_running() || out == nullptr) {
        return false;
    }
    const std::string marker = std::string(kSection) + "7..." + kSection + "7";
    const bool truncated = text.find(marker) != std::string_view::npos;
    std::optional<std::string> result;
    for (const std::string& name : g_state.order) {
        const std::string current = result.has_value() ? *result : std::string(text);
        const std::string& value = g_state.names[name];
        if (truncated && name.size() >= 12) {
            const std::string prefix = name.substr(0, 11);
            if (current.find(prefix) != std::string::npos) {
                result = replace_truncated(current, prefix, value);
                continue;
            }
        }
        if (current.find(name) != std::string::npos) {
            result = replace_all(current, name, value);
        }
    }
    if (!result.has_value()) {
        return false;
    }
    *out = std::move(*result);
    return true;
}

// StreamerModule.getScoreboardName
bool mask_scoreboard_entry(std::string_view formatted, std::string* out) {
    if (out == nullptr || !active()) {
        return false;
    }
    bool replaced = false;
    std::string masked;
    if (code_points(formatted) >= 4 && mask_names_in_text(formatted, &masked)) {
        *out = masked;
        replaced = true;
    }
    const std::string gray = std::string(kSection) + "7";
    if (!starts_with(formatted, gray) || formatted.find('/') == std::string_view::npos ||
        formatted.find(' ') == std::string_view::npos) {
        return replaced;
    }
    const std::vector<std::string_view> parts = split_space(formatted);
    if (parts.size() != 3) {
        return replaced;
    }
    const std::string_view last = parts[2];
    const std::string name = keep_alnum(strip_control_codes(last));
    std::lock_guard<std::recursive_mutex> guard(g_state.lock);
    if (!g_state.server_names.has_value() || !g_state.server_names->matches(name)) {
        std::optional<StreamerName> created = make_streamer_name(name);
        if (!created.has_value()) {
            return replaced; // Java throws inside the handler: the earlier replacement stays
        }
        g_state.server_names = std::move(created);
    }
    const std::optional<size_t> two = prefix_bytes(last, 2);
    if (!two.has_value()) {
        return replaced;
    }
    const size_t last_space = formatted.rfind(' ');
    *out = std::string(formatted.substr(0, last_space)) + " " + std::string(last.substr(0, *two)) +
           g_state.server_names->display(true);
    return true;
}

// StreamerModule.maskServerIds
bool mask_server_ids(std::string_view green_text, std::string* out) {
    if (out == nullptr || !active() || green_text.find(' ') == std::string_view::npos) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> guard(g_state.lock);
    bool changed = false;
    for (std::string_view word : split_space(green_text)) {
        const std::string name = keep_alnum(word);
        if (!is_server_id(name)) {
            continue;
        }
        const std::optional<StreamerName> replacement = make_streamer_name(name);
        if (!replacement.has_value()) {
            continue;
        }
        // Each match rewrites the original text (Adjust: text.text = original.replaceAll(name, ...)).
        *out = replace_all(green_text, name, replacement->display(false));
        changed = true;
    }
    return changed;
}

// StreamerModule.extractProfileClickName
bool profile_click_name(std::string_view run_command, std::string* out) {
    constexpr std::string_view kCommand = "/viewprofile ";
    if (out == nullptr || run_command.size() < kCommand.size() ||
        !equals_ignore_case(run_command.substr(0, kCommand.size()), kCommand)) {
        return false;
    }
    std::string_view name = run_command.substr(kCommand.size());
    while (!name.empty() && static_cast<unsigned char>(name.front()) <= ' ') {
        name.remove_prefix(1);
    }
    while (!name.empty() && static_cast<unsigned char>(name.back()) <= ' ') {
        name.remove_suffix(1);
    }
    if (!is_word_name(name)) {
        return false;
    }
    *out = std::string(name);
    return true;
}

// StreamerModule.getChatSenderName (formatted-text part, CHAT_SENDER = ^(?:\[[^]]+]\s+)*([A-Za-z0-9_]{1,16})\s*:\s)
bool chat_sender_name(std::string_view formatted, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    const size_t colon = formatted.find(':');
    const std::string_view section = kSection;
    if (colon == std::string_view::npos || colon < section.size() + 1) {
        return false;
    }
    const std::string_view before = formatted.substr(colon - section.size() - 1, section.size() + 1);
    if (before.substr(0, section.size()) != section) {
        return false;
    }
    const char code = static_cast<char>(std::tolower(static_cast<unsigned char>(before.back())));
    if (code != 'f' && code != '7') {
        return false;
    }

    const std::string text = strip_control_codes(formatted);
    size_t i = 0;
    while (i < text.size() && text[i] == '[') { // (?:\[[^]]+]\s+)*
        size_t j = i + 1;
        while (j < text.size() && text[j] != ']') {
            ++j;
        }
        if (j == i + 1 || j >= text.size()) {
            break;
        }
        size_t k = j + 1;
        const size_t spaces_start = k;
        while (k < text.size() && is_java_space(text[k])) {
            ++k;
        }
        if (k == spaces_start) {
            break;
        }
        i = k;
    }
    const size_t name_start = i;
    while (i < text.size() && is_word_char(text[i])) {
        ++i;
    }
    const size_t name_len = i - name_start;
    if (name_len < 1 || name_len > 16) {
        return false;
    }
    while (i < text.size() && is_java_space(text[i])) {
        ++i;
    }
    if (i >= text.size() || text[i] != ':' || i + 1 >= text.size() || !is_java_space(text[i + 1])) {
        return false;
    }
    const std::string sender = text.substr(name_start, name_len);
    if (!is_word_name(sender)) {
        return false;
    }
    *out = sender;
    return true;
}

// onChatReceived, ChatComponentTranslation "chat.type.text": the first format argument (String or the component's
// unformatted text), control codes stripped, registered when it matches ^\w{2,16}$.
void register_chat_sender_argument(std::string_view raw) {
    if (!active()) {
        return;
    }
    const std::string sender = strip_control_codes(raw);
    if (is_word_name(sender)) {
        replace_names(sender);
    }
}

// GuiChat sending: s = restoreNameInText(s, name) for every PlayerNameCompleter.complete(null, true) name, i.e.
// stripControlCodes(replaceNames(profile name)) of every tab-list player.
std::string restore_typed_names(std::string_view text) {
    std::string result(text);
    if (!active()) {
        return result;
    }
    std::vector<std::string> tab;
    {
        std::lock_guard<std::recursive_mutex> guard(g_state.lock);
        tab = g_state.tab_names;
    }
    for (const std::string& name : tab) {
        const std::string masked = strip_control_codes(replace_names(name));
        if (!masked.empty()) {
            result = restore_name_in_text(result, masked);
        }
    }
    return result;
}

uint64_t names_generation() noexcept {
    std::lock_guard<std::recursive_mutex> guard(g_state.lock);
    return g_state.generation;
}

} // namespace adjust_streamer
} // namespace slinky::modules
