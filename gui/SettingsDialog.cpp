/*
 * SettingsDialog.cpp - implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "SettingsDialog.hpp"

#include "Config.hpp"
#include "DeviceEnumerator.hpp"
#include "InstallationsTab.hpp"
#include "LoadHistogram.hpp"

#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QString>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace
{

const int kBufferSizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };

struct SampleRateItem
{
    const char *label;
    int         value;
};
const SampleRateItem kSampleRates[] = {
    { "Follow PipeWire", 0 }, { "44100", 44100 }, { "48000", 48000 },
    { "88200", 88200 },       { "96000", 96000 }, { "192000", 192000 },
};

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent, SettingsDialogOptions options) : QDialog(parent)
{
    /* Qt appends the application display name to any other title. */
    setWindowTitle(QStringLiteral("PipeASIO Manager"));
    resize(880, 720);

    auto *tabs = new QTabWidget(this);
    if (options.installationsDiscoveryEnabled)
    {
        m_installations = new InstallationsTab(this);
        tabs->addTab(m_installations, QStringLiteral("Installations"));
    }
    const int settingsTab = tabs->addTab(buildSettingsTab(), QStringLiteral("Settings"));
    const int monitorTab  = tabs->addTab(buildMonitorTab(), QStringLiteral("Monitor"));
    tabs->addTab(buildAboutTab(), QStringLiteral("About"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel
                                                 | QDialogButtonBox::RestoreDefaults,
                                         this);
    buttons->button(QDialogButtonBox::Apply)->setObjectName(QStringLiteral("applyButton"));
    buttons->button(QDialogButtonBox::RestoreDefaults)
            ->setObjectName(QStringLiteral("restoreDefaultsButton"));
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            &SettingsDialog::onApply);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
            &SettingsDialog::onRestoreDefaults);
    auto updateSettingsButtons = [tabs, buttons, settingsTab]
    {
        const bool settingsVisible = tabs->currentIndex() == settingsTab;
        buttons->button(QDialogButtonBox::Apply)->setVisible(settingsVisible);
        buttons->button(QDialogButtonBox::RestoreDefaults)->setVisible(settingsVisible);
        buttons->button(QDialogButtonBox::Cancel)
                ->setText(settingsVisible ? tr("Cancel") : tr("Close"));
    };
    connect(tabs, &QTabWidget::currentChanged, this, updateSettingsButtons);
    updateSettingsButtons();

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);

    const pipeasio_config cfg = Config::load();
    m_pendingOutputDevice     = QString::fromUtf8(cfg.output_device);
    m_pendingInputDevice      = QString::fromUtf8(cfg.input_device);
    m_outputDevice->addItem(QStringLiteral("Loading devices..."), m_pendingOutputDevice);
    m_inputDevice->addItem(QStringLiteral("Loading devices..."), m_pendingInputDevice);
    m_outputDevice->setEnabled(false);
    m_inputDevice->setEnabled(false);
    applyConfig(cfg);

    m_deviceRequest = new DeviceEnumerator::Request(std::move(options.deviceRequest), this);
    connect(m_deviceRequest, &DeviceEnumerator::Request::finished, this,
            &SettingsDialog::onDevicesEnumerated);
    m_deviceRequest->start();

    if (options.monitorEnabled)
    {
        connect(&m_monitor, &PipeWireMonitor::updated, this, &SettingsDialog::onMonitorUpdated);
        m_monitor.setTarget(cfg.node_name[0] ? QString::fromUtf8(cfg.node_name) : QString());
        /* The Profiler pushes one point per audio cycle (47/s at 1024 frames,
         * 375/s at 128), so hold the connection only while the tab that
         * consumes it is showing. */
        connect(tabs, &QTabWidget::currentChanged, this,
                [this, monitorTab](int index)
                {
                    if (index == monitorTab)
                        m_monitor.start();
                    else
                        m_monitor.stop();
                });
        if (tabs->currentIndex() == monitorTab)
            m_monitor.start();
    }
}

void
SettingsDialog::done(int result)
{
    if (m_installations && m_installations->busy())
        return;
    QDialog::done(result);
}

