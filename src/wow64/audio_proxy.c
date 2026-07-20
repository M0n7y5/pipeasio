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

/* 32-bit PE implementation of include/audio.h backed by the WoW64 unixlib. */

#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>
#include <unixlib.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pmmintrin.h>
#include <xmmintrin.h>

#include "audio.h"
#include "pipeasio_config.h"
#include "pipeasio_rt.h"
#include "pipeasio_unix_abi.h"
#include "pipeasio_wow64_pe.h"

/* Unixlib bootstrap. */

static BOOL
ensure_unixlib(void)
{
    static BOOL     attempted;
    static NTSTATUS status;

    if (!attempted)
    {
        status    = __wine_init_unix_call();
        attempted = TRUE;
    }
    return status == 0;
}

#define UCALL(code, params) WINE_UNIX_CALL((code), (params))

/* audio_client_t proxy passed to src/asio.c. */

typedef struct proxy_ctx
{
    pa_handle unix_client; /* token returned by PAU_OPEN */
    void     *This;        /* IPipeASIOImpl*, from audio_set_process_callback */

    audio_buffer_size_cb buf_cb;
    void                *buf_arg;
    audio_sample_rate_cb rate_cb;
    void                *rate_arg;
    audio_latency_cb     lat_cb;
    void                *lat_arg;

    HANDLE pump;
    DWORD  pump_tid;
} proxy_ctx;

/* Stable return buffers for const char* audio API results. */
static char g_client_name[PAU_PORTNAME_MAX];
static char g_port_name[PAU_PORTNAME_MAX];
static char g_port_type[PAU_PORTNAME_MAX];

static pa_handle
port_tok(const audio_port_t *port)
{
    return (pa_handle)(uintptr_t)port;
}

/* PE pump thread. */

static DWORD WINAPI
pump_proc(void *arg)
{
    proxy_ctx *ctx = arg;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    for (;;)
    {
        pa_wait_params  w;
        pa_reply_params r;

        memset(&w, 0, sizeof w);
        w.version = PIPEASIO_UNIX_ABI_VERSION;
        w.client  = ctx->unix_client;
        if (UCALL(PAU_WAIT_CALLBACK, &w) != 0 || w.shutdown)
            break;

        memset(&r, 0, sizeof r);
        r.version = PIPEASIO_UNIX_ABI_VERSION;
        r.client  = ctx->unix_client;
        r.seq     = w.seq;

        switch (w.kind)
        {
        case PAU_CB_BUFFER_SWITCH:
            /* Start() primes directly; process-time switches require Running. */
            if (pipeasio_host_is_running(ctx->This))
            {
                pipeasio_host_buffer_switch(ctx->This, (int32_t)w.index, w.nframes,
                                            pa_i64_to(w.time_nsec));
                r.produced = 1;
            }
            break;
        case PAU_CB_BUFFER_SIZE:
            if (ctx->buf_cb)
                r.result = ctx->buf_cb((audio_nframes_t)w.value, ctx->buf_arg);
            break;
        case PAU_CB_SAMPLE_RATE:
            if (ctx->rate_cb)
                r.result = ctx->rate_cb((audio_nframes_t)w.value, ctx->rate_arg);
            break;
        case PAU_CB_LATENCY:
            if (ctx->lat_cb)
                ctx->lat_cb((audio_latency_mode_t)w.value, ctx->lat_arg);
            break;
        default:
            break;
        }

        UCALL(PAU_REPLY_CALLBACK, &r);
    }
    return 0;
}

static void
pump_join(proxy_ctx *ctx)
{
    /* Deactivate may run on the pump thread (a host that stops from inside a
     * buffer switch); skip the self-join then and let audio_close join later.
     * That leaves ctx->pump set, so a restart without an intervening off-thread
     * deactivate would not respawn the pump - not reached in practice, as hosts
     * do not dispose buffers from a callback. */
    if (ctx->pump && GetCurrentThreadId() != ctx->pump_tid)
    {
        WaitForSingleObject(ctx->pump, INFINITE);
        CloseHandle(ctx->pump);
        ctx->pump     = NULL;
        ctx->pump_tid = 0;
    }
}

