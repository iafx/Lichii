// SPDX-FileCopyrightText: Copyright 2026 RenHDrum Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QObject>
#include <QRandomGenerator>
#include <QTimer>

#include <functional>
#include <set>
#include <string>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace Core::HID {
class HIDCore;
}

/// Shared two-phase thunder test pattern sent through HIDCore to all connected emulated pads.
class ThunderRumblePlayer final : public QObject {
    Q_OBJECT

public:
    explicit ThunderRumblePlayer(Core::HID::HIDCore& hid_core_, QObject* parent = nullptr);
    ~ThunderRumblePlayer() override;

    void Play();
    void Stop();

private:
    void Tick();
    void Send(float low_amp, float low_freq, float high_amp, float high_freq);

    Core::HID::HIDCore& hid_core;
    QTimer timer;
    QElapsedTimer clock;
    QRandomGenerator random;
    qint64 next_jitter_ms{};
    float jitter{};
};

/// Neutral Qt dialog for RenHDrum: live controller list, per-controller toggles, test rumble, and
/// problem report bundle.
class HdRumbleDialog final : public QDialog {
    Q_OBJECT

public:
    explicit HdRumbleDialog(QWidget* parent, Core::HID::HIDCore& hid_core_);
    ~HdRumbleDialog() override;

private:
    void RefreshStatus();
    void RefreshPads();
    void ApplyEnabledState();

    Core::HID::HIDCore& hid_core;
    ThunderRumblePlayer* thunder{};

    QCheckBox* hd_enabled{};
    QLabel* status_dot{};
    QLabel* status_label{};
    QLabel* missing_link{};

    QTreeWidget* pad_tree{};
    std::string last_pad_signature;

    QGroupBox* switch_group{};
    QCheckBox* switch_enabled{};
    QComboBox* switch_bt_mode{};

    QGroupBox* dualsense_group{};
    QCheckBox* dualsense_enabled{};

    QGroupBox* xbox_group{};
    QCheckBox* xbox_enabled{};
    QWidget* xbox_raw_box{};

    QGroupBox* steam_group{};
    QCheckBox* steam_enabled{};

    QPushButton* test_button{};
    QCheckBox* rumble_on_connect{};
    QLabel* footer_credit{};

    QTimer refresh_timer;
};

/// Plays a brief confirmation rumble when a RenHDrum controller connects outside emulation.
class RumbleOnConnect final : public QObject {
    Q_OBJECT

public:
    explicit RumbleOnConnect(Core::HID::HIDCore& hid_core_, std::function<bool()> running_,
                             QObject* parent = nullptr);

private:
    void Poll();

    std::function<bool()> game_running;
    ThunderRumblePlayer* thunder{};
    QTimer poll_timer;
    std::set<std::string> seen;
};

void ReportControllerProblem(QWidget* parent, Core::HID::HIDCore& hid_core);