void
SettingsDialog::closeEvent(QCloseEvent *event)
{
    if (m_installations && m_installations->busy())
    {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

/* Plain-text tooltips render as one long unwrapped line on most Linux
 * styles. Wrap at word boundaries to a readable measure instead. */
static QString
wrapToolTip(const QString &tip, int width = 72)
{
    QStringList       lines;
    QString           line;
    const QStringList words = tip.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &word : words)
    {
        if (!line.isEmpty() && line.size() + 1 + word.size() > width)
        {
            lines += line;
            line.clear();
        }
        if (!line.isEmpty())
            line += QLatin1Char(' ');
        line += word;
    }
    if (!line.isEmpty())
        lines += line;
    return lines.join(QLatin1Char('\n'));
}

/* A column of at most maxWidth, centered in page, so forms and prose keep a
 * readable measure in a wide window. */
static QVBoxLayout *
centeredColumn(QWidget *page, int maxWidth)
{
    auto *content = new QWidget(page);
    content->setMaximumWidth(maxWidth);
    auto *outer = new QHBoxLayout(page);
    outer->addStretch(1);
    outer->addWidget(content, 100);
    outer->addStretch(1);
    auto *column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    return column;
}

QWidget *
SettingsDialog::buildSettingsTab()
{
    auto *page   = new QWidget(this);
    auto *column = centeredColumn(page, 680);
    column->setSpacing(12);

    QFormLayout    *form = nullptr;
    QList<QLabel *> labels;
    const auto      addSection = [&](const QString &title)
    {
        auto *group = new QGroupBox(title, page);
        form        = new QFormLayout(group);
        column->addWidget(group);
    };

    /* Add a labelled row and attach the same tooltip to both the descriptive
     * label and the field, so hovering either explains the option. */
    auto addRow = [&](const QString &label, QWidget *field, const QString &tip)
    {
        const QString wrapped = wrapToolTip(tip);
        field->setToolTip(wrapped);
        auto *lbl = new QLabel(label, page);
        lbl->setToolTip(wrapped);
        labels += lbl;
        form->addRow(lbl, field);
    };

    addSection(QStringLiteral("Audio"));
    m_inputs = new QSpinBox(page);
    m_inputs->setObjectName(QStringLiteral("inputs"));
    m_inputs->setRange(0, 256);
    addRow(QStringLiteral("Inputs"), m_inputs,
           QStringLiteral("Number of capture (input) channels the driver exposes to the "
                          "host. Applies on the next driver start."));

    m_outputs = new QSpinBox(page);
    m_outputs->setObjectName(QStringLiteral("outputs"));
    m_outputs->setRange(0, 256);
    addRow(QStringLiteral("Outputs"), m_outputs,
           QStringLiteral("Number of playback (output) channels the driver exposes to the "
                          "host. Applies on the next driver start."));

    m_bufferSize = new QComboBox(page);
    m_bufferSize->setObjectName(QStringLiteral("bufferSize"));
    for (int sz : kBufferSizes)
        m_bufferSize->addItem(QString::number(sz), sz);
    addRow(QStringLiteral("Buffer size"), m_bufferSize,
           QStringLiteral("Preferred buffer size in frames. Smaller means lower latency but "
                          "more CPU and a higher risk of dropouts (xruns)."));

    m_latency = new QLabel(page);
    m_latency->setObjectName(QStringLiteral("latency"));
    addRow(QStringLiteral("Latency"), m_latency,
           QStringLiteral("Length of one buffer (buffer size / sample rate), the driver's "
                          "approximate one-way latency. Read-only."));

    m_sampleRate = new QComboBox(page);
    m_sampleRate->setObjectName(QStringLiteral("sampleRate"));
    for (const SampleRateItem &it : kSampleRates)
        m_sampleRate->addItem(QString::fromUtf8(it.label), it.value);
    addRow(QStringLiteral("Sample rate"), m_sampleRate,
           QStringLiteral("\"Follow PipeWire\" tracks the graph's current rate. Set a fixed "
                          "value only if the host requires a specific rate."));

    addSection(QStringLiteral("Devices"));
    m_outputDevice = new QComboBox(page);
    m_outputDevice->setObjectName(QStringLiteral("outputDevice"));
    addRow(QStringLiteral("Output device"), m_outputDevice,
           QStringLiteral("PipeWire sink used for automatic output connections. \"Follow "
                          "default\" tracks the system default sink (e.g. when you switch to "
                          "Bluetooth)."));

    m_inputDevice = new QComboBox(page);
    m_inputDevice->setObjectName(QStringLiteral("inputDevice"));
    addRow(QStringLiteral("Input device"), m_inputDevice,
           QStringLiteral("PipeWire source used for automatic input connections. \"Follow "
                          "default\" tracks the system default source."));

    m_autoConnect = new QCheckBox(page);
    m_autoConnect->setObjectName(QStringLiteral("autoConnect"));
    addRow(QStringLiteral("Auto-connect"), m_autoConnect,
           QStringLiteral("Automatically connect the driver's ports to the selected (or "
                          "default) device. Turn off to wire connections yourself."));

    addSection(QStringLiteral("Advanced"));
    m_fixedBuffer = new QCheckBox(page);
    m_fixedBuffer->setObjectName(QStringLiteral("fixedBuffer"));
    addRow(QStringLiteral("Fixed buffer size"), m_fixedBuffer,
           QStringLiteral("When on, PipeWire controls the buffer size and the host cannot "
                          "change it. When off, the host may set PipeWire's quantum."));

    m_followDeviceClock = new QCheckBox(page);
    m_followDeviceClock->setObjectName(QStringLiteral("followDeviceClock"));
    addRow(QStringLiteral("Follow device clock"), m_followDeviceClock,
           QStringLiteral("Follow the target device's clock instead of forcing the graph "
                          "quantum, and schedule the node asynchronously. Costs one buffer "
                          "period, so leave it off for wired output. Try it when a "
                          "Bluetooth sink is silent or drifts, since its clock is the "
                          "radio link and cannot always be slaved."));

    m_scheduling = new QLabel(page);
    m_scheduling->setObjectName(QStringLiteral("scheduling"));
    /* A shown window does not grow with its hint; reserve the widest reading now. */
    m_scheduling->setMinimumWidth(m_scheduling->fontMetrics().horizontalAdvance(
            QStringLiteral("asynchronous (+1 period, 000.0 ms)")));
    addRow(QStringLiteral("Scheduling"), m_scheduling,
           QStringLiteral("Read-only, set by the option above. Synchronous: the host's "
                          "bufferSwitch must return inside one buffer period or the graph "
                          "stalls. Asynchronous: the device drives the cycle and one extra "
                          "buffer period absorbs an overrun. pw-top marks an asynchronous "
                          "node \"=\" and a synchronous one \"+\"."));

    m_realtime = new QCheckBox(page);
    m_realtime->setObjectName(QStringLiteral("realtime"));
    addRow(QStringLiteral("Real-time audio thread (experimental)"), m_realtime,
           QStringLiteral("Off by default. Raises only the thread that carries the host's "
                          "bufferSwitch callback to SCHED_FIFO priority 15. It does not "
                          "change the host's worker-thread priorities, so multi-threaded "
                          "hosts under Wine "
                          "(measured with FL Studio) can get far more xruns with this on. "
                          "Applies the next time the host starts the driver."));

    m_nodeName = new QLineEdit(page);
    m_nodeName->setObjectName(QStringLiteral("nodeName"));
    m_nodeName->setPlaceholderText(QStringLiteral("(derive from application name)"));
    addRow(QStringLiteral("Node name"), m_nodeName,
           QStringLiteral("Override the PipeWire node/client name. Leave empty to use the "
                          "host application name."));

    connect(m_bufferSize, &QComboBox::currentIndexChanged, this,
            &SettingsDialog::updateLatencyLabel);
    connect(m_sampleRate, &QComboBox::currentIndexChanged, this,
            &SettingsDialog::updateLatencyLabel);
    connect(m_bufferSize, &QComboBox::currentIndexChanged, this,
            &SettingsDialog::updateSchedulingLabel);
    connect(m_sampleRate, &QComboBox::currentIndexChanged, this,
            &SettingsDialog::updateSchedulingLabel);
    connect(m_followDeviceClock, &QCheckBox::toggled, this, &SettingsDialog::updateSchedulingLabel);

    /* Device names are long: let the combos fill the field column and elide
     * rather than widen the window. */
    for (QComboBox *combo : { m_outputDevice, m_inputDevice })
    {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(20);
        combo->setSizePolicy(QSizePolicy::Expanding, combo->sizePolicy().verticalPolicy());
    }

    /* One label column across the sections, so their fields line up. */
    int labelWidth = 0;
    for (QLabel *label : labels)
        labelWidth = std::max(labelWidth, label->sizeHint().width());
    for (QLabel *label : labels)
    {
        label->setMinimumWidth(labelWidth);
        label->setAlignment(form->labelAlignment() | Qt::AlignVCenter);
    }
    column->addStretch(1);

    /* The sections are the tallest page; scroll them rather than hold every
     * tab at their height on short screens. */
    auto *scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(page);
    return scroll;
}

QWidget *
SettingsDialog::buildMonitorTab()
{
    auto *page   = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setSpacing(12);

    /* Headline readings as tiles: the caption on the frame, the value large. */
    auto *tiles = new QHBoxLayout;
    tiles->setSpacing(12);
    layout->addLayout(tiles);
    const auto addTile = [&](const QString &title, QLabel *value, const QString &tip)
    {
        auto *tile = new QGroupBox(title, page);
        tile->setToolTip(wrapToolTip(tip));
        value->setToolTip(tile->toolTip());
        QFont font = value->font();
        font.setPointSizeF(font.pointSizeF() * 1.4);
        font.setWeight(QFont::DemiBold);
        value->setFont(font);
        value->setAlignment(Qt::AlignCenter);
        value->setWordWrap(true);
        (new QVBoxLayout(tile))->addWidget(value);
        tiles->addWidget(tile, 1);
    };

    m_monQuantum = new QLabel(QStringLiteral("waiting for audio..."), page);
    m_monQuantum->setObjectName(QStringLiteral("monQuantum"));
    addTile(QStringLiteral("Buffer / quantum"), m_monQuantum,
            QStringLiteral("Current PipeWire quantum (frames per processing cycle)."));

    m_monRate = new QLabel(QStringLiteral("waiting for audio..."), page);
    m_monRate->setObjectName(QStringLiteral("monRate"));
    addTile(QStringLiteral("Sample rate"), m_monRate,
            QStringLiteral("Current node sample rate in Hz."));

    m_monXruns = new QLabel(QStringLiteral("waiting for audio..."), page);
    m_monXruns->setObjectName(QStringLiteral("monXruns"));
    addTile(QStringLiteral("Xruns"), m_monXruns,
            QStringLiteral("Number of buffer under/overruns (dropouts) reported for this "
                           "node since it started."));

    m_monState = new QLabel(QStringLiteral("waiting for audio..."), page);
    m_monState->setObjectName(QStringLiteral("monState"));
    addTile(QStringLiteral("State"), m_monState,
            QStringLiteral("PipeWire node state: R running, I idle, S suspended, E error."));

    auto *load = new QGroupBox(QStringLiteral("DSP load"), page);
    load->setToolTip(
            wrapToolTip(QStringLiteral("Share of each audio cycle the node spends processing "
                                       "(busy/quantum) over the last minute. The band spans "
                                       "each second's lowest to highest reading and the line "
                                       "follows its average. Sustained high values risk "
                                       "dropouts.")));
    m_monLoad = new LoadHistogram(load);
    m_monLoad->setObjectName(QStringLiteral("monLoad"));
    m_monLoad->setToolTip(load->toolTip());
    m_monLoad->setMinimumHeight(120);
    (new QVBoxLayout(load))->addWidget(m_monLoad);
    layout->addWidget(load, 1);

    auto *devices = new QGroupBox(QStringLiteral("Devices"), page);
    auto *form    = new QFormLayout(devices);
    layout->addWidget(devices);
    auto addRow = [&](const QString &label, QWidget *field, const QString &tip)
    {
        const QString wrapped = wrapToolTip(tip);
        field->setToolTip(wrapped);
        auto *lbl = new QLabel(label, page);
        lbl->setToolTip(wrapped);
        /* Wrapped device names would otherwise stay at their narrow size hint. */
        QSizePolicy policy = field->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::Expanding);
        field->setSizePolicy(policy);
        form->addRow(lbl, field);
    };

    m_monOutput = new QLabel(QStringLiteral("—"), page);
    m_monOutput->setObjectName(QStringLiteral("monOutput"));
    m_monOutput->setWordWrap(true);
    addRow(QStringLiteral("Output device"), m_monOutput,
           QStringLiteral("The sink the driver's output ports currently feed, with its live "
                          "format and state (Bluetooth codec included when applicable)."));

    m_monInput = new QLabel(QStringLiteral("—"), page);
    m_monInput->setObjectName(QStringLiteral("monInput"));
    m_monInput->setWordWrap(true);
    addRow(QStringLiteral("Input device"), m_monInput,
           QStringLiteral("The source currently feeding the driver's input ports, with its "
                          "live format and state (Bluetooth codec included when applicable)."));

    return page;
}

