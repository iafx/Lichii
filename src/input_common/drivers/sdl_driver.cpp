// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <SDL3/SDL_hidapi.h>
#include "common/logging.h"
#include "common/math_util.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/thread.h"
#include "common/vector_math.h"
#include "input_common/drivers/renhdrum_pads.h"
#include "input_common/drivers/sdl_driver.h"

// HD Rumble modes added by .patch/sdl3/0002-hdrumble-idle-write.patch (port of Ryuhdr's SDL2 patch; SDL3 is linked
// statically). Each returns 1 when the mode is active, 0 when SDL refused it, -1 for a pad that is not a HIDAPI Switch pad.
extern "C" {
int SDLCALL SDL_HDRumbleSetNoQueue(SDL_Joystick* joystick, int enable, int gap_ms, int pause_ms);
int SDLCALL SDL_HDRumbleSetNoQueueBluetooth(SDL_Joystick* joystick, int enable, int gap_ms);
int SDLCALL SDL_HDRumbleSetBluetoothAfterInput(SDL_Joystick* joystick, int enable);
}

namespace InputCommon {

namespace {
Common::UUID GetGUID(SDL_Joystick* joystick) {
    const SDL_GUID guid = SDL_GetJoystickGUID(joystick);
    std::array<u8, 16> data{};
    std::memcpy(data.data(), guid.data, sizeof(data));
    // Clear controller name crc
    std::memset(data.data() + 2, 0, sizeof(u16));
    return Common::UUID{data};
}

using GamepadBindings = std::vector<SDL_GamepadBinding>;

SDL_GamepadBinding EmptyBinding() {
    SDL_GamepadBinding binding{};
    binding.input_type = SDL_GAMEPAD_BINDTYPE_NONE;
    return binding;
}

GamepadBindings GetBindings(SDL_Gamepad* controller) {
    if (controller == nullptr) {
        return {};
    }

    int binding_count = 0;
    SDL_GamepadBinding** bindings = SDL_GetGamepadBindings(controller, &binding_count);
    if (bindings == nullptr) {
        return {};
    }

    GamepadBindings cached_bindings{};
    cached_bindings.reserve(static_cast<std::size_t>(binding_count));
    for (int i = 0; i < binding_count; ++i) {
        if (const auto* current = bindings[i]) {
            cached_bindings.emplace_back(*current);
        }
    }
    SDL_free(bindings);
    return cached_bindings;
}

template <typename Predicate>
SDL_GamepadBinding FindBinding(const GamepadBindings& bindings, Predicate matches) {
    const auto it = std::find_if(bindings.begin(), bindings.end(), matches);
    return it != bindings.end() ? *it : EmptyBinding();
}

// Pads shown in Brontide's HD Rumble dialog, keyed by SDLJoystick. Never held while calling SDL.
struct PadEntry {
    RenHDrumPadInfo info;
    const std::atomic<int64_t>* last_vibrate_us{nullptr};
};
std::mutex pads_mutex;
std::map<const void*, PadEntry> pads;

void SetPadInfo(const void* key, RenHDrumPadInfo info, const std::atomic<int64_t>* last_vibrate_us = nullptr) {
    std::scoped_lock lock{pads_mutex};
    pads[key] = {std::move(info), last_vibrate_us};
}

void ErasePadInfo(const void* key) {
    std::scoped_lock lock{pads_mutex};
    pads.erase(key);
}

// Bumped by the HD Rumble dialog; PumpEvents re-inits every pad when it differs from the applied one
std::atomic<uint32_t> settings_generation{0};
std::atomic<uint32_t> applied_settings_generation{0};

// The main HD Rumble switch (settings file, Brontide-App or flag files). Off = the emulator's own rumble
bool HdRumbleOn() {
    RenHdcConfig config{};
    config.enable_hd_rumble = true;
    RenHdcSettings settings{};
    return renhdrum_settings_resolve(&config, &settings) != RENHDRUM_OK || settings.enabled;
}

SDL_GamepadBinding GetBindingForButton(const GamepadBindings& bindings, SDL_GamepadButton button) {
    return FindBinding(bindings, [button](const SDL_GamepadBinding& current) {
        return current.output_type == SDL_GAMEPAD_BINDTYPE_BUTTON &&
               current.output.button == static_cast<SDL_GamepadButton>(button);
    });
}

SDL_GamepadBinding GetBindingForAxis(const GamepadBindings& bindings, SDL_GamepadAxis axis) {
    return FindBinding(bindings, [axis](const SDL_GamepadBinding& current) {
        return current.output_type == SDL_GAMEPAD_BINDTYPE_AXIS &&
               current.output.axis.axis == static_cast<SDL_GamepadAxis>(axis);
    });
}
} // Anonymous namespace

std::vector<RenHDrumPadInfo> GetRenHDrumPads() {
    using namespace std::chrono;
    const int64_t now_us = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    std::scoped_lock lock{pads_mutex};
    std::vector<RenHDrumPadInfo> result;
    for (const auto& [key, entry] : pads) {
        RenHDrumPadInfo info = entry.info;
        if (entry.last_vibrate_us) {
            const int64_t last_vibrate = entry.last_vibrate_us->load(std::memory_order_relaxed);
            info.is_vibrating = (last_vibrate > 0 && (now_us - last_vibrate) < 300'000);
        } else {
            info.is_vibrating = false;
        }
        result.push_back(std::move(info));
    }
    return result;
}

void ReapplyRenHDrumSettings() {
    LOG_INFO(Input, "HD Rumble settings saved: re-applying to connected controllers");
    settings_generation.fetch_add(1, std::memory_order_relaxed);
}

static bool SDLEventWatcher(void* user_data, SDL_Event* event) {
    auto* const sdl_state = static_cast<SDLDriver*>(user_data);

    sdl_state->HandleGameControllerEvent(*event);

    return true;
}

class SDLJoystick {
public:
    SDLJoystick(Common::UUID guid_, int port_, SDL_Joystick* joystick,
                                SDL_Gamepad* game_controller)
                : guid{guid_}, port{port_}, sdl_joystick{joystick, &SDL_CloseJoystick},
                    sdl_controller{game_controller, &SDL_CloseGamepad} {
        EnableMotion();
        if (sdl_joystick) {
            InitRenHDrum();
        }
    }

    ~SDLJoystick() {
        CleanupRenHDrum();
    }

    void EnableMotion() {
        if (!sdl_controller) {
            return;
        }
        SDL_Gamepad* controller = sdl_controller.get();
        last_motion_report = std::chrono::steady_clock::now();
        if (HasMotion()) {
            SDL_SetGamepadSensorEnabled(controller, SDL_SENSOR_ACCEL, false);
            SDL_SetGamepadSensorEnabled(controller, SDL_SENSOR_GYRO, false);
        }
        has_accel = SDL_GamepadHasSensor(controller, SDL_SENSOR_ACCEL);
        has_gyro = SDL_GamepadHasSensor(controller, SDL_SENSOR_GYRO);
        if (has_accel) {
            if (!SDL_SetGamepadSensorEnabled(controller, SDL_SENSOR_ACCEL, true)) {
                LOG_WARNING(Input, "Failed to enable accelerometer sensor: {}", SDL_GetError());
            }
        }
        if (has_gyro) {
            if (!SDL_SetGamepadSensorEnabled(controller, SDL_SENSOR_GYRO, true)) {
                LOG_WARNING(Input, "Failed to enable gyroscope sensor: {}", SDL_GetError());
            }
        }

        // EnableMotion is also called to recover a stalled sensor, so only log when the reported
        // capabilities actually change instead of on every restart.
        if (!motion_capabilities_logged || logged_accel != has_accel || logged_gyro != has_gyro) {
            LOG_INFO(Input, "Controller motion capabilities: accel={} gyro={}", has_accel,
                     has_gyro);
            motion_capabilities_logged = true;
            logged_accel = has_accel;
            logged_gyro = has_gyro;
        }
    }

    bool HasMotion() const {
        return has_gyro || has_accel;
    }

    /// A connected controller with sensors that sent no report for a while: some controllers
    /// stop reporting when another program (Steam Input) changes their sensor mode, or after
    /// they reconnect. Returns true when EnableMotion should restart them, backing off up to
    /// MaxMotionStallRetry while they stay silent.
    bool IsMotionStalled(std::chrono::steady_clock::time_point now) {
        std::scoped_lock lock{mutex};
        if (!sdl_controller || !HasMotion() || now - last_motion_report < motion_stall_retry) {
            return false;
        }
        motion_stall_retry = (std::min)(motion_stall_retry * 2, MaxMotionStallRetry);
        return true;
    }

    bool UpdateMotion(SDL_GamepadSensorEvent event) {
        constexpr float gravity_constant = 9.80665f;
        std::scoped_lock lock{mutex};
        last_motion_report = std::chrono::steady_clock::now();
        motion_stall_retry = InitialMotionStallRetry;
        const u64 sensor_timestamp = event.sensor_timestamp != 0 ? event.sensor_timestamp
                                                                 : event.timestamp;

        if (last_motion_update == 0) {
            last_motion_update = sensor_timestamp;
            return false;
        }
        if (sensor_timestamp < last_motion_update) {
            return false;
        }

        // SDL3 reports sensor timestamps in nanoseconds, while the input stack expects
        // delta timestamps in microseconds.
        const u64 time_difference = (sensor_timestamp - last_motion_update) / 1000;
        last_motion_update = sensor_timestamp;
        switch (event.sensor) {
        case SDL_SENSOR_ACCEL: {
            motion.accel_x = -event.data[0] / gravity_constant;
            motion.accel_y = event.data[2] / gravity_constant;
            motion.accel_z = -event.data[1] / gravity_constant;
            break;
        }
        case SDL_SENSOR_GYRO: {
            motion.gyro_x = event.data[0] / (f32(std::numbers::pi_v<float>) * 2);
            motion.gyro_y = -event.data[2] / (f32(std::numbers::pi_v<float>) * 2);
            motion.gyro_z = event.data[1] / (f32(std::numbers::pi_v<float>) * 2);
            break;
        }
        }

        // Ignore duplicated timestamps
        if (time_difference == 0) {
            return false;
        }

        // Motion data is invalid
        if (motion.accel_x == 0 && motion.gyro_x == 0 && motion.accel_y == 0 &&
            motion.gyro_y == 0 && motion.accel_z == 0 && motion.gyro_z == 0) {
            if (motion_error_count++ < motion_restart_threshold) {
                return false;
            }
            // Try restarting the sensor. Back off exponentially so a sensor that keeps reporting
            // zeros (observed on the Steam Deck) isn't toggled several times a
            // second for the whole session.
            motion_error_count = 0;
            motion_restart_threshold =
                std::min(motion_restart_threshold * 2, MaxMotionRestartThreshold);
            EnableMotion();
            return false;
        }

        motion_error_count = 0;
        motion_restart_threshold = InitialMotionRestartThreshold;
        motion.delta_timestamp = time_difference;
        return true;
    }

    const BasicMotion& GetMotion() const {
        return motion;
    }

    bool RumblePlay(const Common::Input::VibrationStatus vibration) {
        constexpr u32 rumble_max_duration_ms = 2000;
        constexpr f32 low_start_sensitivity_limit = 140.0f;
        constexpr f32 low_width_sensitivity_limit = 400.0f;
        constexpr f32 high_start_sensitivity_limit = 200.0f;
        constexpr f32 high_width_sensitivity_limit = 700.0f;
        // Try to provide some feeling of the frequency by reducing the amplitude depending on it.
        f32 low_frequency_scale = 1.0f;
        if (vibration.low_frequency > low_start_sensitivity_limit) {
            low_frequency_scale =
                (std::max)(1.0f - (vibration.low_frequency - low_start_sensitivity_limit) /
                                    low_width_sensitivity_limit,
                         0.3f);
        }
        f32 low_amplitude = vibration.low_amplitude * low_frequency_scale;

        f32 high_frequency_scale = 1.0f;
        if (vibration.high_frequency > high_start_sensitivity_limit) {
            high_frequency_scale =
                (std::max)(1.0f - (vibration.high_frequency - high_start_sensitivity_limit) /
                                    high_width_sensitivity_limit,
                         0.3f);
        }
        f32 high_amplitude = vibration.high_amplitude * high_frequency_scale;

        if (sdl_controller) {
            return SDL_RumbleGamepad(sdl_controller.get(), static_cast<u16>(low_amplitude),
                                     static_cast<u16>(high_amplitude), rumble_max_duration_ms);
        } else if (sdl_joystick) {
            return SDL_RumbleJoystick(sdl_joystick.get(), static_cast<u16>(low_amplitude),
                                      static_cast<u16>(high_amplitude),
                                      rumble_max_duration_ms);
        }

        return false;
    }

    // HLE thread: only store the newest sample (SDL rumble can block for ms on WGI/XInput)
    void QueueVibration(const Common::Input::VibrationStatus& vibration) {
        std::scoped_lock lock{pending_vibration_mutex};
        pending_vibration = vibration;
    }

    // PumpEvents thread: send the stored sample, outside the lock
    void FlushPendingVibration() {
        std::optional<Common::Input::VibrationStatus> vibration;
        {
            std::scoped_lock lock{pending_vibration_mutex};
            vibration.swap(pending_vibration);
        }
        if (vibration) {
            RumblePlay(*vibration);
        }
    }

    bool HasHDRumble() const {
        if (renhdrum_engine || (dualsense_dsp && ds_audio_stream) ||
            (dualsense_dsp && ds_bt_running.load(std::memory_order_relaxed))) {
            return true;
        }

        constexpr Uint16 valve_vendor_id = 0x28DE;
        const auto is_known_hd_type = [](SDL_GamepadType type) {
            return type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ||
                   type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
                   type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT ||
                   type == SDL_GAMEPAD_TYPE_PS5;
        };

        // Valve hardware doesn't have any enums in SDL, so we have to support it manually.
        // Since they have HD rumble, we can assume that all their hardware supports it, even if we can't detect the exact type.
        if (sdl_controller) {
            if (is_known_hd_type(SDL_GetGamepadType(sdl_controller.get())) ||
                SDL_GetGamepadVendor(sdl_controller.get()) == valve_vendor_id) {
                return true;
            }
        }

        if (sdl_joystick) {
            if (SDL_GetJoystickVendor(sdl_joystick.get()) == valve_vendor_id) {
                return true;
            }
        }

        return false;
    }

    void EnableVibration(bool is_enabled) {
        has_vibration = is_enabled;
        is_vibration_tested = true;
    }

    bool HasVibration() const {
        return has_vibration;
    }

    bool IsVibrationTested() const {
        return is_vibration_tested;
    }

    /**
     * The Pad identifier of the joystick
     */
    const PadIdentifier GetPadIdentifier() const {
        return {
            .guid = guid,
            .port = static_cast<std::size_t>(port),
            .pad = 0,
        };
    }

    /**
     * The guid of the joystick
     */
    const Common::UUID& GetGUID() const {
        return guid;
    }

    /**
     * The number of joystick from the same type that were connected before this joystick
     */
    int GetPort() const {
        return port;
    }

    SDL_Joystick* GetSDLJoystick() const {
        return sdl_joystick.get();
    }

    SDL_Gamepad* GetSDLGameController() const {
        return sdl_controller.get();
    }

    // HD Rumble dialog closed after a change: same teardown and setup as a reconnect
    void ReapplyRenHDrum() {
        {
            // A sample queued for the standard path must not land on the new mode
            std::scoped_lock lock{pending_vibration_mutex};
            pending_vibration.reset();
        }
        std::scoped_lock lock{renhdrum_mutex};
        // SDL remembers the last standard rumble and re-sends it for up to 2 s; each re-send drops the
        // Switch driver out of HD mode. Zero it so the pad starts the new mode clean
        if (sdl_controller) {
            SDL_RumbleGamepad(sdl_controller.get(), 0, 0, 0);
        } else if (sdl_joystick) {
            SDL_RumbleJoystick(sdl_joystick.get(), 0, 0, 0);
        }
        CleanupRenHDrum();
        if (sdl_joystick) {
            InitRenHDrum();
        }
    }

    void SetSDLJoystick(SDL_Joystick* joystick, SDL_Gamepad* controller) {
        std::scoped_lock lock{renhdrum_mutex};
        CleanupRenHDrum();
        sdl_joystick.reset(joystick);
        sdl_controller.reset(controller);
        if (sdl_joystick) {
            InitRenHDrum();
        }
    }

    bool IsJoyconLeft() const {
        const std::string controller_name = GetControllerName();
        if (std::strstr(controller_name.c_str(), "Joy-Con Left") != nullptr) {
            return true;
        }
        if (std::strstr(controller_name.c_str(), "Joy-Con (L)") != nullptr) {
            return true;
        }
        return false;
    }

    bool IsJoyconRight() const {
        const std::string controller_name = GetControllerName();
        if (std::strstr(controller_name.c_str(), "Joy-Con Right") != nullptr) {
            return true;
        }
        if (std::strstr(controller_name.c_str(), "Joy-Con (R)") != nullptr) {
            return true;
        }
        return false;
    }

    Common::Input::BatteryLevel GetBatteryLevel(SDL_PowerState battery_level, int percent) {
        if (battery_level == SDL_POWERSTATE_CHARGING) {
            return Common::Input::BatteryLevel::Charging;
        }

        if (percent >= 0 && percent <= 100) {
            if (percent <= 5) {
                return Common::Input::BatteryLevel::Empty;
            }
            if (percent <= 20) {
                return Common::Input::BatteryLevel::Critical;
            }
            if (percent <= 40) {
                return Common::Input::BatteryLevel::Low;
            }
            if (percent <= 70) {
                return Common::Input::BatteryLevel::Medium;
            }
            return Common::Input::BatteryLevel::Full;
        }

        switch (battery_level) {
        case SDL_POWERSTATE_ON_BATTERY:
            return Common::Input::BatteryLevel::Medium;
        case SDL_POWERSTATE_NO_BATTERY:
            return Common::Input::BatteryLevel::None;
        case SDL_POWERSTATE_CHARGED:
            return Common::Input::BatteryLevel::Full;
        case SDL_POWERSTATE_ERROR:
        case SDL_POWERSTATE_UNKNOWN:
        default:
            return Common::Input::BatteryLevel::None;
        }
    }

    std::string GetControllerName() const {
        if (sdl_controller) {
            switch (SDL_GetGamepadType(sdl_controller.get())) {
            case SDL_GAMEPAD_TYPE_XBOX360:
                return "Xbox 360 Controller";
            case SDL_GAMEPAD_TYPE_XBOXONE:
                return "Xbox One Controller";
            case SDL_GAMEPAD_TYPE_PS3:
                return "DualShock 3 Controller";
            case SDL_GAMEPAD_TYPE_PS4:
                return "DualShock 4 Controller";
            case SDL_GAMEPAD_TYPE_PS5:
                return "DualSense Controller";
            default:
                break;
            }
            const auto name = SDL_GetGamepadName(sdl_controller.get());
            if (name) {
                return name;
            }
        }

        if (sdl_joystick) {
            const auto name = SDL_GetJoystickName(sdl_joystick.get());
            if (name) {
                return name;
            }
        }

        return "Unknown";
    }

    RenHdcEngine* GetRenHDrumEngine() const {
        return renhdrum_engine;
    }

    RenHdcDualSenseDsp* GetDualSenseDsp() const {
        return dualsense_dsp;
    }

    RenHdcXboxRumble* GetXboxRumble() const {
        return xbox_rumble;
    }

    void TickXboxRumble(int64_t now_us) {
        constexpr u32 grip_duration_ms = 2000;
        // Trigger pulses are short: a lost stop must not leave these motors running
        constexpr u32 trigger_duration_ms = 200;
        uint16_t motors[4];
        SDL_Joystick* joystick = nullptr;
        {
            std::scoped_lock lock{renhdrum_mutex};
            if (!xbox_rumble || !sdl_joystick ||
                renhdrum_xbox_rumble_tick(xbox_rumble, now_us, motors) != 1) {
                return;
            }
            joystick = sdl_joystick.get();
        }
        const bool sent = SDL_RumbleJoystick(joystick, motors[0], motors[1], grip_duration_ms);
        // XInput has no trigger motors, so only the grips decide the retry
        SDL_RumbleJoystickTriggers(joystick, motors[2], motors[3], trigger_duration_ms);
        if (!sent) {
            // rawinput refuses rumble until the first button press
            std::scoped_lock lock{renhdrum_mutex};
            if (xbox_rumble) {
                renhdrum_xbox_rumble_resend(xbox_rumble);
            }
        }
    }

    bool XboxRumbleVibrate(int64_t now_us, bool is_right,
                           const Common::Input::VibrationStatus& v) {
        {
            std::scoped_lock lock{renhdrum_mutex};
            if (!xbox_rumble) {
                return false;
            }
            renhdrum_xbox_rumble_push_half_sample(xbox_rumble, now_us, is_right, v.low_amplitude,
                                                  v.low_frequency, v.high_amplitude,
                                                  v.high_frequency);
        }
        TickXboxRumble(now_us);
        return true;
    }

    bool HasDualSenseAudio() const {
        return ds_audio_stream != nullptr;
    }

    void TickRenHDrum(int64_t now_us) {
        uint8_t packet[10];
        SDL_Joystick* joystick = nullptr;
        {
            std::scoped_lock lock{renhdrum_mutex};
            if (!renhdrum_engine || !sdl_joystick ||
                renhdrum_engine_tick(renhdrum_engine, now_us, packet) != 1) {
                return;
            }
            joystick = sdl_joystick.get();
        }
        SDL_SendJoystickEffect(joystick, packet, sizeof(packet));
    }

    // Vibration boost for the RenHDrum paths: Configure Vibration > HD Rumble > Boost (100-400 %,
    // 100 = what the game asks for). RenHDrum sends the game's amplitude as is, without the curve
    // the standard rumble applies, so quiet effects feel weak.
    // a' = 1 - (1 - a)^boost lifts quiet and medium effects while 0 stays 0 and 1.0 (the most the
    // motors accept) stays the top, so strong effects do not all flatten into one level.
    static float RenHDrumBoost() {
        return static_cast<float>(std::clamp(Settings::values.hd_rumble_boost.GetValue(), 100, 400)) /
               100.0f;
    }

    static f32 BoostAmplitude(f32 amplitude, float boost) {
        amplitude = std::clamp(amplitude, 0.0f, 1.0f);
        return boost == 1.0f ? amplitude : 1.0f - std::pow(1.0f - amplitude, boost);
    }

    // Called on the HLE thread. Returns true when RenHDrum took the vibration (Switch HD engine, or
    // DualSense haptics audio running); false = use the standard rumble below.
    bool RenHDrumVibrate(int64_t now_us, bool is_right, const Common::Input::VibrationStatus& raw) {
        const float boost = RenHDrumBoost();
        Common::Input::VibrationStatus v = raw;
        v.low_amplitude = BoostAmplitude(raw.low_amplitude, boost);
        v.high_amplitude = BoostAmplitude(raw.high_amplitude, boost);
        if (v.low_amplitude > 0.01f || v.high_amplitude > 0.01f) {
            last_vibrate_us.store(now_us, std::memory_order_relaxed);
        }
        if (XboxRumbleVibrate(now_us, is_right, v)) {
            return true;
        }
        if (SteamTritonVibrate(now_us, is_right, v)) {
            return true;
        }
        {
            std::scoped_lock lock{renhdrum_mutex};
            if (!renhdrum_engine) {
                if (!dualsense_dsp) {
                    return false;
                }
                renhdrum_dualsense_dsp_set_half_target(dualsense_dsp, is_right, v.low_amplitude,
                                                       v.low_frequency, v.high_amplitude,
                                                       v.high_frequency);
                return ds_audio_stream != nullptr || ds_bt_running.load(std::memory_order_relaxed);
            }
            renhdrum_engine_push_half_sample(renhdrum_engine, now_us, is_right, v.low_amplitude,
                                             v.low_frequency, v.high_amplitude, v.high_frequency);
        }
        TickRenHDrum(now_us);
        return true;
    }

    void MaintainDualSenseAudio(int64_t now_us) {
        std::scoped_lock lock{renhdrum_mutex};
        if (!dualsense_dsp || ds_audio_stream || ds_audio_attempts >= DS_AUDIO_MAX_ATTEMPTS ||
            now_us < ds_audio_next_us) {
            return;
        }
        ds_audio_next_us = now_us + 2'000'000;
        TryOpenDualSenseAudio();
    }

private:
    void InitRenHDrum() {
        if (!sdl_joystick) {
            return;
        }

        const Uint16 vid = SDL_GetJoystickVendor(sdl_joystick.get());
        const Uint16 pid = SDL_GetJoystickProduct(sdl_joystick.get());

        // Nintendo Switch controllers (VID 0x057E)
        if (vid == 0x057E) {
            if (pid == 0x2009 || pid == 0x2006 || pid == 0x2007) {
                const bool is_bluetooth =
                    (SDL_GetJoystickConnectionState(sdl_joystick.get()) == SDL_JOYSTICK_CONNECTION_WIRELESS);
                const bool is_pro = (pid == 0x2009);
                if (!HdRumbleOn()) {
                    // No engine: SetVibration takes the emulator's own SDL rumble, so on/off can be compared
                    pad_name = is_pro ? "Pro Controller" : pid == 0x2006 ? "Joy-Con (L)" : "Joy-Con (R)";
                    LOG_INFO(Input, "HD Rumble off: Switch controller {:04X}:{:04X} uses plain rumble", vid, pid);
                    PublishPad({pad_name, false, is_bluetooth, "off", false});
                    return;
                }
                renhdrum_engine = renhdrum_engine_create(is_bluetooth, is_pro);
                if (renhdrum_engine) {
                    pad_name = is_pro ? "Pro Controller" : pid == 0x2006 ? "Joy-Con (L)" : "Joy-Con (R)";
                    LOG_INFO(Input, "Initialized RenHDrum engine for Switch controller {:04X}:{:04X} (BT: {})",
                             vid, pid, is_bluetooth);
                    ApplyRenHDrumSettings(is_bluetooth);
                }
            }
        }
        // Sony DualSense (VID 0x054C, PID 0x0CE6 or 0x0DF2)
        else if (vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2)) {
            if (!HdRumbleOn()) {
                pad_name = pid == 0x0DF2 ? "DualSense Edge" : "DualSense";
                LOG_INFO(Input, "HD Rumble off: DualSense {:04X}:{:04X} uses plain rumble", vid, pid);
                PublishPad({pad_name, true,
                            SDL_GetJoystickConnectionState(sdl_joystick.get()) ==
                                SDL_JOYSTICK_CONNECTION_WIRELESS,
                            "off", false});
                return;
            }
            dualsense_dsp = renhdrum_dualsense_dsp_create(100, true);
            if (dualsense_dsp) {
                LOG_INFO(Input, "Initialized RenHDrum DualSense DSP for controller {:04X}:{:04X}", vid, pid);
                pad_name = pid == 0x0DF2 ? "DualSense Edge" : "DualSense";
                PublishPad({pad_name, true,
                            SDL_GetJoystickConnectionState(sdl_joystick.get()) ==
                                SDL_JOYSTICK_CONNECTION_WIRELESS,
                            "DualSense standard rumble", false});
                TryOpenDualSenseAudio();
            }
        }
        // Xbox One S family. Only the wired PIDs get 10 ms, anything else takes the safer 20 ms
        else if (vid == 0x045E && sdl_controller &&
                 SDL_GetGamepadType(sdl_controller.get()) == SDL_GAMEPAD_TYPE_XBOXONE) {
            // The main HD Rumble switch also covers Xbox: off means SDL's plain 2-motor rumble
            if (!HdRumbleOn()) {
                LOG_INFO(Input, "HD Rumble off: Xbox controller {:04X}:{:04X} uses plain rumble", vid, pid);
                return;
            }
            // Wired or wireless from SDL's own connection state (any Microsoft pad, as DualSense above);
            // when SDL cannot tell, fall back to the two known wired PIDs
            const auto connection = SDL_GetJoystickConnectionState(sdl_joystick.get());
            const bool is_bluetooth =
                (connection == SDL_JOYSTICK_CONNECTION_WIRED || connection == SDL_JOYSTICK_CONNECTION_WIRELESS)
                    ? connection == SDL_JOYSTICK_CONNECTION_WIRELESS
                    : (pid != 0x02EA && pid != 0x02FF);
            xbox_rumble = renhdrum_xbox_rumble_create(is_bluetooth);
            if (xbox_rumble) {
                LOG_INFO(Input,
                         "Initialized Xbox rumble converter for controller {:04X}:{:04X} (BT: {}, SDL connection: {})",
                         vid, pid, is_bluetooth, static_cast<int>(connection));
                pad_name = "Xbox One Controller";
                PublishPad({pad_name, false, is_bluetooth, "Xbox rumble", false});
            }
        }
        // Steam Controller (2026, "Triton"): wired, Bluetooth LE or the Steam dongles
        else if (vid == 0x28DE && pid >= 0x1302 && pid <= 0x1305) {
            if (!HdRumbleOn()) {
                LOG_INFO(Input, "HD Rumble off: Steam Controller {:04X}:{:04X} uses plain rumble", vid, pid);
                return;
            }
            const bool is_bluetooth = pid == 0x1303;
            // Like the other pads here: callers already serialize Init/Cleanup (SetSDLJoystick holds renhdrum_mutex)
            steam_triton = true;
            steam_tone = {};
            steam_grips = {};
            pad_name = "Steam Controller";
            LOG_INFO(Input, "Initialized Steam Controller HD tones for {:04X}:{:04X}", vid, pid);
            PublishPad({pad_name, false, is_bluetooth, SteamSplitBands() ? "Steam HD · 4 motors" : "Steam HD tones", false});
        }
    }

    // Steam Controller HD rumble over its four LRAs, two bands per side:
    //  - low band  -> grip motor of that side: rumble report 0x80, both grips in one report.
    //    Amplitude only, the firmware picks the waveform. Sent here rather than through
    //    SDL_RumbleJoystick, which skips repeated values while the pad stops after ~50 ms.
    //  - high band -> trackpad motor of that side: tone report 0x83 at the game's own frequency.
    // Configure Vibration > HD Rumble > Steam Controller can go back to one tone per side (stronger
    // band) and no grips.
    // Everything is sent for 60 ms and refreshed while the game vibrates: the pad stops by itself
    // (and the firmware's ~50 ms safety timeout never cuts in) if the emulator stalls.
    static constexpr u8 STEAM_REPORT_RUMBLE = 0x80;
    static constexpr u8 STEAM_REPORT_COMMAND = 0x82;
    static constexpr u8 STEAM_REPORT_LFO_TONE = 0x83;
    static constexpr int64_t STEAM_MIN_GAP_US = 8'000;
    static constexpr int64_t STEAM_REFRESH_US = 30'000;
    static constexpr u16 STEAM_DURATION_MS = 60;

    struct SteamToneSide {
        bool on{false};
        u16 frequency{0};
        s8 gain_db{0};
        int64_t last_write_us{0};
    };

    struct SteamGrips {
        std::array<u16, 2> speed{};      // what the game asks for now, per side
        std::array<u16, 2> sent_speed{}; // what the pad is running
        int64_t last_write_us{0};
    };

    // Decides the tone report for one side (under renhdrum_mutex). 0 = nothing to send.
    int BuildSteamTone(std::array<u8, 10>& report, int64_t now_us, bool is_right, f32 amplitude,
                       f32 frequency_hz) {
        auto& side = steam_tone[is_right ? 1 : 0];
        const u8 side_id = is_right ? 0x02 : 0x01;
        if (amplitude < 0.02f) {
            if (!side.on) {
                return 0;
            }
            side.on = false;
            side.last_write_us = now_us;
            report = {STEAM_REPORT_COMMAND, side_id, 0 /* off */, 0};
            return 4;
        }
        const u16 frequency = static_cast<u16>(std::clamp(frequency_hz, 40.0f, 1250.0f));
        const s8 gain_db = static_cast<s8>(
            std::clamp(std::lround(20.0f * std::log10(amplitude)) + SteamGainOffsetDb(), -40L, 6L));
        const bool changed = !side.on || gain_db != side.gain_db ||
                             std::abs(frequency - side.frequency) > side.frequency / 32;
        const int64_t since = now_us - side.last_write_us;
        if (since < STEAM_MIN_GAP_US || (!changed && since < STEAM_REFRESH_US)) {
            return 0;
        }
        side = {true, frequency, gain_db, now_us};
        report = {STEAM_REPORT_LFO_TONE,
                  side_id,
                  static_cast<u8>(gain_db),
                  static_cast<u8>(frequency & 0xFF),
                  static_cast<u8>(frequency >> 8),
                  static_cast<u8>(STEAM_DURATION_MS & 0xFF),
                  static_cast<u8>(STEAM_DURATION_MS >> 8),
                  0, 0, 0 /* no LFO */};
        return 10;
    }

    // Decides the grip rumble write (under renhdrum_mutex): both grips go in one report.
    bool BuildSteamGrips(int64_t now_us, bool is_right, f32 amplitude, std::array<u16, 2>& out) {
        auto& grips = steam_grips;
        grips.speed[is_right ? 1 : 0] =
            amplitude < 0.02f ? 0 : static_cast<u16>(std::clamp(amplitude, 0.0f, 1.0f) * 0xFFFF);
        const bool running = grips.sent_speed[0] != 0 || grips.sent_speed[1] != 0;
        const bool wanted = grips.speed[0] != 0 || grips.speed[1] != 0;
        if (!running && !wanted) {
            return false;
        }
        const auto moved = [](u16 a, u16 b) { return std::abs(int{a} - int{b}) > 0xFFFF / 64; };
        const bool changed = moved(grips.speed[0], grips.sent_speed[0]) ||
                             moved(grips.speed[1], grips.sent_speed[1]) || (running && !wanted);
        const int64_t since = now_us - grips.last_write_us;
        if ((since < STEAM_MIN_GAP_US && wanted) || (!changed && since < STEAM_REFRESH_US)) {
            return false;
        }
        grips.sent_speed = grips.speed;
        grips.last_write_us = now_us;
        out = grips.speed;
        return true;
    }

    bool SteamTritonVibrate(int64_t now_us, bool is_right, const Common::Input::VibrationStatus& v) {
        std::array<u8, 10> tone{};
        int tone_size = 0;
        std::array<u16, 2> grip_speed{};
        bool grip_write = false;
        {
            std::scoped_lock lock{renhdrum_mutex};
            if (!steam_triton) {
                return false;
            }
            const f32 low = std::clamp(v.low_amplitude, 0.0f, 1.0f);
            const f32 high = std::clamp(v.high_amplitude, 0.0f, 1.0f);
            if (SteamSplitBands()) {
                grip_write = BuildSteamGrips(now_us, is_right, low, grip_speed);
                tone_size = BuildSteamTone(tone, now_us, is_right, high, v.high_frequency);
            } else {
                const f32 amplitude = std::min(1.0f, std::max(low, high) + 0.5f * std::min(low, high));
                tone_size = BuildSteamTone(tone, now_us, is_right, amplitude,
                                           low >= high ? v.low_frequency : v.high_frequency);
            }
        }
        if (grip_write) {
            SendSteamGrips(grip_speed[0], grip_speed[1]);
        }
        if (tone_size && !SDL_SendJoystickEffect(sdl_joystick.get(), tone.data(), tone_size)) {
            LOG_DEBUG(Input, "Steam Controller haptic report failed: {}", SDL_GetError());
        }
        return true;
    }

    // Report 0x80: type, intensity (u16), then speed (u16) + gain (s8) for the left and right grip
    void SendSteamGrips(u16 left, u16 right) {
        const std::array<u8, 10> report{STEAM_REPORT_RUMBLE, 0, 0, 0,
                                        static_cast<u8>(left & 0xFF), static_cast<u8>(left >> 8), 0,
                                        static_cast<u8>(right & 0xFF), static_cast<u8>(right >> 8), 0};
        if (!SDL_SendJoystickEffect(sdl_joystick.get(), report.data(), static_cast<int>(report.size()))) {
            LOG_DEBUG(Input, "Steam Controller grip rumble failed: {}", SDL_GetError());
        }
    }

    void StopSteamTones() {
        SendSteamGrips(0, 0);
        SDL_RumbleJoystick(sdl_joystick.get(), 0, 0, 0);
        for (const u8 side_id : {u8{0x01}, u8{0x02}}) {
            const std::array<u8, 4> off{STEAM_REPORT_COMMAND, side_id, 0, 0};
            SDL_SendJoystickEffect(sdl_joystick.get(), off.data(), static_cast<int>(off.size()));
        }
    }

    // Configure Vibration > HD Rumble > Steam Controller
    static bool SteamSplitBands() {
        return Settings::values.steam_hd_four_motors.GetValue();
    }

    // Configure Vibration > HD Rumble > Steam Controller trackpads, in dB (-20 to +6)
    static long SteamGainOffsetDb() {
        return std::clamp<long>(Settings::values.steam_trackpad_gain_db.GetValue(), -20, 6);
    }

    // Ryuhdr's recommended mode for a configured pad (0.2.6): wired 15 ms + cards 1 / 3 / 5 / 7 / 8, Bluetooth level 3 +
    // B1 / B3 / B2. Flag files next to eden.exe still win (renhdrum_settings_resolve). Then the SDL modes are switched on; a mode
    // SDL refused is cleared, so the engine never paces for writes that SDL does not gate (Ryuhdr: _noQueueActive / _btFastActive).
    void ApplyRenHDrumSettings(bool is_bluetooth) {
        RenHdcConfig config{};
        config.enable_hd_rumble = true;
        config.hd_rumble_usb_interval_ms = 15;
        config.hd_rumble_bluetooth_level = 3;
        config.hd_rumble_usb_auto_slow = true;
        config.hd_rumble_experimental_usb_no_queue = true;
        config.hd_rumble_experimental_envelope_5ms = true;
        config.hd_rumble_experimental_frequency_in_packet = true;
        config.hd_rumble_experimental_frequency_both_bands = true;
        config.hd_rumble_experimental_crisp_notes = true;
        config.hd_rumble_experimental_bt_after_input = true;
        config.hd_rumble_experimental_bt_fast = true;
        config.hd_rumble_experimental_bt_crisp_notes = true;

        RenHdcSettings settings{};
        if (renhdrum_settings_resolve(&config, &settings) != RENHDRUM_OK) {
            return;
        }

        SDL_Joystick* joystick = sdl_joystick.get();
        if (is_bluetooth) {
            // Card B3 needs B1 on (Ryuhdr HdApplyBtFast); SDL never runs both on one pad, B3 wins
            settings.bt_fast = settings.enabled && settings.bt_fast && settings.bt_after_input &&
                               SDL_HDRumbleSetNoQueueBluetooth(joystick, 1, HD_NO_QUEUE_ENVELOPE_GAP_MS) == 1;
            settings.bt_after_input = settings.enabled && settings.bt_after_input && !settings.bt_fast &&
                                      SDL_HDRumbleSetBluetoothAfterInput(joystick, 1) == 1;
            settings.usb_no_queue = false;
            LOG_INFO(Input, "RenHDrum HD Rumble Bluetooth: level {}, {}{}", settings.bluetooth_level,
                     settings.bt_fast          ? "card B3 (5 ms envelope, written while SDL is idle)"
                     : settings.bt_after_input ? "card B1 (written right after an input report)"
                                               : "clock based writes",
                     settings.enabled && settings.bt_crisp_notes ? ", card B2 crisp notes" : "");
        } else {
            // Card 3 sends one packet per 15 ms window itself: a smaller SDL gap (Ryuhdr HdApplyNoQueue)
            const int gap_ms =
                settings.envelope_5ms ? HD_NO_QUEUE_ENVELOPE_GAP_MS : settings.usb_interval_ms;
            settings.usb_no_queue = settings.enabled && settings.usb_no_queue &&
                                    SDL_HDRumbleSetNoQueue(joystick, 1, gap_ms, settings.usb_pause_ms) == 1;
            settings.bt_after_input = false;
            settings.bt_fast = false;
            const bool envelope = settings.usb_no_queue && settings.envelope_5ms;
            LOG_INFO(Input, "RenHDrum HD Rumble wired: {} ms, {}{}{}{}", settings.usb_interval_ms,
                     settings.usb_no_queue ? "no-queue (written while SDL is idle)" : "queued writes",
                     envelope ? ", card 3 5 ms envelope" : "",
                     envelope && settings.frequency_in_packet && settings.frequency_both_bands ? ", cards 5 + 7" : "",
                     envelope && settings.crisp_notes ? ", card 8 crisp notes" : "");
        }
        renhdrum_engine_apply_settings(renhdrum_engine, &settings);

        std::string mode;
        if (!settings.enabled) {
            mode = "off";
        } else if (is_bluetooth) {
            mode = "BT level " + std::to_string(settings.bluetooth_level);
            std::string cards;
            cards += settings.bt_after_input ? " B1" : "";
            cards += settings.bt_fast ? " B3" : "";
            cards += settings.bt_crisp_notes ? " B2" : "";
            mode += cards.empty() ? "" : " · " + cards.substr(1);
        } else if (!settings.usb_no_queue) {
            mode = "wired " + std::to_string(settings.usb_interval_ms) + " ms · queued";
        } else {
            std::string cards = " 1";
            if (settings.envelope_5ms) {
                cards += " 3";
                cards += settings.frequency_in_packet ? " 5" : "";
                cards += settings.frequency_in_packet && settings.frequency_both_bands ? " 7" : "";
                cards += settings.crisp_notes ? " 8" : "";
            }
            mode = "wired " + std::to_string(settings.usb_interval_ms) + " ms · cards" + cards;
        }
        PublishPad({pad_name, false, is_bluetooth, std::move(mode), false});
    }

    // The guid and port tell the HD Rumble dialog which pads are mapped to a player
    void PublishPad(RenHDrumPadInfo info) {
        info.guid = guid.RawString();
        info.port = port;
        SetPadInfo(this, std::move(info), &last_vibrate_us);
    }

    void TryOpenDualSenseAudio() {
        if (!dualsense_dsp || !sdl_joystick || ds_audio_stream) {
            return;
        }
        ++ds_audio_attempts;
        if (SDL_GetJoystickConnectionState(sdl_joystick.get()) == SDL_JOYSTICK_CONNECTION_WIRELESS) {
            ds_audio_attempts = DS_AUDIO_MAX_ATTEMPTS;
            StartDualSenseBluetooth();
            return;
        }
        if (!SDL_WasInit(SDL_INIT_AUDIO)) {
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                LOG_WARNING(Input, "DualSense haptics: SDL audio init failed: {}", SDL_GetError());
                return;
            }
            ds_audio_inited = true;
        }

        int dev_count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&dev_count);
        SDL_AudioDeviceID matched_dev = 0;
        std::string seen;
        for (int i = 0; devices && i < dev_count; ++i) {
            const char* name = SDL_GetAudioDeviceName(devices[i]);
            SDL_AudioSpec dev_spec{};
            const int channels =
                SDL_GetAudioDeviceFormat(devices[i], &dev_spec, nullptr) ? dev_spec.channels : 0;
            seen += std::string("\"") + (name ? name : "?") + "\" (" + std::to_string(channels) + " ch); ";
            if (name && channels >= 4 &&
                (std::strstr(name, "DualSense") != nullptr ||
                 std::strstr(name, "Wireless Controller") != nullptr)) {
                matched_dev = devices[i];
                break;
            }
        }
        SDL_free(devices);

