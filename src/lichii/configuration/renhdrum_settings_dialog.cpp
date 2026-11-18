// SPDX-License-Identifier: GPL-3.0-or-later

#include "renhdrum_settings_dialog.h"

#include <algorithm>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace RenHDrumQt {

namespace {

constexpr unsigned DEV_SWITCH = 1;
constexpr unsigned DEV_DUALSENSE = 2;
constexpr unsigned DEV_XBOX = 4;
constexpr unsigned DEV_STEAM = 8;
constexpr unsigned DEV_ALL = 15;
constexpr char REPO_URL[] = "https://github.com/trzz1214/RenHDrum";

QLabel* MutedLabel(const QString& text, int indent = 0) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setContentsMargins(indent, 0, 0, 0);
    label->setStyleSheet(QStringLiteral("color: palette(mid);"));
    return label;
}

QString DeviceName(unsigned device) {
    switch (device) {
    case DEV_SWITCH:
        return QObject::tr("Switch Pro Controller / Joy-Con");
    case DEV_DUALSENSE:
        return QObject::tr("DualSense / DualSense Edge");
    case DEV_XBOX:
        return QObject::tr("Xbox and Xbox-style controllers");
    case DEV_STEAM:
        return QObject::tr("Steam Controller (2026) · Experimental");
    case DEV_ALL:
        return QObject::tr("All controllers");
    default:
        return QObject::tr("Controllers");
    }
}

QString LinkName(bool bluetooth) {
    return bluetooth ? QObject::tr("Bluetooth") : QObject::tr("USB");
}

} // namespace

struct SettingsWidget::Row {
    int index{};
    QWidget* box{};
    bool link_off{};
};

SettingsWidget::SettingsWidget(Host host_, QWidget* parent)
    : QWidget(parent), host{std::move(host_)} {
    if (!host.api.Complete()) {
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(MutedLabel(tr("renhdrum.dll was not found next to the program, so "
                                        "the controllers use the standard rumble.")));
        return;
    }
    options = host.api.options(&option_count, &stride);
    store = host.api.store_create();
    if (!options || !store) {
        return;
    }
    // Saved values over the recommended defaults; unknown saved keys are never asked for
    for (int i = 0; i < option_count; ++i) {
        int value = 0;
        const char* key = ((const RenHdcOption*)((const char*)options + i * stride))->key;
        if (host.load && host.load(key, &value)) {
            host.api.store_set(store, key, value);
        }
    }
    if (host.active_pad) {
        pad = host.active_pad();
    }

    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    // 1. Master switch and the test button
    auto* top = new QHBoxLayout;
    for (int i = 0; i < option_count; ++i) {
        const auto& o = *(const RenHdcOption*)((const char*)options + i * stride);
        if (QByteArray(o.key) != "enable_hd_rumble") {
            continue;
        }
        auto* master = new QCheckBox(QString::fromUtf8(o.label));
        master->setToolTip(QString::fromUtf8(o.help));
        master->setChecked(ValueAt(i) != 0);
        master_box = master;
        top->addWidget(master);
        connect(master, &QCheckBox::toggled, this, [this, i](bool on) {
            SetValue(i, on ? 1 : 0);
            if (body) {
                body->setEnabled(on);
            }
            if (test_button) {
                test_button->setEnabled(on);
            }
            if (host.master_changed) {
                host.master_changed(on);
            }
        });
        break;
    }
    top->addStretch(1);
    if (host.test_rumble) {
        test_button = new QPushButton(tr("Test rumble"));
        connect(test_button, &QPushButton::clicked, this, [this] { host.test_rumble(); });
        top->addWidget(test_button);
    }
    root->addLayout(top);

    // 2-4. The part that follows the controller in use
    body = new QWidget;
    body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    root->addWidget(body);

    // 5. Footer
    auto* footer = new QHBoxLayout;
    QString credit = tr("Powered by <a href=\"%1\">RenHDrum</a>").arg(QString::fromLatin1(REPO_URL));
    if (host.credit_suffix) {
        const QString suffix = host.credit_suffix();
        if (!suffix.isEmpty()) {
            credit += QStringLiteral(" ") + suffix.toHtmlEscaped();
        }
    }
    auto* credit_label = MutedLabel(credit);
    credit_label->setWordWrap(false);
    credit_label->setOpenExternalLinks(true);
    footer->addWidget(credit_label, 1);
    if (host.report_problem) {
        auto* report = new QPushButton(tr("Report a problem"));
        connect(report, &QPushButton::clicked, this, [this] { host.report_problem(); });
        footer->addWidget(report);
    }
    root->addLayout(footer);

    Rebuild();

    poll = new QTimer(this);
    poll->setInterval(1000);
    connect(poll, &QTimer::timeout, this, [this] {
        if (!host.active_pad) {
            return;
        }
        const ActivePad now = host.active_pad();
        if (!(now == pad)) {
            pad = now;
            Rebuild();
        }
    });
    poll->start();
}