QWidget *
SettingsDialog::buildAboutTab()
{
    auto *page   = new QWidget(this);
    auto *layout = centeredColumn(page, 560);
    layout->addStretch(1);

    const QPixmap logo = windowIcon().pixmap(QSize(64, 64), devicePixelRatioF());
    if (!logo.isNull())
    {
        auto *icon = new QLabel(page);
        icon->setPixmap(logo);
        layout->addWidget(icon, 0, Qt::AlignHCenter);
    }

    auto *title     = new QLabel(QStringLiteral("PipeASIO Manager"), page);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.7);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *version = new QLabel(QStringLiteral("Version " PIPEASIO_VERSION), page);
    version->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(version);

    auto *desc = new QLabel(
            QStringLiteral("Install and manage official PipeASIO releases for Wine, Faugus, "
                           "and Bottles. Configure the PipeWire-native ASIO driver and monitor "
                           "its audio processing without leaving the manager."),
            page);
    desc->setWordWrap(true);
    layout->addWidget(desc);

    layout->addSpacing(16);

    auto *links = new QLabel(page);
    links->setTextFormat(Qt::RichText);
    links->setOpenExternalLinks(true);
    links->setText(QStringLiteral(
            "<a href=\"https://m0n7y5.github.io/pipeasio/\">Website &amp; documentation</a><br>"
            "<a href=\"https://github.com/M0n7y5/pipeasio\">Source code on GitHub</a><br>"
            "<a href=\"https://github.com/M0n7y5/pipeasio/issues\">Report an issue</a><br>"
            "<a href=\"https://ko-fi.com/m0n7y5\">Support development on Ko-fi</a>"));
    layout->addWidget(links);

    layout->addStretch(1);

    auto *legal = new QLabel(
            QStringLiteral("Copyright \u00a9 2026 PipeASIO contributors.<br>"
                           "Licensed under the GNU General Public License v3.0 or later.<br>"
                           "A fork of <a href=\"https://github.com/wineasio/wineasio\">"
                           "WineASIO</a>."),
            page);
    legal->setTextFormat(Qt::RichText);
    legal->setOpenExternalLinks(true);
    legal->setWordWrap(true);
    QFont legalFont = legal->font();
    legalFont.setPointSizeF(legalFont.pointSizeF() * 0.9);
    legal->setFont(legalFont);
    layout->addWidget(legal);

    for (QLabel *label : { title, version, desc, links, legal })
        label->setAlignment(Qt::AlignHCenter);
    return page;
}

