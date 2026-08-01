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

/* 64-bit unixlib dispatch layer for the 32-bit WoW64 front end. */

#define WINE_UNIX_LIB
#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <unixlib.h>

#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "audio.h"
#include "pipeasio_config.h"
#include "pipeasio_handle_table.h"
#include "pipeasio_offsets.h"
#include "pipeasio_rt_priority.h"
#include "pipeasio_unix_abi.h"

/* Minimum host-reply wait per RT cycle. */
#define PAU_RT_DEADLINE_FLOOR_NS 5000000L

/* Priority shared with the native callback thread. */
#define PAU_PUMP_RT_PRIORITY PIPEASIO_RT_PRIO_DEFAULT

enum token_kind
{
    TOKEN_CLIENT = 1,
    TOKEN_PORT   = 2
};

typedef struct token_entry
{
    void           *value;
    enum token_kind kind;
} token_entry;

static pipeasio_handle_table token_table;
static pthread_once_t        token_once = PTHREAD_ONCE_INIT;
static bool                  token_ready;

static void
token_initialize(void)
{
    token_ready = pipeasio_handle_table_init(&token_table);
}

static pa_handle
tok_add(void *value, enum token_kind kind)
{
    token_entry *entry;
    pa_handle    handle;
    pthread_once(&token_once, token_initialize);
    if (!token_ready || !value)
        return 0;
    entry = malloc(sizeof(*entry));
    if (!entry)
        return 0;
    entry->value = value;
    entry->kind  = kind;
    handle       = pipeasio_handle_table_insert(&token_table, entry);
    if (!handle)
        free(entry);
    return handle;
}

static void *
tok_get(pa_handle handle, enum token_kind kind)
{
    token_entry *entry;
    pthread_once(&token_once, token_initialize);
    if (!token_ready)
        return NULL;
    entry = pipeasio_handle_table_get(&token_table, handle);
    return entry && entry->kind == kind ? entry->value : NULL;
}

static void *
tok_clear(pa_handle handle, enum token_kind kind)
{
    token_entry *entry;
    void        *value = NULL;
    pthread_once(&token_once, token_initialize);
    if (!token_ready)
        return NULL;
    entry = pipeasio_handle_table_remove(&token_table, handle);
    if (entry)
    {
        if (entry->kind == kind)
            value = entry->value;
        free(entry);
    }
    return value;
}

/* Client context. */

typedef struct client_ctx
{
    audio_client_t *client;

    /* Set by PAU_BIND_RT. */
    audio_sample_t *buffer_base; /* PE callback_audio_buffer (one address space) */
    uint32_t        buffer_size; /* samples per channel-half                     */
    uint32_t        n_in;
    uint32_t        n_out;
    uint8_t         in_active[PAU_RT_MAX_PORTS];
    uint8_t         out_active[PAU_RT_MAX_PORTS];
    audio_port_t   *in_port[PAU_RT_MAX_PORTS]; /* by channel, from PAU_PORT_REGISTER */
    audio_port_t   *out_port[PAU_RT_MAX_PORTS];
    bool            half; /* current ASIO double-buffer index */
    pa_handle       port_handles[PAU_RT_MAX_PORTS * 2];
    uint32_t        port_handle_count;
    long            rt_deadline_ns; /* per-cycle reply budget           */

    /* RT/aux producer to PE pump bridge. */
    pthread_mutex_t prod_mutex;
    pthread_mutex_t mutex;
    pthread_cond_t  ready;
    pthread_cond_t  done;
    bool            sync_init;
    bool            rt_raised;     /* pump SCHED_FIFO self-raise done (one-shot) */
    bool            want_realtime; /* config.ini realtime, env overrides */
    bool            installed;
    bool            pending;
    bool            delivered;
    bool            reply_ready;
    bool            shutdown;
    uint32_t        seq;
    pa_wait_params  evt;
    uint32_t        produced;
    bool            admitted;
    bool            cycle_complete;
    int32_t         result;
} client_ctx;

