// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>

class QWidget;

/// The HD Rumble settings page of this emulator: RenHDrum's shared Qt page (drawn from the core's
/// settings table) wired to the emulator. Values are saved in qt-config.ini under [RenHDrum]
/// (Settings::values.renhdrum_values); the SDL driver rebuilds its store when they change, so
/// connected controllers pick a change up on their next vibration. `test_rumble` and
/// `report_problem` are the actions of the page's two buttons.
QWidget* CreateHdRumbleSettingsPage(QWidget* parent, std::function<void()> test_rumble,
                                    std::function<void()> report_problem);
