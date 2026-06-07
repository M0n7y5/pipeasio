/*
 * SettingsDialog.cpp — implementation.
 *
 * Copyright (C) 2026 PipeASIO contributors
 */
#include "SettingsDialog.hpp"

#include "Config.hpp"
#include "DeviceEnumerator.hpp"

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cmath>

namespace {

const int kBufferSizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};

struct SampleRateItem {
    const char *label;
    int value;
};
const SampleRateItem kSampleRates[] = {
    {"Follow PipeWire", 0}, {"44100", 44100}, {"48000", 48000},
    {"88200", 88200},       {"96000", 96000}, {"192000", 192000},
};

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("PipeASIO Settings"));

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildSettingsTab(), QStringLiteral("Settings"));
    tabs->addTab(buildMonitorTab(), QStringLiteral("Monitor"));

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
        QDialogButtonBox::RestoreDefaults, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults),
            &QPushButton::clicked, this, &SettingsDialog::onRestoreDefaults);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);

    /* Populate device combos before applying the saved config so the saved
     * device names can be matched and selected. */
    const QList<DeviceEnumerator::Device> devices = DeviceEnumerator::enumerate();
    m_outputDevice->addItem(QStringLiteral("Follow default"), QString());
    m_inputDevice->addItem(QStringLiteral("Follow default"), QString());
    for (const DeviceEnumerator::Device &d : devices) {
        const QString label = d.description.isEmpty() ? d.name : d.description;
        if (d.isSink)
            m_outputDevice->addItem(label, d.name);
        else
            m_inputDevice->addItem(label, d.name);
    }

    const pipeasio_config cfg = Config::load();
    applyConfig(cfg);

    connect(&m_monitor, &PipeWireMonitor::updated, this,
            &SettingsDialog::onMonitorUpdated);
    const QString target =
        (cfg.node_name[0] == '\0') ? QStringLiteral("PipeASIO")
                                   : QString::fromUtf8(cfg.node_name);
    m_monitor.setTarget(target);
    m_monitor.start();
}

QWidget *SettingsDialog::buildSettingsTab()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_inputs = new QSpinBox(page);
    m_inputs->setRange(0, 256);
    form->addRow(QStringLiteral("Inputs"), m_inputs);

    m_outputs = new QSpinBox(page);
    m_outputs->setRange(0, 256);
    form->addRow(QStringLiteral("Outputs"), m_outputs);

    m_bufferSize = new QComboBox(page);
    for (int sz : kBufferSizes)
        m_bufferSize->addItem(QString::number(sz), sz);
    form->addRow(QStringLiteral("Buffer size"), m_bufferSize);

    m_latency = new QLabel(page);
    form->addRow(QStringLiteral("Latency"), m_latency);

    m_sampleRate = new QComboBox(page);
    for (const SampleRateItem &it : kSampleRates)
        m_sampleRate->addItem(QString::fromUtf8(it.label), it.value);
    form->addRow(QStringLiteral("Sample rate"), m_sampleRate);

    m_outputDevice = new QComboBox(page);
    form->addRow(QStringLiteral("Output device"), m_outputDevice);

    m_inputDevice = new QComboBox(page);
    form->addRow(QStringLiteral("Input device"), m_inputDevice);

    m_autoConnect = new QCheckBox(page);
    form->addRow(QStringLiteral("Auto-connect"), m_autoConnect);

    m_fixedBuffer = new QCheckBox(page);
    form->addRow(QStringLiteral("Fixed buffer size"), m_fixedBuffer);

    m_nodeName = new QLineEdit(page);
    m_nodeName->setPlaceholderText(QStringLiteral("(derive from application name)"));
    form->addRow(QStringLiteral("Node name"), m_nodeName);

    connect(m_bufferSize, &QComboBox::currentIndexChanged, this,
            &SettingsDialog::updateLatencyLabel);
    connect(m_sampleRate, &QComboBox::currentIndexChanged, this,
            &SettingsDialog::updateLatencyLabel);

    return page;
}

