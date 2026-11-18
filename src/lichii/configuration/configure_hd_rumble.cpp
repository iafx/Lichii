// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <climits>

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QString>

#include "common/scm_rev.h"
#include "common/settings.h"
#include "input_common/drivers/renhdrum_pad.h"
#include "lichii/configuration/configure_hd_rumble.h"
#include "lichii/configuration/renhdrum_settings_dialog.h"

namespace {

/// An official Xbox controller whose trigger motors are out of reach: the one setting that
/// reaches them, offered where the user is setting up the vibration.
QWidget* MakeXboxTriggerRow(QWidget* parent) {
    if (!InputCommon::GetRenHDrumXboxNeedsRawInput() ||
        Settings::values.enable_raw_input.GetValue()) {
        return nullptr;
    }
    auto* box = new QWidget(parent);
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    auto* note = new QLabel(QObject::tr("Trigger rumble needs \"XInput 8 player support\"."));
    note->setWordWrap(true);
    auto* button = new QPushButton(QObject::tr("Turn it on"));
    row->addWidget(note, 1);
    row->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, box, [box] {
        Settings::values.enable_raw_input.SetValue(true);
        // Keep the Controls > Advanced checkbox in step, or its OK would undo this
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (auto* check = top->findChild<QCheckBox*>(QStringLiteral("enable_raw_input"))) {
                check->setChecked(true);
            }
        }
        box->setVisible(false);
        QMessageBox::information(
            box, QObject::tr("HD Rumble"),
            QObject::tr("Turned on. Restart the emulator to feel the triggers.\n\nWhile this is "
                        "on, games can't open their web pages (some manuals and logins). You can "
                        "turn it off in Controls > Advanced."));
    });
    return box;
}

} // namespace

QWidget* CreateHdRumbleSettingsPage(QWidget* parent, std::function<void()> test_rumble,
                                    std::function<void()> report_problem) {
    RenHDrumQt::Host host;
    // The table and the store come from the DLL the input driver already loaded
    const auto symbol = [](const char* name) { return InputCommon::GetRenHDrumSymbol(name); };
    host.api.options = reinterpret_cast<decltype(host.api.options)>(symbol("renhdrum_options"));
    host.api.store_create =
        reinterpret_cast<decltype(host.api.store_create)>(symbol("renhdrum_store_create"));
    host.api.store_destroy =
        reinterpret_cast<decltype(host.api.store_destroy)>(symbol("renhdrum_store_destroy"));
    host.api.store_get =
        reinterpret_cast<decltype(host.api.store_get)>(symbol("renhdrum_store_get"));
    host.api.store_set =
        reinterpret_cast<decltype(host.api.store_set)>(symbol("renhdrum_store_set"));
    host.api.store_reset =
        reinterpret_cast<decltype(host.api.store_reset)>(symbol("renhdrum_store_reset"));
    host.api.store_is_recommended = reinterpret_cast<decltype(host.api.store_is_recommended)>(
        symbol("renhdrum_store_is_recommended"));

    host.active_pad = [] {
        RenHDrumQt::ActivePad active;
        const int kind = InputCommon::GetRenHDrumConnectedKind();
        if (kind < 0) {
            return active;
        }
        for (const auto& pad : InputCommon::GetRenHDrumPads()) {
            if (pad.kind == kind) {
                active.device = 1u << kind; // 0 Switch, 1 DualSense, 2 Xbox, 3 Steam
                active.bluetooth = pad.is_bluetooth;
                active.name = QString::fromStdString(pad.name);
            }
        }
        return active;
    };
    host.load = [](const char* key, int* value) {
        const int saved = Settings::values.renhdrum_values.Get(key, INT_MIN);
        if (saved == INT_MIN) {
            return false;
        }
        *value = saved;
        return true;
    };
    host.save = [](const char* key, int value) { Settings::values.renhdrum_values.Set(key, value); };
    host.test_rumble = std::move(test_rumble);
    host.report_problem = std::move(report_problem);
    host.xbox_extra_row = MakeXboxTriggerRow;
    host.credit_suffix = [] {
        const std::string version = InputCommon::GetRenHDrumVersionString();
        return QStringLiteral("%1 · %2 %3")
            .arg(version.empty() ? QObject::tr("not loaded") : QString::fromStdString(version),
                 QString::fromLatin1(Common::g_build_name),
                 QString::fromLatin1(Common::g_build_version));
    };
    return new RenHDrumQt::SettingsWidget(std::move(host), parent);
}