SettingsWidget::~SettingsWidget() {
    for (Row* row : rows) {
        delete row;
    }
    if (store) {
        host.api.store_destroy(store);
    }
}

int SettingsWidget::Value(const char* key) const {
    int value = 0;
    if (store) {
        host.api.store_get(store, key, &value);
    }
    return value;
}

int SettingsWidget::ValueAt(int index) const {
    const auto& o = *(const RenHdcOption*)((const char*)options + index * stride);
    int value = o.def;
    host.api.store_get(store, o.key, &value);
    return value;
}

void SettingsWidget::SetValue(int index, int value) {
    const auto& o = *(const RenHdcOption*)((const char*)options + index * stride);
    host.api.store_set(store, o.key, value);
    if (host.save) {
        host.save(o.key, ValueAt(index));
    }
    Refresh();
}

bool SettingsWidget::RowVisibleOnLink(int index, const ActivePad& active) const {
    const auto& o = *(const RenHdcOption*)((const char*)options + index * stride);
    return o.link == 0 || (o.link == 2) == active.bluetooth;
}

QWidget* SettingsWidget::MakeRow(int index, QWidget* parent) {
    const auto& o = *(const RenHdcOption*)((const char*)options + index * stride);
    auto* box = new QWidget(parent);
    auto* col = new QVBoxLayout(box);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(2);
    const QString label = QString::fromUtf8(o.label);
    const QString help = QString::fromUtf8(o.help);
    const int value = ValueAt(index);

    if (o.kind == 0) {
        auto* check = new QCheckBox(label);
        check->setChecked(value != 0);
        connect(check, &QCheckBox::toggled, this,
                [this, index](bool on) { SetValue(index, on ? 1 : 0); });
        col->addWidget(check);
    } else if (o.kind == 2) {
        auto* line = new QHBoxLayout;
        auto* combo = new QComboBox;
        combo->addItems(QString::fromUtf8(o.choices).split(QLatin1Char('|')));
        combo->setCurrentIndex(std::clamp(value, 0, std::max(0, combo->count() - 1)));
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, index](int i) { SetValue(index, i); });
        line->addWidget(new QLabel(label));
        line->addStretch(1);
        line->addWidget(combo);
        col->addLayout(line);
    } else {
        auto* line = new QHBoxLayout;
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(o.min, o.max);
        slider->setSingleStep(std::max(1, o.step));
        slider->setPageStep(std::max(1, o.step) * 4);
        slider->setValue(value);
        auto* spin = new QSpinBox;
        spin->setRange(o.min, o.max);
        spin->setSingleStep(std::max(1, o.step));
        spin->setSuffix(QStringLiteral(" %"));
        spin->setValue(value);
        connect(slider, &QSlider::valueChanged, this, [this, index, spin](int v) {
            {
                const QSignalBlocker block{spin};
                spin->setValue(v);
            }
            SetValue(index, v);
        });
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [slider](int v) {
            slider->setValue(v); // the slider's signal carries it on
        });
        line->addWidget(new QLabel(label));
        line->addWidget(slider, 1);
        line->addWidget(spin);
        col->addLayout(line);
    }
    if (!help.isEmpty()) {
        col->addWidget(MutedLabel(help, o.kind == 0 ? 22 : 0));
    }
    return box;
}