        if (matched_dev == 0) {
            LOG_WARNING(Input, "DualSense haptics: no 4-channel DualSense audio device yet (attempt {}): {}",
                        ds_audio_attempts, seen);
            return;
        }

        const SDL_AudioSpec spec{
            .format = SDL_AUDIO_F32,
            .channels = 4,
            .freq = 48000,
        };
        ds_audio_stream =
            SDL_OpenAudioDeviceStream(matched_dev, &spec, &SDLJoystick::DualSenseAudioCallback, this);
        if (!ds_audio_stream) {
            LOG_WARNING(Input, "DualSense haptics: could not open audio device: {}", SDL_GetError());
            return;
        }
        SDL_ResumeAudioStreamDevice(ds_audio_stream);
        PublishPad({pad_name, true, false, "DualSense haptics", false});

        // Stop SDL's HID rumble once so the pad plays the audio haptics (Ryuhdr: _hapticsStopSdlRumble).
        if (sdl_controller) {
            SDL_RumbleGamepad(sdl_controller.get(), 0, 0, 0);
        } else {
            SDL_RumbleJoystick(sdl_joystick.get(), 0, 0, 0);
        }
        int device_frames = 0;
        SDL_AudioSpec device_spec{};
        SDL_GetAudioDeviceFormat(matched_dev, &device_spec, &device_frames);
        LOG_INFO(Input, "DualSense haptics audio opened on \"{}\" (attempt {}, device {} ch {} Hz, {} frames per buffer)",
                 SDL_GetAudioDeviceName(matched_dev), ds_audio_attempts, device_spec.channels,
                 device_spec.freq, device_frames);
    }

    // DualSense on Bluetooth has no audio endpoint: open a second HID handle on the pad and send voice-coil
    // haptics as output report 0x32 (s8 3 kHz PCM built by RenHDrum) every 10.667 ms on a thread of its own.
    void StartDualSenseBluetooth() {
        if (!dualsense_dsp || !sdl_joystick || ds_bt_hid) {
            return;
        }
        const char* path = SDL_GetJoystickPath(sdl_joystick.get());
        if (!path || !*path) {
            LOG_INFO(Input, "DualSense BT haptics: no HID path, using standard rumble");
            return;
        }
        ds_bt_hid = SDL_hid_open_path(path);
        if (!ds_bt_hid) {
            LOG_WARNING(Input, "DualSense BT haptics: could not open HID path {}: {}, using standard rumble",
                        path, SDL_GetError());
            return;
        }
        ds_bt_running.store(true, std::memory_order_relaxed);
        ds_bt_thread = std::thread([this] { DualSenseBtLoop(); });

        // Stop SDL's HID rumble once so the pad plays the voice-coil haptics (same as the USB audio path).
        if (sdl_controller) {
            SDL_RumbleGamepad(sdl_controller.get(), 0, 0, 0);
        } else {
            SDL_RumbleJoystick(sdl_joystick.get(), 0, 0, 0);
        }
        PublishPad({pad_name, true, true, "DualSense haptics · Bluetooth", false});
        LOG_INFO(Input, "DualSense haptics over Bluetooth started (report 0x32, path {})", path);
    }

    // Runs on ds_bt_thread. Never takes renhdrum_mutex: the DSP has its own lock, and CleanupRenHDrum joins this
    // thread (while holding renhdrum_mutex) before the DSP and the HID handle are freed.
    void DualSenseBtLoop() {
        Common::SetCurrentThreadName("DualSenseBT");
        std::array<uint8_t, RENHDRUM_DS5_BT_REPORT_SIZE> report{};
        Uint64 next = SDL_GetTicksNS();
        Uint64 last_signal = 0, stats_at = next;
        int sent = 0, failed = 0, fails_in_row = 0;
        while (ds_bt_running.load(std::memory_order_relaxed)) {
            next += RENHDRUM_DS5_BT_PERIOD_NS;
            const int r = renhdrum_dualsense_dsp_build_bt_report(
                dualsense_dsp, RENHDRUM_DS5_BT_BUFFER_LENGTH_DEFAULT, report.data());
            const Uint64 now = SDL_GetTicksNS();
            if (r == 1) {
                last_signal = now;
            }
            // Silence gate: keep sending 2 s after the last vibration, then stay quiet on the radio.
            if (r >= 0 && last_signal != 0 && now - last_signal < 2'000'000'000ULL) {
                if (SDL_hid_write(ds_bt_hid, report.data(), report.size()) < 0) {
                    ++failed;
                    ++fails_in_row;
                } else {
                    ++sent;
                    fails_in_row = 0;
                }
            }
            if (fails_in_row >= 50) { // ~0.5 s of failed writes: pad gone, fall back to standard rumble
                LOG_WARNING(Input, "DualSense BT haptics: writes failing ({}), stopping", SDL_GetError());
                break;
            }
            if (now - stats_at >= 5'000'000'000ULL) {
                if (sent > 0 || failed > 0) {
                    LOG_INFO(Input, "DualSense BT haptics: {} reports/s, {} failed", sent / 5, failed);
                }
                sent = failed = 0;
                stats_at = now;
            }
            if (next > now) {
                SDL_DelayPrecise(next - now);
            } else if (now - next > 100'000'000ULL) {
                next = now; // stalled > 100 ms: resync instead of bursting
            }
        }
        ds_bt_running.store(false, std::memory_order_relaxed);
    }

    static void DualSenseAudioCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                       int total_amount) {
        if (additional_amount <= 0 || !userdata) {
            return;
        }
        auto* self = static_cast<SDLJoystick*>(userdata);
        if (!self->dualsense_dsp) {
            return;
        }

        constexpr int bytes_per_frame = 4 * static_cast<int>(sizeof(float));
        int frames_needed = additional_amount / bytes_per_frame;
        if (frames_needed <= 0) {
            return;
        }

        constexpr int CHUNK_FRAMES = 256;
        float chunk[CHUNK_FRAMES * 4];

        while (frames_needed > 0) {
            const int cur_frames = (std::min)(frames_needed, CHUNK_FRAMES);
            if (renhdrum_dualsense_dsp_fill_f32(self->dualsense_dsp, chunk,
                                                static_cast<size_t>(cur_frames)) != RENHDRUM_OK) {
                std::memset(chunk, 0, sizeof(chunk)); // never send stack garbage to the actuators
            }
            SDL_PutAudioStreamData(stream, chunk, cur_frames * bytes_per_frame);
            frames_needed -= cur_frames;
        }
    }

    void CleanupRenHDrum() {
        ErasePadInfo(this);
        if (ds_audio_stream) {
            SDL_PauseAudioStreamDevice(ds_audio_stream);
            SDL_ClearAudioStream(ds_audio_stream);
            SDL_DestroyAudioStream(ds_audio_stream);
            ds_audio_stream = nullptr;
        }
        if (ds_audio_inited) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            ds_audio_inited = false;
        }
        ds_audio_attempts = 0;
        ds_audio_next_us = 0;
        // Stop the Bluetooth sender before the DSP and the HID handle it uses are freed
        ds_bt_running.store(false, std::memory_order_relaxed);
        if (ds_bt_thread.joinable()) {
            ds_bt_thread.join();
        }
        if (ds_bt_hid) {
            SDL_hid_close(ds_bt_hid);
            ds_bt_hid = nullptr;
        }
        if (renhdrum_engine) {
            if (sdl_joystick) {
                uint8_t stop_pkt[10] = {0x10, 0, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
                SDL_SendJoystickEffect(sdl_joystick.get(), stop_pkt, sizeof(stop_pkt));
                // A re-applied setting may want other SDL write modes (or none, when HD Rumble is off)
                SDL_HDRumbleSetNoQueue(sdl_joystick.get(), 0, 0, 0);
                SDL_HDRumbleSetBluetoothAfterInput(sdl_joystick.get(), 0);
            }
            renhdrum_engine_destroy(renhdrum_engine);
            renhdrum_engine = nullptr;
        }
        if (dualsense_dsp) {
            renhdrum_dualsense_dsp_destroy(dualsense_dsp);
            dualsense_dsp = nullptr;
        }
        if (xbox_rumble) {
            if (sdl_joystick) {
                SDL_RumbleJoystick(sdl_joystick.get(), 0, 0, 0);
                SDL_RumbleJoystickTriggers(sdl_joystick.get(), 0, 0, 0);
            }
            renhdrum_xbox_rumble_destroy(xbox_rumble);
            xbox_rumble = nullptr;
        }
        if (steam_triton) {
            if (sdl_joystick) {
                StopSteamTones();
            }
            steam_triton = false;
        }
    }

    Common::UUID guid;
    int port;
    std::unique_ptr<SDL_Joystick, decltype(&SDL_CloseJoystick)> sdl_joystick;
    std::unique_ptr<SDL_Gamepad, decltype(&SDL_CloseGamepad)> sdl_controller;
    mutable std::mutex mutex;

    // Guards renhdrum_engine / dualsense_dsp / ds_audio_stream / xbox_rumble: SetVibration runs on the HLE thread while
    // CloseJoystick frees them. Never held while calling SDL_SendJoystickEffect from the HLE thread.
    std::mutex renhdrum_mutex;

    RenHdcEngine* renhdrum_engine{nullptr};
    std::string pad_name;
    RenHdcDualSenseDsp* dualsense_dsp{nullptr};
    RenHdcXboxRumble* xbox_rumble{nullptr};
    bool steam_triton{false};
    std::array<SteamToneSide, 2> steam_tone{};
    SteamGrips steam_grips{};
    std::mutex pending_vibration_mutex;
    std::optional<Common::Input::VibrationStatus> pending_vibration;
    SDL_AudioStream* ds_audio_stream{nullptr};
    static constexpr int DS_AUDIO_MAX_ATTEMPTS = 10;
    // SDL no-queue gap while the 5 ms envelope runs (Ryuhdr AmFmNoQueueGapMs)
    static constexpr int HD_NO_QUEUE_ENVELOPE_GAP_MS = 10;
    int ds_audio_attempts{0};
    int64_t ds_audio_next_us{0};
    bool ds_audio_inited{false};
    // DualSense on Bluetooth: second HID handle + sender thread for report 0x32
    SDL_hid_device* ds_bt_hid{nullptr};
    std::thread ds_bt_thread;
    std::atomic<bool> ds_bt_running{false};
    std::atomic<int64_t> last_vibrate_us{0};

    u64 last_motion_update{};
    static constexpr std::size_t InitialMotionRestartThreshold = 200;
    static constexpr std::size_t MaxMotionRestartThreshold = 200 * 64;
    std::size_t motion_error_count{};
    std::size_t motion_restart_threshold{InitialMotionRestartThreshold};
    static constexpr std::chrono::milliseconds InitialMotionStallRetry{1000};
    static constexpr std::chrono::milliseconds MaxMotionStallRetry{8000};
    std::chrono::steady_clock::time_point last_motion_report{};
    std::chrono::milliseconds motion_stall_retry{InitialMotionStallRetry};
    bool motion_capabilities_logged{false};
    bool logged_accel{false};
    bool logged_gyro{false};
    bool has_gyro{false};
    bool has_accel{false};
    bool has_vibration{false};
    bool is_vibration_tested{false};
    BasicMotion motion;
};

