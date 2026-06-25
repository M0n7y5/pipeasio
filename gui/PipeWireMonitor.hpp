/*
 * PipeWireMonitor.hpp - live telemetry for the Monitor tab via `pw-top`.
 *
 * parsePwTop() is PURE (operates on captured output) so it is unit testable.
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

#include <QByteArray>
#include <QObject>
#include <QString>

class QTimer;
class QProcess;

struct NodeStats
{
    bool    found = false;
    QString name;
    int     quantum = 0;
    int     rate    = 0;
    double  dspLoad = 0.0;
    long    xruns   = 0;
    QString state;
};

/* Parse `pw-top -b -n 1` output, returning stats for the first data row whose
 * trailing NAME contains `nodeNameSubstr`. Pure. */
NodeStats parsePwTop(const QByteArray &out, const QString &nodeNameSubstr);

class PipeWireMonitor : public QObject
{
    Q_OBJECT
  public:
    explicit PipeWireMonitor(QObject *parent = nullptr);
    ~PipeWireMonitor() override;

    void setTarget(const QString &nodeNameSubstr);

  public slots:
    void start();
    void stop();

  signals:
    void updated(const NodeStats &stats);

  private slots:
    void poll();
    void onTopFinished();
    void onDumpFinished();

  private:
    void startTop();
    void startDump();

    QTimer    *m_timer;
    QString    m_target;
    bool       m_autoDiscover;   /* true => resolve our node via the marker prop */
    QProcess  *m_proc = nullptr; /* in-flight child (pw-top or pw-dump) */
    bool       m_busy = false;   /* a poll cycle is in flight */
    QByteArray m_lastTop;        /* last pw-top output, re-parsed after discovery */
};
