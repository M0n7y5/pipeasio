/*
 * DeviceEnumerator.cpp - implementation.
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
#include "DeviceEnumerator.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QTimer>
#include <QHash>
#include <QStringList>
#include <utility>

namespace DeviceEnumerator
{

std::optional<QList<Device>>
parsePwDump(const QByteArray &json)
{
    QList<Device> devices;

    QJsonParseError     err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return std::nullopt;

    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr)
    {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        if (obj.value(QStringLiteral("type")).toString()
            != QLatin1String("PipeWire:Interface:Node"))
            continue;

        const QJsonObject props = obj.value(QStringLiteral("info"))
                                          .toObject()
                                          .value(QStringLiteral("props"))
                                          .toObject();

        const QString mediaClass = props.value(QStringLiteral("media.class")).toString();
        const bool    isSink     = (mediaClass == QLatin1String("Audio/Sink"));
        const bool    isSource   = (mediaClass == QLatin1String("Audio/Source"));
        if (!isSink && !isSource)
            continue;

        const QString name = props.value(QStringLiteral("node.name")).toString();
        if (name.isEmpty())
            continue;

        QString description = props.value(QStringLiteral("node.description")).toString();
        if (description.isEmpty())
            description = props.value(QStringLiteral("node.nick")).toString();
        if (description.isEmpty())
            description = name;

        Device d;
        d.name        = name;
        d.description = description;
        d.isSink      = isSink;
        devices.append(d);
    }

    return devices;
}

QString
findOwnNode(const QByteArray &json)
{
    QJsonParseError     err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return {};

    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr)
    {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        if (obj.value(QStringLiteral("type")).toString()
            != QLatin1String("PipeWire:Interface:Node"))
            continue;
        const QJsonObject props = obj.value(QStringLiteral("info"))
                                          .toObject()
                                          .value(QStringLiteral("props"))
                                          .toObject();
        /* The driver sets pipeasio.node="1" (a string), but pw-dump serialises
         * that numeric-looking value as a JSON number, so accept both forms. */
        const QJsonValue marker = props.value(QStringLiteral("pipeasio.node"));
        if (marker.toString() == QLatin1String("1") || marker.toInt() == 1)
            return props.value(QStringLiteral("node.name")).toString();
    }
    return {};
}

int
graphClockRate(const QByteArray &json)
{
    QJsonParseError     err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return 0;

    int              rate = 0;
    const QJsonArray arr  = doc.array();
    for (const QJsonValue &v : arr)
    {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        if (obj.value(QStringLiteral("type")).toString()
            != QLatin1String("PipeWire:Interface:Metadata"))
            continue;
        if (obj.value(QStringLiteral("props"))
                    .toObject()
                    .value(QStringLiteral("metadata.name"))
                    .toString()
            != QLatin1String("settings"))
            continue;
        const QJsonArray metadata = obj.value(QStringLiteral("metadata")).toArray();
        for (const QJsonValue &mv : metadata)
        {
            const QJsonObject entry = mv.toObject();
            const QString     key   = entry.value(QStringLiteral("key")).toString();
            /* pw-dump serialises numeric-looking values as JSON numbers, but
             * accept strings too (matches the findOwnNode caveat above). */
            const QJsonValue value = entry.value(QStringLiteral("value"));
            const int        n     = value.isString() ? value.toString().toInt() : value.toInt();
            if (key == QLatin1String("clock.force-rate") && n > 0)
                return n;
            if (key == QLatin1String("clock.rate") && n > 0)
                rate = n;
        }
        break;
    }
    return rate;
}

