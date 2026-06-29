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

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("PipeASIO Settings - " PIPEASIO_VERSION));

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildSettingsTab(), QStringLiteral("Settings"));
    tabs->addTab(buildMonitorTab(), QStringLiteral("Monitor"));
    tabs->addTab(buildAboutTab(), QStringLiteral("About"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel
                                                 | QDialogButtonBox::RestoreDefaults,
                                         this);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            &SettingsDialog::onApply);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
            &SettingsDialog::onRestoreDefaults);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);

    /* Populate device combos before applying the saved config so the saved
     * device names can be matched and selected. */
    const QList<DeviceEnumerator::Device> devices = DeviceEnumerator::enumerate();
    m_outputDevice->addItem(QStringLiteral("Follow default"), QString());
    m_inputDevice->addItem(QStringLiteral("Follow default"), QString());
    for (const DeviceEnumerator::Device &d : devices)
    {
        const QString label = d.description.isEmpty() ? d.name : d.description;
        if (d.isSink)
            m_outputDevice->addItem(label, d.name);
        else
            m_inputDevice->addItem(label, d.name);
    }

    const pipeasio_config cfg = Config::load();
    applyConfig(cfg);

    connect(&m_monitor, &PipeWireMonitor::updated, this, &SettingsDialog::onMonitorUpdated);
    /* Empty target => the monitor auto-discovers our node via the driver's
     * "pipeasio.node" marker (the host names the node after its own exe). */
    m_monitor.setTarget(cfg.node_name[0] ? QString::fromUtf8(cfg.node_name) : QString());
    m_monitor.start();
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
        field->setToolTip(tip);
        auto *lbl = new QLabel(label, page);
        lbl->setToolTip(tip);
        form->addRow(lbl, field);
    };

    m_inputs = new QSpinBox(page);
    m_inputs->setRange(0, 256);
    addRow(QStringLiteral("Inputs"), m_inputs,
           QStringLiteral("Number of capture (input) channels the driver exposes to the "
                          "host. Applied when the driver next (re)starts."));

    m_outputs = new QSpinBox(page);
    m_outputs->setRange(0, 256);
    addRow(QStringLiteral("Outputs"), m_outputs,
           QStringLiteral("Number of playback (output) channels the driver exposes to the "
                          "host. Applied when the driver next (re)starts."));

    m_bufferSize = new QComboBox(page);
    for (int sz : kBufferSizes)
        m_bufferSize->addItem(QString::number(sz), sz);
    addRow(QStringLiteral("Buffer size"), m_bufferSize,
           QStringLiteral("Preferred buffer size in frames. Smaller means lower latency but "
                          "more CPU and a higher risk of dropouts (xruns)."));

    m_latency = new QLabel(page);
    addRow(QStringLiteral("Latency"), m_latency,
           QStringLiteral("Length of one buffer (buffer size / sample rate) - the driver's "
                          "approximate one-way latency. Read-only."));

    m_sampleRate = new QComboBox(page);
    for (const SampleRateItem &it : kSampleRates)
        m_sampleRate->addItem(QString::fromUtf8(it.label), it.value);
    addRow(QStringLiteral("Sample rate"), m_sampleRate,
           QStringLiteral("\"Follow PipeWire\" uses the graph's current rate; a fixed value "
                          "pins the rate. Prefer Follow unless the host needs a specific rate."));

    m_outputDevice = new QComboBox(page);
    addRow(QStringLiteral("Output device"), m_outputDevice,
           QStringLiteral("PipeWire sink to auto-connect outputs to. \"Follow default\" "
                          "tracks the system default sink (e.g. when you switch to Bluetooth)."));

    m_inputDevice = new QComboBox(page);
    addRow(QStringLiteral("Input device"), m_inputDevice,
           QStringLiteral("PipeWire source to auto-connect inputs from. \"Follow default\" "
                          "tracks the system default source."));

    m_autoConnect = new QCheckBox(page);
    addRow(QStringLiteral("Auto-connect"), m_autoConnect,
           QStringLiteral("Automatically connect the driver's ports to the selected (or "
                          "default) device. Turn off to wire connections yourself."));

    m_fixedBuffer = new QCheckBox(page);
    addRow(QStringLiteral("Fixed buffer size"), m_fixedBuffer,
           QStringLiteral("When on, PipeWire controls the buffer size and the host cannot "
                          "change it. When off, the host may set PipeWire's quantum."));

    m_followDeviceClock = new QCheckBox(page);
    addRow(QStringLiteral("Follow device clock (Bluetooth)"), m_followDeviceClock,
           QStringLiteral("Follow the target device's clock instead of forcing the graph "
                          "quantum. Required for Bluetooth sinks (their clock can't be "
                          "slaved); raises latency. Leave off for wired low-latency output."));

    m_nodeName = new QLineEdit(page);
    m_nodeName->setPlaceholderText(QStringLiteral("(derive from application name)"));
    addRow(QStringLiteral("Node name"), m_nodeName,
           QStringLiteral("Override the PipeWire node/client name. Empty derives it from the "
                          "host application's name."));

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
        field->setToolTip(tip);
        auto *lbl = new QLabel(label, page);
        lbl->setToolTip(tip);
        form->addRow(lbl, field);
    };

    m_monQuantum = new QLabel(QStringLiteral("waiting for audio..."), page);
    addRow(QStringLiteral("Buffer / quantum"), m_monQuantum,
           QStringLiteral("PipeWire processing block size (frames per cycle) the driver's "
                          "node is currently running at."));

    m_monRate = new QLabel(QStringLiteral("waiting for audio..."), page);
    addRow(QStringLiteral("Sample rate"), m_monRate,
           QStringLiteral("Sample rate the driver's node is currently running at."));

    m_monLoad = new LoadHistogram(page);
    addRow(QStringLiteral("DSP load"), m_monLoad,
           QStringLiteral("Rolling history of the share of each audio cycle this node "
                          "spends processing (busy/quantum). Sustained high values risk "
                          "dropouts."));

    m_monXruns = new QLabel(QStringLiteral("waiting for audio..."), page);
    addRow(QStringLiteral("Xruns"), m_monXruns,
           QStringLiteral("Number of buffer under/overruns (dropouts) reported for this "
                          "node since it started."));

    m_monState = new QLabel(QStringLiteral("waiting for audio..."), page);
    addRow(QStringLiteral("State"), m_monState,
           QStringLiteral("PipeWire node state: R running, I idle, S suspended, E error."));

    m_monOutput = new QLabel(QStringLiteral("—"), page);
    m_monOutput->setWordWrap(true);
    addRow(QStringLiteral("Output device"), m_monOutput,
           QStringLiteral("The sink the driver's output ports currently feed, with its live "
                          "format and state (and Bluetooth codec when applicable)."));

    m_monInput = new QLabel(QStringLiteral("—"), page);
    m_monInput->setWordWrap(true);
    addRow(QStringLiteral("Input device"), m_monInput,
           QStringLiteral("The source currently feeding the driver's input ports, with its "
                          "live format and state (and Bluetooth codec when applicable)."));

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
            "<a href=\"https://github.com/M0n7y5/pipeasio/issues\">Report an issue</a>"));
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
    const int    buffer = currentBufferSize();
    const int    sr     = currentSampleRate();
    const int    rate   = sr > 0 ? sr : 48000;
    const double ms     = buffer * 1000.0 / rate;
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

    const QString out    = QString::fromUtf8(c.output_device);
    int           outIdx = m_outputDevice->findData(out);
    if (outIdx < 0 && !out.isEmpty())
    {
        m_outputDevice->addItem(out + QStringLiteral(" (unavailable)"), out);
        outIdx = m_outputDevice->count() - 1;
    }
    m_outputDevice->setCurrentIndex(outIdx >= 0 ? outIdx : 0);

    const QString in    = QString::fromUtf8(c.input_device);
    int           inIdx = m_inputDevice->findData(in);
    if (inIdx < 0 && !in.isEmpty())
    {
        m_inputDevice->addItem(in + QStringLiteral(" (unavailable)"), in);
        inIdx = m_inputDevice->count() - 1;
    }
    m_inputDevice->setCurrentIndex(inIdx >= 0 ? inIdx : 0);

    m_autoConnect->setChecked(c.auto_connect);
    m_fixedBuffer->setChecked(c.fixed_buffer_size);
    m_followDeviceClock->setChecked(c.follow_device_clock);
    m_nodeName->setText(QString::fromUtf8(c.node_name));

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

    const QByteArray out = m_outputDevice->currentData().toString().toUtf8();
    qstrncpy(cfg.output_device, out.constData(), sizeof(cfg.output_device));
    const QByteArray in = m_inputDevice->currentData().toString().toUtf8();
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
    m_monOutput->setText(formatDevice(stats.outputDevice, stats.outputDeviceDetail));
    m_monInput->setText(formatDevice(stats.inputDevice, stats.inputDeviceDetail));

    if (!stats.found)
    {
        const QString waiting = QStringLiteral("waiting for audio...");
        m_monQuantum->setText(waiting);
        m_monRate->setText(waiting);
        m_monXruns->setText(waiting);
        m_monState->setText(waiting);
        m_monLoad->setWaiting();
        return;
    }

    m_monQuantum->setText(QString::number(stats.quantum));
    m_monRate->setText(QString::number(stats.rate) + QStringLiteral(" Hz"));
    m_monXruns->setText(QString::number(stats.xruns));
    m_monState->setText(stats.state);
    m_monLoad->pushSample(stats.dspLoad);
}
