/*
 * SettingsDialog.hpp — the PipeASIO settings panel window.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING.GUI for the full license text.
 */
#pragma once

#include <QDialog>

#include "PipeWireMonitor.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class LoadHistogram;
class QSpinBox;

class SettingsDialog : public QDialog
{
    Q_OBJECT
  public:
    explicit SettingsDialog(QWidget *parent = nullptr);

  private slots:
    void onApply();
    void onRestoreDefaults();
    void onMonitorUpdated(const NodeStats &stats);

  private:
    QWidget *buildSettingsTab();
    QWidget *buildMonitorTab();
    void     applyConfig(const struct pipeasio_config &c);
    void     updateLatencyLabel();
    int      currentBufferSize() const;
    int      currentSampleRate() const;

    /* Settings widgets */
    QSpinBox  *m_inputs            = nullptr;
    QSpinBox  *m_outputs           = nullptr;
    QComboBox *m_bufferSize        = nullptr;
    QLabel    *m_latency           = nullptr;
    QComboBox *m_sampleRate        = nullptr;
    QComboBox *m_outputDevice      = nullptr;
    QComboBox *m_inputDevice       = nullptr;
    QCheckBox *m_autoConnect       = nullptr;
    QCheckBox *m_fixedBuffer       = nullptr;
    QCheckBox *m_followDeviceClock = nullptr;
    QLineEdit *m_nodeName          = nullptr;

    /* Monitor widgets */
    QLabel        *m_monQuantum = nullptr;
    QLabel        *m_monRate    = nullptr;
    LoadHistogram *m_monLoad    = nullptr;
    QLabel        *m_monXruns   = nullptr;
    QLabel        *m_monState   = nullptr;
    QLabel        *m_monRt      = nullptr;

    PipeWireMonitor m_monitor;
};
