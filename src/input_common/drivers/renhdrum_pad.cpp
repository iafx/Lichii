// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_common/drivers/renhdrum_pad.h"

#ifdef ENABLE_RENHDRUM

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <renhdrum.h>

#include "common/dynamic_library.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "common/uuid.h"

#ifdef RENHDRUM_SDL_HDRUMBLE
// Added by .patch/sdl3-hdrumble/0002-hdrumble-idle-write.patch (SDL3 is linked statically). Each
// returns 1 when the mode is active, 0 when SDL refused it, -1 for a pad that is not a HIDAPI
// Switch pad.
extern "C" {
int SDLCALL SDL_HDRumbleSetNoQueue(SDL_Joystick* joystick, int enable, int gap_ms, int pause_ms);
int SDLCALL SDL_HDRumbleSetNoQueueBluetooth(SDL_Joystick* joystick, int enable, int gap_ms);
int SDLCALL SDL_HDRumbleSetBluetoothAfterInput(SDL_Joystick* joystick, int enable);
}
#endif

namespace InputCommon {

namespace {

// The RenHDrum C ABI, resolved from the shared library at runtime: nothing to link, and a build
// without the library just keeps the standard rumble.
struct RenHDrumApi {
    decltype(&renhdrum_engine_create) engine_create{};
    decltype(&renhdrum_engine_destroy) engine_destroy{};
    decltype(&renhdrum_engine_apply_settings) engine_apply_settings{};
    decltype(&renhdrum_engine_push_half_sample) engine_push_half_sample{};
    decltype(&renhdrum_engine_tick) engine_tick{};
    decltype(&renhdrum_settings_resolve) settings_resolve{};
    decltype(&renhdrum_dualsense_dsp_create) dsp_create{};
    decltype(&renhdrum_dualsense_dsp_destroy) dsp_destroy{};
    decltype(&renhdrum_dualsense_dsp_set_half_target) dsp_set_half_target{};
    decltype(&renhdrum_dualsense_dsp_fill_f32) dsp_fill_f32{};
    // Optional (RenHDrum 0.2.1+): DualSense haptics over Bluetooth
    decltype(&renhdrum_dualsense_dsp_build_bt_report) dsp_build_bt_report{};
    // Optional (RenHDrum 0.2.0+): Xbox-style controllers
    decltype(&renhdrum_xbox_rumble_create) xbox_create{};
    decltype(&renhdrum_xbox_rumble_destroy) xbox_destroy{};
    decltype(&renhdrum_xbox_rumble_push_half_sample) xbox_push_half_sample{};
    decltype(&renhdrum_xbox_rumble_tick) xbox_tick{};
    decltype(&renhdrum_xbox_rumble_resend) xbox_resend{};
    // Optional (RenHDrum 0.3.0+): Switch 2 rumble Mode. An older library keeps Switch 1 behaviour.
    decltype(&renhdrum_set_nx2_mode) set_nx2_mode{};
    // Optional (RenHDrum after 0.3.1): Steam Controller (2026) grips. The DSP handle is a
    // RenHdcDualSenseDsp, so set_half_target and dsp_destroy work on it too.
    decltype(&renhdrum_valve_grip_dsp_create) valve_create{};
    decltype(&renhdrum_valve_grip_build_report) valve_build_report{};
    // Optional (RenHDrum 0.4.0+): runtime library version
    decltype(&renhdrum_version) version{};
    // Optional (plan 118): DualSense speaker channel and the vibration-reactive trigger
    decltype(&renhdrum_dualsense_dsp_set_speaker) dsp_set_speaker{};
    decltype(&renhdrum_tri_trigger_create) tri_create{};
    decltype(&renhdrum_tri_trigger_destroy) tri_destroy{};
    decltype(&renhdrum_tri_trigger_update) tri_update{};
    bool HasTriTrigger() const {
        return tri_create && tri_destroy && tri_update;
    }
    bool HasValve() const {
        return valve_create && valve_build_report;
    }
    bool HasXbox() const {
        return xbox_create && xbox_destroy && xbox_push_half_sample && xbox_tick && xbox_resend;
    }