std::shared_ptr<SDLJoystick> SDLDriver::GetSDLJoystickByGUID(const Common::UUID& guid, int port) {
    std::scoped_lock lock{joystick_map_mutex};
    const auto it = joystick_map.find(guid);

    if (it != joystick_map.end()) {
        while (it->second.size() <= static_cast<std::size_t>(port)) {
            auto joystick = std::make_shared<SDLJoystick>(guid, static_cast<int>(it->second.size()),
                                                          nullptr, nullptr);
            it->second.emplace_back(std::move(joystick));
        }

        return it->second[static_cast<std::size_t>(port)];
    }

    auto joystick = std::make_shared<SDLJoystick>(guid, 0, nullptr, nullptr);

    return joystick_map[guid].emplace_back(std::move(joystick));
}

std::shared_ptr<SDLJoystick> SDLDriver::GetSDLJoystickByGUID(const std::string& guid, int port) {
    return GetSDLJoystickByGUID(Common::UUID{guid}, port);
}

std::shared_ptr<SDLJoystick> SDLDriver::GetSDLJoystickBySDLID(SDL_JoystickID sdl_id) {
    auto sdl_joystick = SDL_GetJoystickFromID(sdl_id);
    if (sdl_joystick == nullptr) {
        return nullptr;
    }
    const auto guid = GetGUID(sdl_joystick);

    std::scoped_lock lock{joystick_map_mutex};
    const auto map_it = joystick_map.find(guid);

    if (map_it == joystick_map.end()) {
        return nullptr;
    }

    const auto vec_it = std::find_if(map_it->second.begin(), map_it->second.end(),
                                     [&sdl_joystick](const auto& joystick) {
                                         return joystick->GetSDLJoystick() == sdl_joystick;
                                     });

    if (vec_it == map_it->second.end()) {
        return nullptr;
    }

    return *vec_it;
}

