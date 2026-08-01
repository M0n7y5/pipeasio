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

#include "PipeWireMonitor.hpp"
#include "DeviceEnumerator.hpp"
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class LoadHistogram;
class QSpinBox;

struct SettingsDialogOptions
{
    DeviceEnumerator::RequestOptions deviceRequest;
    bool                             monitorEnabled = true;
};

class SettingsDialog : public QDialog
{
    Q_OBJECT
  public:
    explicit SettingsDialog(QWidget *parent = nullptr, SettingsDialogOptions options = {});

  private slots:
    void onApply();
    void onRestoreDefaults();
    void onMonitorUpdated(const NodeStats &stats);
    void onDevicesEnumerated(bool success, const QList<DeviceEnumerator::Device> &devices,
                             const QString &error);

  private:
    QWidget *buildSettingsTab();
    QWidget *buildMonitorTab();
    QWidget *buildAboutTab();
    void     applyConfig(const struct pipeasio_config &c);
    void     updateLatencyLabel();
    int      currentBufferSize() const;
    int      currentSampleRate() const;

    /* Settings widgets */
    QSpinBox                  *m_inputs            = nullptr;
    QSpinBox                  *m_outputs           = nullptr;
    QComboBox                 *m_bufferSize        = nullptr;
    QLabel                    *m_latency           = nullptr;
    QComboBox                 *m_sampleRate        = nullptr;
    QComboBox                 *m_outputDevice      = nullptr;
    QComboBox                 *m_inputDevice       = nullptr;
    QCheckBox                 *m_autoConnect       = nullptr;
    QCheckBox                 *m_fixedBuffer       = nullptr;
    QCheckBox                 *m_followDeviceClock = nullptr;
    QCheckBox                 *m_realtime          = nullptr;
    QLineEdit                 *m_nodeName          = nullptr;
    DeviceEnumerator::Request *m_deviceRequest     = nullptr;
    bool                       m_devicesLoading    = true;
    QString                    m_pendingOutputDevice;
    QString                    m_pendingInputDevice;

    /* Monitor widgets */
    QLabel        *m_monQuantum    = nullptr;
    QLabel        *m_monRate       = nullptr;
    LoadHistogram *m_monLoad       = nullptr;
    QLabel        *m_monXruns      = nullptr;
    QLabel        *m_monState      = nullptr;
    QLabel        *m_monOutput     = nullptr;
    QLabel        *m_monInput      = nullptr;
    int            m_monMisses     = 0;     /* consecutive samples missing our node */
    int            m_monIdleFrames = 0;     /* consecutive 0/0 (idle) samples */
    bool           m_monHasData    = false; /* a good sample was rendered at least once */

    PipeWireMonitor m_monitor;
};
