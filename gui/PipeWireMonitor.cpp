/*
 * PipeWireMonitor.cpp - implementation (all data via PipeWireGraph).
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
#include "PipeWireMonitor.hpp"

PipeWireMonitor::PipeWireMonitor(QObject *parent) : QObject(parent)
{
}

PipeWireMonitor::~PipeWireMonitor() = default;

void
PipeWireMonitor::setTarget(const QString &nodeName)
{
    /* An explicit (configured) name disables auto-discovery; empty re-enables it. */
    m_target       = nodeName;
    m_autoDiscover = nodeName.isEmpty();
}

void
PipeWireMonitor::setGraph(PipeWireGraph *graph)
{
    m_graph = graph;
    connect(m_graph, &PipeWireGraph::changed, this, &PipeWireMonitor::refreshFromGraph);
}

void
PipeWireMonitor::start()
{
    refreshFromGraph();
}

void
PipeWireMonitor::stop()
{
}

void
PipeWireMonitor::refreshFromGraph()
{
    if (!m_graph)
        return;

    /* Empty target => auto-discover our node via the driver's "pipeasio.node"
     * marker (the host names the node after its own exe). */
    QString name = m_target;
    if (m_autoDiscover)
    {
        const QString own = m_graph->ownNodeName();
        if (!own.isEmpty())
            name = own;
    }

    const PipeWireGraph::ProfilerStats prof
            = m_graph->profilerStats(m_autoDiscover ? QString() : m_target);
    const PipeWireGraph::Connections conn = m_graph->ownConnections();

    NodeStats st;
    st.found              = prof.found;
    st.name               = name;
    st.quantum            = prof.quantum;
    st.rate               = prof.rate;
    st.dspLoad            = prof.dspLoad;
    st.xruns              = prof.xruns;
    st.state              = prof.state;
    st.inputDevice        = conn.input;
    st.inputDeviceDetail  = conn.inputDetail;
    st.outputDevice       = conn.output;
    st.outputDeviceDetail = conn.outputDetail;
    emit updated(st);
}
