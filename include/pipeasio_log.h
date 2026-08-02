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

/* Raw stderr logging shared by native and WoW64 PE builds. */
#pragma once

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef PIPEASIO_WOW64_PE
extern int _write(int fd, const void *buf, unsigned int count);
#define PIPEASIO_WRITE_ERR(buf, n) (void)_write(2, (buf), (unsigned)(n))
#else
#include <unistd.h>
#define PIPEASIO_WRITE_ERR(buf, n) (void)write(STDERR_FILENO, (buf), (n))
#endif

#undef TRACE
#undef WARN
#undef ERR

static inline int
pipeasio_log_on(void)
{
    static atomic_int on    = -1;
    int               value = atomic_load_explicit(&on, memory_order_relaxed);
    if (value < 0)
    {
        value = getenv("PIPEASIO_DEBUG") ? 1 : 0;
        atomic_store_explicit(&on, value, memory_order_relaxed);
    }
    return value;
}
#define PIPEASIO_LOG(pfx, fmt, ...)                                                                \
    do                                                                                             \
    {                                                                                              \
        char _buf[1024];                                                                           \
        int  _n = snprintf(_buf, sizeof _buf, pfx fmt, ##__VA_ARGS__);                             \
        if (_n > 0)                                                                                \
            PIPEASIO_WRITE_ERR(_buf, (size_t)_n < sizeof _buf ? (size_t)_n : sizeof _buf - 1);     \
    } while (0)
#define TRACE(fmt, ...)                                                                            \
    do                                                                                             \
    {                                                                                              \
        if (pipeasio_log_on())                                                                     \
            PIPEASIO_LOG("[pipeasio] ", fmt, ##__VA_ARGS__);                                       \
    } while (0)
#define WARN(fmt, ...) PIPEASIO_LOG("[pipeasio] WARN: ", fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) PIPEASIO_LOG("[pipeasio] ERR: ", fmt, ##__VA_ARGS__)
