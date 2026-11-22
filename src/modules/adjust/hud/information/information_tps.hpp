#pragma once

// Server TPS estimate of the Adjust "Information" HUD element (port of the S03PacketTimeUpdate / S01PacketJoinGame
// part of com.adjustclient.util.anticheat.ServerTracker).
//
// Adjust measures the TPS on the low-level packet receive event. The Adjust HUD module does not forward
// kEventIncomingPacket to its elements yet, so the module has to call this from its own kEventIncomingPacket
// handler (Netty thread; the function only does an IsInstanceOf check against cached classes and atomics).
// Until it is wired, the "TPS" line shows the default 20.0.

#include "events/events.hpp"

namespace slinky::modules::adjust_hud {

void information_on_incoming_packet(events::NettyPacketEvent* ev) noexcept;

// Current estimate (ServerTracker.tps), 20.0 by default and after joining a server.
float information_server_tps() noexcept;

} // namespace slinky::modules::adjust_hud