int
SettingsDialog::currentBufferSize() const
{
    return m_bufferSize->currentData().toInt();
}

int
SettingsDialog::currentSampleRate() const
{
    return m_sampleRate->currentData().toInt();
}

void
SettingsDialog::updateLatencyLabel()
{
    m_latency->setText(QString::number(currentPeriodMs(), 'f', 1) + QStringLiteral(" ms"));
}

double
SettingsDialog::currentPeriodMs() const
{
    const int buffer = currentBufferSize();
    const int sr     = currentSampleRate();
    /* "Follow PipeWire" (0): use the graph's actual clock rate when pw-dump
     * resolved it, so the readout doesn't lie at non-48k rates (issue #20). */
    const int rate = sr > 0 ? sr : (m_graphRate > 0 ? m_graphRate : 48000);
    return buffer * 1000.0 / rate;
}

void
SettingsDialog::updateSchedulingLabel()
{
    /* audio.c clears PW_KEY_NODE_ASYNC only while not following the device. */
    if (!m_followDeviceClock->isChecked())
    {
        m_scheduling->setText(QStringLiteral("synchronous"));
        return;
    }
    m_scheduling->setText(QStringLiteral("asynchronous (+1 period, %1 ms)")
                                  .arg(QString::number(currentPeriodMs(), 'f', 1)));
}

