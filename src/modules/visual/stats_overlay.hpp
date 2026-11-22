#pragma once
#include "events/module.hpp"

namespace slinky::modules {

// "Overlay" - Bed Wars stats overlay (both visual styles). When the Bed Wars start message appears it sends /who,
// reads the "ONLINE:" line, looks the players up via the Hypixel API (UUIDs from the tab list) and lists them by
// team, highest FKDR first, with stars, FKDR and WR. Drag it with the mouse while the chat is open.
events::Module* get_stats_overlay_module() noexcept;

} // namespace slinky::modules