std::shared_ptr<SDLJoystick> SDLDriver::GetSDLJoystickByGamepadID(SDL_JoystickID sdl_id) {
    auto* const sdl_gamepad = SDL_GetGamepadFromID(sdl_id);
    if (sdl_gamepad == nullptr) {
        return nullptr;
    }

    auto* const sdl_joystick = SDL_GetGamepadJoystick(sdl_gamepad);
    if (sdl_joystick == nullptr) {
        return nullptr;
    }

    const auto guid = GetGUID(sdl_joystick);

    std::scoped_lock lock{joystick_map_mutex};
    const auto map_it = joystick_map.find(guid);
    if (map_it == joystick_map.end()) {
        return nullptr;
    }

    const auto vec_it = std::find_if(map_it->second.begin(), map_it->second.end(),
                                     [sdl_joystick, sdl_gamepad](const auto& joystick) {
                                         return joystick->GetSDLJoystick() == sdl_joystick ||
                                                joystick->GetSDLGameController() == sdl_gamepad;
                                     });

    if (vec_it == map_it->second.end()) {
        return nullptr;
    }

    return *vec_it;
}

void SDLDriver::InitJoystick(SDL_JoystickID joystick_id) {
    SDL_Joystick* sdl_joystick = SDL_OpenJoystick(joystick_id);
    SDL_Gamepad* sdl_gamecontroller = nullptr;
    int battery_percent = -1;
    SDL_PowerState battery_state = SDL_POWERSTATE_UNKNOWN;

    if (SDL_IsGamepad(joystick_id)) {
        sdl_gamecontroller = SDL_OpenGamepad(joystick_id);
    }

    if (!sdl_joystick) {
        LOG_ERROR(Input, "Failed to open joystick {}", joystick_id);
        return;
    }

    battery_state = SDL_GetJoystickPowerInfo(sdl_joystick, &battery_percent);

    const auto guid = GetGUID(sdl_joystick);

    if (Settings::values.enable_joycon_driver) {
        if (guid.uuid[5] == 0x05 && guid.uuid[4] == 0x7e &&
            (guid.uuid[8] == 0x06 || guid.uuid[8] == 0x07)) {
            LOG_WARNING(Input, "Preferring joycon driver for device index {}", joystick_id);
            SDL_CloseJoystick(sdl_joystick);
            return;
        }
    }

    if (Settings::values.enable_procon_driver) {
        if (guid.uuid[5] == 0x05 && guid.uuid[4] == 0x7e && guid.uuid[8] == 0x09) {
            LOG_WARNING(Input, "Preferring joycon driver for device index {}", joystick_id);
            SDL_CloseJoystick(sdl_joystick);
            return;
        }
    }

    std::scoped_lock lock{joystick_map_mutex};
    if (joystick_map.find(guid) == joystick_map.end()) {
        auto joystick = std::make_shared<SDLJoystick>(guid, 0, sdl_joystick, sdl_gamecontroller);
        PreSetController(joystick->GetPadIdentifier());
        joystick->EnableMotion();
        SetBattery(joystick->GetPadIdentifier(),
                   joystick->GetBatteryLevel(battery_state, battery_percent));
        joystick_map[guid].emplace_back(std::move(joystick));
        return;
    }

    auto& joystick_guid_list = joystick_map[guid];
    const auto joystick_it =
        std::find_if(joystick_guid_list.begin(), joystick_guid_list.end(),
                     [](const auto& joystick) { return !joystick->GetSDLJoystick(); });

    if (joystick_it != joystick_guid_list.end()) {
        (*joystick_it)->SetSDLJoystick(sdl_joystick, sdl_gamecontroller);
        (*joystick_it)->EnableMotion();
        SetBattery((*joystick_it)->GetPadIdentifier(),
                   (*joystick_it)->GetBatteryLevel(battery_state, battery_percent));
        return;
    }

    const int port = static_cast<int>(joystick_guid_list.size());
    auto joystick = std::make_shared<SDLJoystick>(guid, port, sdl_joystick, sdl_gamecontroller);
    PreSetController(joystick->GetPadIdentifier());
    joystick->EnableMotion();
    SetBattery(joystick->GetPadIdentifier(),
               joystick->GetBatteryLevel(battery_state, battery_percent));
    joystick_guid_list.emplace_back(std::move(joystick));
}

