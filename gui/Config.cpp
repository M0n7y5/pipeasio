/*
 * Config.cpp — implementation of the panel INI read/write.
 *
 * Copyright (C) 2026 PipeASIO contributors
 */
#include "Config.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStringList>
#include <QFileInfo>
#include <QTextStream>

#include <cstring>

namespace Config {

namespace {

void setStr(char *dst, size_t cap, const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    const size_t n = qMin<size_t>(utf8.size(), cap - 1);
    std::memcpy(dst, utf8.constData(), n);
    dst[n] = '\0';
}

bool parseBool(const QString &raw, bool fallback)
{
    const QString v = raw.trimmed().toLower();
    if (v == QLatin1String("1") || v == QLatin1String("true") ||
        v == QLatin1String("on") || v == QLatin1String("yes"))
        return true;
    if (v == QLatin1String("0") || v == QLatin1String("false") ||
        v == QLatin1String("off") || v == QLatin1String("no"))
        return false;
    return fallback;
}

bool isPowerOfTwo(int v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

} // namespace

pipeasio_config defaults()
{
    pipeasio_config c;
    c.inputs = PIPEASIO_DEFAULT_INPUTS;
    c.outputs = PIPEASIO_DEFAULT_OUTPUTS;
    c.buffer_size = PIPEASIO_DEFAULT_BUFFER_SIZE;
    c.fixed_buffer_size = PIPEASIO_DEFAULT_FIXED_BUFFER_SIZE;
    c.sample_rate = PIPEASIO_DEFAULT_SAMPLE_RATE;
    c.auto_connect = PIPEASIO_DEFAULT_AUTO_CONNECT;
    c.output_device[0] = '\0';
    c.input_device[0] = '\0';
    c.node_name[0] = '\0';
    return c;
}

pipeasio_config parseIni(const QString &text)
{
    pipeasio_config c = defaults();

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
            continue;
        if (line.startsWith(QLatin1Char('[')))   /* section header — single section */
            continue;

        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;

        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();

        if (key == QLatin1String(PIPEASIO_KEY_INPUTS)) {
            bool ok = false;
            const int v = value.toInt(&ok);
            if (ok && v >= 0)
                c.inputs = v;
        } else if (key == QLatin1String(PIPEASIO_KEY_OUTPUTS)) {
            bool ok = false;
            const int v = value.toInt(&ok);
            if (ok && v >= 0)
                c.outputs = v;
        } else if (key == QLatin1String(PIPEASIO_KEY_BUFFER_SIZE)) {
            bool ok = false;
            const int v = value.toInt(&ok);
            if (ok && isPowerOfTwo(v) && v >= PIPEASIO_MIN_BUFFER_SIZE &&
                v <= PIPEASIO_MAX_BUFFER_SIZE)
                c.buffer_size = v;
        } else if (key == QLatin1String(PIPEASIO_KEY_FIXED_BUFFER_SIZE)) {
            c.fixed_buffer_size = parseBool(value, c.fixed_buffer_size);
        } else if (key == QLatin1String(PIPEASIO_KEY_SAMPLE_RATE)) {
            bool ok = false;
            const int v = value.toInt(&ok);
            if (ok)
                c.sample_rate = v < 0 ? 0 : v;
        } else if (key == QLatin1String(PIPEASIO_KEY_AUTO_CONNECT)) {
            c.auto_connect = parseBool(value, c.auto_connect);
        } else if (key == QLatin1String(PIPEASIO_KEY_OUTPUT_DEVICE)) {
            setStr(c.output_device, sizeof(c.output_device), value);
        } else if (key == QLatin1String(PIPEASIO_KEY_INPUT_DEVICE)) {
            setStr(c.input_device, sizeof(c.input_device), value);
        } else if (key == QLatin1String(PIPEASIO_KEY_NODE_NAME)) {
            setStr(c.node_name, sizeof(c.node_name), value);
        }
        /* unknown keys ignored */
    }

    return c;
}

QString serializeIni(const pipeasio_config &c)
{
    QString out;
    QTextStream s(&out);
    s << "# PipeASIO settings - written by pipeasio-settings\n";
    s << '[' << PIPEASIO_CONFIG_SECTION << "]\n";
    s << PIPEASIO_KEY_INPUTS << " = " << c.inputs << '\n';
    s << PIPEASIO_KEY_OUTPUTS << " = " << c.outputs << '\n';
    s << PIPEASIO_KEY_BUFFER_SIZE << " = " << c.buffer_size << '\n';
    s << PIPEASIO_KEY_FIXED_BUFFER_SIZE << " = " << (c.fixed_buffer_size ? 1 : 0) << '\n';
    s << PIPEASIO_KEY_SAMPLE_RATE << " = " << c.sample_rate << '\n';
    s << PIPEASIO_KEY_AUTO_CONNECT << " = " << (c.auto_connect ? 1 : 0) << '\n';
    s << PIPEASIO_KEY_OUTPUT_DEVICE << " = " << QString::fromUtf8(c.output_device) << '\n';
    s << PIPEASIO_KEY_INPUT_DEVICE << " = " << QString::fromUtf8(c.input_device) << '\n';
    s << PIPEASIO_KEY_NODE_NAME << " = " << QString::fromUtf8(c.node_name) << '\n';
    return out;
}

QString configPath()
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return base + QLatin1Char('/') + QLatin1String(PIPEASIO_CONFIG_DIR) +
           QLatin1Char('/') + QLatin1String(PIPEASIO_CONFIG_FILE);
}

pipeasio_config load()
{
    QFile f(configPath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text))
        return defaults();
    const QString text = QString::fromUtf8(f.readAll());
    f.close();
    return parseIni(text);
}

bool save(const pipeasio_config &c)
{
    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    const QByteArray bytes = serializeIni(c).toUtf8();
    const bool ok = f.write(bytes) == bytes.size();
    f.close();
    return ok;
}

} // namespace Config