QWidget* SettingsWidget::MakeCard(const QString& title, unsigned device, bool only_matching_link,
                                  const ActivePad& active) {
    auto* card = new QGroupBox(title);
    auto* col = new QVBoxLayout(card);
    col->setSpacing(6);
    bool any = false;
    for (int i = 0; i < option_count; ++i) {
        const auto& o = *(const RenHdcOption*)((const char*)options + i * stride);
        if (o.tier != 0 || !(o.devices & device) || o.devices == DEV_ALL) {
            continue; // Advanced rows, and the master switch (shown on top)
        }
        if (only_matching_link && !RowVisibleOnLink(i, active)) {
            continue;
        }
        auto* box = MakeRow(i, card);
        rows.push_back(new Row{i, box, false});
        col->addWidget(box);
        any = true;
    }
    if (device == DEV_STEAM) {
        col->addWidget(MutedLabel(tr("Grips · Experimental. Follows the HD Rumble switch. If rumble "
                                     "stops, close Steam or reconnect the controller.")));
        any = true;
    }
    if (device == DEV_XBOX && host.xbox_extra_row) {
        if (QWidget* extra = host.xbox_extra_row(card)) {
            col->addWidget(extra);
            any = true;
        }
    }
    if (!any) {
        card->deleteLater();
        return nullptr;
    }
    return card;
}

QWidget* SettingsWidget::MakeCollapsible(const QString& title, QWidget* content,
                                         bool* open_state) {
    auto* holder = new QWidget;
    auto* col = new QVBoxLayout(holder);
    col->setContentsMargins(0, 0, 0, 0);
    auto* head = new QToolButton;
    head->setText(title);
    head->setCheckable(true);
    head->setChecked(*open_state);
    head->setAutoRaise(true);
    head->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    head->setArrowType(*open_state ? Qt::DownArrow : Qt::RightArrow);
    content->setVisible(*open_state);
    connect(head, &QToolButton::toggled, this, [head, content, open_state](bool open) {
        *open_state = open;
        head->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        content->setVisible(open);
    });
    col->addWidget(head);
    col->addWidget(content);
    return holder;
}