static client_ctx *
cc_get(pa_handle handle)
{
    return tok_get(handle, TOKEN_CLIENT);
}

static audio_port_t *
port_get(pa_handle handle)
{
    return tok_get(handle, TOKEN_PORT);
}

/* Bridge synchronization. */

static bool
bridge_init(client_ctx *cc)
{
    pthread_condattr_t attr;
    int                rc;
    if (pthread_mutex_init(&cc->prod_mutex, NULL))
        return false;
    if (pthread_mutex_init(&cc->mutex, NULL))
        goto err_prod;
    if (pthread_cond_init(&cc->ready, NULL))
        goto err_mutex;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    rc = pthread_cond_init(&cc->done, &attr);
    pthread_condattr_destroy(&attr);
    if (rc)
        goto err_ready;
    cc->sync_init = true;
    return true;

err_ready:
    pthread_cond_destroy(&cc->ready);
err_mutex:
    pthread_mutex_destroy(&cc->mutex);
err_prod:
    pthread_mutex_destroy(&cc->prod_mutex);
    return false;
}

static void
bridge_reset(client_ctx *cc)
{
    if (!cc->sync_init)
        return;
    pthread_mutex_lock(&cc->mutex);
    cc->pending        = false;
    cc->delivered      = false;
    cc->reply_ready    = false;
    cc->shutdown       = false;
    cc->rt_raised      = false;
    cc->produced       = 0;
    cc->admitted       = false;
    cc->cycle_complete = false;
    cc->result         = 0;
    pthread_mutex_unlock(&cc->mutex);
}

/* Wake the pump and any blocked producer during deactivate/close. */
static void
bridge_shutdown(client_ctx *cc)
{
    if (!cc->sync_init)
        return;
    pthread_mutex_lock(&cc->mutex);
    cc->shutdown       = true;
    cc->pending        = false;
    cc->reply_ready    = true;
    cc->admitted       = false;
    cc->cycle_complete = true;
    pthread_cond_broadcast(&cc->ready);
    pthread_cond_broadcast(&cc->done);
    pthread_mutex_unlock(&cc->mutex);
}

static void
bridge_destroy(client_ctx *cc)
{
    if (!cc->sync_init)
        return;
    bridge_shutdown(cc);
    pthread_cond_destroy(&cc->done);
    pthread_cond_destroy(&cc->ready);
    pthread_mutex_destroy(&cc->mutex);
    pthread_mutex_destroy(&cc->prod_mutex);
    cc->sync_init = false;
}

static uint32_t
bridge_invoke_locked(client_ctx *cc, uint32_t kind, uint32_t index, uint32_t nframes,
                     uint64_t time_nsec, int32_t value, bool *admitted)
{
    struct timespec deadline;
    uint32_t        produced = 0;
    *admitted                = false;
    if (!cc->sync_init || !cc->installed)
        return 0;
    pthread_mutex_lock(&cc->mutex);
    if (cc->shutdown)
    {
        pthread_mutex_unlock(&cc->mutex);
        return 0;
    }
    memset(&cc->evt, 0, sizeof(cc->evt));
    cc->evt.version    = PIPEASIO_UNIX_ABI_VERSION;
    cc->evt.seq        = ++cc->seq;
    cc->evt.kind       = kind;
    cc->evt.index      = index;
    cc->evt.nframes    = nframes;
    cc->evt.time_nsec  = pa_i64_from(time_nsec);
    cc->evt.value      = value;
    cc->pending        = true;
    cc->delivered      = false;
    cc->reply_ready    = false;
    cc->cycle_complete = false;
    pthread_cond_signal(&cc->ready);
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += cc->rt_deadline_ns > 0 ? cc->rt_deadline_ns : PAU_RT_DEADLINE_FLOOR_NS;
    while (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_nsec -= 1000000000L;
        ++deadline.tv_sec;
    }
    while (!cc->reply_ready && !cc->shutdown)
    {
        int rc = cc->delivered ? pthread_cond_wait(&cc->done, &cc->mutex)
                               : pthread_cond_timedwait(&cc->done, &cc->mutex, &deadline);
        if (rc && !cc->delivered)
        {
            cc->pending = false;
            pthread_mutex_unlock(&cc->mutex);
            return 0;
        }
    }
    if (cc->reply_ready && !cc->shutdown)
    {
        produced  = cc->produced;
        *admitted = cc->admitted;
    }
    if (kind != PAU_CB_BUFFER_SWITCH)
    {
        cc->pending        = false;
        cc->cycle_complete = true;
        pthread_cond_broadcast(&cc->done);
    }
    pthread_mutex_unlock(&cc->mutex);
    return produced;
}

