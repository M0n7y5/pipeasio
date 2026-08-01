/*
 * Config.cpp - implementation of the panel INI read/write.
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
#include "Config.hpp"
#include "pipeasio_parse.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QFileInfo>
#include <QTextStream>

#include <cstring>

namespace Config
{

namespace
{

bool
setStr(char *destination, size_t capacity, const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    if (utf8.size() < 0 || (size_t)utf8.size() >= capacity)
        return false;
    std::memcpy(destination, utf8.constData(), (size_t)utf8.size());
    destination[utf8.size()] = '\0';
    return true;
}

} // namespace

pipeasio_config
defaults()
{
    pipeasio_config c;
    c.inputs              = PIPEASIO_DEFAULT_INPUTS;
    c.outputs             = PIPEASIO_DEFAULT_OUTPUTS;
    c.buffer_size         = PIPEASIO_DEFAULT_BUFFER_SIZE;
    c.fixed_buffer_size   = PIPEASIO_DEFAULT_FIXED_BUFFER_SIZE;
    c.sample_rate         = PIPEASIO_DEFAULT_SAMPLE_RATE;
    c.auto_connect        = PIPEASIO_DEFAULT_AUTO_CONNECT;
    c.follow_device_clock = PIPEASIO_DEFAULT_FOLLOW_DEVICE_CLOCK;
    c.realtime            = PIPEASIO_DEFAULT_REALTIME;
    c.output_device[0]    = '\0';
    c.input_device[0]     = '\0';
    c.node_name[0]        = '\0';
    return c;
}

pipeasio_config
parseIni(const QString &text)
{
    pipeasio_config c = defaults();

    const QStringList lines     = text.split(QLatin1Char('\n'));
    bool              inSection = true;
    for (const QString &rawLine : lines)
    {
        /* Match the C loader (src/config.c): fgets fills a
         * PIPEASIO_CONFIG_LINE_MAX buffer and discards any line whose content
         * plus its newline does not fit, i.e. content >= LINE_MAX - 1 bytes.
         * The largest accepted line is LINE_MAX - 2 content bytes + newline. */
        if (rawLine.toUtf8().size() >= PIPEASIO_CONFIG_LINE_MAX - 1)
            continue;
        QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
            || line.startsWith(QLatin1Char(';')))
            continue;
        if (line.startsWith(QLatin1Char('[')))
        {
            const int close = line.indexOf(QLatin1Char(']'));
            if (close >= 0)
                inSection = line.mid(1, close - 1) == QLatin1String(PIPEASIO_CONFIG_SECTION);
            continue;
        }
        if (!inSection)
            continue;
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals < 0)
            continue;
        const QString    key   = line.left(equals).trimmed();
        const QString    value = line.mid(equals + 1).trimmed();
        const QByteArray bytes = value.toUtf8();
        int              parsed;
        bool             flag;
        if (key == QLatin1String(PIPEASIO_KEY_INPUTS))
        {
            if (pipeasio_parse_int(bytes.constData(), 0, PIPEASIO_MAX_CHANNELS, &parsed))
                c.inputs = parsed;
        }
        else if (key == QLatin1String(PIPEASIO_KEY_OUTPUTS))
        {
            if (pipeasio_parse_int(bytes.constData(), 0, PIPEASIO_MAX_CHANNELS, &parsed))
                c.outputs = parsed;
        }
        else if (key == QLatin1String(PIPEASIO_KEY_BUFFER_SIZE))
        {
            if (pipeasio_parse_int(bytes.constData(), PIPEASIO_MIN_BUFFER_SIZE,
                                   PIPEASIO_MAX_BUFFER_SIZE, &parsed)
                && !(parsed & (parsed - 1)))
                c.buffer_size = parsed;
        }
        else if (key == QLatin1String(PIPEASIO_KEY_FIXED_BUFFER_SIZE))
        {
            if (pipeasio_parse_bool(bytes.constData(), &flag))
                c.fixed_buffer_size = flag;
        }
        else if (key == QLatin1String(PIPEASIO_KEY_SAMPLE_RATE))
        {
            if (pipeasio_parse_int(bytes.constData(), 0, INT_MAX, &parsed))
                c.sample_rate = parsed;
        }
        else if (key == QLatin1String(PIPEASIO_KEY_AUTO_CONNECT))
        {
            if (pipeasio_parse_bool(bytes.constData(), &flag))
                c.auto_connect = flag;
        }
        else if (key == QLatin1String(PIPEASIO_KEY_FOLLOW_DEVICE_CLOCK))
        {
            if (pipeasio_parse_bool(bytes.constData(), &flag))
                c.follow_device_clock = flag;
        }
        else if (key == QLatin1String(PIPEASIO_KEY_REALTIME))
        {
            if (pipeasio_parse_bool(bytes.constData(), &flag))
                c.realtime = flag;
        }
        else if (key == QLatin1String(PIPEASIO_KEY_OUTPUT_DEVICE))
            setStr(c.output_device, sizeof(c.output_device), value);
        else if (key == QLatin1String(PIPEASIO_KEY_INPUT_DEVICE))
            setStr(c.input_device, sizeof(c.input_device), value);
        else if (key == QLatin1String(PIPEASIO_KEY_NODE_NAME))
            setStr(c.node_name, sizeof(c.node_name), value);
        /* unknown keys ignored */
    }

    return c;
}

