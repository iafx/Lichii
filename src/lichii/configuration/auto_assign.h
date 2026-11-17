// SPDX-FileCopyrightText: Copyright 2026 Brontide
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <set>
#include <string>

#include <QObject>

#include "input_common/drivers/renhdrum_pad.h"

namespace Core::HID {
class HIDCore;
}
namespace InputCommon {
class InputSubsystem;
}

namespace Brontide {

/// 0-7 for Player 1-8, 8 for handheld, -1 when the pad is not mapped to any of them.
int PlayerIndexUsing(const Core::HID::HIDCore& hid_core, const InputCommon::RenHDrumPadInfo& pad);

/// Makes a newly plugged controller Player 1 when Player 1 is not on a connected controller
/// so HD Rumble works without a visit to Settings → Controls.
class AutoAssign : public QObject {
    Q_OBJECT

public:
    /// `busy` says the Settings dialog is open (do not touch the mappings then).
    AutoAssign(QObject* parent, Core::HID::HIDCore& hid_core,
               InputCommon::InputSubsystem& input_subsystem, std::function<bool()> busy,
               std::function<void()> save);

signals:
    void Assigned(const QString& pad_name);

private:
    void Tick();
    bool Assign(const InputCommon::RenHDrumPadInfo& pad);

    Core::HID::HIDCore& hid_core;
    InputCommon::InputSubsystem& input_subsystem;
    std::function<bool()> busy;
    std::function<void()> save;
    // ponytail: two pads with the same guid count as one; add the port once RenHDrumPadInfo has it
    std::set<std::string> seen;
};

} // namespace Brontide
