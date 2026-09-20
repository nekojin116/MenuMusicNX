#pragma once

#include <switch.h>

namespace pm {

constexpr u64 QlaunchTitleId{0x0100000000001000ULL};

/// Where the foreground currently is, as reported by pdm:qry.
enum class FocusOwner : u8 {
    Unknown,     ///< No decisive focus event was found.
    Home,        ///< qlaunch (HOME Menu) owns the foreground.
    Application, ///< The running application (game / homebrew in application mode).
    Applet,      ///< A system/library applet owns the foreground (Settings, Album, hbmenu, ...).
};

auto Initialize() -> Result;
void Exit();
void getCurrentPidTid(u64* pid_out, u64* tid_out);

/// True when qlaunch/HOME Menu is the active foreground UI.
/// While no application runs this is true unless a non-HOME applet took focus.
auto IsHomeMenuForeground() -> bool;

/// Focus owner decided by the last query that resolved (diagnostics).
auto GetFocusOwner() -> FocusOwner;

} // namespace pm
