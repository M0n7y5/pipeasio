/*
 * Config.hpp - INI read/write for the PipeASIO settings panel.
 *
 * Thin Qt wrapper over the driver's own reader/writer (src/config.c): there
 * is exactly ONE INI parser and ONE serializer in the project, so the panel
 * and the driver can never drift apart on the on-disk format.
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

#include <QString>

extern "C"
{
#include "pipeasio_config.h"
}

namespace Config
{

/* Defaults straight from the PIPEASIO_DEFAULT_* macros; device/node strings
 * are left empty. */
pipeasio_config defaults();

/* Absolute path to the config file (QStandardPaths ConfigLocation). */
QString configPath();

/* Read the config file via the driver's C reader; defaults() when missing. */
pipeasio_config load();

/* Write via the driver's C writer (atomic tmp+rename). */
bool save(const pipeasio_config &c);

} // namespace Config
