#pragma once

// Adjust "Streamer" (com.adjustclient.module.impl.render.streamer.{StreamerModule, StreamerName, StreamerNameType,
// FirstNameSource}): hides player names and game information.
//
// The module keeps Adjust's name map (real name -> generated name) up to date from the session and the tab list;
// the Minecraft hooks in src/adjust/hooks/streamer_hooks.cpp rewrite the text Minecraft draws (chat, scoreboard,
// tab list, name tags, signs) and restore typed names before a chat message is sent, through the StreamerModule
// entry points below. All strings are (modified) UTF-8 with Minecraft "§" codes. Every function is a no-op
// (returns its input / false) while the module is not running.

#include <cstdint>
#include <string>
#include <string_view>

#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_streamer_module() noexcept;

namespace adjust_streamer {

bool active() noexcept; // StreamerModule.nameMap != null

std::string replace_names(std::string_view name);                       // StreamerModule.replaceNames
std::string replace_name(std::string_view text, std::string_view name); // StreamerModule.replaceName
std::string replace_known_names(std::string_view text);                 // StreamerModule.replaceKnownNames
std::string lookup_masked_name(std::string_view name);                  // StreamerModule.lookupMaskedName
std::string mask_name_if_enabled(std::string_view name);                // StreamerModule.maskNameIfEnabled
std::string restore_name_in_text(std::string_view text, std::string_view masked); // StreamerModule.restoreNameInText
bool mask_names_in_text(std::string_view text, std::string* out);       // StreamerModule.maskNamesInText (false = null)

// StreamerModule.getScoreboardName (ScoreboardEntryEvent): `formatted` = ScorePlayerTeam.formatPlayerName(team,
// score.getPlayerName()). Returns true with the text of the replacement Score (same points / locked flag).
bool mask_scoreboard_entry(std::string_view formatted, std::string* out);

// StreamerModule.maskServerIds for a ChatComponentText whose style colour is GREEN. True if the text changed.
bool mask_server_ids(std::string_view green_text, std::string* out);

// StreamerModule.getChatSenderName, split: `run_command` = ClickEvent value of a RUN_COMMAND click event of the
// component or one of its siblings (empty if none), `formatted` = component.getFormattedText().
bool profile_click_name(std::string_view run_command, std::string* out); // extractProfileClickName
bool chat_sender_name(std::string_view formatted, std::string* out);     // formatted-text fallback
// onChatReceived, "chat.type.text": registers the stripped first format argument if it matches ^\w{2,16}$.
void register_chat_sender_argument(std::string_view raw);

// GuiChat send path: restoreNameInText(text, masked) for the masked name of every tab-list player
// (Adjust: PlayerNameCompleter.complete(null, true)).
std::string restore_typed_names(std::string_view text);

// Changes whenever the name map changes (cache key for hook-side text caches).
uint64_t names_generation() noexcept;

} // namespace adjust_streamer
} // namespace slinky::modules