QWidget* SettingsWidget::MakeAdvanced(const ActivePad& active) {
    auto* content = new QWidget;
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(0, 0, 0, 0);

    // First row: Recommended / Custom + reset
    auto* status = new QHBoxLayout;
    recommended_label = new QLabel;
    reset_button = new QPushButton(tr("Reset to recommended"));
    connect(reset_button, &QPushButton::clicked, this, [this] {
        host.api.store_reset(store);
        // Save every value back, then redraw so the widgets show them
        for (int i = 0; i < option_count; ++i) {
            const auto& o = *(const RenHdcOption*)((const char*)options + i * stride);
            if (host.save) {
                host.save(o.key, ValueAt(i));
            }
        }
        if (auto* check = qobject_cast<QCheckBox*>(master_box)) {
            const QSignalBlocker block{check};
            int value = 1;
            host.api.store_get(store, "enable_hd_rumble", &value);
            check->setChecked(value != 0);
            body->setEnabled(value != 0);
        }
        Rebuild();
    });
    status->addWidget(recommended_label);
    status->addStretch(1);
    status->addWidget(reset_button);
    col->addLayout(status);

    // Groups: controller family, then link, in the order the table lists them
    std::vector<std::pair<unsigned, int>> groups;
    for (int i = 0; i < option_count; ++i) {
        const auto& o = *(const RenHdcOption*)((const char*)options + i * stride);
        if (o.tier != 1) {
            continue;
        }
        const std::pair<unsigned, int> key{o.devices, o.link};
        if (std::find(groups.begin(), groups.end(), key) == groups.end()) {
            groups.push_back(key);
        }
    }
    for (const auto& [devices, link] : groups) {
        QString title = DeviceName(devices);
        if (link == 1) {
            title += tr(" · Wired (USB)");
        } else if (link == 2) {
            title += tr(" · Bluetooth");
        }
        auto* group = new QGroupBox(title);
        auto* gcol = new QVBoxLayout(group);
        gcol->setSpacing(6);
        for (int i = 0; i < option_count; ++i) {
            const auto& o = *(const RenHdcOption*)((const char*)options + i * stride);
            if (o.tier != 1 || o.devices != devices || o.link != link) {
                continue;
            }
            auto* box = MakeRow(i, group);
            // A row for one link only: greyed when the controller of its family is on the other
            const bool link_off = active.device != 0 && (o.devices & active.device) &&
                                  !RowVisibleOnLink(i, active);
            if (link_off) {
                auto* inner = qobject_cast<QVBoxLayout*>(box->layout());
                if (inner) {
                    inner->addWidget(MutedLabel(
                        tr("Not in use: the controller is connected by %1.")
                            .arg(LinkName(active.bluetooth)),
                        22));
                }
            }
            rows.push_back(new Row{i, box, link_off});
            gcol->addWidget(box);
        }
        col->addWidget(group);
    }
    return content;
}

void SettingsWidget::Rebuild() {
    // Drop the old rows; the values live in the store, so nothing is lost
    for (Row* row : rows) {
        delete row;
    }
    rows.clear();
    recommended_label = nullptr;
    reset_button = nullptr;
    while (QLayoutItem* item = body_layout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }

    // 2. The controller in use
    if (pad.device == 0) {
        body_layout->addWidget(MutedLabel(tr("Connect a controller to see its settings.")));
    } else {
        QString title = DeviceName(pad.device);
        if (!pad.name.isEmpty()) {
            title = pad.name;
        }
        title += QStringLiteral(" · ") + LinkName(pad.bluetooth);
        if (QWidget* card = MakeCard(title, pad.device, true, pad)) {
            body_layout->addWidget(card);
        }
    }

    // 3. Other controllers (collapsed)
    auto* others = new QWidget;
    auto* ocol = new QVBoxLayout(others);
    ocol->setContentsMargins(0, 0, 0, 0);
    for (const unsigned device : {DEV_SWITCH, DEV_DUALSENSE, DEV_XBOX, DEV_STEAM}) {
        if (device == pad.device) {
            continue;
        }
        if (QWidget* card = MakeCard(DeviceName(device), device, false, pad)) {
            ocol->addWidget(card);
        }
    }
    body_layout->addWidget(MakeCollapsible(tr("Other controllers"), others, &others_open));

    // 4. Advanced (collapsed)
    body_layout->addWidget(MakeCollapsible(tr("Advanced"), MakeAdvanced(pad), &advanced_open));

    if (auto* check = qobject_cast<QCheckBox*>(master_box)) {
        body->setEnabled(check->isChecked());
        if (test_button) {
            test_button->setEnabled(check->isChecked());
        }
    }
    Refresh();
}

void SettingsWidget::Refresh() {
    if (!store) {
        return;
    }
    for (const Row* row : rows) {
        const auto& o = *(const RenHdcOption*)((const char*)options + row->index * stride);
        const bool needed = o.requires_row < 0 || ValueAt(o.requires_row) != 0;
        row->box->setEnabled(needed && !row->link_off);
    }
    if (recommended_label) {
        bool ok = true;
        host.api.store_is_recommended(store, &ok);
        recommended_label->setText(ok ? tr("Recommended ✓") : tr("Custom"));
        reset_button->setVisible(!ok);
    }
}

} // namespace RenHDrumQt
