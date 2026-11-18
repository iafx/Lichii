// SPDX-FileCopyrightText: Copyright 2026 RenHDrum Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/configuration/hd_rumble_dialog.h"

#include <algorithm>
#include <array>

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QStyle>
#include <QSysInfo>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include "common/fs/path_util.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include "input_common/drivers/renhdrum_pad.h"
#include "qt_common/util/compress.h"
#include "lichii/configuration/configure_hd_rumble.h"

namespace {

constexpr char RELEASES_URL[] = "https://github.com/trzz1214/RenHDrum/releases";
constexpr char ISSUE_URL[] = "https://github.com/trzz1214/RenHDrum/issues/new";
constexpr int MAX_URL_LENGTH = 6000;
constexpr int OLD_LOG_DAYS = 7;

constexpr qint64 CRACK_END_MS = 60;
constexpr float CRACK_HIGH_AMP = 1.0f;
constexpr float CRACK_HIGH_HZ = 320.0f;
constexpr float CRACK_LOW_AMP = 0.6f;
constexpr float CRACK_LOW_HZ = 160.0f;
constexpr qint64 SILENCE_END_MS = 90;
constexpr qint64 RUMBLE_END_MS = 750;
constexpr float RUMBLE_LOW_AMP = 0.9f;
constexpr float RUMBLE_LOW_HZ_START = 80.0f;
constexpr float RUMBLE_LOW_HZ_END = 41.0f;
constexpr float RUMBLE_HIGH_AMP = 0.3f;
constexpr float RUMBLE_HIGH_HZ = 180.0f;
constexpr qint64 JITTER_STEP_MS = 30;
constexpr float JITTER = 0.1f;
constexpr int TEST_TICK_MS = 10;

QLabel* MakeMutedLabel(const QString& text, QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
    label->setPalette(pal);
    return label;
}

QString PlayerUsing(Core::HID::HIDCore& hid_core, const InputCommon::RenHDrumPadInfo& pad) {
    if (pad.guid.empty()) {
        return {};
    }
    for (std::size_t index = 0; index < 9; ++index) {
        const auto* controller = hid_core.GetEmulatedControllerByIndex(index);
        if (!controller || !controller->IsConnected(true)) {
            continue;
        }
        for (const auto& device : controller->GetMappedDevices()) {
            if (device.Get("engine", "") == "sdl" &&
                (device.Get("guid", "") == pad.guid || device.Get("guid2", "") == pad.guid)) {
                return index < 8 ? QObject::tr("Player %1").arg(index + 1) : QObject::tr("Handheld");
            }
        }
    }
    return {};
}

QString StyleName(int style) {
    switch (style) {
    case 3:
        return QStringLiteral("Pro Controller");
    case 4:
        return QStringLiteral("Handheld");
    case 5:
        return QStringLiteral("Dual Joy-Con");
    case 6:
        return QStringLiteral("Left Joy-Con");
    case 7:
        return QStringLiteral("Right Joy-Con");
    default:
        return QStringLiteral("style %1").arg(style);
    }
}

QString Scrub(QString text) {
    const QString home = QDir::homePath();
    if (!home.isEmpty()) {
        const QString profile = QStringLiteral("%USERPROFILE%");
        text.replace(QDir::toNativeSeparators(home), profile, Qt::CaseInsensitive);
        text.replace(home, profile, Qt::CaseInsensitive);
    }
    if (const QString user = qEnvironmentVariable("USERNAME"); user.size() >= 3) {
        text.replace(user, QStringLiteral("<user>"), Qt::CaseInsensitive);
    }
    if (const QString pc = qEnvironmentVariable("COMPUTERNAME"); pc.size() >= 3) {
        text.replace(pc, QStringLiteral("<pc>"), Qt::CaseInsensitive);
    }
    if (const QString host = QSysInfo::machineHostName(); host.size() >= 3) {
        text.replace(host, QStringLiteral("<pc>"), Qt::CaseInsensitive);
    }
    return text;
}

bool WriteText(const QString& path, const QString& text) {
    QFile out(path);
    return out.open(QIODevice::WriteOnly | QIODevice::Truncate) && out.write(text.toUtf8()) >= 0;
}

bool CopyScrubbed(const QString& from, const QString& to) {
    QFile in(from);
    return in.open(QIODevice::ReadOnly) && WriteText(to, Scrub(QString::fromUtf8(in.readAll())));
}

struct ReportSummary {
    QString header;
    QString controllers;
    QString details;
};

ReportSummary DescribeReport(Core::HID::HIDCore& hid_core) {
    ReportSummary out;
    const std::string ver_str = InputCommon::GetRenHDrumVersionString();
    const QString core = ver_str.empty() ? QStringLiteral("not loaded") : QString::fromStdString(ver_str);
    const QString build = QStringLiteral("%1 %2").arg(QString::fromLatin1(Common::g_build_name),
                                                      QString::fromLatin1(Common::g_build_version));
    out.header = QStringLiteral("%1 · RenHDrum %2 · %3")
                     .arg(build, core, QSysInfo::prettyProductName());

    QStringList lines{build,
                      QStringLiteral("RenHDrum %1").arg(core),
                      QStringLiteral("commit %1").arg(QString::fromLatin1(Common::g_scm_desc)),
                      QSysInfo::prettyProductName(),
                      QString{},
                      QStringLiteral("Connected controllers (RenHDrum):")};
    QStringList short_list;
    const auto pads = InputCommon::GetRenHDrumPads();
    for (const auto& pad : pads) {
        const QString name = QString::fromStdString(pad.name);
        const QString link = pad.is_bluetooth ? QStringLiteral("Bluetooth") : QStringLiteral("USB");
        const QString player = PlayerUsing(hid_core, pad);
        lines << QStringLiteral("- %1 (%2:%3) | %4 | in use: %5")
                     .arg(name)
                     .arg(pad.vid, 4, 16, QLatin1Char('0'))
                     .arg(pad.pid, 4, 16, QLatin1Char('0'))
                     .arg(link, player.isEmpty() ? QStringLiteral("no") : player);
        short_list << QStringLiteral("%1 (%2)").arg(name, link);
    }
    if (pads.empty()) {
        lines << QStringLiteral("(none)");
    }

    lines << QString{} << QStringLiteral("Emulated controllers:");
    for (std::size_t index = 0; index < 9; ++index) {
        const auto* controller = hid_core.GetEmulatedControllerByIndex(index);
        if (!controller || !controller->IsConnected(true)) {
            continue;
        }
        QStringList devices;
        for (const auto& device : controller->GetMappedDevices()) {
            devices << QStringLiteral("%1:%2")
                           .arg(QString::fromStdString(device.Get("engine", "")))
                           .arg(device.Get("port", 0));
        }
        lines << QStringLiteral("- %1: %2, devices: %3")
                     .arg(index < 8 ? QStringLiteral("Player %1").arg(index + 1)
                                    : QStringLiteral("Handheld"),
                          StyleName(static_cast<int>(controller->GetNpadStyleIndex(true))),
                          devices.isEmpty() ? QStringLiteral("none")
                                            : devices.join(QStringLiteral(", ")));
    }
    out.details = lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n");
    out.controllers =
        short_list.isEmpty() ? QStringLiteral("none") : short_list.join(QStringLiteral(" · "));
    return out;
}

QString CreateReportZip(Core::HID::HIDCore& hid_core, ReportSummary& summary, QString& error) {
    const QDir desktop(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
    const QString build_tag = QString::fromLatin1(Common::g_build_name);
    const QString name = QStringLiteral("%1-report-%2")
                             .arg(build_tag, QDateTime::currentDateTime().toString(
                                                 QStringLiteral("yyyyMMdd-HHmmss")));
    const QString folder = desktop.filePath(name);
    const QString zip = folder + QStringLiteral(".zip");
    if (!QDir().mkpath(folder)) {
        error = QObject::tr("Could not create %1.").arg(QDir::toNativeSeparators(folder));
        return {};
    }

    const QDir logs(
        QString::fromStdString(Common::FS::GetEdenPathString(Common::FS::EdenPath::LogDir)));
    QStringList wanted{QStringLiteral("eden_log.txt"), QStringLiteral("eden_log.txt.old.txt")};
    for (const QFileInfo& old : logs.entryInfoList({QStringLiteral("eden_log*.txt")}, QDir::Files)) {
        if (old.lastModified().daysTo(QDateTime::currentDateTime()) <= OLD_LOG_DAYS) {
            wanted << old.fileName();
        }
    }
    wanted.removeDuplicates();
    for (const QString& file : wanted) {
        if (logs.exists(file)) {
            CopyScrubbed(logs.filePath(file), QDir(folder).filePath(file));
        }
    }
    if (const QString appdata = qEnvironmentVariable("APPDATA"); !appdata.isEmpty()) {
        CopyScrubbed(QDir(appdata).filePath(QStringLiteral("RenHDrum/renhdrum-settings.txt")),
                     QDir(folder).filePath(QStringLiteral("renhdrum-settings.txt")));
    }
    summary = DescribeReport(hid_core);
    WriteText(QDir(folder).filePath(QStringLiteral("controllers.txt")), Scrub(summary.details));

    if (!QtCommon::Compress::compressDir(zip, folder)) {
        error = QObject::tr("Could not create the zip. The files are in %1.")
                    .arg(QDir::toNativeSeparators(folder));
        return {};
    }
    QDir(folder).removeRecursively();
    return zip;
}

QUrl BuildIssueUrl(const ReportSummary& summary, const QString& zip_name) {
    const QString build = QStringLiteral("%1 %2").arg(QString::fromLatin1(Common::g_build_name),
                                                      QString::fromLatin1(Common::g_build_version));
    const auto build_url = [&](const QString& controllers) {
        QUrl url{QString::fromLatin1(ISSUE_URL)};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("template"), QStringLiteral("controller_problem.md"));
        query.addQueryItem(QStringLiteral("labels"), QStringLiteral("controller"));
        query.addQueryItem(QStringLiteral("title"),
                           QStringLiteral("[%1] Controller problem: ").arg(build));
        query.addQueryItem(
            QStringLiteral("body"),
            QStringLiteral("**What happened?**\n(describe the rumble or controller problem)\n\n"
                           "**Which game, and what were you doing?**\n\n"
                           "**Did it work before? On which version?**\n\n"
                           "**Report file:** drag %1 from your Desktop here.\n\n---\n%2\n"
                           "Controllers: %3\n")
                .arg(zip_name, Scrub(summary.header), Scrub(controllers)));
        url.setQuery(query);
        return url;
    };
    QUrl url = build_url(summary.controllers);
    if (url.toEncoded().size() > MAX_URL_LENGTH) {
        url = build_url(QStringLiteral("see controllers.txt in the report"));
    }
    return url;
}

} // namespace

