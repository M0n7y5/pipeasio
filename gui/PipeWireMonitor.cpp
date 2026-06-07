/*
 * PipeWireMonitor.cpp — implementation.
 *
 * Copyright (C) 2026 PipeASIO contributors
 */
#include "PipeWireMonitor.hpp"
#include "DeviceEnumerator.hpp"

#include <QProcess>
#include <QStringList>
#include <QRegularExpression>
#include <QTimer>

#include <cmath>

namespace {

int intOrZero(const QString &tok)
{
    if (tok == QLatin1String("---"))
        return 0;
    bool ok = false;
    const int v = tok.toInt(&ok);
    return ok ? v : 0;
}

double doubleOrZero(const QString &tok)
{
    if (tok == QLatin1String("---"))
        return 0.0;
    bool ok = false;
    const double v = tok.toDouble(&ok);
    return ok ? v : 0.0;
}

long longOrZero(const QString &tok)
{
    if (tok == QLatin1String("---"))
        return 0;
    bool ok = false;
    const long v = tok.toLong(&ok);
    return ok ? v : 0;
}

} // namespace

NodeStats parsePwTop(const QByteArray &out, const QString &nodeNameSubstr)
{
    const QString text = QString::fromUtf8(out);
    const QStringList lines = text.split(QLatin1Char('\n'));

    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;
        /* Skip the header row. */
        if (line.contains(QLatin1String("ID")) &&
            line.contains(QLatin1String("QUANT")))
            continue;

        const QStringList tok =
            line.split(QRegularExpression(QStringLiteral("\\s+")),
                       Qt::SkipEmptyParts);
        /* Columns: S ID QUANT RATE WAIT BUSY W/Q B/Q ERR FORMAT NAME...
         * token[0..8] fixed, token[9]=FORMAT, token[10..]=NAME. */
        if (tok.size() < 11)
            continue;

        const QString name = QStringList(tok.mid(10)).join(QLatin1Char(' '));
        if (!name.contains(nodeNameSubstr))
            continue;

        NodeStats st;
        st.found = true;
        st.name = name;
        st.state = tok.at(0);
        st.quantum = intOrZero(tok.at(2));
        st.rate = intOrZero(tok.at(3));
        st.dspLoad = doubleOrZero(tok.at(7));
        st.xruns = longOrZero(tok.at(8));
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

void PipeWireMonitor::setTarget(const QString &nodeNameSubstr)
{
    /* An explicit (configured) name disables auto-discovery; empty re-enables it. */
    m_target = nodeNameSubstr;
    m_autoDiscover = nodeNameSubstr.isEmpty();
}

void PipeWireMonitor::start()
{
    poll();
    m_timer->start();
}

void PipeWireMonitor::stop()
{
    m_timer->stop();
}

void PipeWireMonitor::poll()
{
    QProcess proc;
    proc.start(QStringLiteral("pw-top"),
               {QStringLiteral("-b"), QStringLiteral("-n"), QStringLiteral("1")});
    if (!proc.waitForStarted(2000))
        return;
    if (!proc.waitForFinished(3000)) {
        proc.kill();
        proc.waitForFinished(1000);
        return;
    }
    const QByteArray out = proc.readAllStandardOutput();

    /* The host names our node after its own executable, so when no explicit
     * node_name is configured we resolve it from the "pipeasio.node" marker
     * the driver stamps on the filter — refreshed whenever the current target
     * isn't present (e.g. the host (re)started since we last looked). */
    if (m_autoDiscover && (m_target.isEmpty() || !parsePwTop(out, m_target).found)) {
        const QString name = DeviceEnumerator::findOwnNode(DeviceEnumerator::runPwDump());
        if (!name.isEmpty())
            m_target = name;
    }

    emit updated(parsePwTop(out, m_target));
}