static void
bridge_complete_locked(client_ctx *cc)
{
    pthread_mutex_lock(&cc->mutex);
    cc->pending        = false;
    cc->cycle_complete = true;
    pthread_cond_broadcast(&cc->done);
    pthread_mutex_unlock(&cc->mutex);
}

static uint32_t
bridge_invoke(client_ctx *cc, uint32_t kind, uint32_t index, uint32_t nframes, uint64_t time_nsec,
              int32_t value)
{
    bool admitted;
    pthread_mutex_lock(&cc->prod_mutex);
    uint32_t produced = bridge_invoke_locked(cc, kind, index, nframes, time_nsec, value, &admitted);
    pthread_mutex_unlock(&cc->prod_mutex);
    return produced;
}

/* Backend callbacks. */

static int
wow64_rt_process(audio_nframes_t nframes, void *arg)
{
    client_ctx *cc       = arg;
    bool        admitted = false;
    uint32_t    produced;
    pthread_mutex_lock(&cc->prod_mutex);
    bool half = cc->half;
    if (!cc->buffer_base)
    {
        for (uint32_t i = 0; i < cc->n_out; ++i)
            if (cc->out_port[i])
                audio_port_publish_output(cc->out_port[i], NULL, nframes, false, false);
        pthread_mutex_unlock(&cc->prod_mutex);
        return 0;
    }
    for (uint32_t i = 0; i < cc->n_in; ++i)
        if (cc->in_active[i] && cc->in_port[i])
        {
            audio_sample_t *source = audio_port_get_buffer(cc->in_port[i], nframes);
            audio_sample_t *destination
                    = cc->buffer_base + pipeasio_host_input_offset_samples(i, cc->buffer_size)
                      + pipeasio_host_half_offset_samples(half, cc->buffer_size);
            if (source)
                memcpy(destination, source, sizeof(*destination) * nframes);
            else
                memset(destination, 0, sizeof(*destination) * nframes);
        }
    produced = bridge_invoke_locked(cc, PAU_CB_BUFFER_SWITCH, half, nframes,
                                    audio_get_time_nsec(cc->client), 0, &admitted);
    for (uint32_t i = 0; i < cc->n_out; ++i)
    {
        const audio_sample_t *source
                = admitted && produced
                          ? cc->buffer_base
                                    + pipeasio_host_output_offset_samples(i, cc->n_in,
                                                                          cc->buffer_size)
                                    + pipeasio_host_half_offset_samples(half, cc->buffer_size)
                          : NULL;
        if (cc->out_port[i])
            audio_port_publish_output(cc->out_port[i], source, nframes, admitted && produced,
                                      cc->out_active[i]);
    }
    if (admitted)
        cc->half = !half;
    bridge_complete_locked(cc);
    pthread_mutex_unlock(&cc->prod_mutex);
    return 0;
}

static int
wow64_sample_rate_cb(audio_nframes_t nframes, void *arg)
{
    bridge_invoke((client_ctx *)arg, PAU_CB_SAMPLE_RATE, 0, 0, 0, (int32_t)nframes);
    return 0;
}

/* Small helpers. */

