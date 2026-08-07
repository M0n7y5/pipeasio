/*
 * PipeWireMonitor.hpp - live telemetry for the Monitor tab.
 *
 * A native libpipewire client: it binds the graph's Profiler global and is
 * pushed one timing point per audio cycle, the same source `pw-top` reads.
 * Names, states, formats and links come from the registry on that connection.
 * The loop runs on the Qt event loop via a QSocketNotifier on pw_loop_get_fd(),
 * so there is no second thread and no locking.
 *
 * describePeer() and resolveConnections() are PURE so they stay unit testable
 * without a running daemon.
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

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>

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

    /* Non-empty when no telemetry can be collected at all (no daemon, or no
     * Profiler interface). Shown instead of "waiting for audio...". */
    QString unavailable;
};

/* Everything the Monitor tab needs about one peer node in the graph. */
struct PeerInfo
{
    QString description;       /* node.description */
    QString nick;              /* node.nick */
    QString name;              /* node.name */
    QString btCodec;           /* api.bluez5.codec, empty when not Bluetooth */
    bool    bluetooth = false; /* device.api == bluez5 */
    int     rate      = 0;     /* negotiated Format rate, 0 when suspended */
    int     channels  = 0;
    QString sampleFormat;
    QString state; /* "running" / "idle" / "suspended" / "error" */
};

/* Split a peer into the Monitor row's display name and its detail line: codec
 * (Bluetooth only) / negotiated rate / channels+format / state. Attributes the
 * graph does not expose are dropped (a suspended device has no Format). Pure. */
void describePeer(const PeerInfo &peer, QString *name, QString *detail);

struct GraphLink
{
    uint32_t outputNode = 0;
    uint32_t inputNode  = 0;
};

/* What our own filter node is wired to. Each side's `*Detail` carries the
 * peer's codec/format/state for a second display line (empty when several
 * peers share a side and the name string already lists them). Empty name ==
 * nothing connected. */
struct Connections
{
    QString output;       /* sink name(s) our outputs feed */
    QString outputDetail; /* codec / rate / channels / state of a single sink */
    QString input;        /* source name(s) feeding our inputs */
    QString inputDetail;
};

/* Resolve `ownId`'s peers: a link FROM our node lands on a sink we play to, a
 * link TO our node comes from a source we capture from. Distinct peers, link
 * order kept. Pure. */
Connections resolveConnections(uint32_t ownId, const QHash<uint32_t, PeerInfo> &nodes,
                               const QVector<GraphLink> &links);

class PipeWireMonitor : public QObject
{
    Q_OBJECT
  public:
    explicit PipeWireMonitor(QObject *parent = nullptr);
    ~PipeWireMonitor() override;

    /* Substring of the node.name to watch. Empty re-enables auto-discovery via
     * the driver's "pipeasio.node" marker property. */
    void setTarget(const QString &nodeNameSubstr);

    /* Opaque, defined in the .cpp. Public only so the file-local NodeRecord
     * can refer to it. */
    struct Impl;

  public slots:
    void start();
    void stop();

  signals:
    void updated(const NodeStats &stats);

  private:
    std::unique_ptr<Impl> d;
};
