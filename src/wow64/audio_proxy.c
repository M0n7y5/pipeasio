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
    pa_handle            unix_client;
    void                *This;
    char                 client_name[PAU_NAME_MAX];
    audio_sample_rate_cb rate_cb;
    void                *rate_arg;
    uint32_t             input_ports;
    uint32_t             output_ports;
    audio_port_t        *ports[PAU_RT_MAX_PORTS * 2];
    uint32_t             port_count;
    HANDLE               pump;
    DWORD                pump_tid;
} proxy_ctx;

typedef struct proxy_port
{
    pa_handle unix_token;
    char      local_name[PAU_NAME_MAX];
    uint32_t  flags;
} proxy_port;

static pa_handle
port_tok(const audio_port_t *port)
{
    return port ? ((const proxy_port *)port)->unix_token : 0;
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
        pa_wait_params           wait_params   = { 0 };
        pa_reply_params          reply         = { 0 };
        pipeasio_host_call_token process_token = { 0 };
        bool                     process_event = false;
        wait_params.version                    = PIPEASIO_UNIX_ABI_VERSION;
        wait_params.client                     = ctx->unix_client;
        if (UCALL(PAU_WAIT_CALLBACK, &wait_params) != 0 || wait_params.shutdown)
            break;
        if (!wait_params.delivered)
            continue;
        reply.version = PIPEASIO_UNIX_ABI_VERSION;
        reply.client  = ctx->unix_client;
        reply.seq     = wait_params.seq;
        reply.reply   = 1;
        switch (wait_params.kind)
        {
        case PAU_CB_BUFFER_SWITCH:
            process_event = true;
            if (pipeasio_host_call_begin(ctx->This, PIPEASIO_HOST_PROCESS, &process_token))
            {
                reply.admitted = 1;
                pipeasio_host_call_process(&process_token, (int32_t)wait_params.index,
                                           wait_params.nframes, pa_i64_to(wait_params.time_nsec));
                reply.produced = 1;
            }
            break;
        case PAU_CB_SAMPLE_RATE:
            if (ctx->rate_cb)
            {
                reply.admitted = 1;
                reply.result   = ctx->rate_cb((audio_nframes_t)wait_params.value, ctx->rate_arg);
            }
            break;
        default:
            break;
        }
        UCALL(PAU_REPLY_CALLBACK, &reply);
        if (process_event)
            pipeasio_host_call_end(&process_token);
    }
    return 0;
}

static void
pump_join(proxy_ctx *ctx)
{
    /* Deactivate may run on the pump thread (a host that stops from inside a
     * buffer switch). Avoid the self-join then: the handle stays set and a
     * later off-thread deactivate or audio_close joins and closes it. */
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
            *status = AUDIO_STATUS_NO_UNIXLIB;
        return NULL;
    }
    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
    {
        if (status)
            *status = AUDIO_STATUS_NO_MEMORY;
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
            *status = p.status ? p.status : AUDIO_STATUS_ERROR;
        return NULL;
    }
    memcpy(ctx->client_name, p.name, strlen(p.name) + 1);
    ctx->unix_client = p.client;
    if (status)
        *status = p.status;
    return (audio_client_t *)ctx;
}

static bool
proxy_deactivate(proxy_ctx *ctx)
{
    pa_simple_params params = { 0 };
    NTSTATUS         status;
    params.version = PIPEASIO_UNIX_ABI_VERSION;
    params.client  = ctx->unix_client;
    status         = UCALL(PAU_DEACTIVATE, &params);
    pump_join(ctx);
    return status == 0 && params.result != 0;
}

bool
audio_close(audio_client_t *client)
{
    proxy_ctx       *ctx    = (proxy_ctx *)client;
    pa_simple_params params = { 0 };
    bool             ok;
    if (!ctx)
        return false;
    if (ctx->pump)
        proxy_deactivate(ctx);
    params.version = PIPEASIO_UNIX_ABI_VERSION;
    params.client  = ctx->unix_client;
    ok             = UCALL(PAU_CLOSE, &params) == 0 && params.result != 0;
    for (uint32_t i = 0; i < ctx->port_count; ++i)
        free(ctx->ports[i]);
    free(ctx);
    return ok;
}