QString
serializeIni(const pipeasio_config &c)
{
    QString     out;
    QTextStream s(&out);
    s << "# PipeASIO settings - written by pipeasio-settings\n";
    s << '[' << PIPEASIO_CONFIG_SECTION << "]\n";
    s << PIPEASIO_KEY_INPUTS << " = " << c.inputs << '\n';
    s << PIPEASIO_KEY_OUTPUTS << " = " << c.outputs << '\n';
    s << PIPEASIO_KEY_BUFFER_SIZE << " = " << c.buffer_size << '\n';
    s << PIPEASIO_KEY_FIXED_BUFFER_SIZE << " = " << (c.fixed_buffer_size ? 1 : 0) << '\n';
    s << PIPEASIO_KEY_SAMPLE_RATE << " = " << c.sample_rate << '\n';
    s << PIPEASIO_KEY_AUTO_CONNECT << " = " << (c.auto_connect ? 1 : 0) << '\n';
    s << PIPEASIO_KEY_FOLLOW_DEVICE_CLOCK << " = " << (c.follow_device_clock ? 1 : 0) << '\n';
    s << PIPEASIO_KEY_REALTIME << " = " << (c.realtime ? 1 : 0) << '\n';
    s << PIPEASIO_KEY_OUTPUT_DEVICE << " = " << QString::fromUtf8(c.output_device) << '\n';
    s << PIPEASIO_KEY_INPUT_DEVICE << " = " << QString::fromUtf8(c.input_device) << '\n';
    s << PIPEASIO_KEY_NODE_NAME << " = " << QString::fromUtf8(c.node_name) << '\n';
    return out;
}

QString
configPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return base + QLatin1Char('/') + QLatin1String(PIPEASIO_CONFIG_DIR) + QLatin1Char('/')
           + QLatin1String(PIPEASIO_CONFIG_FILE);
}

pipeasio_config
load()
{
    QFile f(configPath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text))
        return defaults();
    const QString text = QString::fromUtf8(f.readAll());
    f.close();
    return parseIni(text);
}

bool
save(const pipeasio_config &c)
{
    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    /* QSaveFile writes to a temporary file and atomically renames it into place
     * on commit(), so the driver's config watcher (and loader) never observes a
     * half-written config.ini. */
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    const QByteArray bytes = serializeIni(c).toUtf8();
    if (f.write(bytes) != bytes.size())
        return false;
    return f.commit();
}

} // namespace Config
