/*
 * Config.cpp - Qt wrapper over the driver's INI reader/writer (src/config.c).
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

#include <QStandardPaths>

namespace Config
{

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
    c.rt_priority         = PIPEASIO_DEFAULT_RT_PRIORITY;
    c.output_device[0]    = '\0';
    c.input_device[0]     = '\0';
    c.node_name[0]        = '\0';
    return c;
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
    pipeasio_config c;
    /* pipeasio_config_load leaves defaults in c when the file is missing;
     * its XDG_CONFIG_HOME/HOME resolution matches QStandardPaths. */
    pipeasio_config_load(&c);
    return c;
}

bool
save(const pipeasio_config &c)
{
    return pipeasio_config_save(&c);
}

} // namespace Config
