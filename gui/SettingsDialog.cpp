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
#include "LoadHistogram.hpp"

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTabWidget>
#include <QVBoxLayout>
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
    setWindowTitle(QStringLiteral("PipeASIO Settings - " PIPEASIO_VERSION));

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildSettingsTab(), QStringLiteral("Settings"));
    tabs->addTab(buildMonitorTab(), QStringLiteral("Monitor"));
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
        m_monitor.start();
    }
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

QWidget *
SettingsDialog::buildSettingsTab()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    /* Add a labelled row and attach the same tooltip to both the descriptive
     * label and the field, so hovering either explains the option. */
    auto addRow = [&](const QString &label, QWidget *field, const QString &tip)
    {
        const QString wrapped = wrapToolTip(tip);
        field->setToolTip(wrapped);
        auto *lbl = new QLabel(label, page);
        lbl->setToolTip(wrapped);
        form->addRow(lbl, field);
    };

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

    m_fixedBuffer = new QCheckBox(page);
    m_fixedBuffer->setObjectName(QStringLiteral("fixedBuffer"));
    addRow(QStringLiteral("Fixed buffer size"), m_fixedBuffer,
           QStringLiteral("When on, PipeWire controls the buffer size and the host cannot "
                          "change it. When off, the host may set PipeWire's quantum."));

    m_followDeviceClock = new QCheckBox(page);
    m_followDeviceClock->setObjectName(QStringLiteral("followDeviceClock"));
    addRow(QStringLiteral("Follow device clock (Bluetooth)"), m_followDeviceClock,
           QStringLiteral("Follow the target device's clock instead of forcing the graph "
                          "quantum. Required for Bluetooth sinks, whose clock cannot be "
                          "slaved. Raises latency, so leave it off for wired output."));

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

    return page;
}

QWidget *
SettingsDialog::buildMonitorTab()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    auto addRow = [&](const QString &label, QWidget *field, const QString &tip)
    {
        const QString wrapped = wrapToolTip(tip);
        field->setToolTip(wrapped);
        auto *lbl = new QLabel(label, page);
        lbl->setToolTip(wrapped);
        form->addRow(lbl, field);
    };

    m_monQuantum = new QLabel(QStringLiteral("waiting for audio..."), page);
    m_monQuantum->setObjectName(QStringLiteral("monQuantum"));
    addRow(QStringLiteral("Buffer / quantum"), m_monQuantum,
           QStringLiteral("Current PipeWire quantum (frames per processing cycle)."));

    m_monRate = new QLabel(QStringLiteral("waiting for audio..."), page);
    m_monRate->setObjectName(QStringLiteral("monRate"));
    addRow(QStringLiteral("Sample rate"), m_monRate,
           QStringLiteral("Current node sample rate in Hz."));

    m_monLoad = new LoadHistogram(page);
    m_monLoad->setObjectName(QStringLiteral("monLoad"));
    addRow(QStringLiteral("DSP load"), m_monLoad,
           QStringLiteral("Share of each audio cycle the node spends processing "
                          "(busy/quantum), shown as a rolling history. Sustained high "
                          "values risk dropouts."));

    m_monXruns = new QLabel(QStringLiteral("waiting for audio..."), page);
    m_monXruns->setObjectName(QStringLiteral("monXruns"));
    addRow(QStringLiteral("Xruns"), m_monXruns,
           QStringLiteral("Number of buffer under/overruns (dropouts) reported for this "
                          "node since it started."));

    m_monState = new QLabel(QStringLiteral("waiting for audio..."), page);
    m_monState->setObjectName(QStringLiteral("monState"));
    addRow(QStringLiteral("State"), m_monState,
           QStringLiteral("PipeWire node state: R running, I idle, S suspended, E error."));

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
    auto *layout = new QVBoxLayout(page);
    layout->setAlignment(Qt::AlignTop);

    auto *title     = new QLabel(QStringLiteral("PipeASIO"), page);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.7);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *version = new QLabel(QStringLiteral("Version " PIPEASIO_VERSION), page);
    version->setStyleSheet(QStringLiteral("color: gray;"));
    layout->addWidget(version);

    auto *desc = new QLabel(
            QStringLiteral("A PipeWire-native ASIO driver for Wine and Proton. It gives "
                           "Windows music software fast, low-latency audio on Linux, routed "
                           "straight into PipeWire."),
            page);
    desc->setWordWrap(true);
    layout->addWidget(desc);

    layout->addSpacing(10);

    auto *links = new QLabel(page);
    links->setTextFormat(Qt::RichText);
    links->setOpenExternalLinks(true);
    links->setText(QStringLiteral(
            "<a href=\"https://m0n7y5.github.io/pipeasio/\">Website &amp; documentation</a><br>"
            "<a href=\"https://github.com/M0n7y5/pipeasio\">Source code on GitHub</a><br>"
            "<a href=\"https://github.com/M0n7y5/pipeasio/issues\">Report an issue</a><br>"
            "<a href=\"https://ko-fi.com/m0n7y5\">Support development on Ko-fi</a>"));
    layout->addWidget(links);

    layout->addSpacing(10);

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

    layout->addStretch(1);
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
    const int buffer = currentBufferSize();
    const int sr     = currentSampleRate();
    /* "Follow PipeWire" (0): use the graph's actual clock rate when pw-dump
     * resolved it, so the readout doesn't lie at non-48k rates (issue #20). */
    const int    rate = sr > 0 ? sr : (m_graphRate > 0 ? m_graphRate : 48000);
    const double ms   = buffer * 1000.0 / rate;
    m_latency->setText(QString::number(ms, 'f', 1) + QStringLiteral(" ms"));
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
    /* Polling a live graph has one-frame artifacts: the node vanishes from a
     * pw-top iteration while restaging, and a brief idle cycle reports
     * quantum/rate 0. Hold the last good frame across the first transient of
     * either kind. A persistent absence reverts to the waiting state (and
     * clears the device rows). A persistent idle/suspended node renders its
     * actual I/S state. */
    if (!stats.found)
    {
        ++m_monMisses;
        m_monIdleFrames = 0;
        if (m_monHasData && m_monMisses < 2)
            return; /* transient: keep the previous frame */
        const QString waiting = QStringLiteral("waiting for audio...");
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