static QString
prettyBtCodec(const QString &codec)
{
    static const QHash<QString, QString> names = {
        { QStringLiteral("sbc"), QStringLiteral("SBC") },
        { QStringLiteral("sbc_xq"), QStringLiteral("SBC-XQ") },
        { QStringLiteral("aac"), QStringLiteral("AAC") },
        { QStringLiteral("aptx"), QStringLiteral("aptX") },
        { QStringLiteral("aptx_hd"), QStringLiteral("aptX HD") },
        { QStringLiteral("aptx_ll"), QStringLiteral("aptX LL") },
        { QStringLiteral("aptx_ll_duplex"), QStringLiteral("aptX LL") },
        { QStringLiteral("ldac"), QStringLiteral("LDAC") },
        { QStringLiteral("lc3"), QStringLiteral("LC3") },
        { QStringLiteral("faststream"), QStringLiteral("FastStream") },
        { QStringLiteral("opus_05"), QStringLiteral("Opus") },
    };
    return names.value(codec.toLower(), codec.toUpper());
}

/* Split a peer node into a display `*name` and a `*detail` line: codec (BT
 * only) / negotiated rate / channels+format / state. Drops attributes the
 * graph does not expose (a suspended device has no negotiated Format). `info`
 * is the node's pw-dump "info" object. */
static void
describePeer(const QJsonObject &info, QString *name, QString *detail)
{
    const QJsonObject props = info.value(QStringLiteral("props")).toObject();

    QString n = props.value(QStringLiteral("node.description")).toString();
    if (n.isEmpty())
        n = props.value(QStringLiteral("node.nick")).toString();
    if (n.isEmpty())
        n = props.value(QStringLiteral("node.name")).toString();
    *name = n;

    QStringList attrs;

    const QString codec = props.value(QStringLiteral("api.bluez5.codec")).toString();
    if (!codec.isEmpty())
        attrs << prettyBtCodec(codec);
    else if (props.value(QStringLiteral("device.api")).toString() == QLatin1String("bluez5"))
        attrs << QStringLiteral("Bluetooth");

    /* Negotiated format (rate/channels/sample format); present only while the
     * device is active. */
    const QJsonArray fmt      = info.value(QStringLiteral("params"))
                                        .toObject()
                                        .value(QStringLiteral("Format"))
                                        .toArray();
    int              rate     = 0;
    int              channels = 0;
    QString          sampleFmt;
    if (!fmt.isEmpty())
    {
        const QJsonObject f = fmt.first().toObject();
        rate                = f.value(QStringLiteral("rate")).toInt(0);
        channels            = f.value(QStringLiteral("channels")).toInt(0);
        sampleFmt           = f.value(QStringLiteral("format")).toString();
    }
    if (channels == 0)
        channels = props.value(QStringLiteral("audio.channels")).toInt(0);

    if (rate > 0)
        attrs << QStringLiteral("%1 Hz").arg(rate);
    if (channels > 0)
    {
        QString ch = QStringLiteral("%1 ch").arg(channels);
        if (!sampleFmt.isEmpty())
            ch += QLatin1Char(' ') + sampleFmt;
        attrs << ch;
    }

    const QString state = info.value(QStringLiteral("state")).toString();
    if (!state.isEmpty())
        attrs << state;

    *detail = attrs.join(QStringLiteral(" \u00b7 "));
}

