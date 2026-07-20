/*
 * PipeWireMonitor.hpp - live telemetry for the Monitor tab.
 *
 * Everything comes from PipeWireGraph's event-driven PipeWire connection:
 * node discovery and connections via the registry listener, DSP stats
 * (quantum/rate/load/xruns/state) via the daemon's Profiler interface.
 * No subprocesses, no text parsing.
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

#include <QObject>
#include <QString>

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

class PipeWireMonitor : public QObject
{
    Q_OBJECT
  public:
    explicit PipeWireMonitor(QObject *parent = nullptr);
    ~PipeWireMonitor() override;

    void setTarget(const QString &nodeName);
    /* Live graph for discovery, connections and profiler stats; its
     * changed() signal re-resolves everything event-driven. */
    void setGraph(PipeWireGraph *graph);

  public slots:
    void start();
    void stop();

  signals:
    void updated(const NodeStats &stats);

  private slots:
    void refreshFromGraph(); /* graph changed: re-resolve stats + connections */

  private:
    QString        m_target;
    bool           m_autoDiscover = true; /* true => resolve our node via the marker */
    PipeWireGraph *m_graph        = nullptr;
};
