#pragma once

// Adjust "Main Menu" (com.adjustclient.ui.mainmenu.AdjustMainMenu + VoronoiBackground / VoronoiCell /
// MenuParticle, the quit confirmation of ui.dialog.DialogBuilder / DialogScreen and ui.dialog.AdjustButton).
//
// How it works in OpenSlinky++ (Adjust replaced GuiMainMenu by its own GuiScreen subclass; we cannot add Java
// classes, so vanilla's GuiMainMenu stays the real screen and is restyled in place):
//   * Drawing: the vanilla GuiMainMenu.drawScreen call is skipped (mainmenu_hooks.cpp) and the whole menu -
//     Voronoi background, title, splash, buttons, quit dialog - is drawn on the OpenSlinky++ NanoVG GUI layer
//     (kEventGuiFramePoll / kEventGuiRender2D, composited in hook_update_display, which also runs without a world).
//     Layout uses Minecraft's scaled resolution (GuiScreen.width/height and the drawScreen mouse position), so the
//     menu lines up with Minecraft's input coordinates exactly like Adjust's GuiScreen.
//   * Input: the GuiScreen.handleInput call in Minecraft.runTick is replaced; while the menu is shown the LWJGL
//     mouse queue is drained and dispatched to the Adjust buttons / dialog, keyboard events still go to vanilla's
//     handleKeyboardInput (F2/F11 keep working, ESC closes the quit dialog). The ClickGUI keeps receiving input
//     through the WndProc path (it consumes events before LWJGL sees them).
//   * Buttons open the real vanilla screens through JNI (Minecraft.displayGuiScreen(new GuiSelectWorld(menu)) ...).
//   * Frame limit: Adjust's getMaxFps (175 / 60 when unfocused instead of vanilla's 30 fps menu cap) via the
//     Minecraft.getLimitFramerate call in runGameLoop.
//
// Every screen that Adjust patched to open AdjustMainMenu (GuiGameOver, GuiIngameMenu, GuiMemoryErrorScreen,
// NetHandlerPlayClient disconnect, Minecraft startup / displayGuiScreen(null), Realms.inTitleScreen) opens
// vanilla's GuiMainMenu, which is restyled here - no extra hook is needed for them.
//
// Only active while the "Adjust" visual style is selected and the module is enabled (default: enabled). If one of
// the hooks is not installed (e.g. a client that renders screens through reflection), the vanilla menu is kept.

#include <jni.h>

#include "events/events.hpp"
#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_main_menu_module() noexcept;

namespace adjust_main_menu {

// EntityRenderer.updateCameraAndRender -> GuiScreen.drawScreen(mouseX, mouseY, partialTicks).
// Returns true if the Adjust menu took over this frame (the caller must NOT call the vanilla drawScreen).
bool on_draw_screen(events::JniContext* jni, jobject screen, int mouse_x, int mouse_y) noexcept;

// Minecraft.runTick -> GuiScreen.handleInput(). Returns true if the input was handled (the caller must NOT call
// the vanilla handleInput). A Java exception thrown by a vanilla call made here stays pending.
bool on_handle_input(events::JniContext* jni, jobject screen) noexcept;

// Minecraft.runGameLoop -> Minecraft.getLimitFramerate(). Returns true and sets *out_fps while the menu is shown.
bool on_limit_framerate(events::JniContext* jni, jobject minecraft, int* out_fps) noexcept;

} // namespace adjust_main_menu

} // namespace slinky::modules
