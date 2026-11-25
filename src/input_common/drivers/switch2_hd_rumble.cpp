// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#include "common/logging.h"
#include "input_common/drivers/switch2_hd_rumble.h"

namespace InputCommon {
namespace {

constexpr u16 NintendoVendorId = 0x057E;

// A change within MIN_GAP of the last packet is not dropped: Tick sends it once the gap has
// passed. The game's vibration reaches us only when it changes, so Tick also refreshes a steady
// one before SDL's 60 ms hold runs out, for up to STALE after the game's last change.
constexpr s64 MIN_GAP_US = 5'000;
constexpr s64 REFRESH_US = 30'000;
constexpr s64 STALE_US = 1'000'000;
constexpr f32 AMP_FULL_SCALE = 1023.0f;

// Frequency codes are 10 bits and logarithmic: Hz = 10 x 2^(code / 96), measured with a Switch 2
// Pro Controller's own accelerometer. The whole Switch 1 range (40-1252 Hz) fits.
u16 FrequencyCode(f32 hz) {
    const f32 code = 96.0f * std::log2(std::clamp(hz, 40.0f, 1252.0f) / 10.0f);
    return static_cast<u16>(std::clamp(std::lround(code), 0L, 1023L));
}

// "Hz = dB" lines next to the executable: the gain the pad needs at each frequency to feel as
// strong as a DualSense. No file: no change.
std::vector<std::pair<f32, f32>> LoadCalibrationFile(const char* name) {
    std::vector<std::pair<f32, f32>> points;
    const char* base = SDL_GetBasePath();
    if (!base) {
        return points;
    }
    std::FILE* file = std::fopen((std::string(base) + name).c_str(), "r");
    if (!file) {
        return points;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        if (char* hash = std::strchr(line, '#')) {
            *hash = '\0';
        }
        float hz = 0.0f, db = 0.0f;
        if (std::sscanf(line, " %f = %f", &hz, &db) == 2 && hz > 0.0f) {
            points.emplace_back(hz, db);
        }
    }
    std::fclose(file);
    std::sort(points.begin(), points.end());
    return points;
}

f32 CalibrationGain(const std::vector<std::pair<f32, f32>>& points, f32 hz) {
    if (points.empty() || hz <= 0.0f) {
        return 1.0f;
    }
    f32 db = points.front().second;
    if (hz >= points.back().first) {
        db = points.back().second;
    } else {
        for (size_t i = 1; i < points.size(); ++i) {
            const auto& [f0, d0] = points[i - 1];
            const auto& [f1, d1] = points[i];
            if (hz >= f0 && hz <= f1) {
                db = d0 + (d1 - d0) * std::log(hz / f0) / std::log(f1 / f0);
                break;
            }
        }
    }
    return std::pow(10.0f, db / 20.0f);
}

// The game's two bands of one motor, shaped like RenHDrum shapes them for the DualSense (crisp
// notes, amp^0.5, sum <= 1, x0.8), so the calibration against a DualSense holds in games.
std::array<f32, 2> DualSenseLevels(f32 low, f32 high, s64& onset_us, s64 now_us) {
    static constexpr f32 SILENT = 1.0f / 65535.0f;
    if (low < SILENT && high < SILENT) {
        onset_us = -1;
        return {0.0f, 0.0f};
    }
    if (onset_us < 0) {
        onset_us = now_us;
    }
    const s64 since = now_us - onset_us;
    const f32 gain = since < 10'667 ? 1.41f : since < 42'667 ? 0.71f : 1.0f;
    f32 l = (std::min)(1.0f, low * gain);
    f32 h = (std::min)(1.0f, high * gain);
    if (gain > 1.0f) {
        const f32 limit = (std::max)(1.0f, low + high);
        if (l + h > limit) {
            const f32 scale = limit / (l + h);
            l *= scale;
            h *= scale;
        }
    }
    l = std::sqrt(l);
    h = std::sqrt(h);
    if (l + h > 1.0f) {
        const f32 scale = 1.0f / (l + h);
        l *= scale;
        h *= scale;
    }
    return {0.8f * l, 0.8f * h};
}

} // namespace

std::shared_ptr<Switch2HdRumble> Switch2HdRumble::Create(SDL_Joystick* joystick) {
    if (!joystick || SDL_GetJoystickVendor(joystick) != NintendoVendorId) {
        return nullptr;
    }
    const Uint16 pid = SDL_GetJoystickProduct(joystick);
    if (pid != 0x2069 && pid != 0x2066 && pid != 0x2067) {
        return nullptr;
    }
    // The BLE pad is an SDL virtual joystick, and SDL's Switch 2 driver plays HD rumble on USB only
    if (SDL_IsJoystickVirtual(SDL_GetJoystickID(joystick)) ||
        SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS) {
        return nullptr;
    }
    const char* name = pid == 0x2069 ? "Switch 2 Pro Controller"
                       : pid == 0x2067 ? "Joy-Con 2 (L)"
                                       : "Joy-Con 2 (R)";
    return std::make_shared<Switch2HdRumble>(joystick, name);
}

Switch2HdRumble::Switch2HdRumble(SDL_Joystick* joystick_, const char* name) : joystick{joystick_} {
    const char* mode = SDL_getenv("LICHII_SWITCH2_RUMBLE");
    raw = mode && std::strcmp(mode, "raw") == 0;
    if (raw) {
        LOG_INFO(Input, "Switch 2 HD rumble for {}: raw (the game's amplitudes, no shaping)", name);
        return;
    }
    calibration = LoadCalibrationFile("calibration_switch2.txt");
    if (calibration.empty()) {
        calibration = LoadCalibrationFile("calibracion_switch2.txt");
    }
    LOG_INFO(Input, "Switch 2 HD rumble for {} ({} calibration points)", name, calibration.size());
}

bool Switch2HdRumble::IsActive() const {
    std::scoped_lock lock{mutex};
    return active;
}

bool Switch2HdRumble::Vibrate(s64 now_us, bool is_right,
                              const Common::Input::VibrationStatus& v) {
    {
        std::scoped_lock lock{mutex};
        if (!active) {
            return false;
        }
        game[is_right ? 1 : 0] = {std::clamp(v.low_amplitude, 0.0f, 1.0f), v.low_frequency,
                                  std::clamp(v.high_amplitude, 0.0f, 1.0f), v.high_frequency};
        last_game_us = now_us;
    }
    return SendIfDue(now_us);
}

void Switch2HdRumble::Tick(s64 now_us) {
    SendIfDue(now_us);
}

bool Switch2HdRumble::SendIfDue(s64 now_us) {
    std::array<Motor, 2> motors{};
    SDL_Joystick* target = nullptr;
    {
        std::scoped_lock lock{mutex};
        if (!active || !joystick) {
            return false;
        }
        target = joystick;
        const auto code = [](f32 amp) {
            return static_cast<u16>(std::lround(std::clamp(amp, 0.0f, 1.0f) * AMP_FULL_SCALE));
        };
        const bool fresh = now_us - last_game_us < STALE_US;
        for (size_t i = 0; i < 2; ++i) {
            const auto& g = game[i];
            const auto level = !fresh ? std::array<f32, 2>{}
                               : raw  ? std::array<f32, 2>{g.lo_amp, g.hi_amp}
                                      : DualSenseLevels(g.lo_amp, g.hi_amp, onset_us[i], now_us);
            motors[i].lo_freq = FrequencyCode(g.lo_hz);
            motors[i].hi_freq = FrequencyCode(g.hi_hz);
            motors[i].lo_amp = code(level[0] * CalibrationGain(calibration, g.lo_hz));
            motors[i].hi_amp = code(level[1] * CalibrationGain(calibration, g.hi_hz));
        }
        const bool changed = motors != sent;
        const bool playing =
            motors[0].lo_amp || motors[0].hi_amp || motors[1].lo_amp || motors[1].hi_amp;
        const s64 since = now_us - last_send_us;
        const bool due = changed ? since >= MIN_GAP_US : playing && since >= REFRESH_US;
        if (!due) {
            return true;
        }
        sent = motors;
        last_send_us = now_us;
    }
    if (Send(target, motors)) {
        return true;
    }
    LOG_WARNING(Input, "Switch 2 HD rumble not available here ({}), using plain rumble",
                SDL_GetError());
    std::scoped_lock lock{mutex};
    active = false;
    return false;
}

void Switch2HdRumble::Shutdown() {
    std::scoped_lock lock{mutex};
    if (active && joystick) {
        Send(joystick, {});
    }
    active = false;
    joystick = nullptr;
}

// SDL patch 0007 effect: "HDR2", flags, then per motor low freq, low amp, high freq, high amp
bool Switch2HdRumble::Send(SDL_Joystick* joystick, const std::array<Motor, 2>& motors) {
    if (!joystick) {
        return false;
    }
    std::array<u8, 21> effect{'H', 'D', 'R', '2', 0};
    for (size_t i = 0; i < motors.size(); ++i) {
        const auto& m = motors[i];
        u8* out = &effect[5 + i * 8];
        for (const u16 value : {m.lo_freq, m.lo_amp, m.hi_freq, m.hi_amp}) {
            *out++ = static_cast<u8>(value & 0xFF);
            *out++ = static_cast<u8>(value >> 8);
        }
    }
    return SDL_SendJoystickEffect(joystick, effect.data(), static_cast<int>(effect.size()));
}

} // namespace InputCommon