/* Lifecycle. */

audio_client_t *
audio_open(const char *client_name, uint32_t options, uint32_t *status)
{
    proxy_ctx     *ctx;
    pa_open_params p;

    if (!ensure_unixlib())
    {
        if (status)
            *status = 1;
        return NULL;
    }
    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
    {
        if (status)
            *status = 1;
        return NULL;
    }

    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.options = options;
    if (client_name)
    {
        strncpy(p.name, client_name, sizeof p.name - 1);
        p.name[sizeof p.name - 1] = '\0';
    }
    if (UCALL(PAU_OPEN, &p) != 0 || p.client == 0)
    {
        free(ctx);
        if (status)
            *status = p.status ? p.status : 1;
        return NULL;
    }
    ctx->unix_client = p.client;
    if (status)
        *status = p.status;
    return (audio_client_t *)ctx;
}

bool
audio_close(audio_client_t *client)
{
    proxy_ctx       *ctx = (proxy_ctx *)client;
    pa_simple_params p;

    if (!ctx)
        return false;
    if (ctx->pump)
    {
        /* Close without deactivate: unblock and join the pump before free. */
        pa_simple_params d;
        memset(&d, 0, sizeof d);
        d.version = PIPEASIO_UNIX_ABI_VERSION;
        d.client  = ctx->unix_client;
        UCALL(PAU_DEACTIVATE, &d);
        pump_join(ctx);
    }
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    UCALL(PAU_CLOSE, &p);
    free(ctx);
    return true;
}

bool
audio_activate(audio_client_t *client)
{
    proxy_ctx       *ctx = (proxy_ctx *)client;
    pa_simple_params p;

    if (!ctx)
        return false;

    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    UCALL(PAU_INSTALL_CALLBACKS, &p);

    if (!ctx->pump)
        ctx->pump = CreateThread(NULL, 8 * 1024 * 1024, pump_proc, ctx,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, &ctx->pump_tid);

    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    UCALL(PAU_ACTIVATE, &p);
    return p.result != 0;
}

bool
audio_deactivate(audio_client_t *client)
{
    proxy_ctx       *ctx = (proxy_ctx *)client;
    pa_simple_params p;

    if (!ctx)
        return false;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    UCALL(PAU_DEACTIVATE, &p); /* unix side also sets bridge shutdown */
    pump_join(ctx);
    return p.result != 0;
}

const char *
audio_get_client_name(audio_client_t *client)
{
    proxy_ctx     *ctx = (proxy_ctx *)client;
    pa_name_params p;

    g_client_name[0] = '\0';
    if (!ctx)
        return g_client_name;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    if (UCALL(PAU_GET_CLIENT_NAME, &p) == 0)
    {
        memcpy(g_client_name, p.name, sizeof g_client_name);
        g_client_name[sizeof g_client_name - 1] = '\0';
    }
    return g_client_name;
}

static uint32_t
simple_u32(proxy_ctx *ctx, unsigned int code)
{
    pa_simple_params p;
    if (!ctx)
        return 0;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    if (UCALL(code, &p) != 0)
        return 0;
    return p.result;
}

static uint32_t
set_u32(proxy_ctx *ctx, unsigned int code, uint32_t value)
{
    pa_set_u32_params p;
    if (!ctx)
        return 0;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    p.value   = value;
    if (UCALL(code, &p) != 0)
        return 0;
    return p.result;
}

audio_nframes_t
audio_get_sample_rate(audio_client_t *client)
{
    return simple_u32((proxy_ctx *)client, PAU_GET_SAMPLE_RATE);
}

audio_nframes_t
audio_get_buffer_size(audio_client_t *client)
{
    return simple_u32((proxy_ctx *)client, PAU_GET_BUFFER_SIZE);
}

