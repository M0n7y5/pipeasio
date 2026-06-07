/*
 * DeviceEnumerator.cpp — implementation.
 *
 * Copyright (C) 2026 PipeASIO contributors
 */
#include "DeviceEnumerator.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>

namespace DeviceEnumerator {

QList<Device> parsePwDump(const QByteArray &json)
{
    QList<Device> devices;

    QJsonParseError err {};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return devices;

    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr) {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        if (obj.value(QStringLiteral("type")).toString() !=
            QLatin1String("PipeWire:Interface:Node"))
            continue;

        const QJsonObject props =
            obj.value(QStringLiteral("info")).toObject()
               .value(QStringLiteral("props")).toObject();

        const QString mediaClass =
            props.value(QStringLiteral("media.class")).toString();
        const bool isSink = (mediaClass == QLatin1String("Audio/Sink"));
        const bool isSource = (mediaClass == QLatin1String("Audio/Source"));
        if (!isSink && !isSource)
            continue;

        const QString name = props.value(QStringLiteral("node.name")).toString();
        if (name.isEmpty())
            continue;

        QString description =
            props.value(QStringLiteral("node.description")).toString();
        if (description.isEmpty())
            description = props.value(QStringLiteral("node.nick")).toString();
        if (description.isEmpty())
            description = name;

        Device d;
        d.name = name;
        d.description = description;
        d.isSink = isSink;
        devices.append(d);
    }

    return devices;
}

QByteArray runPwDump()
{
    QProcess proc;
    proc.start(QStringLiteral("pw-dump"), QStringList());
    if (!proc.waitForStarted(3000))
        return {};
    if (!proc.waitForFinished(5000)) {
        proc.kill();
        proc.waitForFinished(1000);
        return {};
    }
    return proc.readAllStandardOutput();
}

QList<Device> enumerate()
{
    return parsePwDump(runPwDump());
}

} // namespace DeviceEnumerator