    Common::DynamicLibrary library;
    bool loaded{};
};

const RenHDrumApi& Api() {
    static const RenHDrumApi api = [] {
        RenHDrumApi a;
#if defined(_WIN32)
        constexpr const char* name = "renhdrum.dll";
#elif defined(__APPLE__)
        constexpr const char* name = "librenhdrum.dylib";
#else
        constexpr const char* name = "librenhdrum.so";
#endif
        a.library = Common::DynamicLibrary(name);
        if (!a.library.IsOpen()) {
            LOG_INFO(Input, "RenHDrum: {} not found, using the standard rumble", name);
            return a;
        }
        a.loaded = a.library.GetSymbol("renhdrum_engine_create", &a.engine_create) &&
                   a.library.GetSymbol("renhdrum_engine_destroy", &a.engine_destroy) &&
                   a.library.GetSymbol("renhdrum_engine_apply_settings", &a.engine_apply_settings) &&
                   a.library.GetSymbol("renhdrum_engine_push_half_sample",
                                       &a.engine_push_half_sample) &&
                   a.library.GetSymbol("renhdrum_engine_tick", &a.engine_tick) &&
                   a.library.GetSymbol("renhdrum_settings_resolve", &a.settings_resolve) &&
                   a.library.GetSymbol("renhdrum_dualsense_dsp_create", &a.dsp_create) &&
                   a.library.GetSymbol("renhdrum_dualsense_dsp_destroy", &a.dsp_destroy) &&
                   a.library.GetSymbol("renhdrum_dualsense_dsp_set_half_target",
                                       &a.dsp_set_half_target) &&
                   a.library.GetSymbol("renhdrum_dualsense_dsp_fill_f32", &a.dsp_fill_f32);
        if (a.loaded) {
            // Optional: a missing symbol leaves it null
            (void)a.library.GetSymbol("renhdrum_dualsense_dsp_build_bt_report",
                                      &a.dsp_build_bt_report);
            (void)a.library.GetSymbol("renhdrum_xbox_rumble_create", &a.xbox_create);
            (void)a.library.GetSymbol("renhdrum_xbox_rumble_destroy", &a.xbox_destroy);
            (void)a.library.GetSymbol("renhdrum_xbox_rumble_push_half_sample",
                                      &a.xbox_push_half_sample);
            (void)a.library.GetSymbol("renhdrum_xbox_rumble_tick", &a.xbox_tick);
            (void)a.library.GetSymbol("renhdrum_xbox_rumble_resend", &a.xbox_resend);
            (void)a.library.GetSymbol("renhdrum_set_nx2_mode", &a.set_nx2_mode);
            (void)a.library.GetSymbol("renhdrum_valve_grip_dsp_create", &a.valve_create);
            (void)a.library.GetSymbol("renhdrum_valve_grip_build_report",
                                      &a.valve_build_report);
            (void)a.library.GetSymbol("renhdrum_version", &a.version);
            (void)a.library.GetSymbol("renhdrum_dualsense_dsp_set_speaker", &a.dsp_set_speaker);
            (void)a.library.GetSymbol("renhdrum_tri_trigger_create", &a.tri_create);
            (void)a.library.GetSymbol("renhdrum_tri_trigger_destroy", &a.tri_destroy);
            (void)a.library.GetSymbol("renhdrum_tri_trigger_update", &a.tri_update);
            LOG_INFO(Input, "RenHDrum: loaded {}", name);
        } else {
            LOG_WARNING(Input, "RenHDrum: {} is missing functions, using the standard rumble", name);
        }
        return a;
    }();
    return api;
}

/// SDL audio for DualSense USB haptics: initialized once and never quit while the emulator runs.
/// Quitting it on unplug ran SDL's queued "device lost" work on freed state (RenHDrum#4, fix by
/// iafx). Every pad shares this one reference.
bool EnsureAudioForHaptics() {
    static std::mutex audio_mutex;
    static bool audio_ready = false;
    std::scoped_lock lock{audio_mutex};
    if (audio_ready) {
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return false;
    }
    audio_ready = true;
    return true;
}

/// Hands the guest's Switch 2 Mode (set by HID commands 213 / 214) to RenHDrum when it changes.
void ApplyNx2Mode(const RenHDrumApi& api) {
    static std::atomic<int> applied{0};
    const int mode = Settings::values.nx2_active_mode.load(std::memory_order_relaxed);
    if (applied.exchange(mode, std::memory_order_relaxed) != mode) {
        LOG_INFO(Input, "RenHDrum: Switch 2 rumble mode {}", mode);
        if (api.set_nx2_mode) {
            api.set_nx2_mode(mode);
        }
    }
}

constexpr Uint16 NintendoVendorId = 0x057E;
constexpr Uint16 SonyVendorId = 0x054C;
constexpr Uint16 MicrosoftVendorId = 0x045E;
constexpr Uint16 ValveVendorId = 0x28DE;
// Steam Controller (2026): 1302 wired, 1303 Bluetooth (Windows only), 1304 dongle
constexpr Uint8 SteamReportPcmMode = 0x86;
constexpr int SteamGripMaxFailsInRow = 100;
constexpr Uint64 SteamGripTailNs = 100'000'000ULL;
// Grip audio is switched off after this long with nothing to play and on again (with the 200 ms
// settle) at the next vibration: measured lossless after 30 s idle. Once a controller stayed
// silent for good (start-up vibration and 0x80 rumble too, even after the battery was taken out)
// after a long session with the grip audio left on; 0x86 "off" brought it back (iafx, RenHDrum#5).
constexpr Uint64 SteamGripIdleOffNs = 3'000'000'000ULL;
// While it plays, 0x86 "on" again every 3 s and after a stall: the audio can stop with nothing
// failing (controller power-cycled behind the dongle, PC suspend); resending is harmless
constexpr Uint64 SteamGripModeResendNs = 3'000'000'000ULL;
constexpr std::array<Uint8, 10> NeutralPacket{0x10, 0, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
constexpr int DualSenseAudioMaxAttempts = 10;
constexpr int HdRumbleMaxInitialSendFails = 8;
// SDL no-queue gap while the 5 ms envelope runs (Ryuhdr AmFmNoQueueGapMs)
[[maybe_unused]] constexpr int NoQueueEnvelopeGapMs = 10;

/// DualSense "trio" tier 1 over USB (plan 118): a speaker channel and a trigger that kicks on
/// strong hits. Off unless `dualsense_bo3.txt` sits next to the executable: line 1 = speaker
/// percent (0..100, 60 when missing or invalid), line 2 = `1` turns the trigger kick on.
struct Bo3Config {
    bool present{};
    int speaker_percent{};
    bool trigger{};
};

Bo3Config ReadBo3Config() {
    Bo3Config config;
    const char* base = SDL_GetBasePath();
    if (!base) {
        return config;
    }
    std::ifstream file{std::string{base} + "dualsense_bo3.txt"};
    if (!file.is_open()) {
        return config;
    }
    config.present = true;
    config.speaker_percent = 60;
    std::string line;
    if (std::getline(file, line)) {
        try {
            config.speaker_percent = std::clamp(std::stoi(line), 0, 100);
        } catch (const std::exception&) {
            config.speaker_percent = 60;
        }
    }
    if (std::getline(file, line)) {
        config.trigger = line.find('1') != std::string::npos;
    }
    return config;
}

constexpr std::size_t Ds5EffectsSize = 47;

/// Opens the speaker (route "speaker only", volume 100), as proven by ds_loa_thu.
std::array<Uint8, Ds5EffectsSize> Bo3SpeakerOnBlock() {
    std::array<Uint8, Ds5EffectsSize> block{};
    block[0] = 0xA0;
    block[1] = 0x80;
    block[5] = 100;
    block[7] = 0x30;
    block[37] = 0x02;
    return block;
}

std::array<Uint8, Ds5EffectsSize> Bo3SpeakerOffBlock() {
    std::array<Uint8, Ds5EffectsSize> block{};
    block[0] = 0x80;
    block[7] = 0x00;
    return block;
}

/// Right trigger effect sits at [10..21], left at [21..32]; only that side's enable bit is set.
std::array<Uint8, Ds5EffectsSize> Bo3TriggerBlock(bool is_right,
                                                  const std::array<Uint8, 11>& effect) {
    std::array<Uint8, Ds5EffectsSize> block{};
    block[0] = is_right ? 0x04 : 0x08;
    std::copy(effect.begin(), effect.end(), block.begin() + (is_right ? 10 : 21));
    return block;
}

constexpr std::array<Uint8, 11> Bo3TriggerOff{0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

bool IsSwitchHdController(Uint16 vid, Uint16 pid) {
    return vid == NintendoVendorId && (pid == 0x2009 || pid == 0x2006 || pid == 0x2007);
}

/// Xbox One / Series and Xbox 360 style pads, Microsoft's or not (GameSir, 8BitDo, PowerA...).
/// RenHDrum's converter was tuned on an Xbox One S; others get the same four-motor mix, and
/// a pad whose path has no trigger motors (XInput) keeps only the grips.
bool IsXboxStyle(SDL_Joystick* joystick, SDL_Gamepad* gamepad) {
    if (gamepad) {
        const SDL_GamepadType type = SDL_GetGamepadType(gamepad);
        if (type == SDL_GAMEPAD_TYPE_XBOXONE || type == SDL_GAMEPAD_TYPE_XBOX360) {
            return true;
        }
    }
#ifdef _WIN32
    // With raw input on, SDL types a third-party XInput pad by its vendor ID and may not call it
    // an Xbox controller. Windows marks every XInput-capable HID device with "IG_" in its path.
    const char* path = SDL_GetJoystickPath(joystick);
    if (path && (std::strstr(path, "IG_") != nullptr || std::strstr(path, "ig_") != nullptr)) {
        return true;
    }
#else
    (void)joystick;
#endif
    return false;
}

bool IsSteamController(Uint16 vid, Uint16 pid) {
    return vid == ValveVendorId && (pid == 0x1302 || pid == 0x1303 || pid == 0x1304);
}

bool IsDualSense(Uint16 vid, Uint16 pid) {
    return vid == SonyVendorId && (pid == 0x0CE6 || pid == 0x0DF2);
}

struct UserSettings {
    bool hd_enabled{true};
    int usb_interval_ms{15};
    bool envelope_5ms{true};
    bool frequency_in_packet{true};
    bool crisp_notes{true};
    int bluetooth_level{4};
    bool bt_after_input{true};
    bool bt_fast{true};
    bool bt_crisp_notes{true};
    bool switch_enabled{true};
    bool steam_enabled{true};
    bool dualsense_enabled{true};
    int dualsense_volume{100};
    bool dualsense_crisp_notes{true};
    bool xbox_enabled{true};
    int xbox_strength{100};
    bool xbox_direct{false};
    int xbox_detail_mix{50};
    int xbox_hit_accents{50};
    int xbox_trigger_mix{20};
    int gain{100};
    int low_boost{0};
    bool operator==(const UserSettings&) const = default;
};

UserSettings ReadUserSettings() {
    const auto& s = Settings::values;
    // The HD Rumble dialog only shows the master switch. Unless hd_rumble_advanced is set by hand
    // in qt-config.ini, every tuning value is the built-in default, whatever the config or the
    // per-game settings hold (an old config may carry values the dialog no longer shows).
    const bool advanced = s.hd_rumble_advanced.GetValue();
    const auto pick = [advanced](const auto& setting) {
        return advanced ? setting.GetValue() : setting.GetDefault();
    };
    return {
        .hd_enabled = s.hd_rumble_enabled.GetValue(),
        .usb_interval_ms = std::clamp(pick(s.hd_rumble_usb_interval_ms), 5, 100),
        .envelope_5ms = pick(s.hd_rumble_envelope_5ms),
        .frequency_in_packet = pick(s.hd_rumble_frequency_in_packet),
        .crisp_notes = pick(s.hd_rumble_crisp_notes),
        .bluetooth_level = std::clamp(pick(s.hd_rumble_bluetooth_level), 1, 4),
        .bt_after_input = pick(s.hd_rumble_bt_after_input),
        .bt_fast = s.hd_rumble_bt_fast.GetValue(),
        .bt_crisp_notes = pick(s.hd_rumble_bt_crisp_notes),
        .switch_enabled = s.switch_hd_rumble_enabled.GetValue(),
        .steam_enabled = s.steam_hd_rumble_enabled.GetValue(),
        .dualsense_enabled = s.dualsense_haptics_enabled.GetValue(),
        .dualsense_volume = std::clamp(pick(s.dualsense_haptics_volume), 0, 100),
        .dualsense_crisp_notes = pick(s.dualsense_crisp_notes),
        .xbox_enabled = s.xbox_rumble_enabled.GetValue(),
        .xbox_strength = std::clamp(pick(s.xbox_motor_strength), 0, 1000),
        .xbox_direct = pick(s.xbox_rumble_mode) == 1,
        .xbox_detail_mix = std::clamp(pick(s.xbox_detail_mix), 0, 100),
        .xbox_hit_accents = std::clamp(pick(s.xbox_hit_accents), 0, 100),
        .xbox_trigger_mix = std::clamp(pick(s.xbox_trigger_mix), 0, 100),
        .gain = std::clamp(pick(s.hd_rumble_gain), 25, 1000),
        .low_boost = std::clamp(pick(s.hd_rumble_low_boost), 0, 100),
    };
}
// Gain, then a power curve that lifts weak vibrations more than strong ones (exponent 1.0 at
// boost 0 down to 0.4 at boost 100). Below a small knee the curve turns quadratic, so the
// near-silent values games send between hits do not become a constant buzz.
float ShapeAmplitude(float amplitude, const UserSettings& u) {
    const float x = (std::min)(amplitude * static_cast<float>(u.gain) / 100.0f, 1.0f);
    if (x <= 0.0f || u.low_boost <= 0) {
        return x;
    }
    const float exponent = 1.0f - 0.006f * static_cast<float>(u.low_boost);
    constexpr float knee = 0.02f;
    if (x < knee) {
        const float r = x / knee;
        return std::pow(knee, exponent) * r * r;
    }
    return std::pow(x, exponent);
}

// Controllers RenHDrum drives, per kind, for the settings dialog
std::array<std::atomic<int>, 4> connected_count{};
std::atomic<int> last_connected_kind{-1};
// Connected Xbox-style controllers that expose trigger motors
std::atomic<int> xbox_trigger_pads{};
// Connected official (Microsoft) Xbox controllers whose trigger motors SDL does not expose
std::atomic<int> xbox_official_no_triggers{};
// DualSense USB speakers currently owned by a pad. Leaf lock: only touches the vector, never
// calls SDL and never takes `mutex` (`Release()` takes `mutex` then this lock).
std::mutex claimed_audio_mutex;
std::vector<SDL_AudioDeviceID> claimed_audio_devices;

// Connected controllers currently driven by RenHDrum. Leaf lock: never calls SDL or takes `mutex`.
std::mutex active_pads_mutex;
struct ActivePadEntry {
    const void* id;
    RenHDrumPadInfo info;
};
std::vector<ActivePadEntry> active_pads;

void RegisterActivePad(const void* id, RenHDrumPadInfo info) {
    std::scoped_lock lock{active_pads_mutex};
    active_pads.push_back({id, std::move(info)});
}

void UnregisterActivePad(const void* id) {
    std::scoped_lock lock{active_pads_mutex};
    std::erase_if(active_pads, [id](const ActivePadEntry& entry) { return entry.id == id; });
}

std::string PadGuid(SDL_Joystick* joystick) {
    const SDL_GUID guid = SDL_GetJoystickGUID(joystick);
    std::array<std::uint8_t, 16> data{};
    std::memcpy(data.data(), guid.data, sizeof(data));
    std::memset(data.data() + 2, 0, sizeof(std::uint16_t));
    return Common::UUID{data}.RawString();
}

std::string PadName(SDL_Joystick* joystick, SDL_Gamepad* gamepad, int kind, Uint16 vid,
                    Uint16 pid) {
    const char* name = gamepad ? SDL_GetGamepadName(gamepad) : nullptr;
    if (!name || !name[0]) {
        name = SDL_GetJoystickName(joystick);
    }
    if (name && name[0]) {
        return name;
    }
    switch (kind) {
    case 0:
        return pid == 0x2006 ? "Joy-Con (L)"
             : pid == 0x2007 ? "Joy-Con (R)"
                             : "Switch Pro Controller";
    case 1:
        return pid == 0x0DF2 ? "DualSense Edge" : "DualSense";
    case 2:
        return "Xbox Controller";
    case 3:
        return "Steam Controller (2026)";
    default:
        return fmt::format("{:04X}:{:04X}", vid, pid);
    }
}

} // namespace

// Lock rule: `mutex` is never held while calling an SDL function that takes the SDL joystick lock
// (SDL_SendJoystickEffect, SDL_HDRumbleSet*, SDL_GetJoystickConnectionState, SDL_Rumble*). SDL
// runs the joystick-removed event watcher under that lock, and Shutdown() takes `mutex` from
// there. The joystick pointer is copied out and used after unlocking: SDL3 validates joystick
// handles, so a call on one closed meanwhile only fails.
//
// `claimed_audio_mutex` is a leaf lock (ordered after `mutex`): under it only
// `claimed_audio_devices` is touched, with no SDL calls.
//
// A DualSense on Bluetooth has no audio endpoint. A thread of its own sends the haptics as HID
// output report 0x32 on a second handle to the pad. That thread never takes `mutex`; it holds
// `bt_dsp_mutex` while it reads the DSP, and the DSP is replaced only under that lock too.
struct RenHDrumPad::Impl {
    Uint16 vid{};
    Uint16 pid{};
    bool is_bluetooth{};
    int kind{-1};

    mutable std::mutex mutex;
    SDL_Joystick* joystick{};
    SDL_Gamepad* gamepad{};
    RenHdcEngine* engine{};
    std::atomic<bool> engine_ever_sent{};
    int engine_send_fails{};
    RenHdcDualSenseDsp* dsp{};
    SDL_AudioStream* audio_stream{};
    SDL_AudioDeviceID audio_device{};
    bool audio_opening{};
    int audio_attempts{};
    std::int64_t audio_next_us{};
    bool stop_sdl_rumble{}; // haptics just switched on: SDL rumble would keep audio haptics off
    UserSettings user{};

    // Plan 118 (DualSense over USB, only with dualsense_bo3.txt): speaker + trigger kick
    Bo3Config bo3{};
    bool bo3_speaker_on{};
    bool bo3_speaker_logged{};
    bool bo3_restore_speaker{}; // set by Release(): Shutdown sends the speaker-off block
    bool bo3_restore_trigger{}; // set by Release(): Shutdown sends Off to both triggers
    std::array<RenHdcTriTrigger*, 2> tri{};
    std::array<std::array<float, 4>, 2> tri_sample{};

    RenHdcXboxRumble* xbox{};
    bool xbox_triggers{};
    bool official_no_triggers{};
    // Direct mode: the latest shaped bands of each side, and the grip values last sent
    std::array<float, 2> direct_low{};
    std::array<float, 2> direct_high{};
    std::array<std::uint16_t, 2> direct_sent{};
    // Direct mode accents: the level to settle on once the kick ends
    std::array<std::uint16_t, 2> direct_target{};
    std::int64_t kick_until_us{};
    // Direct mode triggers: the fine vibration of each side, and what was last sent
    std::array<std::uint16_t, 2> trigger_target{};
    std::array<std::uint16_t, 2> trigger_sent{};

    // Steam Controller (2026) grips: own DSP so no DualSense branch (`dsp`) ever sees this pad.
    // It reuses bt_hid / bt_thread / bt_running / bt_dsp_mutex below (a pad is never both).
    RenHdcDualSenseDsp* steam_dsp{};
    std::uint8_t steam_format{};

    // Reused by the Steam Controller grip sender (see steam_dsp)
    std::mutex bt_dsp_mutex;
    SDL_hid_device* bt_hid{};
    std::thread bt_thread;
    std::atomic<bool> bt_running{};
    std::atomic<bool> bt_warned{};
    bool bt_unsupported{};
    bool bt_starting{};
    std::int64_t bt_next_us{};

    /// Engine settings from the user's choices (Ryuhdr 0.2.6's recommended mode by default).
    /// Caller holds the mutex.
    bool ResolveEngineSettings(RenHdcSettings& settings) const {
        RenHdcConfig config{};
        config.enable_hd_rumble = user.hd_enabled && user.switch_enabled;
        config.hd_rumble_usb_interval_ms = user.usb_interval_ms;
        config.hd_rumble_bluetooth_level = user.bluetooth_level;
        config.hd_rumble_usb_auto_slow = true;
        config.hd_rumble_experimental_usb_no_queue = true;
        config.hd_rumble_experimental_envelope_5ms = user.envelope_5ms;
        config.hd_rumble_experimental_frequency_in_packet = user.frequency_in_packet;
        config.hd_rumble_experimental_frequency_both_bands = user.frequency_in_packet;
        config.hd_rumble_experimental_crisp_notes = user.crisp_notes;
        config.hd_rumble_experimental_bt_after_input = user.bt_after_input;
        config.hd_rumble_experimental_bt_fast = user.bt_fast;
        config.hd_rumble_experimental_bt_crisp_notes = user.bt_crisp_notes;
        return Api().settings_resolve(&config, &settings) == RENHDRUM_OK;
    }

    /// Switches SDL's HD Rumble transport modes to match `settings`, then clears the modes SDL
    /// refused, so the engine never paces for writes SDL does not gate. Called without the mutex.
    void ApplyTransportModes(SDL_Joystick* js, RenHdcSettings& settings) const {
#ifdef RENHDRUM_SDL_HDRUMBLE
        SDL_HDRumbleSetNoQueue(js, 0, 0, 0);
        SDL_HDRumbleSetNoQueueBluetooth(js, 0, 0);
        SDL_HDRumbleSetBluetoothAfterInput(js, 0);
        if (is_bluetooth) {
            // B3 needs B1 on; SDL never runs both on one pad, B3 wins
            settings.bt_fast = settings.enabled && settings.bt_fast && settings.bt_after_input &&
                               SDL_HDRumbleSetNoQueueBluetooth(js, 1, NoQueueEnvelopeGapMs) == 1;
            settings.bt_after_input = settings.enabled && settings.bt_after_input &&
                                      !settings.bt_fast &&
                                      SDL_HDRumbleSetBluetoothAfterInput(js, 1) == 1;
            settings.usb_no_queue = false;
        } else {
            const int gap_ms =
                settings.envelope_5ms ? NoQueueEnvelopeGapMs : settings.usb_interval_ms;
            settings.usb_no_queue =
                settings.enabled && settings.usb_no_queue &&
                SDL_HDRumbleSetNoQueue(js, 1, gap_ms, settings.usb_pause_ms) == 1;
            settings.bt_after_input = false;
            settings.bt_fast = false;
        }
#else
        // Stock SDL queues every write; the engine paces by its clock.
        (void)js;
        settings.usb_no_queue = false;
        settings.bt_after_input = false;
        settings.bt_fast = false;
#endif
    }

    /// Resolves the settings, switches the SDL modes and hands the result to the engine.
    /// Called without the mutex.
    void ConfigureEngine() {
        RenHdcSettings settings{};
        SDL_Joystick* js = nullptr;
        {
            std::scoped_lock lock{mutex};
            if (!engine || !joystick || !ResolveEngineSettings(settings)) {
                return;
            }
            js = joystick;
        }
        ApplyTransportModes(js, settings);
        {
            std::scoped_lock lock{mutex};
            if (!engine) {
                return;
            }
            Api().engine_apply_settings(engine, &settings);
        }
        if (is_bluetooth) {
            const std::string mode =
                settings.bt_fast ? "Detailed (5 ms envelope while SDL is idle)"
                : settings.bluetooth_level == 4
                    ? "Compatible"
                    : "legacy level " + std::to_string(settings.bluetooth_level);
            LOG_INFO(Input, "RenHDrum HD Rumble Bluetooth: {}{}{}", mode,
                     !settings.bt_fast && settings.bt_after_input
                         ? ", written right after an input report"
                     : !settings.bt_fast ? ", clock based writes"
                                         : "",
                     settings.enabled && settings.bt_crisp_notes &&
                             (settings.bt_fast || settings.bluetooth_level != 4)
                         ? ", crisp notes"
                         : "");
        } else {
            const bool envelope = settings.usb_no_queue && settings.envelope_5ms;
            LOG_INFO(Input, "RenHDrum HD Rumble wired: {} ms, {}{}{}", settings.usb_interval_ms,
                     settings.usb_no_queue ? "no-queue writes" : "queued writes",
                     envelope ? ", 5 ms envelope" : "",
                     envelope && settings.crisp_notes ? ", crisp notes" : "");
        }
    }

    struct SettingsChange {
        bool reconfigure_engine{};
        bool stop_engine{};
    };

    /// Picks up changes made in the HD Rumble dialog. Caller holds the mutex; the returned SDL
    /// work is done after unlocking.
    SettingsChange RefreshUserSettings() {
        const UserSettings now = ReadUserSettings();
        if (now == user) {
            return {};
        }
        const UserSettings old = user;
        user = now;
        if (dsp && ((!old.dualsense_enabled && now.dualsense_enabled) ||
                    (!old.hd_enabled && now.hd_enabled))) {
            stop_sdl_rumble = true;
        }
        if (dsp && (old.dualsense_volume != now.dualsense_volume ||
                    old.dualsense_crisp_notes != now.dualsense_crisp_notes)) {
            // The audio callback runs with the stream locked, so it never sees a freed DSP. The
            // audio stream lock is not the joystick lock, and the callback never takes `mutex`.
            if (audio_stream) {
                SDL_LockAudioStream(audio_stream);
            }
            {
                std::scoped_lock bt_lock{bt_dsp_mutex};
                const auto& api = Api();
                api.dsp_destroy(dsp);
                dsp = api.dsp_create(now.dualsense_volume, now.dualsense_crisp_notes);
                if (dsp && bo3_speaker_on && api.dsp_set_speaker) {
                    api.dsp_set_speaker(dsp, bo3.speaker_percent);
                }
            }
            if (audio_stream) {
                SDL_UnlockAudioStream(audio_stream);
            }
        }
        return {
            .reconfigure_engine = engine != nullptr,
            .stop_engine = engine != nullptr && ((old.hd_enabled && !now.hd_enabled) ||
                                                 (old.switch_enabled && !now.switch_enabled)),
        };
    }

    /// The DualSense exposes its voice coils as a 4-channel USB audio device that Windows may
    /// list a moment after the HID device, hence the retries. Called without the mutex.
    void OpenDualSenseAudio() {
        SDL_Joystick* js = nullptr;
        SDL_Gamepad* gp = nullptr;
        bool last_quick_attempt = false;
        {
            std::scoped_lock lock{mutex};
            if (!dsp || !joystick || audio_stream || audio_opening) {
                return;
            }
            if (is_bluetooth) {
                // No audio endpoint on Bluetooth: Create tries the HID haptics once, Tick restarts it
                audio_attempts = DualSenseAudioMaxAttempts;
            } else {
                audio_opening = true;
                // Past the quick attempts Tick keeps trying every 5 s; the count stays capped
                if (audio_attempts < DualSenseAudioMaxAttempts &&
                    ++audio_attempts == DualSenseAudioMaxAttempts) {
                    last_quick_attempt = true;
                }
            }
            js = joystick;
            gp = gamepad;
        }
        if (is_bluetooth) {
            StartDualSenseBluetooth(js, gp);
            return;
        }
        SDL_AudioStream* stream = nullptr;
        const bool inited = EnsureAudioForHaptics();
        if (inited) {
            stream = OpenDualSenseAudioStream(js);
        } else {
            LOG_WARNING(Input, "RenHDrum: SDL audio init failed: {}", SDL_GetError());
        }
        bool opened = false;
        {
            std::scoped_lock lock{mutex};
            audio_opening = false;
            if (stream && joystick && dsp && !audio_stream) {
                audio_stream = stream;
                opened = true;
                // Start only once published: from here on, the DSP is replaced or freed only
                // under SDL_LockAudioStream or after SDL_DestroyAudioStream. The callback never
                // takes `mutex`.
                if (!SDL_ResumeAudioStreamDevice(stream)) {
                    // Not playing: unpublish so vibration falls back to standard rumble and
                    // the remaining retries can still open the device. The callback never ran.
                    LOG_WARNING(Input, "RenHDrum: DualSense audio did not start: {}",
                                SDL_GetError());
                    audio_stream = nullptr;
                    opened = false;
                }
            }
        }
        if (!opened) {
            // No device yet, or shut down while opening
            if (stream) {
                SDL_DestroyAudioStream(stream);
            }
            ReleaseAudioDevice();
            if (last_quick_attempt) {
                LOG_INFO(Input, "RenHDrum: DualSense speaker not found yet, standard rumble until "
                                "it appears");
            }
        }
        if (opened) {
            // Stop SDL's HID rumble once so the pad plays the audio haptics
            if (gp) {
                SDL_RumbleGamepad(gp, 0, 0, 0);
            } else {
                SDL_RumbleJoystick(js, 0, 0, 0);
            }
            OpenBo3Speaker(js);
        }
    }

    /// Plan 118: opens the pad speaker and gives the DSP its speaker gain. No-op without
    /// dualsense_bo3.txt. Called without the mutex.
    void OpenBo3Speaker(SDL_Joystick* js) {
        bool log = false;
        {
            std::scoped_lock lock{mutex};
            const auto& api = Api();
            if (!dsp || is_bluetooth || bo3.speaker_percent <= 0 || !api.dsp_set_speaker) {
                return;
            }
            api.dsp_set_speaker(dsp, bo3.speaker_percent);
            bo3_speaker_on = true;
            log = !std::exchange(bo3_speaker_logged, true);
        }
        const auto block = Bo3SpeakerOnBlock();
        SDL_SendJoystickEffect(js, block.data(), static_cast<int>(block.size()));
        if (log) {
            LOG_INFO(Input, "RenHDrum: DualSense speaker on ({}%)", bo3.speaker_percent);
        }
    }

    /// Plan 118: one step of the trigger kick for one side. `sample` is the newest raw vibration
    /// of that side, or nullptr on a periodic Tick (so the effect can fall back to Off). Called
    /// without the mutex.
    void Bo3TriggerStep(std::int64_t now_us, bool is_right,
                        const std::array<float, 4>* sample) {
        const std::size_t side = is_right ? 1 : 0;
        SDL_Joystick* js = nullptr;
        SDL_Gamepad* gp = nullptr;
        {
            std::scoped_lock lock{mutex};
            if (!tri[side] || !joystick || !gamepad) {
                return;
            }
            js = joystick;
            gp = gamepad;
            if (sample) {
                tri_sample[side] = *sample;
            }
        }
        const int axis = std::clamp<int>(
            SDL_GetGamepadAxis(gp, is_right ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
                                            : SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
            0, 32767);
        const auto depth = static_cast<Uint8>(axis * 255 / 32767);
        std::array<Uint8, 11> effect{};
        int changed = 0;
        {
            std::scoped_lock lock{mutex};
            if (!tri[side]) {
                return;
            }
            const auto& s = tri_sample[side];
            changed = Api().tri_update(tri[side], static_cast<std::uint32_t>(now_us / 1000),
                                       depth, s[0], s[1], s[2], s[3], sample != nullptr,
                                       effect.data());
        }
        if (changed == 1) {
            const auto block = Bo3TriggerBlock(is_right, effect);
            SDL_SendJoystickEffect(js, block.data(), static_cast<int>(block.size()));
        }
    }

    /// Opens a second HID handle on a Bluetooth DualSense and starts the report 0x32 sender.
    /// Called without the mutex. Without the 0.2.1 function or a HID path, the standard rumble
    /// stays.
    void StartDualSenseBluetooth(SDL_Joystick* js, SDL_Gamepad* gp) {
        if (!Api().dsp_build_bt_report) {
            LOG_INFO(Input, "RenHDrum: DualSense on Bluetooth needs RenHDrum 0.2.1 or newer, "
                            "using the standard rumble");
            std::scoped_lock lock{mutex};
            bt_unsupported = true;
            return;
        }
        const char* path = SDL_GetJoystickPath(js);
        if (!path || !*path) {
            LOG_INFO(Input, "RenHDrum: DualSense on Bluetooth has no HID path, using the "
                            "standard rumble");
            std::scoped_lock lock{mutex};
            bt_unsupported = true;
            return;
        }
        const std::string hid_path{path};
        SDL_hid_device* hid = SDL_hid_open_path(hid_path.c_str());
        if (!hid) {
            if (!bt_warned.exchange(true)) {
                LOG_WARNING(Input,
                            "RenHDrum: could not open the DualSense HID path: {}, using the "
                            "standard rumble",
                            SDL_GetError());
            }
            return;
        }
        {
            std::scoped_lock lock{mutex};
            if (!joystick || !dsp || bt_hid || bt_thread.joinable()) {
                SDL_hid_close(hid);
                return;
            }
            bt_hid = hid;
            bt_warned.store(false);
            bt_running.store(true);
            bt_thread = std::thread([this] { DualSenseBluetoothLoop(); });
        }
        // Stop SDL's HID rumble once so the pad plays the haptics
        if (gp) {
            SDL_RumbleGamepad(gp, 0, 0, 0);
        } else {
            SDL_RumbleJoystick(js, 0, 0, 0);
        }
        LOG_INFO(Input, "RenHDrum: DualSense haptics over Bluetooth (report 0x32)");
    }

    /// Joins a stopped Bluetooth sender thread, closes its HID handle and tries to start a new
    /// one. Called without the mutex after Tick sets `bt_starting`.
    void RestartDualSenseBluetooth() {
        SDL_Joystick* js = nullptr;
        SDL_Gamepad* gp = nullptr;
        std::thread old_thread;
        SDL_hid_device* old_hid = nullptr;
        {
            std::scoped_lock lock{mutex};
            js = joystick;
            gp = gamepad;
            old_thread = std::move(bt_thread);
            old_hid = std::exchange(bt_hid, nullptr);
        }
        if (old_thread.joinable()) {
            old_thread.join();
        }
        if (old_hid) {
            SDL_hid_close(old_hid);
        }
        if (js) {
            StartDualSenseBluetooth(js, gp);
        }
        {
            std::scoped_lock lock{mutex};
            bt_starting = false;
        }
    }

    /// Runs on `bt_thread`: one report every RENHDRUM_DS5_BT_PERIOD_NS while vibrating and for
    /// 2 s after, then quiet on the radio. Stops after about 0.5 s of failed writes, which hands
    /// the vibration back to the standard rumble.
    void DualSenseBluetoothLoop() {
        Common::SetCurrentThreadName("RenHDrumDS5BT");
        constexpr Uint64 KeepSendingNs = 2'000'000'000ULL;
        constexpr Uint64 ResyncNs = 100'000'000ULL;
        constexpr int MaxFailsInRow = 50;
        const auto& api = Api();
        std::array<std::uint8_t, RENHDRUM_DS5_BT_REPORT_SIZE> report{};
        Uint64 next = SDL_GetTicksNS();
        Uint64 last_signal = 0;
        int fails_in_row = 0;
        while (bt_running.load()) {
            next += RENHDRUM_DS5_BT_PERIOD_NS;
            int built = -1;
            {
                std::scoped_lock lock{bt_dsp_mutex};
                if (dsp) {
                    built = api.dsp_build_bt_report(dsp, RENHDRUM_DS5_BT_BUFFER_LENGTH_DEFAULT,
                                                    report.data());
                }
            }
            const Uint64 now = SDL_GetTicksNS();
            if (built == 1) {
                last_signal = now;
            }
            if (built >= 0 && last_signal != 0 && now - last_signal < KeepSendingNs) {
                if (SDL_hid_write(bt_hid, report.data(), report.size()) < 0) {
                    ++fails_in_row;
                } else {
                    fails_in_row = 0;
                }
            }
            if (fails_in_row >= MaxFailsInRow) {
                if (!bt_warned.exchange(true)) {
                    LOG_WARNING(Input,
                                "RenHDrum: DualSense Bluetooth writes failing ({}), using the "
                                "standard rumble",
                                SDL_GetError());
                }
                break;
            }
            if (next > now) {
                SDL_DelayPrecise(next - now);
            } else if (now - next > ResyncNs) {
                next = now; // stalled: resync instead of bursting
            }
        }
        bt_running.store(false);
    }

    /// 0x86 [id, op, channel, format] for both grip channels: op 2 on, op 1 off. An "off" first
    /// names the running format, then format 0.
    static void SendSteamGripMode(SDL_hid_device* hid, Uint8 op, Uint8 format) {
        for (const Uint8 channel : {Uint8{2}, Uint8{5}}) {
            const std::array<Uint8, 4> report{SteamReportPcmMode, op, channel, format};
            SDL_hid_write(hid, report.data(), report.size());
        }
    }

    /// Opens a second HID handle on a Steam Controller (2026), switches the grips to PCM and
    /// starts the report 0x88 sender. Called without the mutex. Every failure leaves the standard
    /// rumble (handle closed) or a running sender. Protocol by iafx (RenHDrum#5, iafx/Lichii).
    void StartSteamGrip(SDL_Joystick* js, Uint16 steam_pid) {
        const char* path = SDL_GetJoystickPath(js);
        if (!path || !*path) {
            LOG_INFO(Input, "RenHDrum: Steam Controller has no HID path, using the standard rumble");
            return;
        }
        const std::string hid_path{path};
        SDL_hid_device* hid = SDL_hid_open_path(hid_path.c_str());
        if (!hid) {
            LOG_WARNING(Input,
                        "RenHDrum: could not open the Steam Controller HID path: {}, using the "
                        "standard rumble",
                        SDL_GetError());
            return;
        }
        // Wired takes 8 kHz; the dongle and Bluetooth (Windows) 4 kHz
        const Uint8 format = steam_pid == 0x1302 ? RENHDRUM_VALVE_GRIP_FORMAT_MULAW_8K
                                                 : RENHDRUM_VALVE_GRIP_FORMAT_MULAW_4K;
        SendSteamGripMode(hid, 1, 0);
        SDL_Delay(10);
        SendSteamGripMode(hid, 2, format);
        SDL_Delay(200); // the controller needs a moment between off and on
        {
            std::scoped_lock lock{mutex};
            if (!joystick || !steam_dsp || bt_hid || bt_thread.joinable()) {
                SendSteamGripMode(hid, 1, format);
                SendSteamGripMode(hid, 1, 0);
                SDL_hid_close(hid);
                return;
            }
            steam_format = format;
            bt_hid = hid;
            bt_warned.store(false);
            bt_running.store(true);
            bt_thread = std::thread([this] { SteamGripLoop(); });
        }
        // The grips' rumble motors would fight the audio
        SDL_RumbleJoystick(js, 0, 0, 0);
        LOG_INFO(Input, "RenHDrum: Steam Controller grips play HD Rumble (experimental, format {})",
                 format);
    }

    /// Runs on `bt_thread`: one report per period while vibrating and 100 ms after the last
    /// vibration, then nothing on the wire so the controller's buffer drains. Stops after 100
    /// failed writes in a row, which hands the pad back to the standard rumble. Never takes
    /// `mutex`.
    // Note: continuous multi-minute vibration without a pause can accumulate ~1.7 ms/min of drift
    // (iafx, RenHDrum#5); drop a few packets during the quietest moments if reported.
    void SteamGripLoop() {
        Common::SetCurrentThreadName("RenHDrumSteamGrips");
        constexpr Uint64 ResyncNs = 100'000'000ULL;
        const auto& api = Api();
        const Uint8 format = steam_format;
        const Uint64 period_ns = format == RENHDRUM_VALVE_GRIP_FORMAT_MULAW_8K
                                     ? RENHDRUM_VALVE_GRIP_PERIOD_NS_8K
                                     : RENHDRUM_VALVE_GRIP_PERIOD_NS_4K;
        std::array<std::uint8_t, RENHDRUM_VALVE_GRIP_REPORT_SIZE> report{};
        Uint64 next = SDL_GetTicksNS();
        Uint64 last_signal = 0;
        Uint64 last_mode = next;
        Uint64 audio_on_since = next;
        bool audio_on = true; // StartSteamGrip switched it on
        int fails_in_row = 0;
        while (bt_running.load()) {
            next += period_ns;
            int built = -1;
            {
                std::scoped_lock lock{bt_dsp_mutex};
                if (steam_dsp) {
                    built = api.valve_build_report(steam_dsp, format, report.data());
                }
            }
            Uint64 now = SDL_GetTicksNS();
            if (built == 1) {
                last_signal = now;
                if (!audio_on) {
                    SendSteamGripMode(bt_hid, 1, 0);
                    SDL_Delay(10);
                    SendSteamGripMode(bt_hid, 2, format);
                    SDL_Delay(200);
                    audio_on = true;
                    now = SDL_GetTicksNS();
                    next = now + period_ns;
                    last_signal = last_mode = audio_on_since = now;
                }
            }
            const Uint64 quiet_since = last_signal != 0 ? last_signal : audio_on_since;
            if (audio_on && now - quiet_since >= SteamGripIdleOffNs) {
                SendSteamGripMode(bt_hid, 1, format);
                SendSteamGripMode(bt_hid, 1, 0);
                audio_on = false;
            }
            if (audio_on && now - last_mode >= SteamGripModeResendNs) {
                SendSteamGripMode(bt_hid, 2, format);
                last_mode = now;
            }
            if (audio_on && built >= 0 && last_signal != 0 && now - last_signal < SteamGripTailNs) {
                if (SDL_hid_write(bt_hid, report.data(), report.size()) < 0) {
                    ++fails_in_row;
                } else {
                    fails_in_row = 0;
                }
            }
            if (fails_in_row >= SteamGripMaxFailsInRow) {
                if (!bt_warned.exchange(true)) {
                    LOG_WARNING(Input,
                                "RenHDrum: Steam Controller grip writes failing ({}), using the "
                                "standard rumble",
                                SDL_GetError());
                }
                break;
            }
            if (next > now) {
                SDL_DelayPrecise(next - now);
            } else if (now - next > ResyncNs) {
                next = now; // stalled: resync instead of bursting
                if (audio_on) {
                    SendSteamGripMode(bt_hid, 2, format); // the audio may have stopped meanwhile
                    last_mode = now;
                }
            }
        }
        bt_running.store(false);
    }

    /// Releases this pad's claimed USB audio device so another DualSense can open it.
    void ReleaseAudioDevice() {
        std::scoped_lock lock{claimed_audio_mutex};
        if (audio_device != 0) {
            std::erase(claimed_audio_devices, audio_device);
            audio_device = 0;
        }
    }

    /// Finds the DualSense audio device and opens a stream on it, paused until it is published
    /// in `audio_stream`, or nullptr. Called without the mutex, with the SDL audio subsystem
    /// initialized.
    SDL_AudioStream* OpenDualSenseAudioStream(SDL_Joystick* js) {
        (void)js;
        int count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
        SDL_AudioDeviceID matched = 0;
        bool seen_dualsense = false;
        bool second_wired = false;
        for (int i = 0; devices && i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(devices[i]);
            SDL_AudioSpec spec{};
            const int channels =
                SDL_GetAudioDeviceFormat(devices[i], &spec, nullptr) ? spec.channels : 0;
            if (name && channels >= 4 &&
                (std::strstr(name, "DualSense") != nullptr ||
                 std::strstr(name, "Wireless Controller") != nullptr)) {
                seen_dualsense = true;
                std::scoped_lock lock{claimed_audio_mutex};
                if (std::find(claimed_audio_devices.begin(), claimed_audio_devices.end(),
                              devices[i]) != claimed_audio_devices.end()) {
                    continue;
                }
                claimed_audio_devices.push_back(devices[i]);
                matched = devices[i];
                audio_device = matched;
                second_wired = claimed_audio_devices.size() == 2;
                break;
            }
        }
        SDL_free(devices);
        if (matched == 0) {
            if (seen_dualsense) {
                LOG_DEBUG(Input, "RenHDrum: DualSense audio devices are all in use");
            } else {
                LOG_DEBUG(Input, "RenHDrum: no DualSense audio device yet");
            }
            return nullptr;
        }
        if (second_wired) {
            LOG_INFO(Input, "RenHDrum: two wired DualSense controllers, speakers are matched in "
                            "the order the system lists them");
        }

        const SDL_AudioSpec spec{
            .format = SDL_AUDIO_F32,
            .channels = 4,
            .freq = 48000,
        };
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(matched, &spec, &AudioCallback, this);
        if (!stream) {
            ReleaseAudioDevice();
            LOG_WARNING(Input, "RenHDrum: could not open the DualSense audio device: {}",
                        SDL_GetError());
            return nullptr;
        }
        LOG_INFO(Input, "RenHDrum: DualSense haptics on \"{}\"", SDL_GetAudioDeviceName(matched));
        return stream;
    }

    static void SDLCALL AudioCallback(void* userdata, SDL_AudioStream* stream,
                                      int additional_amount, int /*total_amount*/) {
        auto* const self = static_cast<Impl*>(userdata);
        if (additional_amount <= 0 || !self || !self->dsp) {
            return;
        }
        constexpr int frame_bytes = 4 * static_cast<int>(sizeof(float));
        constexpr int chunk_frames = 256;
        std::array<float, chunk_frames * 4> chunk{};
        int frames = additional_amount / frame_bytes;
        while (frames > 0) {
            const int count = (std::min)(frames, chunk_frames);
            if (Api().dsp_fill_f32(self->dsp, chunk.data(), static_cast<size_t>(count)) !=
                RENHDRUM_OK) {
                chunk.fill(0.0f); // never send garbage to the actuators
            }
            SDL_PutAudioStreamData(stream, chunk.data(), count * frame_bytes);
            frames -= count;
        }
    }

    /// Frees the engine, the DSP and the audio stream. Caller holds the mutex. Returns whether
    /// an engine was running, so the caller sends the neutral packet after unlocking.
    bool had_xbox{};

    bool Release() {
        const auto& api = Api();
        if (steam_dsp) {
            // Stop the grip sender, switch the grips back to rumble, then free its DSP. The
            // handle is closed by the Bluetooth cleanup below (shared member).
            bt_running.store(false);
            if (bt_thread.joinable()) {
                bt_thread.join();
            }
            if (bt_hid) {
                SendSteamGripMode(bt_hid, 1, steam_format);
                SendSteamGripMode(bt_hid, 1, 0);
            }
            api.dsp_destroy(steam_dsp);
            steam_dsp = nullptr;
        }
        // The sender never takes `mutex`, so joining here cannot deadlock
        bt_running.store(false);
        if (bt_thread.joinable()) {
            bt_thread.join();
        }
        if (bt_hid) {
            SDL_hid_close(bt_hid);
            bt_hid = nullptr;
        }
        if (audio_stream) {
            // Waits for a running audio callback; the callback never takes `mutex`
            SDL_DestroyAudioStream(audio_stream);
            audio_stream = nullptr;
        }
        ReleaseAudioDevice();
        if (xbox) {
            api.xbox_destroy(xbox);
            xbox = nullptr;
            had_xbox = true;
        }
        const bool had_engine = engine != nullptr;
        if (engine) {
            api.engine_destroy(engine);
            engine = nullptr;
        }
        for (auto& handle : tri) {
            if (handle) {
                api.tri_destroy(handle);
                handle = nullptr;
                bo3_restore_trigger = true;
            }
        }
        if (bo3_speaker_on) {
            bo3_restore_speaker = true;
            bo3_speaker_on = false;
        }
        if (dsp) {
            api.dsp_destroy(dsp);
            dsp = nullptr;
        }
        return had_engine;
    }
};

static void SendNeutralPacket(SDL_Joystick* joystick) {
    SDL_SendJoystickEffect(joystick, NeutralPacket.data(), static_cast<int>(NeutralPacket.size()));
}

bool IsRenHDrumBuilt() {
    return true;
}

bool IsRenHDrumLoaded() {
    return Api().loaded;
}

std::string GetRenHDrumVersionString() {
    const auto& api = Api();
    if (!api.loaded || !api.version) {
        return {};
    }
    const std::uint32_t v = api.version();
    return std::to_string((v >> 16) & 0xffU) + "." + std::to_string((v >> 8) & 0xffU) + "." +
           std::to_string(v & 0xffU);
}

std::vector<RenHDrumPadInfo> GetRenHDrumPads() {
    std::scoped_lock lock{active_pads_mutex};
    std::vector<RenHDrumPadInfo> out;
    out.reserve(active_pads.size());
    for (const auto& entry : active_pads) {
        out.push_back(entry.info);
    }
    return out;
}

std::shared_ptr<RenHDrumPad> RenHDrumPad::Create(SDL_Joystick* joystick, SDL_Gamepad* gamepad) {
    if (!joystick || !IsRenHDrumLoaded()) {
        return nullptr;
    }
    const Uint16 vid = SDL_GetJoystickVendor(joystick);
    const Uint16 pid = SDL_GetJoystickProduct(joystick);
    // A virtual joystick (Lichii's own Switch 2 Pro Controller over Bluetooth LE, switch2_ble)
    // takes no HD Rumble packets: it keeps its driver's standard rumble from the start
    if (SDL_IsJoystickVirtual(SDL_GetJoystickID(joystick))) {
        LOG_INFO(Input, "RenHDrum: {:04X}:{:04X} is a virtual joystick, using its standard rumble",
                 vid, pid);
        return nullptr;
    }
    if (IsSteamController(vid, pid)) {
#ifndef _WIN32
        if (pid == 0x1303) {
            LOG_INFO(Input, "RenHDrum: Steam Controller on Bluetooth is only driven on Windows, "
                            "using the standard rumble");
            return nullptr;
        }
#endif
        const auto& steam_api = Api();
        if (!steam_api.HasValve()) {
            LOG_INFO(Input, "RenHDrum: Steam Controller grips need a newer renhdrum.dll, using "
                            "the standard rumble");
            return nullptr;
        }
        // Not shared yet: no other thread can reach `steam` until it is returned.
        auto steam = std::make_unique<Impl>();
        steam->vid = vid;
        steam->pid = pid;
        steam->joystick = joystick;
        steam->gamepad = gamepad;
        steam->is_bluetooth =
            SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS;
        steam->user = ReadUserSettings();
        steam->steam_dsp = steam_api.valve_create(steam->user.dualsense_volume,
                                                  steam->user.dualsense_crisp_notes);
        if (!steam->steam_dsp) {
            return nullptr;
        }
        LOG_INFO(Input, "RenHDrum: Steam Controller {:04X}:{:04X} (experimental)", vid, pid);
        steam->StartSteamGrip(joystick, pid);
        steam->kind = 3;
        ++connected_count[steam->kind];
        last_connected_kind = steam->kind;
        RegisterActivePad(steam.get(), {
            .name = PadName(joystick, gamepad, steam->kind, vid, pid),
            .guid = PadGuid(joystick),
            .vid = vid,
            .pid = pid,
            .kind = steam->kind,
            .is_bluetooth = steam->is_bluetooth,
            .xbox_triggers = false,
        });
        return std::shared_ptr<RenHDrumPad>(new RenHDrumPad(std::move(steam)));
    }
    const bool switch_pad = IsSwitchHdController(vid, pid);
    const auto& api_check = Api();
    const bool xbox_pad = !switch_pad && !IsDualSense(vid, pid) && api_check.HasXbox() &&
                          IsXboxStyle(joystick, gamepad);
    if (!switch_pad && !xbox_pad && !IsDualSense(vid, pid)) {
        LOG_INFO(Input, "RenHDrum: {:04X}:{:04X} \"{}\" is not a controller RenHDrum drives",
                 vid, pid, SDL_GetJoystickName(joystick) ? SDL_GetJoystickName(joystick) : "");
        return nullptr;
    }

    // Not shared yet: no other thread can reach `state` until it is returned.
    const auto& api = Api();
    auto state = std::make_unique<Impl>();
    state->vid = vid;
    state->pid = pid;
    state->joystick = joystick;
    state->gamepad = gamepad;
    state->is_bluetooth =
        SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS;
    state->user = ReadUserSettings();
    if (xbox_pad) {
        // Unknown connection: the safer Bluetooth pacing (20 ms ticks)
        const bool wired =
            SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRED;
        state->xbox = api.xbox_create(!wired);
        if (!state->xbox) {
            return nullptr;
        }
        state->xbox_triggers = SDL_GetBooleanProperty(
            SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_TRIGGER_RUMBLE_BOOLEAN,
            false);
        const char* gamepad_name = SDL_GetGamepadName(gamepad);
        LOG_INFO(Input,
                 "RenHDrum: Xbox rumble for {:04X}:{:04X} \"{}\" (wired: {}, trigger motors: {})",
                 vid, pid, gamepad_name ? gamepad_name : "", wired, state->xbox_triggers);
    } else if (switch_pad) {
        state->engine = api.engine_create(state->is_bluetooth, pid == 0x2009);
        if (!state->engine) {
            return nullptr;
        }
        LOG_INFO(Input, "RenHDrum: HD Rumble for {:04X}:{:04X} (Bluetooth: {})", vid, pid,
                 state->is_bluetooth);
        state->ConfigureEngine();
    } else {
        state->dsp = api.dsp_create(state->user.dualsense_volume, state->user.dualsense_crisp_notes);
        if (!state->dsp) {
            return nullptr;
        }
        LOG_INFO(Input, "RenHDrum: DualSense haptics for {:04X}:{:04X}", vid, pid);
        if (!state->is_bluetooth) {
            state->bo3 = ReadBo3Config();
            if (state->bo3.present && state->bo3.trigger && api.HasTriTrigger() && gamepad) {
                for (auto& handle : state->tri) {
                    handle = api.tri_create();
                }
                LOG_INFO(Input, "RenHDrum: DualSense trigger kick on");
            }
        }
        state->OpenDualSenseAudio();
    }
    state->kind = xbox_pad ? 2 : switch_pad ? 0 : 1;
    ++connected_count[state->kind];
    if (state->xbox_triggers) {
        ++xbox_trigger_pads;
    }
    state->official_no_triggers = xbox_pad && vid == MicrosoftVendorId && !state->xbox_triggers;
    if (state->official_no_triggers) {
        ++xbox_official_no_triggers;
    }
    last_connected_kind = state->kind;
    RegisterActivePad(state.get(), {
        .name = PadName(joystick, gamepad, state->kind, vid, pid),
        .guid = PadGuid(joystick),
        .vid = vid,
        .pid = pid,
        .kind = state->kind,
        .is_bluetooth = state->is_bluetooth,
        .xbox_triggers = state->xbox_triggers,
    });
    return std::shared_ptr<RenHDrumPad>(new RenHDrumPad(std::move(state)));
}

int GetRenHDrumConnectedKind() {
    const int last = last_connected_kind.load();
    if (last >= 0 && connected_count[last].load() > 0) {
        return last;
    }
    for (int kind = 0; kind < static_cast<int>(connected_count.size()); ++kind) {
        if (connected_count[kind].load() > 0) {
            return kind;
        }
    }
    return -1;
}

int GetRenHDrumXboxTriggers() {
    if (connected_count[2].load() == 0) {
        return -1;
    }
    return xbox_trigger_pads.load() > 0 ? 1 : 0;
}

bool GetRenHDrumXboxNeedsRawInput() {
#ifdef _WIN32
    return xbox_official_no_triggers.load() > 0;
#else
    return false;
#endif
}

RenHDrumPad::RenHDrumPad(std::unique_ptr<Impl> impl_) : impl{std::move(impl_)} {}

RenHDrumPad::~RenHDrumPad() {
    Shutdown();
}

void RenHDrumPad::Shutdown() {
    SDL_Joystick* joystick = nullptr;
    bool had_engine = false;
    bool had_xbox = false;
    bool triggers = false;
    bool restore_speaker = false;
    bool restore_trigger = false;
    {
        std::scoped_lock lock{impl->mutex};
        if (!impl->joystick) {
            return;
        }
        joystick = impl->joystick;
        had_engine = impl->Release();
        had_xbox = impl->had_xbox;
        restore_speaker = std::exchange(impl->bo3_restore_speaker, false);
        restore_trigger = std::exchange(impl->bo3_restore_trigger, false);
        if (impl->kind >= 0) {
            --connected_count[impl->kind];
            if (impl->xbox_triggers) {
                --xbox_trigger_pads;
            }
            if (impl->official_no_triggers) {
                --xbox_official_no_triggers;
            }
            impl->kind = -1;
        }
        triggers = impl->xbox_triggers;
        impl->joystick = nullptr;
        impl->gamepad = nullptr;
    }
    UnregisterActivePad(impl.get());
    if (had_engine) {
        SendNeutralPacket(joystick);
    }
    if (restore_trigger) {
        for (const bool is_right : {false, true}) {
            const auto block = Bo3TriggerBlock(is_right, Bo3TriggerOff);
            SDL_SendJoystickEffect(joystick, block.data(), static_cast<int>(block.size()));
        }
    }
    if (restore_speaker) {
        const auto block = Bo3SpeakerOffBlock();
        SDL_SendJoystickEffect(joystick, block.data(), static_cast<int>(block.size()));
    }
    if (had_xbox) {
        SDL_RumbleJoystick(joystick, 0, 0, 0);
        if (triggers) {
            SDL_RumbleJoystickTriggers(joystick, 0, 0, 0);
        }
    }
}

bool RenHDrumPad::IsActive() const {
    std::scoped_lock lock{impl->mutex};
    if (impl->steam_dsp) {
        return impl->bt_running.load();
    }
    return impl->engine || impl->xbox ||
           (impl->dsp && (impl->audio_stream || impl->bt_running.load()));
}

bool RenHDrumPad::Vibrate(std::int64_t now_us, bool is_right,
                          const Common::Input::VibrationStatus& status) {
    Impl::SettingsChange change{};
    SDL_Joystick* joystick = nullptr;
    bool stop_sdl_rumble = false;
    bool handled = false;
    bool engine_sample = false;
    bool send_direct = false;
    std::array<std::uint16_t, 2> direct{};
    bool send_trigger = false;
    std::array<std::uint16_t, 2> trigger{};
    bool bo3_step = false;
    {
        std::scoped_lock lock{impl->mutex};
        if (!impl->joystick) {
            return false;
        }
        joystick = impl->joystick;
        change = impl->RefreshUserSettings();
        stop_sdl_rumble = std::exchange(impl->stop_sdl_rumble, false) && impl->audio_stream;
        const auto& u = impl->user;
        const auto& api = Api();
        ApplyNx2Mode(api);
        if (impl->xbox && u.hd_enabled && u.xbox_enabled && u.xbox_direct) {
            // Both halves share one pad: each motor follows the stronger side
            const std::size_t side = is_right ? 1 : 0;
            impl->direct_low[side] = ShapeAmplitude(status.low_amplitude, u);
            impl->direct_high[side] = ShapeAmplitude(status.high_amplitude, u);
            const float scale = static_cast<float>(u.xbox_strength) / 100.0f;
            const float mix = static_cast<float>(u.xbox_detail_mix) / 100.0f;
            const auto to_motor = [scale](float v) {
                return static_cast<std::uint16_t>((std::min)(v * scale, 1.0f) * 65535.0f);
            };
            const float low = (std::max)(impl->direct_low[0], impl->direct_low[1]);
            const float high = (std::max)(impl->direct_high[0], impl->direct_high[1]);
            // The big motor also carries part of the fine vibration, where it is felt
            const std::array<std::uint16_t, 2> target{to_motor((std::max)(low, high * mix)),
                                                      to_motor(high)};
            // A sudden rise gets a short burst so the motor spins up in time for quick taps
            constexpr int KickRise = 65535 * 15 / 100;
            constexpr std::int64_t KickUs = 25'000;
            const float accent = static_cast<float>(u.xbox_hit_accents) / 100.0f;
            const auto kicked = [accent](std::uint16_t t) {
                return static_cast<std::uint16_t>(t + (65535.0f - t) * accent);
            };
            // Triggers carry the fine vibration, left side on LT and right side on RT
            const float trigger_mix = static_cast<float>(u.xbox_trigger_mix) / 100.0f;
            const auto on_trigger = [trigger_mix](std::uint16_t v) {
                return static_cast<std::uint16_t>(static_cast<float>(v) * trigger_mix);
            };
            const std::array<std::uint16_t, 2> trigger_full{to_motor(impl->direct_high[0]),
                                                            to_motor(impl->direct_high[1])};
            const std::array<std::uint16_t, 2> trigger_target{on_trigger(trigger_full[0]),
                                                              on_trigger(trigger_full[1])};
            const bool kicking = impl->kick_until_us != 0 && now_us < impl->kick_until_us;
            if (!kicking && accent > 0.0f &&
                (target[0] - impl->direct_target[0] >= KickRise ||
                 target[1] - impl->direct_target[1] >= KickRise)) {
                impl->kick_until_us = now_us + KickUs;
                trigger = {on_trigger(kicked(trigger_full[0])),
                           on_trigger(kicked(trigger_full[1]))};
                send_trigger = true;
                direct = {target[0] - impl->direct_target[0] >= KickRise ? kicked(target[0])
                                                                         : target[0],
                          target[1] - impl->direct_target[1] >= KickRise ? kicked(target[1])
                                                                         : target[1]};
                send_direct = true;
            } else if (kicking) {
                // Settle on the latest target when the kick ends (Tick); a stop ends it now
                if (target[0] == 0 && target[1] == 0) {
                    impl->kick_until_us = 0;
                    direct = target;
                    send_direct = true;
                    trigger = trigger_target;
                    send_trigger = true;
                }
            } else {
                impl->kick_until_us = 0;
                direct = target;
                send_direct = direct != impl->direct_sent;
                trigger = trigger_target;
                send_trigger = trigger != impl->trigger_sent;
            }
            impl->direct_target = target;
            impl->trigger_target = trigger_target;
            if (send_direct) {
                impl->direct_sent = direct;
            }
            send_trigger = send_trigger && impl->xbox_triggers;
            if (send_trigger) {
                impl->trigger_sent = trigger;
            }
            handled = true;
        } else if (impl->xbox) {
            if (u.hd_enabled && u.xbox_enabled) {
                api.xbox_push_half_sample(impl->xbox, now_us, is_right,
                                          ShapeAmplitude(status.low_amplitude, u),
                                          status.low_frequency,
                                          ShapeAmplitude(status.high_amplitude, u),
                                          status.high_frequency);
                handled = true;
                engine_sample = true;
            }
        } else if (impl->engine) {
            if (u.hd_enabled && u.switch_enabled) {
                api.engine_push_half_sample(impl->engine, now_us, is_right,
                                            ShapeAmplitude(status.low_amplitude, u),
                                            status.low_frequency,
                                            ShapeAmplitude(status.high_amplitude, u),
                                            status.high_frequency);
                handled = true;
                engine_sample = true;
            }
        } else if (impl->steam_dsp) {
            // Steam Controller grips: the same two bands as a DualSense; the sender thread reads
            // the DSP. General HD Rumble switch or Steam toggle off or a stopped sender: standard
            // rumble.
            const bool on = u.hd_enabled && u.steam_enabled;
            api.dsp_set_half_target(impl->steam_dsp, is_right,
                                    on ? ShapeAmplitude(status.low_amplitude, u) : 0.0f,
                                    status.low_frequency,
                                    on ? ShapeAmplitude(status.high_amplitude, u) : 0.0f,
                                    status.high_frequency);
            handled = on && impl->bt_running.load();
        } else if (impl->dsp) {
            if (u.hd_enabled && u.dualsense_enabled) {
                api.dsp_set_half_target(impl->dsp, is_right,
                                        ShapeAmplitude(status.low_amplitude, u),
                                        status.low_frequency,
                                        ShapeAmplitude(status.high_amplitude, u),
                                        status.high_frequency);
                handled = impl->audio_stream != nullptr || impl->bt_running.load();
                bo3_step = impl->tri[0] != nullptr;
            } else {
                // Haptics off: silence the voice coils, the standard rumble takes over
                api.dsp_set_half_target(impl->dsp, is_right, 0.0f, status.low_frequency, 0.0f,
                                        status.high_frequency);
            }
        }
    }
    if (stop_sdl_rumble) {
        // Same one-off stop as after opening the speaker (OpenDualSenseAudio)
        SDL_RumbleJoystick(joystick, 0, 0, 0);
    }
    if (bo3_step) {
        const std::array<float, 4> sample{status.low_amplitude, status.low_frequency,
                                          status.high_amplitude, status.high_frequency};
        impl->Bo3TriggerStep(now_us, is_right, &sample);
    }
    if (send_direct) {
        // A stop always arrives as a zero; the duration only guards a lost one
        SDL_RumbleJoystick(joystick, direct[0], direct[1], 2000);
    }
    if (send_trigger) {
        SDL_RumbleJoystickTriggers(joystick, trigger[0], trigger[1], 2000);
    }
    if (change.stop_engine) {
        SendNeutralPacket(joystick);
    }
    if (change.reconfigure_engine) {
        impl->ConfigureEngine();
    }
    if (engine_sample) {
        Tick(now_us);
    }
    return handled;
}

void RenHDrumPad::Tick(std::int64_t now_us) {
    std::array<Uint8, 10> packet{};
    SDL_Joystick* joystick = nullptr;
    bool open_audio = false;
    SDL_AudioStream* check_audio_stream = nullptr;
    SDL_AudioDeviceID check_audio_device = 0;
    bool restart_bt = false;
    bool send = false;
    bool send_xbox = false;
    bool triggers = false;
    std::uint32_t strength = 100;
    bool send_settle = false;
    std::array<std::uint16_t, 2> settle{};
    bool send_settle_trigger = false;
    std::array<std::uint16_t, 2> settle_trigger{};
    std::array<std::uint16_t, 4> motors{};
    {
        std::scoped_lock lock{impl->mutex};
        if (!impl->joystick) {
            return;
        }
        joystick = impl->joystick;
        // Bluetooth tries once (it sets the count to the max); wired keeps looking, slower after
        // the quick attempts.
        if (impl->dsp && !impl->audio_stream && !impl->audio_opening &&
            (!impl->is_bluetooth || impl->audio_attempts < DualSenseAudioMaxAttempts) &&
            now_us >= impl->audio_next_us) {
            impl->audio_next_us =
                now_us +
                (impl->audio_attempts < DualSenseAudioMaxAttempts ? 2'000'000 : 5'000'000);
            open_audio = true;
        } else if (impl->dsp && !impl->is_bluetooth && impl->audio_stream &&
                   now_us >= impl->audio_next_us) {
            impl->audio_next_us = now_us + 5'000'000;
            check_audio_stream = impl->audio_stream;
            check_audio_device = impl->audio_device;
        }
        if (impl->dsp && impl->is_bluetooth && !impl->bt_running.load() &&
            !impl->bt_unsupported && !impl->bt_starting && now_us >= impl->bt_next_us) {
            impl->bt_next_us = now_us + 10'000'000;
            impl->bt_starting = true;
            restart_bt = true;
        }
        send = impl->engine && Api().engine_tick(impl->engine, now_us, packet.data()) == 1;
        if (impl->kick_until_us != 0 && now_us >= impl->kick_until_us) {
            impl->kick_until_us = 0;
            settle = impl->direct_target;
            send_settle = settle != impl->direct_sent;
            impl->direct_sent = settle;
            settle_trigger = impl->trigger_target;
            send_settle_trigger =
                impl->xbox_triggers && settle_trigger != impl->trigger_sent;
            if (send_settle_trigger) {
                impl->trigger_sent = settle_trigger;
            }
        }
        send_xbox = impl->xbox && !impl->user.xbox_direct &&
                    Api().xbox_tick(impl->xbox, now_us, motors.data()) == 1;
        triggers = impl->xbox_triggers;
        strength = impl->user.xbox_strength;
    }
    if (send_settle) {
        SDL_RumbleJoystick(joystick, settle[0], settle[1], 2000);
    }
    if (send_settle_trigger) {
        SDL_RumbleJoystickTriggers(joystick, settle_trigger[0], settle_trigger[1], 2000);
    }
    if (send_xbox) {
        // RenHDrum's mix tops out near 10 % of the range (tuned on the Xbox One S motors);
        // weaker motors need more.
        for (auto& motor : motors) {
            const std::uint32_t scaled = static_cast<std::uint32_t>(motor) * strength / 100u;
            motor = static_cast<std::uint16_t>(std::min<std::uint32_t>(65535u, scaled));
        }
        // Grips last longer than a tick; trigger pulses stay short so a lost stop cannot
        // leave those motors running.
        const bool sent = SDL_RumbleJoystick(joystick, motors[0], motors[1], 2000);
        if (triggers) {
            SDL_RumbleJoystickTriggers(joystick, motors[2], motors[3], 200);
        }
        if (!sent) {
            // Some paths refuse rumble until the first button press: retry on the next tick
            std::scoped_lock lock{impl->mutex};
            if (impl->xbox) {
                Api().xbox_resend(impl->xbox);
            }
        }
    }
    if (open_audio) {
        impl->OpenDualSenseAudio();
    }
    if (restart_bt) {
        impl->RestartDualSenseBluetooth();
    }
    if (check_audio_stream && check_audio_device != 0) {
        int count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
        bool present = false;
        for (int i = 0; devices && i < count; ++i) {
            if (devices[i] == check_audio_device) {
                present = true;
                break;
            }
        }
        SDL_free(devices);
        if (!present) {
            bool lost = false;
            bool lost_speaker = false;
            {
                std::scoped_lock lock{impl->mutex};
                if (impl->audio_stream == check_audio_stream) {
                    // Destroy under `mutex` like Release(): AudioCallback reads `dsp` without
                    // taking `mutex`.
                    SDL_DestroyAudioStream(std::exchange(impl->audio_stream, nullptr));
                    impl->ReleaseAudioDevice();
                    impl->audio_attempts = DualSenseAudioMaxAttempts;
                    lost = true;
                    lost_speaker = std::exchange(impl->bo3_speaker_on, false);
                }
            }
            if (lost_speaker) {
                const auto block = Bo3SpeakerOffBlock();
                SDL_SendJoystickEffect(joystick, block.data(), static_cast<int>(block.size()));
            }
            if (lost) {
                LOG_INFO(Input, "RenHDrum: DualSense speaker went away, standard rumble until "
                                "it is back");
            }
        }
    }
    // Plan 118: the trigger kick falls back to Off from here when no new sample arrives
    impl->Bo3TriggerStep(now_us, false, nullptr);
    impl->Bo3TriggerStep(now_us, true, nullptr);
    if (send) {
        if (SDL_SendJoystickEffect(joystick, packet.data(), static_cast<int>(packet.size()))) {
            impl->engine_ever_sent.store(true, std::memory_order_relaxed);
        } else if (!impl->engine_ever_sent.load(std::memory_order_relaxed)) {
            bool drop_engine = false;
            {
                std::scoped_lock lock{impl->mutex};
                if (impl->engine && !impl->engine_ever_sent.load(std::memory_order_relaxed) &&
                    ++impl->engine_send_fails >= HdRumbleMaxInitialSendFails) {
                    Api().engine_destroy(impl->engine);
                    impl->engine = nullptr;
                    if (impl->kind >= 0) {
                        --connected_count[impl->kind];
                        impl->kind = -1;
                    }
                    drop_engine = true;
                }
            }
            if (drop_engine) {
                LOG_WARNING(Input,
                            "RenHDrum: SDL does not take HD Rumble packets for {:04X}:{:04X}, "
                            "using the standard rumble",
                            impl->vid, impl->pid);
            }
        }
    }
}

} // namespace InputCommon

#else // ENABLE_RENHDRUM

namespace InputCommon {

bool IsRenHDrumBuilt() {
    return false;
}

bool IsRenHDrumLoaded() {
    return false;
}

std::string GetRenHDrumVersionString() {
    return {};
}

std::vector<RenHDrumPadInfo> GetRenHDrumPads() {
    return {};
}

int GetRenHDrumConnectedKind() {
    return -1;
}

int GetRenHDrumXboxTriggers() {
    return -1;
}

bool GetRenHDrumXboxNeedsRawInput() {
    return false;
}

struct RenHDrumPad::Impl {};

std::shared_ptr<RenHDrumPad> RenHDrumPad::Create(SDL_Joystick*, SDL_Gamepad*) {
    return nullptr;
}

RenHDrumPad::RenHDrumPad(std::unique_ptr<Impl> impl_) : impl{std::move(impl_)} {}
RenHDrumPad::~RenHDrumPad() = default;
void RenHDrumPad::Shutdown() {}
bool RenHDrumPad::IsActive() const {
    return false;
}
bool RenHDrumPad::Vibrate(std::int64_t, bool, const Common::Input::VibrationStatus&) {
    return false;
}
void RenHDrumPad::Tick(std::int64_t) {}

} // namespace InputCommon

#endif // ENABLE_RENHDRUM
