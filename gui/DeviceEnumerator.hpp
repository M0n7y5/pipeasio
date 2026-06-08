/*
 * DeviceEnumerator.hpp — list PipeWire sinks/sources for the device combos.
 *
 * parsePwDump() is PURE (operates on a JSON blob, no process spawning) so it
 * is unit testable with a captured fixture.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING.GUI for the full license text.
 */
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace DeviceEnumerator
{

struct Device
{
    QString name;           /* node.name */
    QString description;    /* node.description (fallback node.nick / node.name) */
    bool    isSink = false; /* true: Audio/Sink (output); false: Audio/Source */
};

/* Parse `pw-dump` JSON into a device list. Pure. */
QList<Device> parsePwDump(const QByteArray &json);

/* node.name of our own filter node (tagged "pipeasio.node"="1" by the driver),
 * or "" if no such node is present.  Pure. */
QString findOwnNode(const QByteArray &json);

/* Run `pw-dump` and return its stdout (empty on failure). */
QByteArray runPwDump();

/* runPwDump() + parsePwDump(). */
QList<Device> enumerate();

} // namespace DeviceEnumerator