void SDLDriver::CloseJoystick(SDL_Joystick* sdl_joystick) {
    const auto guid = GetGUID(sdl_joystick);

    std::scoped_lock lock{joystick_map_mutex};
    // This call to guid is safe since the joystick is guaranteed to be in the map
    const auto& joystick_guid_list = joystick_map[guid];
    const auto joystick_it = std::find_if(joystick_guid_list.begin(), joystick_guid_list.end(),
                                          [&sdl_joystick](const auto& joystick) {
                                              return joystick->GetSDLJoystick() == sdl_joystick;
                                          });

    if (joystick_it != joystick_guid_list.end()) {
        (*joystick_it)->SetSDLJoystick(nullptr, nullptr);
    }
}

void SDLDriver::PumpEvents() {
    if (initialized) {
        SDL_PumpEvents();

        using namespace std::chrono;
        const auto now = steady_clock::now();
        const int64_t now_us = duration_cast<microseconds>(now.time_since_epoch()).count();

        // Tick outside joystick_map_mutex: these call into SDL, and SDL's rawinput thread holds the
        // SDL lock while SDLEventWatcher waits for that mutex, so holding it here deadlocks
        std::vector<std::shared_ptr<SDLJoystick>> joysticks;
        {
            std::scoped_lock lock{joystick_map_mutex};
            for (const auto& [guid, joystick_list] : joystick_map) {
                joysticks.insert(joysticks.end(), joystick_list.begin(), joystick_list.end());
            }
        }
        // Restart the motion sensors of controllers whose reports stopped (also outside the mutex)
        if (now - last_motion_stall_check >= milliseconds{250}) {
            last_motion_stall_check = now;
            for (const auto& joystick : joysticks) {
                if (joystick && joystick->IsMotionStalled(now)) {
                    LOG_INFO(Input, "Motion sensor of {} port {} stopped reporting, restarting it",
                             joystick->GetControllerName(), joystick->GetPort());
                    joystick->EnableMotion();
                }
            }
        }
        const uint32_t generation = settings_generation.load(std::memory_order_relaxed);
        if (applied_settings_generation.exchange(generation, std::memory_order_relaxed) != generation) {
            LOG_INFO(Input, "Re-applying HD Rumble settings to {} controller(s)", joysticks.size());
            for (const auto& joystick : joysticks) {
                if (joystick) {
                    joystick->ReapplyRenHDrum();
                }
            }
        }
        for (const auto& joystick : joysticks) {
            if (joystick) {
                joystick->FlushPendingVibration();
            }
            if (joystick && joystick->GetRenHDrumEngine()) {
                joystick->TickRenHDrum(now_us);
            }
            if (joystick && joystick->GetDualSenseDsp()) {
                joystick->MaintainDualSenseAudio(now_us);
            }
            if (joystick && joystick->GetXboxRumble()) {
                joystick->TickXboxRumble(now_us);
            }
        }
    }
}