ThunderRumblePlayer::ThunderRumblePlayer(Core::HID::HIDCore& hid_core_, QObject* parent)
    : QObject(parent), hid_core{hid_core_}, random{QRandomGenerator::securelySeeded()} {
    timer.setInterval(TEST_TICK_MS);
    timer.setTimerType(Qt::PreciseTimer);
    connect(&timer, &QTimer::timeout, this, &ThunderRumblePlayer::Tick);
}

ThunderRumblePlayer::~ThunderRumblePlayer() {
    Stop();
}

void ThunderRumblePlayer::Play() {
    jitter = 0.0f;
    next_jitter_ms = SILENCE_END_MS;
    clock.start();
    timer.start();
    Tick();
}

void ThunderRumblePlayer::Stop() {
    if (!timer.isActive()) {
        return;
    }
    timer.stop();
    Send(0.0f, Core::HID::DEFAULT_VIBRATION_VALUE.low_frequency, 0.0f,
         Core::HID::DEFAULT_VIBRATION_VALUE.high_frequency);
}

void ThunderRumblePlayer::Tick() {
    const qint64 t = clock.elapsed();
    if (t < CRACK_END_MS) {
        Send(CRACK_LOW_AMP, CRACK_LOW_HZ, CRACK_HIGH_AMP, CRACK_HIGH_HZ);
    } else if (t < SILENCE_END_MS) {
        Send(0.0f, CRACK_LOW_HZ, 0.0f, CRACK_HIGH_HZ);
    } else if (t < RUMBLE_END_MS) {
        if (t >= next_jitter_ms) {
            jitter = static_cast<float>(random.bounded(2.0) - 1.0) * JITTER;
            next_jitter_ms = t + JITTER_STEP_MS;
        }
        const float p = static_cast<float>(t - SILENCE_END_MS) /
                        static_cast<float>(RUMBLE_END_MS - SILENCE_END_MS);
        const float fade = (1.0f - p) * (1.0f - p);
        const float low_amp = std::clamp(RUMBLE_LOW_AMP * fade + jitter * fade, 0.0f, 1.0f);
        const float low_hz = RUMBLE_LOW_HZ_START + (RUMBLE_LOW_HZ_END - RUMBLE_LOW_HZ_START) * p;
        Send(low_amp, low_hz, RUMBLE_HIGH_AMP * fade, RUMBLE_HIGH_HZ);
    } else {
        timer.stop();
        Send(0.0f, Core::HID::DEFAULT_VIBRATION_VALUE.low_frequency, 0.0f,
             Core::HID::DEFAULT_VIBRATION_VALUE.high_frequency);
    }
}

