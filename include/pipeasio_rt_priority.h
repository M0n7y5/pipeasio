/*
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

/* Scheduling policy of the thread that carries the host callback. */
#pragma once

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Range accepted by PipeWire's thread-utils interface. */
#define PIPEASIO_RT_PRIO_MIN 1
#define PIPEASIO_RT_PRIO_MAX 80

#define PIPEASIO_RT_PRIO_DEFAULT 15

/* Parse PIPEASIO_RT_PRIORITY. The environment overrides the config file. */
#define PIPEASIO_RT_ENV_UNSET (-1)

static inline int
pipeasio_rt_env_override(void)
{
    const char *env = getenv("PIPEASIO_RT_PRIORITY");
    if (!env || !*env)
        return PIPEASIO_RT_ENV_UNSET;

    if (!strcmp(env, "off") || !strcmp(env, "none") || !strcmp(env, "0"))
        return 0;
    if (!strcmp(env, "on") || !strcmp(env, "default") || !strcmp(env, "1"))
        return 1;

    return PIPEASIO_RT_ENV_UNSET;
}