void SDLDriver::HandleGameControllerEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_JOYSTICK_BUTTON_UP: {
        if (const auto joystick = GetSDLJoystickBySDLID(event.jbutton.which)) {
            const PadIdentifier identifier = joystick->GetPadIdentifier();
            SetButton(identifier, event.jbutton.button, false);
        }
        break;
    }
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: {
        if (const auto joystick = GetSDLJoystickBySDLID(event.jbutton.which)) {
            const PadIdentifier identifier = joystick->GetPadIdentifier();
            SetButton(identifier, event.jbutton.button, true);
        }
        break;
    }
    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
        if (const auto joystick = GetSDLJoystickBySDLID(event.jhat.which)) {
            const PadIdentifier identifier = joystick->GetPadIdentifier();
            SetHatButton(identifier, event.jhat.hat, event.jhat.value);
        }
        break;
    }
    case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
        if (const auto joystick = GetSDLJoystickBySDLID(event.jaxis.which)) {
            const PadIdentifier identifier = joystick->GetPadIdentifier();
            SetAxis(identifier, event.jaxis.axis, event.jaxis.value / 32767.0f);
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
        auto joystick = GetSDLJoystickByGamepadID(event.gsensor.which);
        if (!joystick) {
            joystick = GetSDLJoystickBySDLID(event.gsensor.which);
        }
        if (joystick) {
            if (joystick->UpdateMotion(event.gsensor)) {
                const PadIdentifier identifier = joystick->GetPadIdentifier();
                SetMotion(identifier, 0, joystick->GetMotion());
            }
        }
        break;
    }
    case SDL_EVENT_JOYSTICK_BATTERY_UPDATED: {
        if (auto joystick = GetSDLJoystickBySDLID(event.jbattery.which)) {
            const PadIdentifier identifier = joystick->GetPadIdentifier();
            SetBattery(identifier,
                       joystick->GetBatteryLevel(event.jbattery.state, event.jbattery.percent));
        }
        break;
    }
    case SDL_EVENT_JOYSTICK_REMOVED:
        LOG_DEBUG(Input, "Controller removed with Instance_ID {}", event.jdevice.which);
        CloseJoystick(SDL_GetJoystickFromID(event.jdevice.which));
        break;
    case SDL_EVENT_JOYSTICK_ADDED:
        LOG_DEBUG(Input, "Controller connected with device index {}", event.jdevice.which);
        InitJoystick(event.jdevice.which);
        break;
    }
}

void SDLDriver::CloseJoysticks() {
    std::scoped_lock lock{joystick_map_mutex};
    joystick_map.clear();
}

SDLDriver::SDLDriver(std::string input_engine_) : InputEngine(std::move(input_engine_)) {
    // Set our application name. Currently passed to DBus by SDL and visible to the user through
    // their desktop environment.
    SDL_SetHint(SDL_HINT_APP_NAME, "Eden");

    // Disable raw input. When enabled this setting causes SDL to die when a web applet opens
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, Settings::values.enable_raw_input ? "1" : "0");

#ifdef _WIN32
    if (Settings::values.disable_wgi_xinput) {
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_RAWINPUT_CORRELATE_XINPUT, "0", SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_WGI, "0", SDL_HINT_OVERRIDE);
    }
#endif

    // SDL3 defaults Steam Controller Bluetooth HIDAPI support to off, which can disable gyro.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_SENSOR_FUSION, "1");
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_SENSORS, "1");

    // Enable HIDAPI rumble. This prevents SDL from disabling motion on PS4 and PS5 controllers
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    // Disable hidapi drivers for joycon controllers when the custom joycon driver is enabled
    if (Settings::values.enable_joycon_driver) {
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "0");
    } else {
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOYCON_HOME_LED, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "1");
    }

    // Disable hidapi drivers for pro controllers when the custom joycon driver is enabled
    if (Settings::values.enable_procon_driver) {
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "0");
    } else {
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_HOME_LED, "0");
    }

    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_PLAYER_LED, "1");
    // Disable hidapi driver for xbox. Already default on Windows, this causes conflict with native
    // driver on Linux.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");

    // If the frontend is going to manage the event loop, then we don't start one here
    start_thread = SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD) == 0;
    if (start_thread && !SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        LOG_CRITICAL(Input, "SDL_Init failed with: {}", SDL_GetError());
        return;
    }

    SDL_AddEventWatch(&SDLEventWatcher, this);

    initialized = true;
    // Because the events for joystick connection happens before we have our event watcher added, we
    // can just open all the joysticks right here
    int joystick_count = 0;
    SDL_JoystickID* joysticks = SDL_GetJoysticks(&joystick_count);
    if (joysticks != nullptr) {
        for (int i = 0; i < joystick_count; ++i) {
            InitJoystick(joysticks[i]);
        }
        SDL_free(joysticks);
    }
}