void ThunderRumblePlayer::Send(float low_amp, float low_freq, float high_amp, float high_freq) {
    const Core::HID::VibrationValue value{
        .low_amplitude = low_amp,
        .low_frequency = low_freq,
        .high_amplitude = high_amp,
        .high_frequency = high_freq,
    };
    auto& players = Settings::values.players.GetValue();
    for (std::size_t i = 0; i < players.size(); ++i) {
        auto* controller = hid_core.GetEmulatedControllerByIndex(i);
        if (!controller || !(controller->IsConnected() || controller->IsConnected(true))) {
            continue;
        }
        auto& player = players[i];
        const bool old_enabled = player.vibration_enabled;
        const int old_strength = player.vibration_strength;
        player.vibration_enabled = true;
        player.vibration_strength = 100;
        controller->SetVibration(Core::HID::DeviceIndex::Left, value);
        controller->SetVibration(Core::HID::DeviceIndex::Right, value);
        player.vibration_enabled = old_enabled;
        player.vibration_strength = old_strength;
    }
}

HdRumbleDialog::HdRumbleDialog(QWidget* parent, Core::HID::HIDCore& hid_core_)
    : QDialog(parent), hid_core{hid_core_}, thunder{new ThunderRumblePlayer(hid_core_, this)} {
    setWindowTitle(tr("HD Rumble"));
    setMinimumSize(520, 420);
    resize(560, 640);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    // Runtime status
    auto* top_row = new QHBoxLayout;
    top_row->setSpacing(6);
    status_dot = new QLabel(QStringLiteral("●"));
    status_label = MakeMutedLabel(QString{});
    status_label->setWordWrap(false);
    missing_link = new QLabel(
        QStringLiteral("<a href=\"%1\">%2</a>")
            .arg(QString::fromLatin1(RELEASES_URL), tr("Download")));
    missing_link->setOpenExternalLinks(true);
    top_row->addWidget(status_dot);
    top_row->addWidget(status_label);
    top_row->addStretch(1);
    top_row->addWidget(missing_link);
    root->addLayout(top_row);

    // The settings page: master switch, controller in use, Other controllers, Advanced
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(CreateHdRumbleSettingsPage(
        scroll, [this] { thunder->Play(); },
        [this] { ReportControllerProblem(this, hid_core); }));
    root->addWidget(scroll, 1);

    rumble_on_connect = new QCheckBox(tr("Rumble when a controller connects"));
    rumble_on_connect->setChecked(Settings::values.hd_rumble_on_connect.GetValue());
    connect(rumble_on_connect, &QCheckBox::toggled, this,
            [](bool on) { Settings::values.hd_rumble_on_connect.SetValue(on); });
    root->addWidget(rumble_on_connect);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    refresh_timer.setInterval(1000);
    connect(&refresh_timer, &QTimer::timeout, this, [this] { RefreshStatus(); });
    refresh_timer.start();
    RefreshStatus();
}

