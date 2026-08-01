/*
 * PipeWireMonitor.cpp - implementation.
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
#include "DeviceEnumerator.hpp"

#include <QProcess>
#include <QStringList>
#include <QRegularExpression>
#include <QTimer>

#include <cmath>

namespace
{

/* pw-dump cadence: refresh the connected sink/source every Nth poll. pw-top
 * runs every poll for fast stats. Graph topology changes rarely. */
constexpr int kDumpEvery = 5;

int
intOrZero(const QString &tok)
{
    if (tok == QLatin1String("---") || tok == QLatin1String("???"))
        return 0;
    bool      ok = false;
    const int v  = tok.toInt(&ok);
    return ok ? v : 0;
}

double
doubleOrZero(const QString &tok)
{
    if (tok == QLatin1String("---") || tok == QLatin1String("???"))
        return 0.0;
    /* pw-top renders decimals with the active locale; normalise a comma
     * decimal separator (e.g. "0,17") to a dot so toDouble() succeeds. */
    QString s = tok;
    s.replace(QLatin1Char(','), QLatin1Char('.'));
    bool         ok = false;
    const double v  = s.toDouble(&ok);
    return ok ? v : 0.0;
}

long
longOrZero(const QString &tok)
{
    if (tok == QLatin1String("---") || tok == QLatin1String("???"))
        return 0;
    bool       ok = false;
    const long v  = tok.toLong(&ok);
    return ok ? v : 0;
}

} // namespace

NodeStats
parsePwTop(const QByteArray &out, const QString &nodeNameSubstr)
{
    /* An empty target means our node hasn't been discovered yet.  Match
     * nothing (an empty substring is "contained" in every line) so the panel
     * shows "waiting for audio..." instead of a random row's stats. */
    if (nodeNameSubstr.isEmpty())
        return {};

    const QString     text  = QString::fromUtf8(out);
    const QStringList lines = text.split(QLatin1Char('\n'));

    /* In batch mode pw-top prints one full table per iteration, and the first
     * iteration is an all-zero baseline (per-cycle timings not measured yet).
     * Parse only the LAST iteration: locate the final header row, then scan
     * the rows after it. */
    int lastHeader = -1;
    for (int i = 0; i < lines.size(); ++i)
    {
        const QString &l = lines.at(i);
        if (l.contains(QLatin1String("ID")) && l.contains(QLatin1String("QUANT")))
            lastHeader = i;
    }

    for (int i = lastHeader + 1; i < lines.size(); ++i)
    {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;

        const QStringList tok
                = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        /* Fixed columns S ID QUANT RATE WAIT BUSY W/Q B/Q ERR occupy indices
         * 0..8. Everything from index 9 on is the variable-width FORMAT, an
         * optional +/=/* link marker, and the NAME (which may contain spaces).
         * Substring-match the configured node name against that trailing run. */
        if (tok.size() < 10)
            continue;

        const QString tail = QStringList(tok.mid(9)).join(QLatin1Char(' '));
        if (!tail.contains(nodeNameSubstr))
            continue;

        NodeStats st;
        st.found   = true;
        st.name    = nodeNameSubstr;
        st.state   = tok.at(0);
        st.quantum = intOrZero(tok.at(2));
        st.rate    = intOrZero(tok.at(3));
        st.dspLoad = doubleOrZero(tok.at(7));
        st.xruns   = longOrZero(tok.at(8));
        return st;
    }

    return {};
}

PipeWireMonitor::PipeWireMonitor(QObject *parent)
    : QObject(parent), m_timer(new QTimer(this)), m_autoDiscover(true)
{
    m_timer->setInterval(400);
    connect(m_timer, &QTimer::timeout, this, &PipeWireMonitor::poll);
}

PipeWireMonitor::~PipeWireMonitor() = default;

void
PipeWireMonitor::setTarget(const QString &nodeNameSubstr)
{
    /* An explicit (configured) name disables auto-discovery. Empty re-enables it. */
    m_target       = nodeNameSubstr;
    m_autoDiscover = nodeNameSubstr.isEmpty();
}

void
PipeWireMonitor::start()
{
    poll();
    m_timer->start();
}

void
PipeWireMonitor::stop()
{
    m_timer->stop();
    if (m_proc)
    {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_busy = false;
}

void
PipeWireMonitor::poll()
{
    if (m_busy) /* skip a tick while the previous cycle is still running */
        return;
    m_busy = true;
    startTop();
}

void
PipeWireMonitor::startTop()
{
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::finished, this, &PipeWireMonitor::onTopFinished);
    connect(m_proc, &QProcess::errorOccurred, this, &PipeWireMonitor::onTopFinished);
    m_proc->start(QStringLiteral("pw-top"),
                  { QStringLiteral("-b"), QStringLiteral("-n"), QStringLiteral("2") });
}

void
PipeWireMonitor::onTopFinished()
{
    if (!m_proc) /* guard the finished + errorOccurred double-fire */
        return;
    m_lastTop = m_proc->readAllStandardOutput();
    m_proc->deleteLater();
    m_proc = nullptr;

    const NodeStats st = parsePwTop(m_lastTop, m_target);
    /* Dump the graph when we still need to discover our node (the host names it
     * after its own exe and we resolve it via the "pipeasio.node" marker) OR when a
     * connection refresh is due. The same pw-dump yields the sink/source our
     * ports are linked to. Between dumps, reuse the cached connection strings. */
    ++m_pollsSinceDump;
    const bool needDiscover = m_autoDiscover && !st.found;
    const bool refreshConn  = m_pollsSinceDump >= kDumpEvery;
    if (!needDiscover && !refreshConn)
    {
        NodeStats out          = st;
        out.inputDevice        = m_connInput;
        out.outputDevice       = m_connOutput;
        out.inputDeviceDetail  = m_connInputDetail;
        out.outputDeviceDetail = m_connOutputDetail;
        emit updated(out);
        m_busy = false;
        return;
    }
    startDump();
}

void
PipeWireMonitor::startDump()
{
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::finished, this, &PipeWireMonitor::onDumpFinished);
    connect(m_proc, &QProcess::errorOccurred, this, &PipeWireMonitor::onDumpFinished);
    m_proc->start(QStringLiteral("pw-dump"), QStringList());
}

void
PipeWireMonitor::onDumpFinished()
{
    if (!m_proc)
        return;
    const QByteArray dump = m_proc->readAllStandardOutput();
    m_proc->deleteLater();
    m_proc = nullptr;

    if (m_autoDiscover)
    {
        const QString name = DeviceEnumerator::findOwnNode(dump);
        if (!name.isEmpty())
            m_target = name;
    }

    const DeviceEnumerator::Connections conn = DeviceEnumerator::resolveConnections(dump);
    m_connInput                              = conn.input;
    m_connInputDetail                        = conn.inputDetail;
    m_connOutput                             = conn.output;
    m_connOutputDetail                       = conn.outputDetail;
    m_pollsSinceDump                         = 0;

    NodeStats st          = parsePwTop(m_lastTop, m_target);
    st.inputDevice        = m_connInput;
    st.inputDeviceDetail  = m_connInputDetail;
    st.outputDevice       = m_connOutput;
    st.outputDeviceDetail = m_connOutputDetail;
    emit updated(st);
    m_busy = false;
}
