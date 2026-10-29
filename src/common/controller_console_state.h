// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/settings.h"

namespace Common {

/// The state shared between the console-mode engine (in the main window, ticking at
/// 250ms) and its settings UI (the Controllers tab in Controls): which pads are live, which
/// one is the handheld's own, the assigned players, the "identify" handshake and swap
/// requests. The UI reads and declares; the engine observes and obeys.
struct ControllerConsoleState {
    std::mutex mutex;
    /// Live pads as (display name, identity) in engine order
    std::vector<std::pair<std::string, std::string>> pads;
    /// Set by the Identify button: capture the next pad that has a button pressed
    bool identify_armed = false;
    std::chrono::steady_clock::time_point identify_armed_at{};

    /// One assigned player, as the Controllers tab lists it
    struct PlayerRow {
        int player; ///< 0-based player index
        std::string name;
        bool active;  ///< the pad has an input held (or was just tapped)
        bool leaving; ///< the controller left and nothing took its place: shown red, then gone
        int flash;    ///< 0 none, 1 green (a controller took the player), 2 red (one left)
        long long flash_since; ///< ms since epoch; the flash blinks for 1.5 seconds from here
        std::string profile;   ///< the input profile the player's controller carries ("" none)
        /// Its Bluetooth controller can be turned off: 0 no (not Bluetooth), 1 not yet (several
        /// of its model, until a button press tells them apart), 2 yes
        int turn_off = 0;
    };
    /// True while an automatic assignment mode runs (the engine owns the players)
    bool engine_active = false;
    /// The players the engine assigned, in player order
    std::vector<PlayerRow> players;
    /// The handheld's own pad: its name ("" when not identified) and whether it holds a player
    std::string handheld_name;
    bool handheld_has_player = false;
    /// The Controllers tab asked to remove the handheld's controller (its ✕)
    bool builtin_remove_request = false;
    /// The removed controller auto-detect leaves alone, by name ("" when not connected)
    std::string handheld_blocked_name;
    /// Swaps requested from the settings and the overlay, as (player a, player b); the engine
    /// applies them
    /// on its next tick, for this session only (the saved controller setup is untouched)
    std::vector<std::pair<int, int>> swap_requests;
    /// Players whose Bluetooth controller is to be turned off (disconnected): it sits out until
    /// it reconnects, and the player is free for the next controller to connect
    std::vector<int> remove_requests;
    /// Players whose controller the Controllers tab asked to rumble (a click on its row)
    std::vector<int> rumble_requests;
    /// An input profile picked for a player: the engine puts the profile's mappings on the set
    /// the player's controller carries. No name takes the profile off: `restore` puts back the
    /// mappings the set had before this session's first profile, else only the name goes.
    struct ProfileRequest {
        int player;
        std::string name;
        bool restore = false;
    };
    std::vector<ProfileRequest> profile_requests;
    /// Bypass Steam Input took effect at startup: SDL has every controller, and the engine
    /// leaves Steam's virtual pads alone (as launched outside Steam)
    std::atomic<bool> steam_bypassed{false};
    /// Bumped whenever the engine reloads the emulated controllers (a swap, a pad arriving or
    /// leaving): the Controls page refreshes its player tabs from them when it changes
    unsigned long long generation = 0;
};

[[nodiscard]] inline ControllerConsoleState& GetControllerConsoleState() {
    static ControllerConsoleState instance;
    return instance;
}

/// True when Eden was launched through Steam: Steam Input then wraps the controllers in
/// virtual ones
[[nodiscard]] inline bool RunningUnderSteam() {
    static const bool launched =
        std::getenv("SteamGameId") != nullptr || std::getenv("SteamAppId") != nullptr;
    return launched;
}

/// True when Steam Input's virtual pads are the players: under Steam, without Bypass Steam
/// Input. Otherwise (a direct launch, or bypassed) the real controllers are, and everything
/// works as launched outside Steam.
[[nodiscard]] inline bool SteamPadsInUse() {
    return RunningUnderSteam() && !GetControllerConsoleState().steam_bypassed;
}

/// Debug Probes: detailed controller traces in the log, for bug reports
[[nodiscard]] inline bool ControllerProbes() {
    return Settings::values.console_debug_probes.GetValue();
}

} // namespace Common
