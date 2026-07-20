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

/* WoW64 unix-call ABI.  Structs must stay pointer-free and pack(4). */
#pragma once

#include <stdint.h>

#include "pipeasio_config.h"

#define PIPEASIO_UNIX_ABI_VERSION 1

/* Unix-side token handle.  Zero is NULL. */
typedef uint32_t pa_handle;

/* Fixed wire capacities. */
#define PAU_NAME_MAX 32      /* client name / registered port name             */
#define PAU_TYPE_MAX 64      /* port type string ("32 bit float mono audio")   */
#define PAU_PORTNAME_MAX 256 /* full PipeWire port / connection endpoint names  */
#define PAU_PORTS_BLOB 16384 /* packed NUL-terminated port-name list           */
#define PAU_RT_MAX_PORTS 256 /* PIPEASIO_MAX_CHANNELS                           */

/* Unix-call table indices.  Append only. */
enum pa_call
{
    PAU_OPEN = 0,
    PAU_CLOSE,
    PAU_GET_SAMPLE_RATE,
    PAU_GET_BUFFER_SIZE,
    PAU_SET_BUFFER_SIZE,
    PAU_SET_FORCED_RATE,
    PAU_SET_FOLLOW_DEVICE,
    PAU_OBSERVED_QUANTUM,
    PAU_GET_TIME_NSEC,
    PAU_DEFAULT_CHANGED,
    PAU_PORT_REGISTER,
    PAU_PORT_UNREGISTER,
    PAU_PORT_NAME,
    PAU_PORT_TYPE,
    PAU_PORT_BY_NAME,
    PAU_PORT_LATENCY_RANGE,
    PAU_GET_PORTS,
    PAU_GET_DEVICE_PORTS,
    PAU_CONNECT,
    PAU_GET_CLIENT_NAME,
    PAU_ACTIVATE,
    PAU_DEACTIVATE,
    PAU_INSTALL_CALLBACKS,
    PAU_BIND_RT,
    PAU_LOAD_CONFIG,
    PAU_CONFIG_FINGERPRINT,
    PAU_WAIT_CALLBACK,
    PAU_REPLY_CALLBACK,
    PAU_SET_RT_PRIORITY,
    PAU_SAVE_CONFIG,
    PAU_CONFIG_PATH,
    PAU_CALL_COUNT
};

/* Events sent from the unix producer to the PE pump. */
enum pa_cb_kind
{
    PAU_CB_BUFFER_SWITCH = 0,
    PAU_CB_BUFFER_SIZE,
    PAU_CB_SAMPLE_RATE,
    PAU_CB_LATENCY
};

#pragma pack(push, 4)

/* 64-bit scalar without 8-byte ABI alignment. */
typedef struct
{
    uint32_t lo;
    uint32_t hi;
} pa_i64;

/* PAU_OPEN: audio_open(name, options, &status) -> client */
typedef struct
{
    uint32_t  version;
    uint32_t  options;
    char      name[PAU_NAME_MAX];
    pa_handle client; /* out */
    uint32_t  status; /* out */
} pa_open_params;

/* PAU_CLOSE / PAU_ACTIVATE / PAU_DEACTIVATE / PAU_INSTALL_CALLBACKS /
 * PAU_DEFAULT_CHANGED (result = bool) and PAU_OBSERVED_QUANTUM /
 * PAU_GET_SAMPLE_RATE / PAU_GET_BUFFER_SIZE (result = nframes). */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    uint32_t  result; /* out */
} pa_simple_params;

/* PAU_SET_BUFFER_SIZE / PAU_SET_FORCED_RATE / PAU_SET_FOLLOW_DEVICE /
 * PAU_SET_RT_PRIORITY. */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    uint32_t  value;
    uint32_t  result; /* out (bool; unused by void setters) */
} pa_set_u32_params;

/* PAU_GET_TIME_NSEC: audio_get_time_nsec(client) -> nsec. */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    pa_i64    nsec; /* out */
} pa_time_params;

/* PAU_PORT_REGISTER: audio_port_register(client, name, type, flags, channel). */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    char      name[PAU_NAME_MAX];
    char      type[PAU_TYPE_MAX];
    uint32_t  flags;
    uint32_t  channel;
    pa_handle port; /* out */
} pa_port_register_params;

