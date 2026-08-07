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