bool
audio_set_buffer_size(audio_client_t *client, audio_nframes_t nframes)
{
    return set_u32((proxy_ctx *)client, PAU_SET_BUFFER_SIZE, nframes) != 0;
}

void
audio_set_forced_rate(audio_client_t *client, audio_nframes_t rate)
{
    set_u32((proxy_ctx *)client, PAU_SET_FORCED_RATE, rate);
}

void
audio_set_follow_device(audio_client_t *client, bool follow)
{
    set_u32((proxy_ctx *)client, PAU_SET_FOLLOW_DEVICE, follow ? 1 : 0);
}

void
audio_set_rt_priority(audio_client_t *client, int priority)
{
    set_u32((proxy_ctx *)client, PAU_SET_RT_PRIORITY, (uint32_t)priority);
}

audio_nframes_t
audio_observed_quantum(audio_client_t *client)
{
    return simple_u32((proxy_ctx *)client, PAU_OBSERVED_QUANTUM);
}

uint64_t
audio_get_time_nsec(audio_client_t *client)
{
    proxy_ctx     *ctx = (proxy_ctx *)client;
    pa_time_params p;

    if (!ctx)
        return 0;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    if (UCALL(PAU_GET_TIME_NSEC, &p) != 0)
        return 0;
    return pa_i64_to(p.nsec);
}

bool
audio_default_changed(audio_client_t *client)
{
    return simple_u32((proxy_ctx *)client, PAU_DEFAULT_CHANGED) != 0;
}

audio_port_t *
audio_port_register(audio_client_t *client, const char *port_name, const char *port_type,
                    uint64_t flags, uint64_t buffer_size)
{
    proxy_ctx              *ctx = (proxy_ctx *)client;
    pa_port_register_params p;

    if (!ctx)
        return NULL;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    p.flags   = (uint32_t)flags;
    p.channel = (uint32_t)buffer_size; /* asio.c passes the channel index here */
    if (port_name)
    {
        strncpy(p.name, port_name, sizeof p.name - 1);
        p.name[sizeof p.name - 1] = '\0';
    }
    if (port_type)
    {
        strncpy(p.type, port_type, sizeof p.type - 1);
        p.type[sizeof p.type - 1] = '\0';
    }
    if (UCALL(PAU_PORT_REGISTER, &p) != 0 || p.port == 0)
        return NULL;
    return (audio_port_t *)(uintptr_t)p.port;
}

bool
audio_port_unregister(audio_client_t *client, audio_port_t *port)
{
    proxy_ctx     *ctx = (proxy_ctx *)client;
    pa_port_params p;

    if (!ctx || !port)
        return false;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    p.port    = port_tok(port);
    UCALL(PAU_PORT_UNREGISTER, &p);
    return true;
}

void *
audio_port_get_buffer(audio_port_t *port, audio_nframes_t nframes)
{
    /* Process callbacks run in the unixlib. */
    (void)port;
    (void)nframes;
    return NULL;
}

audio_nframes_t
audio_port_buffer_avail_frames(const audio_port_t *port)
{
    /* Process callbacks run in the unixlib. */
    (void)port;
    return 0;
}

const char *
audio_port_name(const audio_port_t *port)
{
    pa_port_params p;

    g_port_name[0] = '\0';
    if (!port)
        return g_port_name;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.port    = port_tok(port);
    if (UCALL(PAU_PORT_NAME, &p) == 0)
    {
        memcpy(g_port_name, p.name, sizeof g_port_name);
        g_port_name[sizeof g_port_name - 1] = '\0';
    }
    return g_port_name;
}

const char *
audio_port_type(const audio_port_t *port)
{
    pa_port_params p;

    g_port_type[0] = '\0';
    if (!port)
        return g_port_type;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.port    = port_tok(port);
    if (UCALL(PAU_PORT_TYPE, &p) == 0)
    {
        memcpy(g_port_type, p.name, sizeof g_port_type);
        g_port_type[sizeof g_port_type - 1] = '\0';
    }
    return g_port_type;
}

