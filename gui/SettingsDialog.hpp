/*
 * SettingsDialog.hpp - the PipeASIO settings panel window.
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
#pragma once

#include <QDialog>

#include "PipeWireGraph.hpp"
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
    void onCfgWatch(); /* config file changed on disk => reload (unless dirty) */
    void markDirty();  /* user touched a setting: pause the file watch */

  private:
    QWidget *buildSettingsTab();
    QWidget *buildMonitorTab();
    QWidget *buildAboutTab();
    void     applyConfig(const struct pipeasio_config &c);
    void     updateLatencyLabel();
    void     refreshDevices(); /* repopulate device combos from the live graph */
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
    QSpinBox  *m_rtPriority        = nullptr;

    /* Monitor widgets */
    QLabel        *m_monQuantum = nullptr;
    QLabel        *m_monRate    = nullptr;
    LoadHistogram *m_monLoad    = nullptr;
    QLabel        *m_monXruns   = nullptr;
    QLabel        *m_monState   = nullptr;
    QLabel        *m_monOutput  = nullptr;
    QLabel        *m_monInput   = nullptr;

    PipeWireGraph   m_graph; /* destroyed first; m_monitor only holds a pointer */
    PipeWireMonitor m_monitor;

    /* Config file watch: reload when the file changes on disk (driver's
     * ControlPanel or an external editor), unless the user has unsaved edits. */
    class QTimer *m_cfgWatch = nullptr;
    QString       m_cfgFp;
    bool          m_dirty    = false;
    bool          m_applying = false; /* applyConfig in progress (not user edit) */
};
