/*
 * PipeWireMonitor.hpp - live telemetry for the Monitor tab.
 *
 * Node stats (quantum/rate/DSP load/xruns/state) come from a long-lived
 * `pw-top -b` subprocess; parsePwTop() is PURE (operates on captured output)
 * so it is unit testable.  Graph topology (our node discovery and its
 * connections) comes from PipeWireGraph's event-driven registry listener -
 * no pw-dump polling.
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

#include "PipeWireGraph.hpp"

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
    QString inputDevice;        /* source feeding our inputs (Monitor tab) */
    QString outputDevice;       /* sink our outputs feed (Monitor tab) */
    QString inputDeviceDetail;  /* codec/format/state line for the input */
    QString outputDeviceDetail; /* codec/format/state line for the output */
};

/* Parse `pw-top -b` output, returning stats for the NEWEST data row whose
 * trailing NAME contains `nodeNameSubstr` (scans backwards; a trailing
 * header-only iteration from block-buffered streaming is skipped). Pure. */
NodeStats parsePwTop(const QByteArray &out, const QString &nodeNameSubstr);

class PipeWireMonitor : public QObject
{
    Q_OBJECT
  public:
    explicit PipeWireMonitor(QObject *parent = nullptr);
    ~PipeWireMonitor() override;

    void setTarget(const QString &nodeNameSubstr);
    /* Live graph for node discovery + connection tracking; its changed()
     * signal re-resolves everything event-driven. */
    void setGraph(PipeWireGraph *graph);

  public slots:
    void start();
    void stop();

  signals:
    void updated(const NodeStats &stats);

  private slots:
    void ensureTopRunning(); /* watchdog: (re)start pw-top if it died */
    void onTopReadyRead();
    void refreshFromGraph(); /* graph changed: re-resolve node + connections */

  private:
    QTimer        *m_watchdog; /* restarts pw-top, no per-tick respawn */
    QString        m_target;
    bool           m_autoDiscover;     /* true => resolve our node via the marker prop */
    PipeWireGraph *m_graph   = nullptr;
    QProcess      *m_topProc = nullptr; /* long-lived `pw-top -b` */
    QByteArray     m_topBuf;            /* unparsed pw-top stdout tail */
    NodeStats      m_lastStats;         /* last pw-top parse (conn fields aside) */
    QString        m_connInput;         /* last-resolved Monitor connections */
    QString        m_connOutput;
    QString        m_connInputDetail;   /* second-line detail for the Monitor rows */
    QString        m_connOutputDetail;
};