QWidget *SettingsDialog::buildMonitorTab()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_monQuantum = new QLabel(QStringLiteral("waiting for audio..."), page);
    form->addRow(QStringLiteral("Buffer / quantum"), m_monQuantum);

    m_monRate = new QLabel(QStringLiteral("waiting for audio..."), page);
    form->addRow(QStringLiteral("Sample rate"), m_monRate);

    m_monLoad = new QProgressBar(page);
    m_monLoad->setRange(0, 100);
    m_monLoad->setValue(0);
    form->addRow(QStringLiteral("DSP load"), m_monLoad);

    m_monXruns = new QLabel(QStringLiteral("waiting for audio..."), page);
    form->addRow(QStringLiteral("Xruns"), m_monXruns);

    m_monState = new QLabel(QStringLiteral("waiting for audio..."), page);
    form->addRow(QStringLiteral("State"), m_monState);

    return page;
}

int SettingsDialog::currentBufferSize() const
{
    return m_bufferSize->currentData().toInt();
}

int SettingsDialog::currentSampleRate() const
{
    return m_sampleRate->currentData().toInt();
}

void SettingsDialog::updateLatencyLabel()
{
    const int buffer = currentBufferSize();
    const int sr = currentSampleRate();
    const int rate = sr > 0 ? sr : 48000;
    const double ms = buffer * 1000.0 / rate;
    m_latency->setText(QString::number(ms, 'f', 1) + QStringLiteral(" ms"));
}

void SettingsDialog::applyConfig(const pipeasio_config &c)
{
    m_inputs->setValue(c.inputs);
    m_outputs->setValue(c.outputs);

    int bufIdx = m_bufferSize->findData(c.buffer_size);
    m_bufferSize->setCurrentIndex(bufIdx >= 0 ? bufIdx : 0);

    int srIdx = m_sampleRate->findData(c.sample_rate);
    m_sampleRate->setCurrentIndex(srIdx >= 0 ? srIdx : 0);

    const QString out = QString::fromUtf8(c.output_device);
    int outIdx = m_outputDevice->findData(out);
    m_outputDevice->setCurrentIndex(outIdx >= 0 ? outIdx : 0);

    const QString in = QString::fromUtf8(c.input_device);
    int inIdx = m_inputDevice->findData(in);
    m_inputDevice->setCurrentIndex(inIdx >= 0 ? inIdx : 0);

    m_autoConnect->setChecked(c.auto_connect);
    m_fixedBuffer->setChecked(c.fixed_buffer_size);
    m_nodeName->setText(QString::fromUtf8(c.node_name));

    updateLatencyLabel();
}

void SettingsDialog::onRestoreDefaults()
{
    applyConfig(Config::defaults());
}

void SettingsDialog::onAccept()
{
    pipeasio_config cfg = Config::defaults();
    cfg.inputs = m_inputs->value();
    cfg.outputs = m_outputs->value();
    cfg.buffer_size = currentBufferSize();
    cfg.fixed_buffer_size = m_fixedBuffer->isChecked();
    cfg.sample_rate = currentSampleRate();
    cfg.auto_connect = m_autoConnect->isChecked();

    const QByteArray out = m_outputDevice->currentData().toString().toUtf8();
    qstrncpy(cfg.output_device, out.constData(), sizeof(cfg.output_device));
    const QByteArray in = m_inputDevice->currentData().toString().toUtf8();
    qstrncpy(cfg.input_device, in.constData(), sizeof(cfg.input_device));
    const QByteArray node = m_nodeName->text().trimmed().toUtf8();
    qstrncpy(cfg.node_name, node.constData(), sizeof(cfg.node_name));

    Config::save(cfg);
    accept();
}

void SettingsDialog::onMonitorUpdated(const NodeStats &stats)
{
    if (!stats.found) {
        const QString waiting = QStringLiteral("waiting for audio...");
        m_monQuantum->setText(waiting);
        m_monRate->setText(waiting);
        m_monXruns->setText(waiting);
        m_monState->setText(waiting);
        m_monLoad->setValue(0);
        return;
    }

    m_monQuantum->setText(QString::number(stats.quantum));
    m_monRate->setText(QString::number(stats.rate) + QStringLiteral(" Hz"));
    m_monXruns->setText(QString::number(stats.xruns));
    m_monState->setText(stats.state);
    m_monLoad->setValue(static_cast<int>(std::lround(stats.dspLoad * 100.0)));
}