void
SettingsDialog::applyConfig(const pipeasio_config &c)
{
    m_inputs->setValue(c.inputs);
    m_outputs->setValue(c.outputs);

    int bufIdx = m_bufferSize->findData(c.buffer_size);
    m_bufferSize->setCurrentIndex(bufIdx >= 0 ? bufIdx : 0);

    int srIdx = m_sampleRate->findData(c.sample_rate);
    if (srIdx < 0 && c.sample_rate > 0)
    {
        m_sampleRate->addItem(QString::number(c.sample_rate) + QStringLiteral(" Hz (unavailable)"),
                              c.sample_rate);
        srIdx = m_sampleRate->count() - 1;
    }
    m_sampleRate->setCurrentIndex(srIdx >= 0 ? srIdx : 0);

    m_pendingOutputDevice = QString::fromUtf8(c.output_device);
    m_pendingInputDevice  = QString::fromUtf8(c.input_device);
    if (m_devicesLoading)
    {
        m_outputDevice->setItemData(0, m_pendingOutputDevice);
        m_inputDevice->setItemData(0, m_pendingInputDevice);
    }
    else
    {
        const auto select = [](QComboBox *combo, const QString &name)
        {
            int index = combo->findData(name);
            if (index < 0 && !name.isEmpty())
            {
                combo->addItem(name + QStringLiteral(" (unavailable)"), name);
                index = combo->count() - 1;
            }
            combo->setCurrentIndex(index >= 0 ? index : 0);
        };
        select(m_outputDevice, m_pendingOutputDevice);
        select(m_inputDevice, m_pendingInputDevice);
    }

    m_autoConnect->setChecked(c.auto_connect);
    m_fixedBuffer->setChecked(c.fixed_buffer_size);
    m_followDeviceClock->setChecked(c.follow_device_clock);
    m_realtime->setChecked(c.realtime);
    m_nodeName->setText(QString::fromUtf8(c.node_name));

    updateLatencyLabel();
    updateSchedulingLabel();
}
void
SettingsDialog::onDevicesEnumerated(bool success, const QList<DeviceEnumerator::Device> &devices,
                                    const QString &error)
{
    (void)error;
    m_devicesLoading = false;
    m_graphRate      = m_deviceRequest ? m_deviceRequest->graphRate() : 0;
    m_outputDevice->clear();
    m_inputDevice->clear();
    m_outputDevice->addItem(QStringLiteral("Follow default"), QString());
    m_inputDevice->addItem(QStringLiteral("Follow default"), QString());
    if (success)
    {
        for (const DeviceEnumerator::Device &device : devices)
        {
            const QString label = device.description.isEmpty() ? device.name : device.description;
            (device.isSink ? m_outputDevice : m_inputDevice)->addItem(label, device.name);
        }
    }

    const auto finishCombo = [success](QComboBox *combo, const QString &pending)
    {
        int index = combo->findData(pending);
        if (index < 0 && !pending.isEmpty())
        {
            combo->addItem(pending + QStringLiteral(" (unavailable)"), pending);
            index = combo->count() - 1;
        }
        else if (!success && pending.isEmpty())
        {
            combo->addItem(QStringLiteral("PipeWire devices unavailable"), QString());
            index = combo->count() - 1;
        }
        combo->setCurrentIndex(index >= 0 ? index : 0);
        combo->setEnabled(true);
    };
    finishCombo(m_outputDevice, m_pendingOutputDevice);
    finishCombo(m_inputDevice, m_pendingInputDevice);
    updateLatencyLabel();
    updateSchedulingLabel();
}

