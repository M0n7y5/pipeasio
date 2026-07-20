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

/* PE-side seams used when src/asio.c is built as the WoW64 i386 front end. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio.h"
#include "pipeasio_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Config is read by the unixlib; native builds use src/config.c directly. */
    bool pipeasio_wow64_load_config(struct pipeasio_config *out);

    /* Config write (ControlPanel); native builds use pipeasio_config_save. */
    bool pipeasio_wow64_save_config(const struct pipeasio_config *c);

    /* Absolute config.ini path for display; native builds use
     * pipeasio_config_path. */
    bool pipeasio_wow64_config_path(char *buf, size_t n);

    /* Live-reload fingerprint.  Zero means no config file. */
    uint64_t pipeasio_wow64_config_fingerprint(void);

    /* Bind the shared callback buffer and channel masks to the unix RT loop. */
    void pipeasio_wow64_bind_rt(audio_client_t *client, float *buffer_base, int buffer_size,
                                int n_in, int n_out, const bool *in_active, const bool *out_active);

#ifdef __cplusplus
}
#endif