/* PAU_PORT_UNREGISTER / PAU_PORT_NAME / PAU_PORT_TYPE / PAU_PORT_BY_NAME /
 * PAU_PORT_LATENCY_RANGE.  `name` is in for PAU_PORT_BY_NAME, out for
 * PAU_PORT_NAME / PAU_PORT_TYPE; lat_* are out for PAU_PORT_LATENCY_RANGE. */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    pa_handle port;                   /* in (out for PAU_PORT_BY_NAME) */
    uint32_t  mode;                   /* in: audio_latency_mode_t     */
    uint32_t  lat_min;                /* out */
    uint32_t  lat_max;                /* out */
    char      name[PAU_PORTNAME_MAX]; /* in/out per call */
} pa_port_params;

/* PAU_GET_PORTS / PAU_GET_DEVICE_PORTS.  `names` packs `count` NUL-terminated
 * strings back-to-back (truncation-safe: handler stops before overflow). */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    uint32_t  flags;
    char      pattern[PAU_PORTNAME_MAX]; /* port_name_pattern (PAU_GET_PORTS) */
    char      type[PAU_PORTNAME_MAX];    /* type_name_pattern (PAU_GET_PORTS) */
    char      node[PAU_PORTNAME_MAX];    /* node.name (PAU_GET_DEVICE_PORTS)  */
    uint32_t  count;                     /* out */
    char      names[PAU_PORTS_BLOB];     /* out */
} pa_ports_params;

/* PAU_CONNECT: audio_connect(client, src, dst). */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    char      src[PAU_PORTNAME_MAX];
    char      dst[PAU_PORTNAME_MAX];
    uint32_t  ok; /* out */
} pa_connect_params;

/* PAU_GET_CLIENT_NAME / PAU_CONFIG_PATH: name is out (client unused for
 * PAU_CONFIG_PATH; empty name = failure). */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    char      name[PAU_PORTNAME_MAX]; /* out */
} pa_name_params;

/* PAU_LOAD_CONFIG: pipeasio_config_load(&cfg) -> found. */
typedef struct
{
    uint32_t               version;
    struct pipeasio_config cfg;   /* out */
    uint32_t               found; /* out */
} pa_config_params;

/* PAU_CONFIG_FINGERPRINT: stat-derived fingerprint of the config file (0 = absent). */
typedef struct
{
    uint32_t version;
    pa_i64   fp; /* out */
} pa_fingerprint_params;

/* PAU_BIND_RT: hand the PE-allocated shared callback buffer + channel activity
 * to the unix RT loop.  buffer_base is the 32-bit PE pointer (WoW64 single
 * address space) cast back to a real pointer unix-side. */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    uint32_t  buffer_base; /* (float *) host callback_audio_buffer */
    uint32_t  buffer_size; /* samples per channel-half             */
    uint32_t  n_in;
    uint32_t  n_out;
    uint8_t   in_active[PAU_RT_MAX_PORTS];
    uint8_t   out_active[PAU_RT_MAX_PORTS];
} pa_bind_params;

/* PAU_WAIT_CALLBACK: the pump blocks here until the unix RT/aux producer posts a
 * bridge event (or shutdown). */
typedef struct
{
    uint32_t  version;
    pa_handle client;    /* bridge owner */
    uint32_t  seq;       /* event sequence; guards against a late/stale reply */
    uint32_t  kind;      /* out: enum pa_cb_kind          */
    uint32_t  index;     /* out: buffer half (BUFFER_SWITCH) */
    uint32_t  nframes;   /* out: frames this cycle           */
    pa_i64    time_nsec; /* out: graph-clock timestamp       */
    int32_t   value;     /* out: nframes/rate/mode for aux events */
    uint32_t  shutdown;  /* out: 1 -> pump should exit       */
} pa_wait_params;

/* PAU_REPLY_CALLBACK: the pump returns the host's verdict for the event. */
typedef struct
{
    uint32_t  version;
    pa_handle client;   /* bridge owner */
    uint32_t  seq;      /* echoes the wait event's seq being answered */
    uint32_t  produced; /* 1 -> outputs were produced (scatter); 0 -> silence */
    int32_t   result;   /* host callback return value                          */
} pa_reply_params;

#pragma pack(pop)

/* 64-bit value packing helpers. */

static inline pa_i64
pa_i64_from(uint64_t v)
{
    pa_i64 r;
    r.lo = (uint32_t)(v & 0xFFFFFFFFu);
    r.hi = (uint32_t)(v >> 32);
    return r;
}

static inline uint64_t
pa_i64_to(pa_i64 v)
{
    return ((uint64_t)v.hi << 32) | (uint64_t)v.lo;
}