audio_port_t *
audio_port_by_name(audio_client_t *client, const char *port_name)
{
    proxy_ctx     *ctx = (proxy_ctx *)client;
    pa_port_params p;

    if (!ctx || !port_name)
        return NULL;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    strncpy(p.name, port_name, sizeof p.name - 1);
    p.name[sizeof p.name - 1] = '\0';
    if (UCALL(PAU_PORT_BY_NAME, &p) != 0 || p.port == 0)
        return NULL;
    return (audio_port_t *)(uintptr_t)p.port;
}

void
audio_port_get_latency_range(audio_port_t *port, uint32_t mode, audio_latency_range_t *range)
{
    pa_port_params p;

    if (range)
    {
        range->min = 0;
        range->max = 0;
    }
    if (!port || !range)
        return;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.port    = port_tok(port);
    p.mode    = mode;
    if (UCALL(PAU_PORT_LATENCY_RANGE, &p) == 0)
    {
        range->min = p.lat_min;
        range->max = p.lat_max;
    }
}

/* Decode a packed NUL-separated port list. */
static const char **
build_port_array(const char *blob, uint32_t count)
{
    const char **arr;
    size_t       off = 0;

    arr = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (count + 1) * sizeof(char *));
    if (!arr)
        return NULL;
    for (uint32_t i = 0; i < count; i++)
    {
        size_t len = strlen(blob + off) + 1;
        char  *dup = HeapAlloc(GetProcessHeap(), 0, len);
        if (!dup)
            break;
        memcpy(dup, blob + off, len);
        arr[i] = dup;
        off += len;
    }
    return arr;
}

static const char **
get_ports_common(proxy_ctx *ctx, unsigned int code, const char *a, const char *b, const char *node,
                 uint64_t flags)
{
    pa_ports_params *p;
    const char     **arr;

    if (!ctx)
        return NULL;
    p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->version = PIPEASIO_UNIX_ABI_VERSION;
    p->client  = ctx->unix_client;
    p->flags   = (uint32_t)flags;
    if (a)
    {
        strncpy(p->pattern, a, sizeof p->pattern - 1);
        p->pattern[sizeof p->pattern - 1] = '\0';
    }
    if (b)
    {
        strncpy(p->type, b, sizeof p->type - 1);
        p->type[sizeof p->type - 1] = '\0';
    }
    if (node)
    {
        strncpy(p->node, node, sizeof p->node - 1);
        p->node[sizeof p->node - 1] = '\0';
    }
    if (UCALL(code, p) != 0)
    {
        free(p);
        return NULL;
    }
    arr = build_port_array(p->names, p->count);
    free(p);
    return arr;
}

const char **
audio_get_ports(audio_client_t *client, const char *port_name_pattern,
                const char *type_name_pattern, uint64_t flags)
{
    return get_ports_common((proxy_ctx *)client, PAU_GET_PORTS, port_name_pattern,
                            type_name_pattern, NULL, flags);
}

const char **
audio_get_device_ports(audio_client_t *client, const char *node_name, uint64_t flags)
{
    return get_ports_common((proxy_ctx *)client, PAU_GET_DEVICE_PORTS, NULL, NULL, node_name,
                            flags);
}

bool
audio_set_process_callback(audio_client_t *client, audio_process_cb cb, void *arg)
{
    proxy_ctx *ctx = (proxy_ctx *)client;
    (void)cb;
    if (!ctx)
        return false;
    ctx->This = arg;
    return true;
}

bool
audio_set_buffer_size_callback(audio_client_t *client, audio_buffer_size_cb cb, void *arg)
{
    proxy_ctx *ctx = (proxy_ctx *)client;
    if (!ctx)
        return false;
    ctx->buf_cb  = cb;
    ctx->buf_arg = arg;
    return true;
}

bool
audio_set_sample_rate_callback(audio_client_t *client, audio_sample_rate_cb cb, void *arg)
{
    proxy_ctx *ctx = (proxy_ctx *)client;
    if (!ctx)
        return false;
    ctx->rate_cb  = cb;
    ctx->rate_arg = arg;
    return true;
}