void
SettingsDialog::onRestoreDefaults()
{
    applyConfig(Config::defaults());
}

void
SettingsDialog::onApply()
{
    pipeasio_config cfg     = Config::defaults();
    cfg.inputs              = m_inputs->value();
    cfg.outputs             = m_outputs->value();
    cfg.buffer_size         = currentBufferSize();
    cfg.fixed_buffer_size   = m_fixedBuffer->isChecked();
    cfg.sample_rate         = currentSampleRate();
    cfg.auto_connect        = m_autoConnect->isChecked();
    cfg.follow_device_clock = m_followDeviceClock->isChecked();
    cfg.realtime            = m_realtime->isChecked();

    const QString outputName
            = m_devicesLoading ? m_pendingOutputDevice : m_outputDevice->currentData().toString();
    const QByteArray out = outputName.toUtf8();
    qstrncpy(cfg.output_device, out.constData(), sizeof(cfg.output_device));
    const QString inputName
            = m_devicesLoading ? m_pendingInputDevice : m_inputDevice->currentData().toString();
    const QByteArray in = inputName.toUtf8();
    qstrncpy(cfg.input_device, in.constData(), sizeof(cfg.input_device));
    const QByteArray node = m_nodeName->text().trimmed().toUtf8();
    qstrncpy(cfg.node_name, node.constData(), sizeof(cfg.node_name));

    Config::save(cfg);
}