#define PAU_CHECK(p)                                                                               \
    do                                                                                             \
    {                                                                                              \
        if (!(p) || ((const pa_open_params *)(p))->version != PIPEASIO_UNIX_ABI_VERSION)           \
            return STATUS_INVALID_PARAMETER;                                                       \
    } while (0)

static NTSTATUS
wow64_open(void *args)
{
    pa_open_params *p = args;
    client_ctx     *cc;
    uint32_t        status = 0;

    PAU_CHECK(p);
    cc = calloc(1, sizeof *cc);
    if (!cc)
        return STATUS_NO_MEMORY;
    if (!bridge_init(cc))
    {
        free(cc);
        return STATUS_NO_MEMORY;
    }
    cc->client = audio_open(p->name, p->options, &status);
    if (!cc->client)
    {
        bridge_destroy(cc);
        free(cc);
        p->client = 0;
        p->status = status;
        return STATUS_SUCCESS;
    }
    p->client = tok_add(cc, TOKEN_CLIENT);
    p->status = status;
    if (!p->client)
    {
        audio_close(cc->client);
        bridge_destroy(cc);
        free(cc);
        return STATUS_NO_MEMORY;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_close(void *args)
{
    pa_simple_params *p = args;
    client_ctx       *cc;

    PAU_CHECK(p);
    cc        = cc_get(p->client);
    p->result = 0;
    if (!cc)
        return STATUS_INVALID_HANDLE;
    bridge_shutdown(cc);
    for (uint32_t i = 0; i < cc->port_handle_count; ++i)
        tok_clear(cc->port_handles[i], TOKEN_PORT);
    cc->port_handle_count = 0;
    audio_close(cc->client);
    bridge_destroy(cc);
    tok_clear(p->client, TOKEN_CLIENT);
    free(cc);
    p->result = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_get_sample_rate(void *args)
{
    pa_simple_params *p = args;
    client_ctx       *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    p->result = audio_get_sample_rate(cc->client);
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_get_buffer_size(void *args)
{
    pa_simple_params *p = args;
    client_ctx       *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    p->result = audio_get_buffer_size(cc->client);
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_set_buffer_size(void *args)
{
    pa_set_u32_params *p = args;
    client_ctx        *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    p->result = audio_set_buffer_size(cc->client, p->value) ? 1 : 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_set_forced_rate(void *args)
{
    pa_set_u32_params *p = args;
    client_ctx        *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    audio_set_forced_rate(cc->client, p->value);
    p->result = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_set_follow_device(void *args)
{
    pa_set_u32_params *p = args;
    client_ctx        *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    audio_set_follow_device(cc->client, p->value != 0);
    p->result = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_set_realtime(void *args)
{
    pa_set_u32_params *p = args;
    client_ctx        *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    cc->want_realtime = (p->value != 0);
    p->result         = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_observed_quantum(void *args)
{
    pa_simple_params *p = args;
    client_ctx       *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    p->result = audio_observed_quantum(cc->client);
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_get_time_nsec(void *args)
{
    pa_time_params *p = args;
    client_ctx     *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    p->nsec = pa_i64_from(audio_get_time_nsec(cc->client));
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_default_changed(void *args)
{
    pa_simple_params *p = args;
    client_ctx       *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    p->result = audio_default_changed(cc->client) ? 1 : 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_port_register(void *args)
{
    pa_port_register_params *p = args;
    client_ctx              *cc;
    audio_port_t            *port;
    PAU_CHECK(p);
    p->port = 0;
    cc      = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    if (!memchr(p->name, '\0', sizeof(p->name)) || !p->name[0] || p->channel >= PAU_RT_MAX_PORTS
        || (p->flags != AUDIO_PORT_IS_INPUT && p->flags != AUDIO_PORT_IS_OUTPUT)
        || cc->port_handle_count >= PAU_RT_MAX_PORTS * 2)
        return STATUS_INVALID_PARAMETER;
    port = audio_port_register(cc->client, p->name, p->flags, p->channel);
    if (!port)
        return STATUS_SUCCESS;
    p->port = tok_add(port, TOKEN_PORT);
    if (!p->port)
    {
        audio_port_unregister(cc->client, port);
        return STATUS_NO_MEMORY;
    }
    if (p->flags == AUDIO_PORT_IS_INPUT)
        cc->in_port[p->channel] = port;
    else
        cc->out_port[p->channel] = port;
    cc->port_handles[cc->port_handle_count++] = p->port;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_port_unregister(void *args)
{
    pa_port_params *p = args;
    client_ctx     *cc;
    audio_port_t   *port;
    PAU_CHECK(p);
    p->result = 0;
    cc        = cc_get(p->client);
    port      = port_get(p->port);
    if (!cc || !port)
        return STATUS_INVALID_HANDLE;
    if (!audio_port_unregister(cc->client, port))
        return STATUS_UNSUCCESSFUL;
    for (uint32_t i = 0; i < PAU_RT_MAX_PORTS; ++i)
    {
        if (cc->in_port[i] == port)
            cc->in_port[i] = NULL;
        if (cc->out_port[i] == port)
            cc->out_port[i] = NULL;
    }
    for (uint32_t i = 0; i < cc->port_handle_count; ++i)
        if (cc->port_handles[i] == p->port)
        {
            memmove(&cc->port_handles[i], &cc->port_handles[i + 1],
                    (cc->port_handle_count - i - 1) * sizeof(cc->port_handles[0]));
            --cc->port_handle_count;
            break;
        }
    tok_clear(p->port, TOKEN_PORT);
    p->result = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_port_latency_range(void *args)
{
    pa_port_params       *p = args;
    audio_port_t         *port;
    audio_latency_range_t range = { 0, 0 };

    PAU_CHECK(p);
    port = port_get(p->port);
    if (!port)
        return STATUS_INVALID_HANDLE;
    audio_port_get_latency_range(port, p->mode, &range);
    p->lat_min = range.min;
    p->lat_max = range.max;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_get_device_ports(void *args)
{
    pa_ports_params  *p = args;
    client_ctx       *cc;
    audio_endpoint_t *endpoints;
    uint32_t          count = 0;
    PAU_CHECK(p);
    p->result   = 0;
    p->complete = 0;
    p->count    = 0;
    p->bytes    = 0;
    cc          = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    if (p->requested > PAU_ENDPOINT_MAX || !memchr(p->node, '\0', sizeof(p->node)))
        return STATUS_INVALID_PARAMETER;
    endpoints = calloc(p->requested ? p->requested : 1, sizeof(*endpoints));
    if (!endpoints)
        return STATUS_NO_MEMORY;
    if (!audio_get_device_endpoints(cc->client, p->node[0] ? p->node : NULL, p->flags, endpoints,
                                    p->requested, &count)
        || count > p->requested)
    {
        free(endpoints);
        return STATUS_SUCCESS;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        pa_endpoint_record *out = &p->endpoints[i];
        memcpy(out->key, endpoints[i].key, strlen(endpoints[i].key) + 1);
        memcpy(out->node_name, endpoints[i].node_name, strlen(endpoints[i].node_name) + 1);
        memcpy(out->port_name, endpoints[i].port_name, strlen(endpoints[i].port_name) + 1);
        out->node_id        = endpoints[i].node_id;
        out->port_id        = endpoints[i].port_id;
        out->direction      = endpoints[i].direction;
        out->global_port_id = endpoints[i].global_port_id;
        p->bytes += (uint32_t)strlen(out->key) + 1;
    }
    free(endpoints);
    p->count    = count;
    p->complete = 1;
    p->result   = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_connect(void *args)
{
    pa_connect_params *p = args;
    client_ctx        *cc;

    PAU_CHECK(p);
    if (!memchr(p->src, '\0', sizeof(p->src)) || !memchr(p->dst, '\0', sizeof(p->dst)))
        return STATUS_INVALID_PARAMETER;
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    p->ok = audio_connect(cc->client, p->src, p->dst) ? 1 : 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_activate(void *args)
{
    pa_simple_params *p = args;
    client_ctx       *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    p->result = audio_activate(cc->client) ? 1 : 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_deactivate(void *args)
{
    pa_simple_params *p = args;
    client_ctx       *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    bridge_shutdown(cc);
    p->result = audio_deactivate(cc->client) ? 1 : 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_install_callbacks(void *args)
{
    pa_simple_params *p = args;
    client_ctx       *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    bridge_reset(cc);
    if (!audio_set_process_callback(cc->client, wow64_rt_process, cc)
        || !audio_set_sample_rate_callback(cc->client, wow64_sample_rate_cb, cc))
        return STATUS_UNSUCCESSFUL;
    cc->installed = true;
    p->result     = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_bind_rt(void *args)
{
    pa_bind_params *p = args;
    client_ctx     *cc;
    audio_nframes_t rate;
    PAU_CHECK(p);
    p->result = 0;
    cc        = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    if (!p->buffer_base || !p->buffer_size || p->n_in > PAU_RT_MAX_PORTS
        || p->n_out > PAU_RT_MAX_PORTS)
        return STATUS_INVALID_PARAMETER;
    for (uint32_t i = 0; i < p->n_in; ++i)
        if (!cc->in_port[i])
            return STATUS_INVALID_PARAMETER;
    for (uint32_t i = 0; i < p->n_out; ++i)
        if (!cc->out_port[i])
            return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&cc->prod_mutex);
    cc->buffer_base = (audio_sample_t *)(uintptr_t)p->buffer_base;
    cc->buffer_size = p->buffer_size;
    cc->n_in        = p->n_in;
    cc->n_out       = p->n_out;
    memset(cc->in_active, 0, sizeof(cc->in_active));
    memset(cc->out_active, 0, sizeof(cc->out_active));
    memcpy(cc->in_active, p->in_active, p->n_in);
    memcpy(cc->out_active, p->out_active, p->n_out);
    cc->half           = false;
    rate               = audio_get_sample_rate(cc->client);
    cc->rt_deadline_ns = PAU_RT_DEADLINE_FLOOR_NS;
    if (rate)
    {
        long period = (long)((double)cc->buffer_size * 1.0e9 / (double)rate);
        if (2 * period > cc->rt_deadline_ns)
            cc->rt_deadline_ns = 2 * period;
    }
    pthread_mutex_unlock(&cc->prod_mutex);
    p->result = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_load_config(void *args)
{
    pa_config_params *p = args;

    PAU_CHECK(p);
    p->found = pipeasio_config_load(&p->cfg) ? 1 : 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_config_fingerprint(void *args)
{
    pa_fingerprint_params *p = args;
    char                   path[4096];
    struct stat            st;
    uint64_t               fp = 0;

    PAU_CHECK(p);
    if (pipeasio_config_path(path, sizeof path) && stat(path, &st) == 0)
    {
        fp = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
        fp ^= (uint64_t)st.st_size << 20;
        fp ^= (uint64_t)st.st_ino << 40;
        if (fp == 0)
            fp = 1; /* keep 0 reserved for "no file" */
    }
    p->fp = pa_i64_from(fp);
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_wait_callback(void *args)
{
    pa_wait_params *p = args;
    client_ctx     *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc || !cc->sync_init)
        return STATUS_INVALID_HANDLE;

    if (!cc->rt_raised)
    {
        cc->rt_raised      = true; /* one attempt per pump-thread lifetime */
        bool      realtime = cc->want_realtime;
        const int env      = pipeasio_rt_env_override();
        if (env != PIPEASIO_RT_ENV_UNSET)
            realtime = (env != 0);
        if (realtime)
            pthread_setschedparam(pthread_self(), SCHED_FIFO,
                                  &(struct sched_param){ .sched_priority = PAU_PUMP_RT_PRIORITY });
        /* best-effort: EPERM (no RLIMIT_RTPRIO / not in audio group) leaves
         * the pump at SCHED_OTHER, still functional. */
    }

    pthread_mutex_lock(&cc->mutex);
    while (!(cc->pending && !cc->delivered) && !cc->shutdown)
        pthread_cond_wait(&cc->ready, &cc->mutex);
    if (cc->shutdown)
    {
        p->shutdown = 1;
    }
    else
    {
        pa_handle owner = p->client;
        *p              = cc->evt;
        p->client       = owner;
        p->delivered    = 1;
        p->shutdown     = 0;
        cc->delivered   = true;
        pthread_cond_broadcast(&cc->done);
    }
    pthread_mutex_unlock(&cc->mutex);
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_reply_callback(void *args)
{
    pa_reply_params *p = args;
    client_ctx      *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc || !cc->sync_init)
        return STATUS_INVALID_HANDLE;

    pthread_mutex_lock(&cc->mutex);
    if (!cc->shutdown && cc->pending && p->seq == cc->seq)
    {
        cc->produced    = p->produced;
        cc->admitted    = p->admitted != 0;
        cc->result      = p->result;
        cc->reply_ready = true;
        pthread_cond_broadcast(&cc->done);
        if (cc->evt.kind == PAU_CB_BUFFER_SWITCH)
            while (!cc->cycle_complete && !cc->shutdown)
                pthread_cond_wait(&cc->done, &cc->mutex);
    }
    pthread_mutex_unlock(&cc->mutex);
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_reserved(void *args)
{
    PAU_CHECK(args);
    return STATUS_NOT_SUPPORTED;
}

/* Call tables.  Order must match enum pa_call. */

const unixlib_entry_t __wine_unix_call_funcs[] = {
    wow64_open,
    wow64_close,
    wow64_get_sample_rate,
    wow64_get_buffer_size,
    wow64_set_buffer_size,
    wow64_set_forced_rate,
    wow64_set_follow_device,
    wow64_observed_quantum,
    wow64_get_time_nsec,
    wow64_default_changed,
    wow64_port_register,
    wow64_port_unregister,
    wow64_reserved,
    wow64_reserved,
    wow64_reserved,
    wow64_port_latency_range,
    wow64_reserved,
    wow64_get_device_ports,
    wow64_connect,
    wow64_reserved,
    wow64_activate,
    wow64_deactivate,
    wow64_install_callbacks,
    wow64_bind_rt,
    wow64_load_config,
    wow64_config_fingerprint,
    wow64_wait_callback,
    wow64_reply_callback,
    wow64_set_realtime,
};

#ifdef _WIN64
/* The i386 PE front end dispatches here. */
const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    wow64_open,
    wow64_close,
    wow64_get_sample_rate,
    wow64_get_buffer_size,
    wow64_set_buffer_size,
    wow64_set_forced_rate,
    wow64_set_follow_device,
    wow64_observed_quantum,
    wow64_get_time_nsec,
    wow64_default_changed,
    wow64_port_register,
    wow64_port_unregister,
    wow64_reserved,
    wow64_reserved,
    wow64_reserved,
    wow64_port_latency_range,
    wow64_reserved,
    wow64_get_device_ports,
    wow64_connect,
    wow64_reserved,
    wow64_activate,
    wow64_deactivate,
    wow64_install_callbacks,
    wow64_bind_rt,
    wow64_load_config,
    wow64_config_fingerprint,
    wow64_wait_callback,
    wow64_reply_callback,
    wow64_set_realtime,
};
_Static_assert(sizeof(__wine_unix_call_wow64_funcs) / sizeof(__wine_unix_call_wow64_funcs[0])
                       == PAU_CALL_COUNT,
               "wow64 unix call table size drift");
#endif

_Static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == PAU_CALL_COUNT,
               "unix call table size drift");
