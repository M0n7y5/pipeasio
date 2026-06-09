/*
 * DeviceEnumerator.cpp — implementation.
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

namespace DeviceEnumerator
{

QList<Device>
parsePwDump(const QByteArray &json)
{
    QList<Device> devices;

    QJsonParseError     err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return devices;

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

QByteArray
runPwDump()
{
    QProcess proc;
    proc.start(QStringLiteral("pw-dump"), QStringList());
    if (!proc.waitForStarted(3000))
        return {};
    if (!proc.waitForFinished(5000))
    {
        proc.kill();
        proc.waitForFinished(1000);
        return {};
    }
    return proc.readAllStandardOutput();
}

QList<Device>
enumerate()
{
    return parsePwDump(runPwDump());
}

} // namespace DeviceEnumerator