bool
audio_set_latency_callback(audio_client_t *client, audio_latency_cb cb, void *arg)
{
    proxy_ctx *ctx = (proxy_ctx *)client;
    if (!ctx)
        return false;
    ctx->lat_cb  = cb;
    ctx->lat_arg = arg;
    return true;
}

bool
audio_connect(audio_client_t *client, const char *src, const char *dst)
{
    proxy_ctx        *ctx = (proxy_ctx *)client;
    pa_connect_params p;

    if (!ctx || !src || !dst)
        return false;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    strncpy(p.src, src, sizeof p.src - 1);
    p.src[sizeof p.src - 1] = '\0';
    strncpy(p.dst, dst, sizeof p.dst - 1);
    p.dst[sizeof p.dst - 1] = '\0';
    if (UCALL(PAU_CONNECT, &p) != 0)
        return false;
    return p.ok != 0;
}

void
audio_free_ports(const char **ports)
{
    if (!ports)
        return;
    for (uint32_t i = 0; ports[i]; i++)
        HeapFree(GetProcessHeap(), 0, (void *)ports[i]);
    HeapFree(GetProcessHeap(), 0, (void *)ports);
}

/* PE seams used by src/asio.c. */

bool
pipeasio_wow64_load_config(struct pipeasio_config *out)
{
    pa_config_params p;

    if (!out || !ensure_unixlib())
        return false;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    if (UCALL(PAU_LOAD_CONFIG, &p) != 0)
        return false;
    *out = p.cfg;
    return p.found != 0;
}

bool
pipeasio_wow64_save_config(const struct pipeasio_config *c)
{
    pa_config_params p;

    if (!c || !ensure_unixlib())
        return false;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.cfg     = *c;
    if (UCALL(PAU_SAVE_CONFIG, &p) != 0)
        return false;
    return p.found != 0;
}

bool
pipeasio_wow64_config_path(char *buf, size_t n)
{
    pa_name_params p;

    if (!buf || !n || !ensure_unixlib())
        return false;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    if (UCALL(PAU_CONFIG_PATH, &p) != 0 || !p.name[0])
        return false;
    lstrcpynA(buf, p.name, (int)n);
    return true;
}

uint64_t
pipeasio_wow64_config_fingerprint(void)
{
    pa_fingerprint_params p;

    if (!ensure_unixlib())
        return 0;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    if (UCALL(PAU_CONFIG_FINGERPRINT, &p) != 0)
        return 0;
    return pa_i64_to(p.fp);
}

void
pipeasio_wow64_bind_rt(audio_client_t *client, float *buffer_base, int buffer_size, int n_in,
                       int n_out, const bool *in_active, const bool *out_active)
{
    proxy_ctx     *ctx = (proxy_ctx *)client;
    pa_bind_params p;

    if (!ctx)
        return;
    memset(&p, 0, sizeof p);
    p.version     = PIPEASIO_UNIX_ABI_VERSION;
    p.client      = ctx->unix_client;
    p.buffer_base = (uint32_t)(uintptr_t)buffer_base;
    p.buffer_size = (uint32_t)buffer_size;
    p.n_in        = n_in > 0 ? (uint32_t)n_in : 0;
    p.n_out       = n_out > 0 ? (uint32_t)n_out : 0;
    if (p.n_in > PAU_RT_MAX_PORTS)
        p.n_in = PAU_RT_MAX_PORTS;
    if (p.n_out > PAU_RT_MAX_PORTS)
        p.n_out = PAU_RT_MAX_PORTS;
    for (uint32_t i = 0; i < p.n_in; i++)
        p.in_active[i] = (in_active && in_active[i]) ? 1 : 0;
    for (uint32_t i = 0; i < p.n_out; i++)
        p.out_active[i] = (out_active && out_active[i]) ? 1 : 0;
    UCALL(PAU_BIND_RT, &p);
}
