#pragma once

// Adjust "Tweaks" hooks (src/adjust/hooks/tweaks_hooks.cpp), Minecraft 1.8.9.
//
// tweaks_hooks() lists the extra bytecode hooks. ChatTweak's message stacking and player-head registration have to
// run around GuiNewChat.setChatLine, but both setChatLine call sites (printChatMessageWithOptionalDeletion and
// refreshChat) are already replaced by the Streamer hooks (adjustStreamerSetChatLine). The Streamer native
// therefore calls tweaks_set_chat_line() wherever it currently calls the original setChatLine:
//
//   streamer_hooks.cpp, native_set_chat_line:
//       const auto set_chat_line = [&](jobject shown, jboolean only_display) noexcept {
//           tweaks_set_chat_line(env, gui, shown, component, id, counter, only_display);
//       };
//
// tweaks_set_chat_line performs ChatTweak.decorateMessage (stacking, " (n)" suffix on `shown` and `component`),
// ChatSenderPatterns.findSender on `component` (Adjust uses the original component), calls
// GuiNewChat.setChatLine(shown, id, counter, displayOnly) non-virtually, then records the new lines. With every
// tweak off it is exactly that one setChatLine call. Exceptions from setChatLine stay pending.

#include <jni.h>

#include "adjust/hooks/hooks.hpp"

namespace slinky::adjust::hooks {

// `shown`: the component to display (Streamer: masked copy, else the printed component).
// `component`: the component Minecraft printed (nullptr = same as `shown`).
void tweaks_set_chat_line(JNIEnv* env, jobject gui_new_chat, jobject shown, jobject component, jint chat_line_id,
                          jint update_counter, jboolean display_only) noexcept;

const bytecode::ExtraHookSpec* const* tweaks_hooks() noexcept;

} // namespace slinky::adjust::hooks
