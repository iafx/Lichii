// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/input.h"

struct SDL_Joystick;

namespace InputCommon {

/// Native HD rumble for the Switch 2 Pro Controller and the Joy-Con 2 over USB (HIDAPI).
///
/// RenHDrum drives only Switch 1 pads (057E:2009/2006/2007); without this a Switch 2 pad gets SDL's
/// plain rumble, which plays every effect at two fixed frequencies. This plays, per motor, the
/// game's two bands with their own frequency and amplitude through SDL patch
/// 0007-switch2-hd-rumble-effect ("HDR2" effect). Where SDL refuses the effect (no patch, a
/// Bluetooth pad, the BLE virtual pad) the first refusal turns it off and plain rumble carries on.
class Switch2HdRumble {
public:
    /// nullptr when the joystick is not a wired Switch 2 Pro Controller / Joy-Con 2.
    static std::shared_ptr<Switch2HdRumble> Create(SDL_Joystick* joystick);

    explicit Switch2HdRumble(SDL_Joystick* joystick, const char* name);

    /// HLE thread. True when the vibration was taken; false = use plain rumble.
    bool Vibrate(s64 now_us, bool is_right, const Common::Input::VibrationStatus& vibration);

    /// Every input poll: sends a change held back by the minimum gap, or a refresh.
    void Tick(s64 now_us);

    /// Silences the motors; the joystick is about to close.
    void Shutdown();

    bool IsActive() const;

private:
    struct Motor {
        u16 lo_freq{384}; // 160 Hz
        u16 lo_amp{0};
        u16 hi_freq{480}; // 320 Hz
        u16 hi_amp{0};
        bool operator==(const Motor&) const = default;
    };
    struct Band {
        f32 lo_amp{0}, lo_hz{160}, hi_amp{0}, hi_hz{320};
    };

    bool SendIfDue(s64 now_us);
    static bool Send(SDL_Joystick* joystick, const std::array<Motor, 2>& motors);

    mutable std::mutex mutex;
    SDL_Joystick* joystick;
    bool active{true};
    std::array<Band, 2> game{};
    std::array<s64, 2> onset_us{-1, -1};
    std::array<Motor, 2> sent{};
    s64 last_send_us{0};
    s64 last_game_us{0};
    std::vector<std::pair<f32, f32>> calibration;
    // LICHII_SWITCH2_RUMBLE=raw: the game's amplitudes as they are, no DualSense shaping or
    // calibration, to compare against a real Switch 2
    bool raw{false};
};

} // namespace InputCommon