SDLDriver::~SDLDriver() {
    CloseJoysticks();
    SDL_RemoveEventWatch(&SDLEventWatcher, this);

    initialized = false;
    if (start_thread) {
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);
    }
}

std::vector<Common::ParamPackage> SDLDriver::GetInputDevices() const {
    std::vector<Common::ParamPackage> devices;
    ::Common::unordered_map<int, std::shared_ptr<SDLJoystick>> joycon_pairs;
    for (const auto& [key, value] : joystick_map) {
        for (const auto& joystick : value) {
            if (!joystick->GetSDLJoystick()) {
                continue;
            }
            const std::string name =
                fmt::format("{} {}", joystick->GetControllerName(), joystick->GetPort());
            devices.emplace_back(Common::ParamPackage{
                {"engine", GetEngineName()},
                {"display", std::move(name)},
                {"guid", joystick->GetGUID().RawString()},
                {"port", std::to_string(joystick->GetPort())},
            });
            if (joystick->IsJoyconLeft()) {
                joycon_pairs.insert_or_assign(joystick->GetPort(), joystick);
            }
        }
    }

    // Add dual controllers
    for (const auto& [key, value] : joystick_map) {
        for (const auto& joystick : value) {
            if (joystick->IsJoyconRight()) {
                if (!joycon_pairs.contains(joystick->GetPort())) {
                    continue;
                }
                const auto joystick2 = joycon_pairs.at(joystick->GetPort());

                const std::string name =
                    fmt::format("{} {}", "Nintendo Dual Joy-Con", joystick->GetPort());
                devices.emplace_back(Common::ParamPackage{
                    {"engine", GetEngineName()},
                    {"display", std::move(name)},
                    {"guid", joystick->GetGUID().RawString()},
                    {"guid2", joystick2->GetGUID().RawString()},
                    {"port", std::to_string(joystick->GetPort())},
                });
            }
        }
    }
    return devices;
}

Common::Input::DriverResult SDLDriver::SetVibration(
    const PadIdentifier& identifier, const Common::Input::VibrationStatus& vibration) {
    const auto joystick =
        GetSDLJoystickByGUID(identifier.guid.RawString(), static_cast<int>(identifier.port));
    if (!joystick) {
        return Common::Input::DriverResult::InvalidHandle;
    }

    using namespace std::chrono;
    const int64_t now_us = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    const bool is_right = (identifier.pad == 1);

    if (joystick->RenHDrumVibrate(now_us, is_right, vibration)) {
        return Common::Input::DriverResult::Success;
    }

    const auto process_amplitude_exp = [](f32 amplitude, f32 factor) {
        return (amplitude + std::pow(amplitude, factor)) * 0.5f * 0xFFFF;
    };

    // Default exponential curve for rumble
    f32 factor = 0.35f;

    // If vibration is set as a linear output use a flatter value
    if (vibration.type == Common::Input::VibrationAmplificationType::Linear) {
        factor = 0.5f;
    }

    // Amplitude for HD rumble needs no modification
    if (joystick->HasHDRumble()) {
        factor = 1.0f;
    }

    const Common::Input::VibrationStatus new_vibration{
        .low_amplitude = process_amplitude_exp(vibration.low_amplitude, factor),
        .low_frequency = vibration.low_frequency,
        .high_amplitude = process_amplitude_exp(vibration.high_amplitude, factor),
        .high_frequency = vibration.high_frequency,
        .type = Common::Input::VibrationAmplificationType::Exponential,
    };

    joystick->QueueVibration(new_vibration);

    return Common::Input::DriverResult::Success;
}

bool SDLDriver::IsVibrationEnabled(const PadIdentifier& identifier) {
    const auto joystick =
        GetSDLJoystickByGUID(identifier.guid.RawString(), static_cast<int>(identifier.port));
    if (!joystick) {
        return false;
    }

    if (joystick->IsVibrationTested()) {
        return joystick->HasVibration();
    }

    if (joystick->GetRenHDrumEngine() || joystick->GetDualSenseDsp() || joystick->GetXboxRumble()) {
        joystick->EnableVibration(true);
        return true;
    }

    static constexpr Common::Input::VibrationStatus test_vibration{
        .low_amplitude = 1,
        .low_frequency = 160.0f,
        .high_amplitude = 1,
        .high_frequency = 320.0f,
        .type = Common::Input::VibrationAmplificationType::Exponential,
    };

    static constexpr Common::Input::VibrationStatus zero_vibration{
        .low_amplitude = 0,
        .low_frequency = 160.0f,
        .high_amplitude = 0,
        .high_frequency = 320.0f,
        .type = Common::Input::VibrationAmplificationType::Exponential,
    };

    // First vibration might fail
    joystick->RumblePlay(test_vibration);

    // Wait for about 15ms to ensure the controller is ready for the stop command
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    if (!joystick->RumblePlay(zero_vibration)) {
        joystick->EnableVibration(false);
        return false;
    }

    joystick->EnableVibration(true);
    return true;
}

Common::ParamPackage SDLDriver::BuildAnalogParamPackageForButton(int port, const Common::UUID& guid,
                                                                 s32 axis, float value) const {
    Common::ParamPackage params{};
    params.Set("engine", GetEngineName());
    params.Set("port", port);
    params.Set("guid", guid.RawString());
    params.Set("axis", axis);
    params.Set("threshold", "0.5");
    params.Set("invert", value < 0 ? "-" : "+");
    return params;
}

Common::ParamPackage SDLDriver::BuildButtonParamPackageForButton(int port, const Common::UUID& guid,
                                                                 s32 button) const {
    Common::ParamPackage params{};
    params.Set("engine", GetEngineName());
    params.Set("port", port);
    params.Set("guid", guid.RawString());
    params.Set("button", button);
    return params;
}

Common::ParamPackage SDLDriver::BuildHatParamPackageForButton(int port, const Common::UUID& guid,
                                                              s32 hat, u8 value) const {
    Common::ParamPackage params{};
    params.Set("engine", GetEngineName());
    params.Set("port", port);
    params.Set("guid", guid.RawString());
    params.Set("hat", hat);
    params.Set("direction", GetHatButtonName(value));
    return params;
}

Common::ParamPackage SDLDriver::BuildMotionParam(int port, const Common::UUID& guid) const {
    Common::ParamPackage params{};
    params.Set("engine", GetEngineName());
    params.Set("motion", 0);
    params.Set("port", port);
    params.Set("guid", guid.RawString());
    return params;
}

Common::ParamPackage SDLDriver::BuildParamPackageForBinding(
    int port, const Common::UUID& guid, const SDL_GamepadBinding& binding) const {
    switch (binding.input_type) {
    case SDL_GAMEPAD_BINDTYPE_NONE:
        break;
    case SDL_GAMEPAD_BINDTYPE_AXIS:
        return BuildAnalogParamPackageForButton(port, guid, binding.input.axis.axis);
    case SDL_GAMEPAD_BINDTYPE_BUTTON:
        return BuildButtonParamPackageForButton(port, guid, binding.input.button);
    case SDL_GAMEPAD_BINDTYPE_HAT:
        return BuildHatParamPackageForButton(port, guid, binding.input.hat.hat,
                                             static_cast<u8>(binding.input.hat.hat_mask));
    }
    return {};
}

Common::ParamPackage SDLDriver::BuildParamPackageForAnalog(PadIdentifier identifier, int axis_x,
                                                           int axis_y, float offset_x,
                                                           float offset_y) const {
    Common::ParamPackage params;
    params.Set("engine", GetEngineName());
    params.Set("port", static_cast<int>(identifier.port));
    params.Set("guid", identifier.guid.RawString());
    params.Set("axis_x", axis_x);
    params.Set("axis_y", axis_y);
    params.Set("offset_x", offset_x);
    params.Set("offset_y", offset_y);
    params.Set("invert_x", "+");
    params.Set("invert_y", "+");
    return params;
}