bool
audio_activate(audio_client_t *client)
{
    proxy_ctx       *ctx    = (proxy_ctx *)client;
    pa_simple_params params = { 0 };
    if (!ctx)
        return false;
    params.version = PIPEASIO_UNIX_ABI_VERSION;
    params.client  = ctx->unix_client;
    if (UCALL(PAU_INSTALL_CALLBACKS, &params) != 0 || !params.result)
        return false;
    ctx->pump = CreateThread(NULL, 8 * 1024 * 1024, pump_proc, ctx,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, &ctx->pump_tid);
    if (!ctx->pump)
    {
        proxy_deactivate(ctx);
        return false;
    }
    memset(&params, 0, sizeof(params));
    params.version = PIPEASIO_UNIX_ABI_VERSION;
    params.client  = ctx->unix_client;
    if (UCALL(PAU_ACTIVATE, &params) != 0 || !params.result)
    {
        proxy_deactivate(ctx);
        return false;
    }
    return true;
}

bool
audio_deactivate(audio_client_t *client)
{
    return client ? proxy_deactivate((proxy_ctx *)client) : false;
}
const char *
audio_get_client_name(audio_client_t *client)
{
    proxy_ctx *ctx = (proxy_ctx *)client;
    return ctx ? ctx->client_name : "";
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
audio_set_realtime(audio_client_t *client, bool realtime)
{
    set_u32((proxy_ctx *)client, PAU_SET_REALTIME, realtime ? 1 : 0);
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

bool
audio_latency_changed(audio_client_t *client)
{
    return simple_u32((proxy_ctx *)client, PAU_LATENCY_CHANGED) != 0;
}

audio_port_t *
audio_port_register(audio_client_t *client, const char *port_name, uint64_t flags, uint32_t channel)
{
    proxy_ctx              *ctx    = (proxy_ctx *)client;
    pa_port_register_params params = { 0 };
    proxy_port             *port;
    size_t                  length;
    if (!ctx || !port_name || ctx->port_count >= PAU_RT_MAX_PORTS * 2)
        return NULL;
    length = strlen(port_name);
    if (!length || length >= sizeof(params.name))
        return NULL;
    port = calloc(1, sizeof(*port));
    if (!port)
        return NULL;
    params.version = PIPEASIO_UNIX_ABI_VERSION;
    params.client  = ctx->unix_client;
    params.flags   = (uint32_t)flags;
    params.channel = channel;
    memcpy(params.name, port_name, length + 1);
    if (UCALL(PAU_PORT_REGISTER, &params) != 0 || !params.port)
    {
        free(port);
        return NULL;
    }
    port->unix_token = params.port;
    port->flags      = (uint32_t)flags;
    memcpy(port->local_name, port_name, length + 1);
    ctx->ports[ctx->port_count++] = (audio_port_t *)port;
    if (flags & AUDIO_PORT_IS_INPUT)
        ++ctx->input_ports;
    else
        ++ctx->output_ports;
    return (audio_port_t *)port;
}

bool
audio_port_unregister(audio_client_t *client, audio_port_t *opaque)
{
    proxy_ctx     *ctx    = (proxy_ctx *)client;
    proxy_port    *port   = (proxy_port *)opaque;
    pa_port_params params = { 0 };
    uint32_t       index;
    if (!ctx || !port)
        return false;
    for (index = 0; index < ctx->port_count; ++index)
        if (ctx->ports[index] == opaque)
            break;
    if (index == ctx->port_count)
        return false;
    params.version = PIPEASIO_UNIX_ABI_VERSION;
    params.client  = ctx->unix_client;
    params.port    = port->unix_token;
    if (UCALL(PAU_PORT_UNREGISTER, &params) != 0)
        return false;
    if (port->flags & AUDIO_PORT_IS_INPUT)
        --ctx->input_ports;
    else
        --ctx->output_ports;
    memmove(&ctx->ports[index], &ctx->ports[index + 1],
            (ctx->port_count - index - 1) * sizeof(ctx->ports[0]));
    --ctx->port_count;
    free(port);
    return true;
}

void *
audio_port_get_buffer(audio_port_t *port, audio_nframes_t nframes)
{
    (void)port;
    (void)nframes;
    return NULL;
}

audio_nframes_t
audio_port_buffer_avail_frames(const audio_port_t *port)
{
    (void)port;
    return 0;
}

bool
audio_port_get_name(const audio_port_t *opaque, char *out, size_t size)
{
    const proxy_port *port = (const proxy_port *)opaque;
    if (!out || !size)
        return false;
    out[0] = '\0';
    if (!port || strlen(port->local_name) >= size)
        return false;
    memcpy(out, port->local_name, strlen(port->local_name) + 1);
    return true;
}

bool
audio_port_publish_output(audio_port_t *port, const audio_sample_t *source, audio_nframes_t frames,
                          bool admitted, bool active)
{
    (void)port;
    (void)source;
    (void)frames;
    (void)admitted;
    (void)active;
    return false;
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

const char **
audio_get_device_ports(audio_client_t *client, const char *node_name, uint64_t flags)
{
    proxy_ctx       *ctx = (proxy_ctx *)client;
    pa_ports_params *params;
    const char     **ports   = NULL;
    uint32_t         decoded = 0;
    uint32_t         bytes   = 0;
    if (!ctx)
        return NULL;
    params = calloc(1, sizeof(*params));
    if (!params)
        return NULL;
    params->version   = PIPEASIO_UNIX_ABI_VERSION;
    params->client    = ctx->unix_client;
    params->flags     = (uint32_t)flags;
    params->requested = flags & AUDIO_PORT_IS_OUTPUT ? ctx->input_ports : ctx->output_ports;
    if (params->requested > PAU_ENDPOINT_MAX)
        goto fail;
    if (node_name)
    {
        size_t length = strlen(node_name);
        if (length >= sizeof(params->node))
            goto fail;
        memcpy(params->node, node_name, length + 1);
    }
    if (UCALL(PAU_GET_DEVICE_PORTS, params) != 0 || !params->result || !params->complete
        || params->count > params->requested || params->count > PAU_ENDPOINT_MAX)
        goto fail;
    ports = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                      ((size_t)params->count + 1) * sizeof(*ports));
    if (!ports)
        goto fail;
    for (; decoded < params->count; ++decoded)
    {
        const char *end = memchr(params->endpoints[decoded].key, '\0',
                                 sizeof(params->endpoints[decoded].key));
        if (!end || end == params->endpoints[decoded].key)
            goto fail;
        size_t length = (size_t)(end - params->endpoints[decoded].key) + 1;
        char  *copy   = HeapAlloc(GetProcessHeap(), 0, length);
        if (!copy)
            goto fail;
        memcpy(copy, params->endpoints[decoded].key, length);
        ports[decoded] = copy;
        bytes += (uint32_t)length;
    }
    if (bytes != params->bytes)
        goto fail;
    free(params);
    return ports;
fail:
    if (ports)
    {
        for (uint32_t i = 0; i < decoded; ++i)
            HeapFree(GetProcessHeap(), 0, (void *)ports[i]);
        HeapFree(GetProcessHeap(), 0, (void *)ports);
    }
    free(params);
    return NULL;
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
audio_connect(audio_client_t *client, const char *src, const char *dst)
{
    proxy_ctx        *ctx = (proxy_ctx *)client;
    pa_connect_params p;

    if (!ctx || !src || !dst || strlen(src) >= sizeof(p.src) || strlen(dst) >= sizeof(p.dst))
        return false;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    p.client  = ctx->unix_client;
    memcpy(p.src, src, strlen(src) + 1);
    memcpy(p.dst, dst, strlen(dst) + 1);
    if (UCALL(PAU_CONNECT, &p) != 0)
        return false;
    return p.ok != 0;
}

void
audio_free(void *ptr)
{
    if (ptr)
        HeapFree(GetProcessHeap(), 0, ptr);
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

    if (!out)
        return false;
    pipeasio_config_defaults(out);
    if (!ensure_unixlib())
        return false;
    memset(&p, 0, sizeof p);
    p.version = PIPEASIO_UNIX_ABI_VERSION;
    if (UCALL(PAU_LOAD_CONFIG, &p) != 0)
        return false;
    *out = p.cfg;
    return p.found != 0;
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

bool
pipeasio_wow64_bind_rt(audio_client_t *client, float *buffer_base, int buffer_size, int n_in,
                       int n_out, const bool *in_active, const bool *out_active)
{
    proxy_ctx     *ctx    = (proxy_ctx *)client;
    pa_bind_params params = { 0 };
    if (!ctx || !buffer_base || buffer_size <= 0 || n_in < 0 || n_out < 0 || n_in > PAU_RT_MAX_PORTS
        || n_out > PAU_RT_MAX_PORTS)
        return false;
    params.version     = PIPEASIO_UNIX_ABI_VERSION;
    params.client      = ctx->unix_client;
    params.buffer_base = pa_i64_from((uintptr_t)buffer_base);
    params.buffer_size = (uint32_t)buffer_size;
    params.n_in        = (uint32_t)n_in;
    params.n_out       = (uint32_t)n_out;
    for (uint32_t i = 0; i < params.n_in; ++i)
        params.in_active[i] = in_active && in_active[i];
    for (uint32_t i = 0; i < params.n_out; ++i)
        params.out_active[i] = out_active && out_active[i];
    return UCALL(PAU_BIND_RT, &params) == 0 && params.result != 0;
}
