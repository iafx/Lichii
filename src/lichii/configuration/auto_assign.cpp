// SPDX-FileCopyrightText: Copyright 2026 Brontide
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QString>
#include <QTimer>

#include "common/logging.h"
#include "common/param_package.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "lichii/configuration/auto_assign.h"

namespace Brontide {

namespace {

bool IsPad(const Common::ParamPackage& device, const InputCommon::RenHDrumPadInfo& pad) {
    return device.Get("engine", "") == "sdl" &&
           (device.Get("guid", "") == pad.guid || device.Get("guid2", "") == pad.guid);
}

} // namespace

int PlayerIndexUsing(const Core::HID::HIDCore& hid_core, const InputCommon::RenHDrumPadInfo& pad) {
    if (pad.guid.empty()) {
        return -1;
    }
    for (std::size_t index = 0; index < 9; ++index) { // 8 players, then handheld
        const auto* controller = hid_core.GetEmulatedControllerByIndex(index);
        if (!controller || !controller->IsConnected(true)) {
            continue;
        }
        for (const auto& device : controller->GetMappedDevices()) {
            if (IsPad(device, pad)) {
                return static_cast<int>(index);
            }
        }
    }
    return -1;
}

AutoAssign::AutoAssign(QObject* parent, Core::HID::HIDCore& hid_core_,
                       InputCommon::InputSubsystem& input_subsystem_, std::function<bool()> busy_,
                       std::function<void()> save_)
    : QObject(parent), hid_core{hid_core_}, input_subsystem{input_subsystem_},
      busy{std::move(busy_)}, save{std::move(save_)} {
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &AutoAssign::Tick);
    timer->start(1000);
}

void AutoAssign::Tick() {
    if (busy()) {
        return; // new pads are looked at once the dialog closes
    }
    const auto pads = InputCommon::GetRenHDrumPads();
    std::set<std::string> now;
    for (const auto& pad : pads) {
        now.emplace(pad.guid);
    }

    // Player 1 counts as "on a controller" when a mapped SDL device is plugged in right now.
    bool player1_on_pad = false;
    const auto* player1 = hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
    for (const auto& device : player1->GetMappedDevices()) {
        for (const auto& pad : pads) {
            player1_on_pad |= IsPad(device, pad);
        }
    }

    for (const auto& pad : pads) {
        if (pad.guid.empty() || seen.contains(pad.guid)) {
            continue;
        }
        if (player1_on_pad || PlayerIndexUsing(hid_core, pad) >= 0) {
            continue;
        }
        if (Assign(pad)) {
            player1_on_pad = true;
            emit Assigned(QString::fromStdString(pad.name));
        }
    }
    seen = std::move(now); // an unplugged pad counts as new when it comes back
}

bool AutoAssign::Assign(const InputCommon::RenHDrumPadInfo& pad) {
    for (const auto& device : input_subsystem.GetInputDevices()) {
        if (!IsPad(device, pad)) {
            continue;
        }
        auto* controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
        controller->EnableConfiguration();
        for (const auto& [index, param] : input_subsystem.GetButtonMappingForDevice(device)) {
            controller->SetButtonParam(index, param);
        }
        for (const auto& [index, param] : input_subsystem.GetAnalogMappingForDevice(device)) {
            controller->SetStickParam(index, param);
        }
        for (const auto& [index, param] : input_subsystem.GetMotionMappingForDevice(device)) {
            controller->SetMotionParam(index, param);
        }
        controller->SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
        controller->Connect();
        controller->DisableConfiguration();
        controller->SaveCurrentConfig(); // left disabled: outside Settings the pad must stay live
        save();
        LOG_INFO(Frontend, "Brontide: {} set as Player 1", pad.name);
        return true;
    }
    return false;
}

} // namespace Brontide