HdRumbleDialog::~HdRumbleDialog() = default;

void HdRumbleDialog::RefreshStatus() {
    const bool loaded = InputCommon::IsRenHDrumLoaded();
    const bool master = Settings::values.renhdrum_values.HdRumbleEnabled();
    const std::string ver = InputCommon::GetRenHDrumVersionString();
    const QString ver_q = ver.empty() ? tr("not loaded") : QString::fromStdString(ver);

    const auto pads = InputCommon::GetRenHDrumPads();
    const bool active = loaded && master && !pads.empty();
    QPalette dot_pal = status_dot->palette();
    dot_pal.setColor(QPalette::WindowText,
                     dot_pal.color(active ? QPalette::Highlight : QPalette::PlaceholderText));
    status_dot->setPalette(dot_pal);

    missing_link->setVisible(!loaded);
    QString tip;
    if (!loaded) {
        status_label->setText(tr("renhdrum.dll not found"));
        tip = tr("renhdrum.dll was not found — using standard rumble.");
    } else if (!master) {
        status_label->setText(tr("off · standard rumble"));
        tip = tr("HD Rumble is turned off — using standard rumble.");
    } else if (pads.empty()) {
        status_label->setText(tr("RenHDrum %1 · ready").arg(ver_q));
        tip = tr("RenHDrum %1 ready — connect a supported controller.").arg(ver_q);
    } else {
        status_label->setText(tr("RenHDrum %1 · active (%2)").arg(ver_q).arg(pads.size()));
        tip = tr("RenHDrum %1 active — %n controller(s) connected.", "",
                 static_cast<int>(pads.size()))
                  .arg(ver_q);
    }
    status_label->setToolTip(tip);
    status_dot->setToolTip(tip);
}