ButtonMapping SDLDriver::GetButtonMappingForDevice(const Common::ParamPackage& params) {
    if (!params.Has("guid") || !params.Has("port")) {
        return {};
    }
    const auto joystick = GetSDLJoystickByGUID(params.Get("guid", ""), params.Get("port", 0));

    // This list is missing ZL/ZR since those are not considered buttons in SDL GameController.
    // We will add those afterwards
    ButtonBindings switch_to_sdl_button = GetDefaultButtonBinding(joystick);
    // Add the missing bindings for ZL/ZR
    static constexpr ZButtonBindings switch_to_sdl_axis{{
        {Settings::NativeButton::ZL, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
        {Settings::NativeButton::ZR, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
    }};
    if (auto* controller = joystick->GetSDLGameController(); controller) {
        // Parameters contain two joysticks return dual
        if (params.Has("guid2")) {
            const auto joystick2 = GetSDLJoystickByGUID(params.Get("guid2", ""), params.Get("port", 0));
            if (joystick2->GetSDLGameController())
                return GetDualControllerMapping(joystick, joystick2, switch_to_sdl_button, switch_to_sdl_axis);
        }
        return GetSingleControllerMapping(joystick, switch_to_sdl_button, switch_to_sdl_axis);
    }
    // Default mappings for when the controller doesn't have an associated gamepad
    // This fixes issues where SDL uses a backend which doesn't support gamepad remappings
    ButtonMapping mapping;
    for (const auto& pair : switch_to_sdl_button) {
        SDL_GamepadBinding binding{};
        binding.input_type = SDL_GAMEPAD_BINDTYPE_BUTTON;
        binding.input.button = pair.second;
        mapping.insert_or_assign(pair.first, BuildParamPackageForBinding(joystick->GetPort(), joystick->GetGUID(), binding));
    }
    for (const auto& pair : switch_to_sdl_axis) {
        SDL_GamepadBinding binding{};
        binding.input_type = SDL_GAMEPAD_BINDTYPE_AXIS;
        binding.input.axis.axis = pair.second;
        mapping.insert_or_assign(pair.first, BuildParamPackageForBinding(joystick->GetPort(), joystick->GetGUID(), binding));
    }
    return mapping;
}

ButtonBindings SDLDriver::GetDefaultButtonBinding(
    const std::shared_ptr<SDLJoystick>& joystick) const {
    // Default SL/SR mapping for other controllers
    auto sll_button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    auto srl_button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    auto slr_button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    auto srr_button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;

    if (joystick->IsJoyconLeft()) {
        sll_button = SDL_GAMEPAD_BUTTON_LEFT_PADDLE1;
        srl_button = SDL_GAMEPAD_BUTTON_LEFT_PADDLE2;
    }
    if (joystick->IsJoyconRight()) {
        slr_button = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2;
        srr_button = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1;
    }

    return {
        std::pair{Settings::NativeButton::A, SDL_GAMEPAD_BUTTON_EAST},
        {Settings::NativeButton::B, SDL_GAMEPAD_BUTTON_SOUTH},
        {Settings::NativeButton::X, SDL_GAMEPAD_BUTTON_NORTH},
        {Settings::NativeButton::Y, SDL_GAMEPAD_BUTTON_WEST},
        {Settings::NativeButton::LStick, SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {Settings::NativeButton::RStick, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {Settings::NativeButton::L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {Settings::NativeButton::R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {Settings::NativeButton::Plus, SDL_GAMEPAD_BUTTON_START},
        {Settings::NativeButton::Minus, SDL_GAMEPAD_BUTTON_BACK},
        {Settings::NativeButton::DLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {Settings::NativeButton::DUp, SDL_GAMEPAD_BUTTON_DPAD_UP},
        {Settings::NativeButton::DRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {Settings::NativeButton::DDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {Settings::NativeButton::SLLeft, sll_button},
        {Settings::NativeButton::SRLeft, srl_button},
        {Settings::NativeButton::SLRight, slr_button},
        {Settings::NativeButton::SRRight, srr_button},
        {Settings::NativeButton::Home, SDL_GAMEPAD_BUTTON_GUIDE},
        {Settings::NativeButton::Screenshot, SDL_GAMEPAD_BUTTON_MISC1},
    };
}

ButtonMapping SDLDriver::GetSingleControllerMapping(
    const std::shared_ptr<SDLJoystick>& joystick, const ButtonBindings& switch_to_sdl_button,
    const ZButtonBindings& switch_to_sdl_axis) const {
    ButtonMapping mapping;
    mapping.reserve(switch_to_sdl_button.size() + switch_to_sdl_axis.size());
    auto* controller = joystick->GetSDLGameController();
    const auto bindings = GetBindings(controller);

    for (const auto& [switch_button, sdl_button] : switch_to_sdl_button) {
        const auto binding = GetBindingForButton(bindings, sdl_button);
        mapping.insert_or_assign(
            switch_button,
            BuildParamPackageForBinding(joystick->GetPort(), joystick->GetGUID(), binding));
    }
    for (const auto& [switch_button, sdl_axis] : switch_to_sdl_axis) {
        const auto binding = GetBindingForAxis(bindings, sdl_axis);
        mapping.insert_or_assign(
            switch_button,
            BuildParamPackageForBinding(joystick->GetPort(), joystick->GetGUID(), binding));
    }

    return mapping;
}

ButtonMapping SDLDriver::GetDualControllerMapping(const std::shared_ptr<SDLJoystick>& joystick,
                                                  const std::shared_ptr<SDLJoystick>& joystick2,
                                                  const ButtonBindings& switch_to_sdl_button,
                                                  const ZButtonBindings& switch_to_sdl_axis) const {
    ButtonMapping mapping;
    mapping.reserve(switch_to_sdl_button.size() + switch_to_sdl_axis.size());
    auto* controller = joystick->GetSDLGameController();
    auto* controller2 = joystick2->GetSDLGameController();
    const auto bindings = GetBindings(controller);
    const auto bindings2 = GetBindings(controller2);

    for (const auto& [switch_button, sdl_button] : switch_to_sdl_button) {
        if (IsButtonOnLeftSide(switch_button)) {
            const auto binding = GetBindingForButton(bindings2, sdl_button);
            mapping.insert_or_assign(
                switch_button,
                BuildParamPackageForBinding(joystick2->GetPort(), joystick2->GetGUID(), binding));
            continue;
        }
        const auto binding = GetBindingForButton(bindings, sdl_button);
        mapping.insert_or_assign(
            switch_button,
            BuildParamPackageForBinding(joystick->GetPort(), joystick->GetGUID(), binding));
    }
    for (const auto& [switch_button, sdl_axis] : switch_to_sdl_axis) {
        if (IsButtonOnLeftSide(switch_button)) {
            const auto binding = GetBindingForAxis(bindings2, sdl_axis);
            mapping.insert_or_assign(
                switch_button,
                BuildParamPackageForBinding(joystick2->GetPort(), joystick2->GetGUID(), binding));
            continue;
        }
        const auto binding = GetBindingForAxis(bindings, sdl_axis);
        mapping.insert_or_assign(
            switch_button,
            BuildParamPackageForBinding(joystick->GetPort(), joystick->GetGUID(), binding));
    }

    return mapping;
}

bool SDLDriver::IsButtonOnLeftSide(Settings::NativeButton::Values button) const {
    switch (button) {
    case Settings::NativeButton::DDown:
    case Settings::NativeButton::DLeft:
    case Settings::NativeButton::DRight:
    case Settings::NativeButton::DUp:
    case Settings::NativeButton::L:
    case Settings::NativeButton::LStick:
    case Settings::NativeButton::Minus:
    case Settings::NativeButton::Screenshot:
    case Settings::NativeButton::ZL:
        return true;
    default:
        return false;
    }
}

AnalogMapping SDLDriver::GetAnalogMappingForDevice(const Common::ParamPackage& params) {
    if (!params.Has("guid") || !params.Has("port")) {
        return {};
    }
    const auto joystick = GetSDLJoystickByGUID(params.Get("guid", ""), params.Get("port", 0));
    const auto joystick2 = GetSDLJoystickByGUID(params.Get("guid2", ""), params.Get("port", 0));
    auto* controller = joystick->GetSDLGameController();
    if (controller == nullptr) {
        return {};
    }

    AnalogMapping mapping = {};
    const auto bindings = GetBindings(controller);
    const auto binding_left_x = GetBindingForAxis(bindings, SDL_GAMEPAD_AXIS_LEFTX);
    const auto binding_left_y = GetBindingForAxis(bindings, SDL_GAMEPAD_AXIS_LEFTY);
    if (params.Has("guid2")) {
        const auto identifier = joystick2->GetPadIdentifier();
        PreSetController(identifier);
        PreSetAxis(identifier, binding_left_x.input.axis.axis);
        PreSetAxis(identifier, binding_left_y.input.axis.axis);
        const auto left_offset_x = -GetAxis(identifier, binding_left_x.input.axis.axis);
        const auto left_offset_y = GetAxis(identifier, binding_left_y.input.axis.axis);
        mapping.insert_or_assign(Settings::NativeAnalog::LStick,
                     BuildParamPackageForAnalog(identifier, binding_left_x.input.axis.axis,
                                    binding_left_y.input.axis.axis,
                                                            left_offset_x, left_offset_y));
    } else {
        const auto identifier = joystick->GetPadIdentifier();
        PreSetController(identifier);
        PreSetAxis(identifier, binding_left_x.input.axis.axis);
        PreSetAxis(identifier, binding_left_y.input.axis.axis);
        const auto left_offset_x = -GetAxis(identifier, binding_left_x.input.axis.axis);
        const auto left_offset_y = GetAxis(identifier, binding_left_y.input.axis.axis);
        mapping.insert_or_assign(Settings::NativeAnalog::LStick,
                     BuildParamPackageForAnalog(identifier, binding_left_x.input.axis.axis,
                                    binding_left_y.input.axis.axis,
                                                            left_offset_x, left_offset_y));
    }
    const auto binding_right_x = GetBindingForAxis(bindings, SDL_GAMEPAD_AXIS_RIGHTX);
    const auto binding_right_y = GetBindingForAxis(bindings, SDL_GAMEPAD_AXIS_RIGHTY);
    const auto identifier = joystick->GetPadIdentifier();
    PreSetController(identifier);
    PreSetAxis(identifier, binding_right_x.input.axis.axis);
    PreSetAxis(identifier, binding_right_y.input.axis.axis);
    const auto right_offset_x = -GetAxis(identifier, binding_right_x.input.axis.axis);
    const auto right_offset_y = GetAxis(identifier, binding_right_y.input.axis.axis);
    mapping.insert_or_assign(Settings::NativeAnalog::RStick,
                             BuildParamPackageForAnalog(identifier, binding_right_x.input.axis.axis,
                                                        binding_right_y.input.axis.axis, right_offset_x,
                                                        right_offset_y));
    return mapping;
}

MotionMapping SDLDriver::GetMotionMappingForDevice(const Common::ParamPackage& params) {
    if (!params.Has("guid") || !params.Has("port")) {
        return {};
    }
    const auto joystick = GetSDLJoystickByGUID(params.Get("guid", ""), params.Get("port", 0));
    const auto joystick2 = GetSDLJoystickByGUID(params.Get("guid2", ""), params.Get("port", 0));
    auto* controller = joystick->GetSDLGameController();
    if (controller == nullptr) {
        return {};
    }

    MotionMapping mapping = {};
    joystick->EnableMotion();

    if (joystick->HasMotion()) {
        mapping.insert_or_assign(Settings::NativeMotion::MotionRight,
                                 BuildMotionParam(joystick->GetPort(), joystick->GetGUID()));
    }
    if (params.Has("guid2")) {
        joystick2->EnableMotion();
        if (joystick2->HasMotion()) {
            mapping.insert_or_assign(Settings::NativeMotion::MotionLeft,
                                     BuildMotionParam(joystick2->GetPort(), joystick2->GetGUID()));
        }
    } else {
        if (joystick->HasMotion()) {
            mapping.insert_or_assign(Settings::NativeMotion::MotionLeft,
                                     BuildMotionParam(joystick->GetPort(), joystick->GetGUID()));
        }
    }

    return mapping;
}

Common::Input::ButtonNames SDLDriver::GetUIName(const Common::ParamPackage& params) const {
    if (params.Has("button")) {
        // TODO(German77): Find how to substitute the values for real button names
        return Common::Input::ButtonNames::Value;
    }
    if (params.Has("hat")) {
        return Common::Input::ButtonNames::Value;
    }
    if (params.Has("axis")) {
        return Common::Input::ButtonNames::Value;
    }
    if (params.Has("axis_x") && params.Has("axis_y") && params.Has("axis_z")) {
        return Common::Input::ButtonNames::Value;
    }
    if (params.Has("motion")) {
        return Common::Input::ButtonNames::Engine;
    }

    return Common::Input::ButtonNames::Invalid;
}

std::string SDLDriver::GetHatButtonName(u8 direction_value) const {
    switch (direction_value) {
    case SDL_HAT_UP:
        return "up";
    case SDL_HAT_DOWN:
        return "down";
    case SDL_HAT_LEFT:
        return "left";
    case SDL_HAT_RIGHT:
        return "right";
    default:
        return {};
    }
}

u8 SDLDriver::GetHatButtonId(const std::string& direction_name) const {
    Uint8 direction;
    if (direction_name == "up") {
        direction = SDL_HAT_UP;
    } else if (direction_name == "down") {
        direction = SDL_HAT_DOWN;
    } else if (direction_name == "left") {
        direction = SDL_HAT_LEFT;
    } else if (direction_name == "right") {
        direction = SDL_HAT_RIGHT;
    } else {
        direction = 0;
    }
    return direction;
}

bool SDLDriver::IsStickInverted(const Common::ParamPackage& params) {
    if (!params.Has("guid") || !params.Has("port")) {
        return false;
    }
    const auto joystick = GetSDLJoystickByGUID(params.Get("guid", ""), params.Get("port", 0));
    if (joystick == nullptr) {
        return false;
    }
    auto* controller = joystick->GetSDLGameController();
    if (controller == nullptr) {
        return false;
    }

    const auto& axis_x = params.Get("axis_x", 0);
    const auto& axis_y = params.Get("axis_y", 0);
    const auto bindings = GetBindings(controller);
    const auto binding_left_x = GetBindingForAxis(bindings, SDL_GAMEPAD_AXIS_LEFTX);
    const auto binding_right_x = GetBindingForAxis(bindings, SDL_GAMEPAD_AXIS_RIGHTX);
    const auto binding_left_y = GetBindingForAxis(bindings, SDL_GAMEPAD_AXIS_LEFTY);
    const auto binding_right_y = GetBindingForAxis(bindings, SDL_GAMEPAD_AXIS_RIGHTY);

    if (axis_x != binding_left_y.input.axis.axis && axis_x != binding_right_y.input.axis.axis) {
        return false;
    }
    if (axis_y != binding_left_x.input.axis.axis && axis_y != binding_right_x.input.axis.axis) {
        return false;
    }
    return true;
}

} // namespace InputCommon