Connections
resolveConnections(const QByteArray &json)
{
    Connections conn;

    QJsonParseError     err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return conn;

    const QJsonArray arr = doc.array();

    /* Pass 1: map every node id -> its "info" object, and find our own node id
     * via the driver's "pipeasio.node" marker (set regardless of node_name). */
    QHash<int, QJsonObject> nodeInfo;
    int                     ownId = -1;
    for (const QJsonValue &v : arr)
    {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        if (obj.value(QStringLiteral("type")).toString()
            != QLatin1String("PipeWire:Interface:Node"))
            continue;
        const int id = obj.value(QStringLiteral("id")).toInt(-1);
        if (id < 0)
            continue;
        const QJsonObject info = obj.value(QStringLiteral("info")).toObject();
        nodeInfo.insert(id, info);

        const QJsonValue marker = info.value(QStringLiteral("props"))
                                          .toObject()
                                          .value(QStringLiteral("pipeasio.node"));
        if (marker.toString() == QLatin1String("1") || marker.toInt() == 1)
            ownId = id;
    }
    if (ownId < 0)
        return conn;

    /* Pass 2: links touching our node. A link FROM our node (output.node ==
     * ours) lands on a sink we play to. A link TO our node (input.node == ours)
     * comes from a source we capture from. Distinct peers, link order kept. */
    QList<int> outIds, inIds;
    for (const QJsonValue &v : arr)
    {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        if (obj.value(QStringLiteral("type")).toString()
            != QLatin1String("PipeWire:Interface:Link"))
            continue;
        const QJsonObject props   = obj.value(QStringLiteral("info"))
                                            .toObject()
                                            .value(QStringLiteral("props"))
                                            .toObject();
        const int         outNode = props.value(QStringLiteral("link.output.node")).toInt(-1);
        const int         inNode  = props.value(QStringLiteral("link.input.node")).toInt(-1);

        if (outNode == ownId && nodeInfo.contains(inNode) && !outIds.contains(inNode))
            outIds.append(inNode);
        if (inNode == ownId && nodeInfo.contains(outNode) && !inIds.contains(outNode))
            inIds.append(outNode);
    }

    /* One peer per side is the norm (our N ports fan into a single device); show
     * its name and detail separately so the UI can stack them. Several distinct
     * peers (manual patching) collapse to a names-only list, no detail. */
    const auto fill = [&](const QList<int> &ids, QString *name, QString *detail)
    {
        if (ids.isEmpty())
            return;
        if (ids.size() == 1)
        {
            describePeer(nodeInfo.value(ids.first()), name, detail);
            return;
        }
        QStringList names;
        for (int id : ids)
        {
            QString n, d;
            describePeer(nodeInfo.value(id), &n, &d);
            names << n;
        }
        *name = names.join(QStringLiteral(", "));
    };
    fill(outIds, &conn.output, &conn.outputDetail);
    fill(inIds, &conn.input, &conn.inputDetail);
    return conn;
}

Request::Request(RequestOptions options, QObject *parent)
    : QObject(parent), m_options(std::move(options))
{
}

Request::~Request()
{
    m_done = true;
    if (m_timer)
        m_timer->stop();
    if (!m_process)
        return;
    disconnect(m_process, nullptr, this, nullptr);
    if (m_process->state() != QProcess::NotRunning)
    {
        m_process->kill();
        m_process->waitForFinished(500);
    }
}

void
Request::start()
{
    if (m_process || m_done)
        return;
    m_process = new QProcess(this);
    m_timer   = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_process, &QObject::destroyed, this, [this] { m_process = nullptr; });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error)
            {
                if (error == QProcess::FailedToStart)
                    finish(false, {}, QStringLiteral("Failed to start pw-dump"));
            });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status)
            {
                if (status != QProcess::NormalExit || exitCode != 0)
                {
                    finish(false, {}, QStringLiteral("pw-dump exited unsuccessfully"));
                    return;
                }
                const QByteArray output  = m_process->readAllStandardOutput();
                auto             devices = parsePwDump(output);
                if (!devices)
                {
                    finish(false, {}, QStringLiteral("pw-dump returned invalid JSON"));
                    return;
                }
                m_graphRate = graphClockRate(output);
                finish(true, std::move(*devices), {});
            });
    connect(m_timer, &QTimer::timeout, this,
            [this] { finish(false, {}, QStringLiteral("pw-dump timed out")); });
    m_process->start(m_options.program, m_options.arguments);
    m_timer->start(m_options.timeoutMs > 0 ? m_options.timeoutMs : 1);
}

void
Request::finish(bool success, QList<Device> devices, QString error)
{
    if (m_done)
        return;
    m_done = true;
    if (m_timer)
        m_timer->stop();
    if (m_process)
    {
        disconnect(m_process, nullptr, this, nullptr);
        connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), m_process,
                &QObject::deleteLater);
        if (m_process->state() == QProcess::NotRunning)
            m_process->deleteLater();
        else if (!success)
            m_process->kill();
    }
    emit finished(success, devices, error);
}

} // namespace DeviceEnumerator
