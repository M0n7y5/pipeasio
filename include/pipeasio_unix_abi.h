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

/* Version 3 makes endpoint and callback delivery transactional. */
#define PIPEASIO_UNIX_ABI_VERSION 3

typedef uint32_t pa_handle;

#define PAU_NAME_MAX 32
#define PAU_PORTNAME_MAX 256
#define PAU_RT_MAX_PORTS 256
#define PAU_ENDPOINT_MAX 256

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
    PAU_RESERVED_12,
    PAU_RESERVED_13,
    PAU_RESERVED_14,
    PAU_PORT_LATENCY_RANGE,
    PAU_RESERVED_16,
    PAU_GET_DEVICE_PORTS,
    PAU_CONNECT,
    PAU_RESERVED_19,
    PAU_ACTIVATE,
    PAU_DEACTIVATE,
    PAU_INSTALL_CALLBACKS,
    PAU_BIND_RT,
    PAU_LOAD_CONFIG,
    PAU_CONFIG_FINGERPRINT,
    PAU_WAIT_CALLBACK,
    PAU_REPLY_CALLBACK,
    PAU_SET_REALTIME,
    PAU_CALL_COUNT
};

/* Events sent from the Unix producer to the PE pump. Values are stable. */
enum pa_cb_kind
{
    PAU_CB_BUFFER_SWITCH = 0,
    PAU_CB_RESERVED_1    = 1,
    PAU_CB_SAMPLE_RATE   = 2,
    PAU_CB_RESERVED_3    = 3
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
 * PAU_SET_REALTIME. */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    uint32_t  value;
    uint32_t  result; /* out (bool, unused by void setters) */
} pa_set_u32_params;

/* PAU_GET_TIME_NSEC: audio_get_time_nsec(client) -> nsec. */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    pa_i64    nsec; /* out */
} pa_time_params;

/* PAU_PORT_REGISTER */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    char      name[PAU_NAME_MAX];
    uint32_t  flags;
    uint32_t  channel;
    pa_handle port;
} pa_port_register_params;

/* PAU_PORT_UNREGISTER / PAU_PORT_LATENCY_RANGE. */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    pa_handle port;
    uint32_t  mode;
    uint32_t  lat_min;
    uint32_t  lat_max;
    uint32_t  result;
} pa_port_params;

typedef struct
{
    char     key[PAU_PORTNAME_MAX];
    char     node_name[PAU_PORTNAME_MAX];
    char     port_name[PAU_PORTNAME_MAX];
    uint32_t node_id;
    uint32_t port_id;
    uint32_t direction;
    uint32_t global_port_id;
} pa_endpoint_record;

/* PAU_GET_DEVICE_PORTS. Returns up to `requested` sorted endpoints (a prefix) with result
 * non-zero, or result is zero on failure. count may be less than requested. */
typedef struct
{
    uint32_t           version;
    pa_handle          client;
    uint32_t           flags;
    uint32_t           requested;
    char               node[PAU_PORTNAME_MAX];
    uint32_t           result;
    uint32_t           complete;
    uint32_t           count;
    uint32_t           bytes;
    pa_endpoint_record endpoints[PAU_ENDPOINT_MAX];
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
    uint32_t  result;
} pa_bind_params;

/* PAU_WAIT_CALLBACK */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    uint32_t  seq;
    uint32_t  kind;
    uint32_t  index;
    uint32_t  nframes;
    pa_i64    time_nsec;
    int32_t   value;
    uint32_t  delivered;
    uint32_t  admitted;
    uint32_t  reply;
    uint32_t  cycle_complete;
    uint32_t  shutdown;
} pa_wait_params;

/* PAU_REPLY_CALLBACK */
typedef struct
{
    uint32_t  version;
    pa_handle client;
    uint32_t  seq;
    uint32_t  admitted;
    uint32_t  produced;
    uint32_t  reply;
    uint32_t  cycle_complete;
    int32_t   result;
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
