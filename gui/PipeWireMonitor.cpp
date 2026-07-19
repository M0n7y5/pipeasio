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

#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTimer>

namespace
{

/* pw-dump cadence: refresh the connected sink/source every Nth pw-top update.
 * pw-top streams continuously; graph topology changes rarely. */
constexpr int kDumpEvery = 5;

/* Bound the unparsed pw-top stdout tail: pw-top -b prints one table per
 * refresh forever.  parsePwTop only reads the LAST table, so dropping the
 * older half mid-stream is lossless for our purposes. */
constexpr qsizetype kTopBufKeep = 64 * 1024;

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

    /* pw-top -b streams one table per iteration with block-buffered stdout:
     * the live buffer typically ENDS with a fresh header whose rows only
     * arrive in the next 4K block, so parsing "below the last header" can
     * see zero rows indefinitely.  Instead scan BACKWARDS for the newest
     * data row of our node: a truncated tail row is rejected by the token
     * count, and the all-zero baseline iteration is skipped naturally.
     * Fixed columns S ID QUANT RATE WAIT BUSY W/Q B/Q ERR occupy indices
     * 0..8; everything from index 9 on is the variable-width FORMAT, an
     * optional link marker (one of + = *), and the NAME (which may contain
     * spaces).  Substring-match the configured node name against that
     * trailing run. */
    for (int i = lines.size() - 1; i >= 0; --i)
    {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;

        const QStringList tok
                = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (tok.size() < 10)
            continue;

        /* The header row also has 10+ tokens; its "ID" column is not numeric. */
        bool idOk = false;
        tok.at(1).toInt(&idOk);
        if (!idOk)
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
    : QObject(parent), m_watchdog(new QTimer(this)), m_autoDiscover(true)
{
    /* The watchdog's only job is to restart pw-top if the child died; the
     * steady state costs zero process spawns. */
    m_watchdog->setInterval(1000);
    connect(m_watchdog, &QTimer::timeout, this, &PipeWireMonitor::ensureTopRunning);
}

PipeWireMonitor::~PipeWireMonitor() = default;

void
PipeWireMonitor::setTarget(const QString &nodeNameSubstr)
{
    /* An explicit (configured) name disables auto-discovery; empty re-enables it. */
    m_target       = nodeNameSubstr;
    m_autoDiscover = nodeNameSubstr.isEmpty();
}

void
PipeWireMonitor::start()
{
    ensureTopRunning();
    m_watchdog->start();
}

void
PipeWireMonitor::stop()
{
    m_watchdog->stop();
    for (QProcess *p : { m_topProc, m_dumpProc })
    {
        if (!p)
            continue;
        p->disconnect(this);
        p->kill();
        p->deleteLater();
    }
    m_topProc  = nullptr;
    m_dumpProc = nullptr;
    m_topBuf.clear();
}

void
PipeWireMonitor::ensureTopRunning()
{
    if (m_topProc)
        return; /* already streaming */

    /* One long-lived `pw-top -b` instead of a fork+exec every poll tick. */
    m_topProc = new QProcess(this);
    connect(m_topProc, &QProcess::readyReadStandardOutput, this,
            &PipeWireMonitor::onTopReadyRead);
    connect(m_topProc, &QProcess::finished, this, [this] {
        m_topProc->deleteLater();
        m_topProc = nullptr;
        /* the watchdog respawns it on its next tick */
    });
    connect(m_topProc, &QProcess::errorOccurred, this, [this] {
        m_topProc->deleteLater();
        m_topProc = nullptr;
    });
    m_topProc->start(QStringLiteral("pw-top"), { QStringLiteral("-b") });
}

void
PipeWireMonitor::onTopReadyRead()
{
    if (!m_topProc)
        return;
    m_topBuf += m_topProc->readAllStandardOutput();
    if (m_topBuf.size() > 2 * kTopBufKeep)
        m_topBuf = m_topBuf.right(kTopBufKeep);

    const NodeStats st = parsePwTop(m_topBuf, m_target);

    /* Dump the graph when we still need to discover our node (the host names it
     * after its own exe; we resolve it via the "pipeasio.node" marker) OR when a
     * connection refresh is due - the same pw-dump yields the sink/source our
     * ports are linked to. Between dumps, reuse the cached connection strings. */
    ++m_updatesSinceDump;
    const bool needDiscover = m_autoDiscover && !st.found;
    const bool refreshConn  = m_updatesSinceDump >= kDumpEvery;
    if (!needDiscover && !refreshConn)
    {
        NodeStats out          = st;
        out.inputDevice        = m_connInput;
        out.outputDevice       = m_connOutput;
        out.inputDeviceDetail  = m_connInputDetail;
        out.outputDeviceDetail = m_connOutputDetail;
        emit updated(out);
        return;
    }

    /* Keep the stats that triggered the dump; applied when the worker-thread
     * parse finishes (no second parsePwTop pass). */
    m_pendingStats = st;
    startDump();
}

void
PipeWireMonitor::startDump()
{
    if (m_dumpProc)
        return; /* a dump is already in flight; its result refreshes everything */

    m_dumpProc = new QProcess(this);
    connect(m_dumpProc, &QProcess::finished, this, &PipeWireMonitor::onDumpFinished);
    connect(m_dumpProc, &QProcess::errorOccurred, this, &PipeWireMonitor::onDumpFinished);
    m_dumpProc->start(QStringLiteral("pw-dump"), QStringList());
}

void
PipeWireMonitor::onDumpFinished()
{
    if (!m_dumpProc)
        return;
    const QByteArray dump = m_dumpProc->readAllStandardOutput();
    m_dumpProc->deleteLater();
    m_dumpProc = nullptr;

    if (dump.isEmpty())
        return;

    /* Parse on a worker thread: a full pw-dump is hundreds of KB of JSON and
     * QJsonDocument::fromJson on the GUI thread visibly janked the panel. */
    QPointer<PipeWireMonitor> guard(this);
    QThreadPool::globalInstance()->start([guard, dump]() {
        DumpResult result;
        result.ownName = DeviceEnumerator::findOwnNode(dump);
        result.conn    = DeviceEnumerator::resolveConnections(dump);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, result]() {
            if (guard)
                guard->applyDumpResult(result);
        }, Qt::QueuedConnection);
    });
}

void
PipeWireMonitor::applyDumpResult(const DumpResult &result)
{
    if (m_autoDiscover && !result.ownName.isEmpty())
        m_target = result.ownName;

    m_connInput         = result.conn.input;
    m_connInputDetail   = result.conn.inputDetail;
    m_connOutput        = result.conn.output;
    m_connOutputDetail  = result.conn.outputDetail;
    m_updatesSinceDump  = 0;

    NodeStats st          = m_pendingStats;
    st.inputDevice        = m_connInput;
    st.inputDeviceDetail  = m_connInputDetail;
    st.outputDevice       = m_connOutput;
    st.outputDeviceDetail = m_connOutputDetail;
    emit updated(st);
}
