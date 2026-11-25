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
    decltype(&renhdrum_valve_grip_dsp_create_ex) valve_create_ex{};
    decltype(&renhdrum_valve_grip_build_report) valve_build_report{};
    decltype(&renhdrum_valve_grip_mode_tick) valve_mode_tick{};
    decltype(&renhdrum_valve_grip_mode_off) valve_mode_off{};
    // Optional (RenHDrum 0.4.0+): runtime library version
    decltype(&renhdrum_version) version{};
    // Optional (plan 118): DualSense speaker channel and the vibration-reactive trigger
    decltype(&renhdrum_dualsense_dsp_set_speaker) dsp_set_speaker{};
    decltype(&renhdrum_tri_trigger_create) tri_create{};
    decltype(&renhdrum_tri_trigger_destroy) tri_destroy{};
    decltype(&renhdrum_tri_trigger_update) tri_update{};
    // Optional (plan 118 step 10a): other buttons pressed mute the trigger kick
    decltype(&renhdrum_tri_trigger_note_action) tri_note_action{};
    // Settings table (plan 120): the saved values live in a store, the core turns them into the
    // engine config and shapes the amplitudes (gain, weak-rumble lift)
    decltype(&renhdrum_store_create) store_create{};
    decltype(&renhdrum_store_destroy) store_destroy{};
    decltype(&renhdrum_store_get) store_get{};
    decltype(&renhdrum_store_set) store_set{};
    decltype(&renhdrum_store_reset) store_reset{};
    decltype(&renhdrum_store_to_config) store_to_config{};
    decltype(&renhdrum_store_shape_amplitude) store_shape_amplitude{};
    // Optional: Xbox Direct mode
    decltype(&renhdrum_xbox_direct_create) xbox_direct_create{};
    decltype(&renhdrum_xbox_direct_destroy) xbox_direct_destroy{};
    decltype(&renhdrum_xbox_direct_push) xbox_direct_push{};
    decltype(&renhdrum_xbox_direct_tick) xbox_direct_tick{};
    // Optional (plan 119): DualSense USB speaker / trigger blocks and the Bluetooth report 0x36
    decltype(&renhdrum_hd_haptics_speaker_block) hd_speaker_block{};
    decltype(&renhdrum_hd_haptics_trigger_block) hd_trigger_block{};
    decltype(&renhdrum_dualsense_dsp_build_bt36_report) dsp_build_bt36_report{};
    bool HasTriTrigger() const {
        return tri_create && tri_destroy && tri_update;
    }
    bool HasHdHaptics() const {
        return HasTriTrigger() && dsp_set_speaker && hd_speaker_block && hd_trigger_block;
    }
    bool HasXboxDirect() const {
        return xbox_direct_create && xbox_direct_destroy && xbox_direct_push && xbox_direct_tick;
    }
    bool HasValve() const {
        return (valve_create || valve_create_ex) && valve_build_report;
    }
    bool HasValveMode() const {
        return valve_mode_tick && valve_mode_off;
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
                   a.library.GetSymbol("renhdrum_dualsense_dsp_fill_f32", &a.dsp_fill_f32) &&
                   a.library.GetSymbol("renhdrum_store_create", &a.store_create) &&
                   a.library.GetSymbol("renhdrum_store_destroy", &a.store_destroy) &&
                   a.library.GetSymbol("renhdrum_store_get", &a.store_get) &&
                   a.library.GetSymbol("renhdrum_store_set", &a.store_set) &&
                   a.library.GetSymbol("renhdrum_store_reset", &a.store_reset) &&
                   a.library.GetSymbol("renhdrum_store_to_config", &a.store_to_config) &&
                   a.library.GetSymbol("renhdrum_store_shape_amplitude", &a.store_shape_amplitude);
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
            (void)a.library.GetSymbol("renhdrum_valve_grip_dsp_create_ex", &a.valve_create_ex);
            (void)a.library.GetSymbol("renhdrum_valve_grip_build_report",
                                      &a.valve_build_report);
            (void)a.library.GetSymbol("renhdrum_valve_grip_mode_tick", &a.valve_mode_tick);
            (void)a.library.GetSymbol("renhdrum_valve_grip_mode_off", &a.valve_mode_off);
            (void)a.library.GetSymbol("renhdrum_version", &a.version);
            (void)a.library.GetSymbol("renhdrum_dualsense_dsp_set_speaker", &a.dsp_set_speaker);
            (void)a.library.GetSymbol("renhdrum_tri_trigger_create", &a.tri_create);
            (void)a.library.GetSymbol("renhdrum_tri_trigger_destroy", &a.tri_destroy);
            (void)a.library.GetSymbol("renhdrum_tri_trigger_update", &a.tri_update);
            (void)a.library.GetSymbol("renhdrum_tri_trigger_note_action", &a.tri_note_action);
            (void)a.library.GetSymbol("renhdrum_xbox_direct_create", &a.xbox_direct_create);
            (void)a.library.GetSymbol("renhdrum_xbox_direct_destroy", &a.xbox_direct_destroy);
            (void)a.library.GetSymbol("renhdrum_xbox_direct_push", &a.xbox_direct_push);
            (void)a.library.GetSymbol("renhdrum_xbox_direct_tick", &a.xbox_direct_tick);
            (void)a.library.GetSymbol("renhdrum_hd_haptics_speaker_block", &a.hd_speaker_block);
            (void)a.library.GetSymbol("renhdrum_hd_haptics_trigger_block", &a.hd_trigger_block);
            (void)a.library.GetSymbol("renhdrum_dualsense_dsp_build_bt36_report",
                                      &a.dsp_build_bt36_report);
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

using Ds5Block = std::array<Uint8, RENHDRUM_DS5_EFFECTS_SIZE>;

/// HD Haptics (plan 118 / 119): the 47-byte DualSense blocks come from the core. Only called once
/// `HasHdHaptics()` is true.
Ds5Block HdSpeakerBlock(bool on) {
    Ds5Block block{};
    Api().hd_speaker_block(on ? 1 : 0, block.data());
    return block;
}

/// `effect` null = trigger Off.
Ds5Block HdTriggerBlock(bool is_right, const std::array<Uint8, 11>* effect) {
    Ds5Block block{};
    Api().hd_trigger_block(is_right ? 1 : 0, effect ? effect->data() : nullptr, block.data());
    return block;
}

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

bool HasSteamInputStandIn() {
    int count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
    bool found = false;
    for (int i = 0; gamepads && i < count; ++i) {
        if (SDL_Gamepad* gp = SDL_GetGamepadFromID(gamepads[i])) {
            if (SDL_GetGamepadSteamHandle(gp) != 0) {
                found = true;
                break;
            }
        }
    }
    SDL_free(gamepads);
    return found;
}

bool IsDualSense(Uint16 vid, Uint16 pid) {
    return vid == SonyVendorId && (pid == 0x0CE6 || pid == 0x0DF2);
}

/// The few settings the host itself acts on; everything else is read by the core from the store.
struct UserSettings {
    bool hd_enabled{true};
    bool dualsense_enabled{true};
    bool hd_haptics{true};
    int dualsense_volume{100};
    bool dualsense_crisp_notes{true};
    bool xbox_enabled{true};
    int xbox_strength{100};
    bool xbox_direct{false};
    bool operator==(const UserSettings&) const = default;
};

int StoreInt(const RenHdcStore* store, const char* key, int fallback) {
    int value = fallback;
    if (store) {
        Api().store_get(store, key, &value);
    }
    return value;
}

UserSettings ReadUserSettings(const RenHdcStore* s) {
    return {
        .hd_enabled = StoreInt(s, "enable_hd_rumble", 1) != 0,
        .dualsense_enabled = StoreInt(s, "dualsense_haptics", 1) != 0,
        .hd_haptics = StoreInt(s, "dualsense_hd_haptics", 1) != 0,
        .dualsense_volume = std::clamp(StoreInt(s, "dualsense_volume", 100), 0, 100),
        .dualsense_crisp_notes = StoreInt(s, "dualsense_crisp_notes", 1) != 0,
        .xbox_enabled = StoreInt(s, "xbox_enabled", 1) != 0,
        .xbox_strength = std::clamp(StoreInt(s, "xbox_strength", 100), 0, 1000),
        .xbox_direct = StoreInt(s, "xbox_mode", 0) == 1,
    };
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
// `claimed_audio_mutex` and `active_pads_mutex` are leaf locks (ordered after `mutex`): under
// them only their vectors are touched, with no SDL calls.
//
// `bo3_tri_mutex` sits outside `mutex` (order: `bo3_tri_mutex` -> `mutex` -> `bt_dsp_mutex`) so
// trigger packets are always sent in state-machine order without holding `mutex` across SDL.
//
// A DualSense on Bluetooth has no audio endpoint, and a Steam Controller (2026) sends grip PCM
// frames on its own HID thread. Those sender threads never take `mutex`; they hold `bt_dsp_mutex`
// while reading the DSP, and `dsp` / `bt_effect` / speaker state are replaced only under
// `bt_dsp_mutex` too (`mutex` -> `bt_dsp_mutex`).
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
    // The saved settings (Settings::values.renhdrum_values) as the core's value store; rebuilt
    // when their version changes. Touched under `mutex` (or before the pad is shared).
    RenHdcStore* store{};
    std::uint32_t store_version{};

    // Plan 118 / 119: HD Haptics = DualSense USB speaker + trigger kick (both links), and on
    // Bluetooth the combined report 0x36 instead of 0x32
    bool bo3_speaker_on{};
    bool bo3_speaker_logged{};
    bool bo3_trigger_logged{};
    bool bo3_restore_speaker{}; // set by Release(): Shutdown sends the speaker-off block
    bool bo3_restore_trigger{}; // set by Release(): Shutdown sends Off to both triggers
    std::array<RenHdcTriTrigger*, 2> tri{};
    std::array<std::array<float, 4>, 2> tri_sample{};
    std::array<bool, 2> tri_fresh{};
    // Held from computing the trigger effect until its packet is sent, so packets leave in the
    // same order the state machine changed (SDL replaces a pending packet with the same bit).
    std::mutex bo3_tri_mutex;
    // Bluetooth: the sender thread puts the trigger effects into report 0x36. The latest effect
    // of each side, written by Bo3TriggerStep and read by the sender, both under `bt_dsp_mutex`.
    std::array<std::array<Uint8, 11>, 2> bt_effect{};
    std::array<bool, 2> bt_effect_set{};
    std::atomic<bool> bt_report36{};

    RenHdcXboxRumble* xbox{};
    bool xbox_triggers{};
    bool official_no_triggers{};
    // Direct mode (core object: shaping, accents and the settle timer live in the core)
    RenHdcXboxDirect* xbox_direct{};

    // Steam Controller (2026) grips: own DSP so no DualSense branch (`dsp`) ever sees this pad.
    // It reuses bt_hid / bt_thread / bt_running / bt_dsp_mutex below (a pad is never both).
    RenHdcDualSenseDsp* steam_dsp{};
    std::uint8_t steam_format{};
    std::int64_t steam_standin_next_us{};

    // Reused by the Steam Controller grip sender (see steam_dsp)
    std::mutex bt_dsp_mutex;
    SDL_hid_device* bt_hid{};
    std::thread bt_thread;
    std::atomic<bool> bt_running{};
    std::atomic<bool> bt_warned{};
    bool bt_unsupported{};
    bool bt_starting{};
    std::int64_t bt_next_us{};

    ~Impl() {
        if (store) {
            Api().store_destroy(store);
        }
    }

    /// Brings `store` in line with the saved settings; true when they changed since the last
    /// call. Caller holds the mutex (or the pad is not shared yet).
    bool SyncStore() {
        const auto& api = Api();
        const std::uint32_t version = Settings::values.renhdrum_values.Version();
        if (store && version == store_version) {
            return false;
        }
        if (!store) {
            store = api.store_create();
            if (!store) {
                return false;
            }
        }
        api.store_reset(store);
        for (const auto& [key, value] : Settings::values.renhdrum_values.Snapshot()) {
            api.store_set(store, key.c_str(), value); // an unknown key is refused and skipped
        }
        store_version = version;
        return true;
    }

    /// Engine settings from the store (the recommended mode unless the user changed it).
    /// Caller holds the mutex.
    bool ResolveEngineSettings(RenHdcSettings& settings) const {
        RenHdcConfig config{};
        if (!store || Api().store_to_config(store, &config) != RENHDRUM_OK) {
            return false;
        }
        return Api().settings_resolve(&config, &settings) == RENHDRUM_OK;
    }

    float Shape(float amplitude) const {
        return Api().store_shape_amplitude(store, amplitude);
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
        bool sync_hd_haptics{};
    };

    /// Picks up changes made in the HD Rumble dialog. Caller holds the mutex; the returned SDL
    /// work is done after unlocking.
    SettingsChange RefreshUserSettings() {
        if (!SyncStore()) {
            return {};
        }
        const UserSettings now = ReadUserSettings(store);
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
                if (dsp && (bo3_speaker_on || (is_bluetooth && bt_report36.load())) &&
                    api.dsp_set_speaker) {
                    api.dsp_set_speaker(dsp, RENHDRUM_HD_HAPTICS_SPEAKER_PERCENT);
                }
            }
            if (audio_stream) {
                SDL_UnlockAudioStream(audio_stream);
            }
        }
        return {
            .reconfigure_engine = engine != nullptr,
            .stop_engine = engine != nullptr && old.hd_enabled && !now.hd_enabled,
            .sync_hd_haptics = dsp != nullptr && (old.hd_enabled != now.hd_enabled ||
                                                  old.dualsense_enabled != now.dualsense_enabled ||
                                                  old.hd_haptics != now.hd_haptics),
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
            SyncHdHaptics();
        }
    }

    /// Plan 118 / 119: brings HD Haptics in line with the settings: on = the speaker (USB, once
    /// its audio stream is open) and a trigger kick for each side, plus report 0x36 on Bluetooth;
    /// off = both triggers Off and the speaker released. Called without the mutex.
    void SyncHdHaptics() {
        const auto& api = Api();
        SDL_Joystick* js = nullptr;
        bool open_speaker = false;
        bool close_speaker = false;
        bool off_triggers = false;
        bool log_speaker = false;
        bool log_trigger = false;
        {
            std::scoped_lock lock{mutex};
            if (!dsp || !joystick) {
                return;
            }
            js = joystick;
            const bool want = api.HasHdHaptics() && user.hd_enabled && user.dualsense_enabled &&
                              user.hd_haptics;
            const bool bt36 = want && is_bluetooth && api.dsp_build_bt36_report != nullptr;
            bt_report36.store(bt36);
            if (want && gamepad) {
                for (auto& handle : tri) {
                    if (!handle) {
                        handle = api.tri_create();
                        log_trigger |= handle && !std::exchange(bo3_trigger_logged, true);
                    }
                }
            } else {
                for (auto& handle : tri) {
                    if (handle) {
                        api.tri_destroy(handle);
                        handle = nullptr;
                        off_triggers = true;
                    }
                }
            }
            if (!is_bluetooth) {
                if (want && audio_stream && !bo3_speaker_on) {
                    api.dsp_set_speaker(dsp, RENHDRUM_HD_HAPTICS_SPEAKER_PERCENT);
                    bo3_speaker_on = true;
                    open_speaker = true;
                    log_speaker = !std::exchange(bo3_speaker_logged, true);
                } else if (!want && bo3_speaker_on) {
                    api.dsp_set_speaker(dsp, 0);
                    bo3_speaker_on = false;
                    close_speaker = true;
                }
            } else if (api.dsp_set_speaker) {
                std::scoped_lock bt_lock{bt_dsp_mutex};
                api.dsp_set_speaker(dsp, bt36 ? RENHDRUM_HD_HAPTICS_SPEAKER_PERCENT : 0);
            }
        }
        if (open_speaker) {
            const auto block = HdSpeakerBlock(true);
            SDL_SendJoystickEffect(js, block.data(), static_cast<int>(block.size()));
            if (log_speaker) {
                LOG_INFO(Input, "RenHDrum: DualSense speaker on ({}%)",
                         RENHDRUM_HD_HAPTICS_SPEAKER_PERCENT);
            }
        }
        if (log_trigger) {
            LOG_INFO(Input, "RenHDrum: DualSense trigger kick on");
        }
        if (off_triggers) {
            {
                std::scoped_lock bt_lock{bt_dsp_mutex};
                bt_effect_set = {};
            }
            std::scoped_lock tri_lock{bo3_tri_mutex};
            for (const bool is_right : {false, true}) {
                const auto block = HdTriggerBlock(is_right, nullptr);
                SDL_SendJoystickEffect(js, block.data(), static_cast<int>(block.size()));
            }
        }
        if (close_speaker) {
            const auto block = HdSpeakerBlock(false);
            SDL_SendJoystickEffect(js, block.data(), static_cast<int>(block.size()));
        }
    }

    /// Caller holds `bt_dsp_mutex`. The 47-byte trigger block for report 0x36 (both sides'
    /// latest effects), or false while no trigger effect exists yet.
    bool BuildBtTriggerState(Ds5Block& out) const {
        if (!bt_effect_set[0] && !bt_effect_set[1]) {
            return false;
        }
        out = {};
        for (const bool is_right : {false, true}) {
            const std::size_t side = is_right ? 1 : 0;
            if (!bt_effect_set[side]) {
                continue;
            }
            const auto one = HdTriggerBlock(is_right, &bt_effect[side]);
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] |= one[i];
            }
        }
        return true;
    }

    /// Plan 118: one step of the trigger kick for one side. Only Tick calls it (every ~1 ms);
    /// Vibrate just stores the newest raw sample and `tri_fresh`. Lock order: `bo3_tri_mutex`,
    /// then `mutex`. Called without `mutex`.
    void Bo3TriggerStep(std::int64_t now_us, bool is_right) {
        std::scoped_lock tri_lock{bo3_tri_mutex};
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
        }
        const int axis = std::clamp<int>(
            SDL_GetGamepadAxis(gp, is_right ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
                                            : SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
            0, 32767);
        const auto depth = static_cast<Uint8>(axis * 255 / 32767);
        // Plan 118 step 10a: another button (or the other trigger) held means rumble belongs to
        // that action, so the trigger must not follow it. START/BACK/GUIDE/TOUCHPAD/MISC/paddles
        // do not count.
        static constexpr std::array<SDL_GamepadButton, 12> action_buttons{
            SDL_GAMEPAD_BUTTON_SOUTH,         SDL_GAMEPAD_BUTTON_EAST,
            SDL_GAMEPAD_BUTTON_WEST,          SDL_GAMEPAD_BUTTON_NORTH,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
            SDL_GAMEPAD_BUTTON_LEFT_STICK,    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
            SDL_GAMEPAD_BUTTON_DPAD_UP,       SDL_GAMEPAD_BUTTON_DPAD_DOWN,
            SDL_GAMEPAD_BUTTON_DPAD_LEFT,     SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
        bool other_action =
            SDL_GetGamepadAxis(gp, is_right ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                            : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) *
                    255 / 32767 >
                12;
        for (std::size_t i = 0; !other_action && i < action_buttons.size(); ++i) {
            other_action = SDL_GetGamepadButton(gp, action_buttons[i]);
        }
        std::array<Uint8, 11> effect{};
        int changed = 0;
        {
            std::scoped_lock lock{mutex};
            if (!tri[side]) {
                return;
            }
            const bool fresh = std::exchange(tri_fresh[side], false);
            const auto& s = tri_sample[side];
            const auto now_ms = static_cast<std::uint32_t>(now_us / 1000);
            if (other_action && Api().tri_note_action) {
                Api().tri_note_action(tri[side], now_ms);
            }
            changed = Api().tri_update(tri[side], now_ms,
                                       depth, s[0], s[1], s[2], s[3], fresh, effect.data());
        }
        if (changed == 1) {
            if (bt_report36.load()) {
                // Bluetooth: the sender thread carries it in the next report 0x36
                std::scoped_lock bt_lock{bt_dsp_mutex};
                bt_effect[side] = effect;
                bt_effect_set[side] = true;
            } else {
                const auto block = HdTriggerBlock(is_right, &effect);
                SDL_SendJoystickEffect(js, block.data(), static_cast<int>(block.size()));
            }
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
        LOG_INFO(Input, "RenHDrum: DualSense haptics over Bluetooth (report 0x32, 0x36 with HD Haptics)");
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
        {
            std::scoped_lock lock{mutex};
            if (!joystick || !dsp) {
                js = nullptr;
                gp = nullptr;
            }
        }
        if (js) {
            StartDualSenseBluetooth(js, gp);
        }
        {
            std::scoped_lock lock{mutex};
            bt_starting = false;
        }
    }

    /// Runs on `bt_thread`: one report (0x32, or 0x36 with HD Haptics) every
    /// RENHDRUM_DS5_BT_PERIOD_NS while vibrating and for 2 s after, then quiet on the radio. Stops after about 0.5 s of failed writes, which hands
    /// the vibration back to the standard rumble.
    void DualSenseBluetoothLoop() {
        Common::SetCurrentThreadName("RenHDrumDS5BT");
        constexpr Uint64 KeepSendingNs = 2'000'000'000ULL;
        constexpr Uint64 ResyncNs = 100'000'000ULL;
        constexpr int MaxFailsInRow = 50;
        const auto& api = Api();
        std::array<std::uint8_t, RENHDRUM_DS5_BT_REPORT_SIZE> report{};
        std::array<std::uint8_t, RENHDRUM_DS5_BT36_REPORT_SIZE> report36{};
        Uint64 next = SDL_GetTicksNS();
        Uint64 last_signal = 0;
        int fails_in_row = 0;
        while (bt_running.load()) {
            next += RENHDRUM_DS5_BT_PERIOD_NS;
            // HD Haptics on: the combined report 0x36 (rumble + triggers), which the core stops
            // by itself after 2 s of silence. Off: the plain 0x32 as before.
            const bool combined = bt_report36.load();
            int built = -1;
            {
                std::scoped_lock lock{bt_dsp_mutex};
                if (dsp) {
                    if (combined) {
                        Ds5Block state{};
                        const bool have_state = BuildBtTriggerState(state);
                        built = api.dsp_build_bt36_report(dsp, have_state ? state.data() : nullptr,
                                                          0, report36.data());
                    } else {
                        built = api.dsp_build_bt_report(
                            dsp, RENHDRUM_DS5_BT_BUFFER_LENGTH_DEFAULT, report.data());
                    }
                }
            }
            const Uint64 now = SDL_GetTicksNS();
            if (built == 1) {
                last_signal = now;
            }
            const bool send = combined ? built == 1
                                       : built >= 0 && last_signal != 0 &&
                                             now - last_signal < KeepSendingNs;
            if (send) {
                const int written = combined
                                        ? SDL_hid_write(bt_hid, report36.data(), report36.size())
                                        : SDL_hid_write(bt_hid, report.data(), report.size());
                if (written < 0) {
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

    static void SendSteamGripModeOff(SDL_hid_device* hid, Uint8 format) {
        if (const auto& api = Api(); api.valve_mode_off) {
            std::array<std::uint8_t, RENHDRUM_VALVE_GRIP_MODE_REPORTS_SIZE> reports{};
            for (const Uint8 fmt : {format, Uint8{0}}) {
                if (api.valve_mode_off(fmt, reports.data()) == RENHDRUM_VALVE_GRIP_MODE_REPORT_COUNT) {
                    SDL_hid_write(hid, reports.data(), RENHDRUM_VALVE_GRIP_MODE_REPORT_SIZE);
                    SDL_hid_write(hid, reports.data() + RENHDRUM_VALVE_GRIP_MODE_REPORT_SIZE,
                                  RENHDRUM_VALVE_GRIP_MODE_REPORT_SIZE);
                }
            }
            return;
        }
        SendSteamGripMode(hid, 1, format);
        SendSteamGripMode(hid, 1, 0);
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
        if (Api().HasValveMode()) {
            // Leave the grips in standard rumble mode until vibration starts; mode_tick toggles
            // 0x86 on/off dynamically and refreshes every 3 s or on write stall.
            SendSteamGripModeOff(hid, format);
        } else {
            SendSteamGripMode(hid, 1, 0);
            SDL_Delay(10);
            SendSteamGripMode(hid, 2, format);
            SDL_Delay(200); // the controller needs a moment between off and on
        }
        {
            std::scoped_lock lock{mutex};
            if (!joystick || !steam_dsp || bt_hid || bt_thread.joinable()) {
                SendSteamGripModeOff(hid, format);
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
        if (Api().HasValveMode()) {
            SteamGripLoopCoreMode();
        } else {
            SteamGripLoopOwnMode();
        }
        // The grips back to standard rumble on every way out (Release no longer does it)
        if (bt_hid) {
            SendSteamGripModeOff(bt_hid, steam_format);
        }
        bt_running.store(false);
    }

    /// RenHDrum 0.5.0 and later: the core says when to send the 0x86 mode reports ("on" at the
    /// first vibration and every 3 s, at once after a stalled write, "off" after 100 ms of
    /// silence) and asks for one report in ~25,000 to be skipped (clock drift).
    void SteamGripLoopCoreMode() {
        constexpr Uint64 ResyncNs = 100'000'000ULL;
        const auto& api = Api();
        const Uint8 format = steam_format;
        const Uint64 period_ns = format == RENHDRUM_VALVE_GRIP_FORMAT_MULAW_8K
                                     ? RENHDRUM_VALVE_GRIP_PERIOD_NS_8K
                                     : RENHDRUM_VALVE_GRIP_PERIOD_NS_4K;
        std::array<std::uint8_t, RENHDRUM_VALVE_GRIP_REPORT_SIZE> report{};
        std::array<std::uint8_t, RENHDRUM_VALVE_GRIP_MODE_REPORTS_SIZE> mode_reports{};
        Uint64 next = SDL_GetTicksNS();
        Uint64 last_signal = 0;
        int fails_in_row = 0;
        bool write_stalled = false;
        while (bt_running.load()) {
            next += period_ns;
            const Uint64 now = SDL_GetTicksNS();
            int built = -1;
            int mode_count = 0;
            {
                std::scoped_lock lock{bt_dsp_mutex};
                if (steam_dsp) {
                    built = api.valve_build_report(steam_dsp, format, report.data());
                    if (built >= 0) {
                        mode_count = api.valve_mode_tick(steam_dsp, format, now, write_stalled,
                                                         mode_reports.data());
                    }
                }
            }
            write_stalled = false;
            for (int i = 0; i < mode_count; ++i) {
                if (SDL_hid_write(bt_hid,
                                  mode_reports.data() + i * RENHDRUM_VALVE_GRIP_MODE_REPORT_SIZE,
                                  RENHDRUM_VALVE_GRIP_MODE_REPORT_SIZE) < 0) {
                    write_stalled = true;
                    ++fails_in_row;
                } else {
                    fails_in_row = 0;
                }
            }
            if (built == 1 || built == RENHDRUM_VALVE_GRIP_SKIP) {
                last_signal = now;
            }
            if (built >= 0 && built != RENHDRUM_VALVE_GRIP_SKIP && last_signal != 0 &&
                now - last_signal < SteamGripTailNs) {
                if (SDL_hid_write(bt_hid, report.data(), report.size()) < 0) {
                    write_stalled = true;
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
            const Uint64 after = SDL_GetTicksNS();
            if (next > after) {
                SDL_DelayPrecise(next - after);
            } else if (after - next > ResyncNs) {
                next = after; // stalled: resync instead of bursting
                write_stalled = true; // the core sends 0x86 "on" again
            }
        }
    }

    /// Older renhdrum.dll without the core mode manager: Lichii keeps the 0x86 mode itself.
    void SteamGripLoopOwnMode() {
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
            // Stop the grip sender (SteamGripLoop writes 0x86 off before exiting) and free its
            // DSP. The handle is closed by the Bluetooth cleanup below (shared member).
            bt_running.store(false);
            if (bt_thread.joinable()) {
                bt_thread.join();
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
        if (xbox_direct) {
            api.xbox_direct_destroy(xbox_direct);
            xbox_direct = nullptr;
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

void* GetRenHDrumSymbol(const char* name) {
    const auto& api = Api();
    return api.loaded ? api.library.GetSymbolAddress(name) : nullptr;
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
    if (gamepad && SDL_GetGamepadSteamHandle(gamepad) != 0) {
        LOG_INFO(Input, "RenHDrum: skipping Steam Input virtual stand-in gamepad");
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
        if (HasSteamInputStandIn()) {
            LOG_INFO(Input,
                     "RenHDrum: Steam Input stand-in present, skipping Steam Controller {:04X}:{:04X}",
                     vid, pid);
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
        steam->SyncStore();
        steam->user = ReadUserSettings(steam->store);
        const bool use_core_mode = steam_api.valve_create_ex && steam_api.HasValveMode();
        steam->steam_dsp = use_core_mode
                               ? steam_api.valve_create_ex(steam->user.dualsense_volume,
                                                           steam->user.dualsense_crisp_notes, true)
                               : steam_api.valve_create(steam->user.dualsense_volume,
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
    state->SyncStore();
    state->user = ReadUserSettings(state->store);
    if (xbox_pad) {
        // Unknown connection: the safer Bluetooth pacing (20 ms ticks)
        const bool wired =
            SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRED;
        state->xbox = api.xbox_create(!wired);
        if (!state->xbox) {
            return nullptr;
        }
        if (api.HasXboxDirect()) {
            state->xbox_direct = api.xbox_direct_create();
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
        // HD Haptics: trigger kick (and report 0x36 on Bluetooth) now; the speaker once its
        // audio stream is open
        state->SyncHdHaptics();
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
        std::scoped_lock tri_lock{impl->bo3_tri_mutex};
        for (const bool is_right : {false, true}) {
            const auto block = HdTriggerBlock(is_right, nullptr);
            SDL_SendJoystickEffect(joystick, block.data(), static_cast<int>(block.size()));
        }
    }
    if (restore_speaker) {
        const auto block = HdSpeakerBlock(false);
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
        if (impl->xbox && impl->xbox_direct && u.hd_enabled && u.xbox_enabled && u.xbox_direct) {
            // Direct mode lives in the core: it shapes the bands, adds the hit accents and tells
            // which motors to drive now (bit 0: grips, bit 1: triggers)
            std::array<std::uint16_t, 4> motors{};
            const std::int32_t mask = api.xbox_direct_push(
                impl->xbox_direct, impl->store, now_us, is_right, status.low_amplitude,
                status.high_amplitude, impl->xbox_triggers, motors.data());
            send_direct = mask > 0 && (mask & 1) != 0;
            send_trigger = mask > 0 && (mask & 2) != 0;
            direct = {motors[0], motors[1]};
            trigger = {motors[2], motors[3]};
            handled = true;
        } else if (impl->xbox) {
            if (u.hd_enabled && u.xbox_enabled) {
                api.xbox_push_half_sample(impl->xbox, now_us, is_right,
                                          impl->Shape(status.low_amplitude),
                                          status.low_frequency,
                                          impl->Shape(status.high_amplitude),
                                          status.high_frequency);
                handled = true;
                engine_sample = true;
            }
        } else if (impl->engine) {
            if (u.hd_enabled) {
                api.engine_push_half_sample(impl->engine, now_us, is_right,
                                            impl->Shape(status.low_amplitude),
                                            status.low_frequency,
                                            impl->Shape(status.high_amplitude),
                                            status.high_frequency);
                handled = true;
                engine_sample = true;
            }
        } else if (impl->steam_dsp) {
            // Steam Controller grips: the same two bands as a DualSense; the sender thread reads
            // the DSP. General HD Rumble switch off or a stopped sender: standard rumble.
            const bool on = u.hd_enabled;
            api.dsp_set_half_target(impl->steam_dsp, is_right,
                                    on ? impl->Shape(status.low_amplitude) : 0.0f,
                                    status.low_frequency,
                                    on ? impl->Shape(status.high_amplitude) : 0.0f,
                                    status.high_frequency);
            handled = on && impl->bt_running.load();
        } else if (impl->dsp) {
            if (u.hd_enabled && u.dualsense_enabled) {
                api.dsp_set_half_target(impl->dsp, is_right,
                                        impl->Shape(status.low_amplitude),
                                        status.low_frequency,
                                        impl->Shape(status.high_amplitude),
                                        status.high_frequency);
                handled = impl->audio_stream != nullptr || impl->bt_running.load();
                const std::size_t side = is_right ? 1 : 0;
                if (impl->tri[side]) {
                    impl->tri_sample[side] = {status.low_amplitude, status.low_frequency,
                                              status.high_amplitude, status.high_frequency};
                    impl->tri_fresh[side] = true;
                }
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
    if (change.sync_hd_haptics) {
        impl->SyncHdHaptics();
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
    bool check_steam_standin = false;
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
        if (impl->steam_dsp && impl->bt_running.load() && now_us >= impl->steam_standin_next_us) {
            impl->steam_standin_next_us = now_us + 1'000'000;
            check_steam_standin = true;
        }
        send = impl->engine && Api().engine_tick(impl->engine, now_us, packet.data()) == 1;
        const bool direct_mode = impl->xbox_direct && impl->user.xbox_direct;
        if (impl->xbox_direct) {
            // When an accent burst ends the core hands back the level to settle on
            std::array<std::uint16_t, 4> direct_motors{};
            const std::int32_t mask = Api().xbox_direct_tick(
                impl->xbox_direct, now_us, impl->xbox_triggers, direct_motors.data());
            send_settle = mask > 0 && (mask & 1) != 0;
            send_settle_trigger = mask > 0 && (mask & 2) != 0;
            settle = {direct_motors[0], direct_motors[1]};
            settle_trigger = {direct_motors[2], direct_motors[3]};
        }
        send_xbox = impl->xbox && !direct_mode &&
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
    if (check_steam_standin && HasSteamInputStandIn()) {
        bool stopped = false;
        {
            std::scoped_lock lock{impl->mutex};
            if (impl->steam_dsp) {
                impl->Release();
                if (impl->kind >= 0) {
                    --connected_count[impl->kind];
                    impl->kind = -1;
                }
                stopped = true;
            }
        }
        if (stopped) {
            UnregisterActivePad(impl.get());
            LOG_INFO(Input, "RenHDrum: Steam Input stand-in opened, yielding Steam Controller "
                            "grips to standard rumble");
        }
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
                    if (lost_speaker && impl->dsp && Api().dsp_set_speaker) {
                        Api().dsp_set_speaker(impl->dsp, 0);
                    }
                }
            }
            if (lost_speaker) {
                const auto block = HdSpeakerBlock(false);
                SDL_SendJoystickEffect(joystick, block.data(), static_cast<int>(block.size()));
            }
            if (lost) {
                LOG_INFO(Input, "RenHDrum: DualSense speaker went away, standard rumble until "
                                "it is back");
            }
        }
    }
    // Plan 118: the trigger kick falls back to Off from here when no new sample arrives
    impl->Bo3TriggerStep(now_us, false);
    impl->Bo3TriggerStep(now_us, true);
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

void* GetRenHDrumSymbol(const char*) {
    return nullptr;
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