RumbleOnConnect::RumbleOnConnect(Core::HID::HIDCore& hid_core_, std::function<bool()> running_,
                                 QObject* parent)
    : QObject(parent), game_running{std::move(running_)},
      thunder{new ThunderRumblePlayer(hid_core_, this)} {
    poll_timer.setInterval(1000);
    connect(&poll_timer, &QTimer::timeout, this, &RumbleOnConnect::Poll);
    poll_timer.start();
}

void RumbleOnConnect::Poll() {
    bool fresh = false;
    for (const auto& pad : InputCommon::GetRenHDrumPads()) {
        const std::string key =
            pad.guid + "|" + pad.name + (pad.is_bluetooth ? "/bt" : "/usb");
        fresh |= seen.insert(key).second;
    }
    if (fresh && !game_running() && Settings::values.renhdrum_values.HdRumbleEnabled() &&
        Settings::values.hd_rumble_on_connect.GetValue()) {
        thunder->Play();
    }
}

void ReportControllerProblem(QWidget* parent, Core::HID::HIDCore& hid_core) {
    QDialog explain(parent);
    explain.setWindowTitle(QObject::tr("Report a controller problem"));
    explain.setMinimumWidth(480);
    auto* layout = new QVBoxLayout(&explain);
    auto* intro = new QLabel(
        QObject::tr("This will save a report zip to your Desktop containing:\n"
                    "  • Recent rumble logs (eden_log.txt)\n"
                    "  • Your HD Rumble settings and connected controller list\n\n"
                    "Your Windows username, user folder path, and PC name are scrubbed. "
                    "No keys, firmware, games, or saves are included.\n\n"
                    "Next, GitHub will open in your browser so you can describe the problem, "
                    "drag the zip file onto the page, and submit it."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* create_btn = btns->addButton(QObject::tr("Create report"), QDialogButtonBox::AcceptRole);
    create_btn->setDefault(true);
    QObject::connect(btns, &QDialogButtonBox::accepted, &explain, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &explain, &QDialog::reject);
    layout->addWidget(btns);
    if (explain.exec() != QDialog::Accepted) {
        return;
    }

    ReportSummary summary;
    QString error;
    const QString zip = CreateReportZip(hid_core, summary, error);
    if (zip.isEmpty()) {
        QMessageBox::warning(parent, QObject::tr("Report failed"), error);
        return;
    }

    const QString native = QDir::toNativeSeparators(zip);
    const QString zip_name = QFileInfo(zip).fileName();
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), native});
    QDesktopServices::openUrl(BuildIssueUrl(summary, zip_name));

    QDialog done(parent);
    done.setWindowTitle(QObject::tr("Report saved"));
    done.setMinimumWidth(440);
    auto* done_layout = new QVBoxLayout(&done);
    auto* done_text = new QLabel(
        QObject::tr("Report saved to your Desktop:\n%1\n\n"
                    "GitHub has opened in your browser. Describe the problem, drag the "
                    "zip file onto the page, and press Submit.")
            .arg(zip_name));
    done_text->setWordWrap(true);
    done_layout->addWidget(done_text);
    auto* done_footer = new QHBoxLayout;
    auto* copy_btn = new QPushButton(QObject::tr("Copy file path"));
    auto* ok_btn = new QPushButton(QObject::tr("OK"));
    ok_btn->setDefault(true);
    QObject::connect(copy_btn, &QPushButton::clicked, &done, [native, copy_btn] {
        QGuiApplication::clipboard()->setText(native);
        copy_btn->setText(QObject::tr("Copied"));
    });
    QObject::connect(ok_btn, &QPushButton::clicked, &done, &QDialog::accept);
    done_footer->addWidget(copy_btn);
    done_footer->addStretch(1);
    done_footer->addWidget(ok_btn);
    done_layout->addLayout(done_footer);
    done.exec();
}
