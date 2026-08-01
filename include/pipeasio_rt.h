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

/* Host-callback entry points exported by src/asio.c for the WoW64 PE pump. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "audio.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum pipeasio_host_call_kind
    {
        PIPEASIO_HOST_PROCESS,
        PIPEASIO_HOST_SAMPLE_RATE,
        PIPEASIO_HOST_CONFIG_RESET,
        PIPEASIO_HOST_TIME_INFO
    } pipeasio_host_call_kind;

    typedef struct pipeasio_host_call_token
    {
        void                            *owner;
        void                            *callbacks;
        void                            *gate;
        void                            *idle_event;
        pipeasio_host_call_kind          kind;
        bool                             counted;
        bool                             admitted;
        struct pipeasio_host_call_token *previous;
    } pipeasio_host_call_token;

    bool pipeasio_host_call_begin(void *owner, pipeasio_host_call_kind kind,
                                  pipeasio_host_call_token *token);
    void pipeasio_host_call_end(pipeasio_host_call_token *token);
    bool pipeasio_host_call_is_reentrant(void *owner);

    void    pipeasio_host_call_process(pipeasio_host_call_token *token, int32_t buffer_index,
                                       audio_nframes_t add_samples, uint64_t time_nsec);
    int32_t pipeasio_host_call_notify(pipeasio_host_call_token *token, int32_t selector,
                                      int32_t value, void *message, double *opt);
    void    pipeasio_host_call_sample_rate(pipeasio_host_call_token *token,
                                           audio_nframes_t           sample_rate);

#ifdef __cplusplus
}
#endif
