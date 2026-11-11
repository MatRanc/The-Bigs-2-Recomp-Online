// thebigs2 - the game's main menu without XBOX LIVE (no online service) and
// with RECOMP SETTINGS (opens the settings menu) and EXIT GAME at the bottom.
// The game data stays as it is: the menu's screen file is changed in memory
// as the game loads it.
#pragma once

#include <atomic>
#include <functional>

namespace tb2 {

// Set once the main menu's screen file has been patched (the menu is loading); TB2_AUTOKEY's untilmenu waits on it.
inline std::atomic<bool> g_main_menu_loaded{false};

// quit: the clean quit (the same as F10).
void InitMainMenuEntries(std::function<void()> quit);

}  // namespace tb2