/* Device name on top, its attributes on a smaller muted line below. */
static QString
formatDevice(const QString &name, const QString &detail)
{
    if (name.isEmpty())
        return QStringLiteral("—");
    if (detail.isEmpty())
        return name.toHtmlEscaped();
    return QStringLiteral("%1<br><span style=\"color:gray; font-size:small;\">%2</span>")
            .arg(name.toHtmlEscaped(), detail.toHtmlEscaped());
}

void
SettingsDialog::onMonitorUpdated(const NodeStats &stats)
{
    /* Polling a live graph has one-frame artifacts: the node vanishes for a
     * cycle while restaging, and a brief idle cycle reports quantum/rate 0.
     * Hold the last good frame across the first transient of either kind. A
     * persistent absence reverts to the waiting state (and clears the device
     * rows). A persistent idle/suspended node renders its actual I/S state. */
    if (!stats.found)
    {
        ++m_monMisses;
        m_monIdleFrames = 0;
        if (m_monHasData && m_monMisses < 2)
            return; /* transient: keep the previous frame */
        /* Distinguish "no telemetry at all" (dead daemon, no module-profiler)
         * from an idle graph. */
        const QString waiting = stats.unavailable.isEmpty() ? QStringLiteral("waiting for audio...")
                                                            : stats.unavailable;
        m_monQuantum->setText(waiting);
        m_monRate->setText(waiting);
        m_monXruns->setText(waiting);
        m_monState->setText(waiting);
        m_monLoad->setWaiting();
        m_monOutput->setText(QStringLiteral("—"));
        m_monInput->setText(QStringLiteral("—"));
        return;
    }

    const bool idle = stats.quantum == 0 && stats.rate == 0;
    if (idle)
    {
        ++m_monIdleFrames;
        m_monMisses = 0;
        if (m_monHasData && m_monIdleFrames < 2)
            return; /* transient idle: keep the previous frame */
    }
    else
    {
        m_monMisses     = 0;
        m_monIdleFrames = 0;
    }

    m_monHasData = true;

    m_monOutput->setText(formatDevice(stats.outputDevice, stats.outputDeviceDetail));
    m_monInput->setText(formatDevice(stats.inputDevice, stats.inputDeviceDetail));
    m_monQuantum->setText(QString::number(stats.quantum));
    m_monRate->setText(QString::number(stats.rate) + QStringLiteral(" Hz"));
    m_monXruns->setText(QString::number(stats.xruns));
    m_monState->setText(stats.state);
    m_monLoad->pushSample(stats.dspLoad);
}
