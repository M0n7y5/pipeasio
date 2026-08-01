/*
 * DeviceEnumerator.hpp - list PipeWire sinks/sources for the device combos.
 *
 * parsePwDump() is PURE (operates on a JSON blob, no process spawning) so it
 * is unit testable with a captured fixture.
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

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QPointer>
#include <optional>
#include <QString>
class QProcess;
class QTimer;

namespace DeviceEnumerator
{

struct RequestOptions
{
    QString     program = QStringLiteral("pw-dump");
    QStringList arguments;
    int         timeoutMs = 8000;
};

struct Device
{
    QString name;           /* node.name */
    QString description;    /* node.description (fallback node.nick / node.name) */
    bool    isSink = false; /* true: Audio/Sink (output), false: Audio/Source */
};

/* Invalid JSON and non-array top levels return std::nullopt. */
std::optional<QList<Device>> parsePwDump(const QByteArray &json);

/* node.name of our own filter node (tagged "pipeasio.node"="1" by the driver),
 * or "" if no such node is present.  Pure. */
QString findOwnNode(const QByteArray &json);

/* What our own filter node (tagged "pipeasio.node"="1") is wired to in the
 * PipeWire graph: the sink our outputs feed and the source feeding our inputs.
 * Each side's `*Detail` carries the peer's codec/format/state for a second
 * display line (empty when unknown, or when several peers share a side and
 * the name string already lists them). Empty name == nothing connected. Pure. */
struct Connections
{
    QString output;       /* sink name(s) our outputs feed */
    QString outputDetail; /* codec / rate / channels / state of a single sink */
    QString input;        /* source name(s) feeding our inputs */
    QString inputDetail;
};
Connections resolveConnections(const QByteArray &json);

class Request final : public QObject
{
    Q_OBJECT
  public:
    explicit Request(RequestOptions options = {}, QObject *parent = nullptr);
    ~Request() override;

    void start();

  signals:
    void finished(bool success, const QList<Device> &devices, const QString &error);

  private:
    void finish(bool success, QList<Device> devices, QString error);

    RequestOptions     m_options;
    QPointer<QProcess> m_process;
    QTimer            *m_timer = nullptr;
    bool               m_done  = false;
};

} // namespace DeviceEnumerator
