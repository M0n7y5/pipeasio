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
#include "pipeasio_offsets.h"
#include "pipeasio_unix_abi.h"

/* Minimum host-reply wait per RT cycle. */
#define PAU_RT_DEADLINE_FLOOR_NS 5000000L

/* SCHED_FIFO priority the PE pump self-raises to (see wow64_wait_callback). */
#define PAU_PUMP_RT_PRIORITY 80

/* Token table for unix-side pointers. */

#define PAU_TOKEN_MAX 512

static void           *g_token[PAU_TOKEN_MAX];
static pthread_mutex_t g_token_lock = PTHREAD_MUTEX_INITIALIZER;

static pa_handle
tok_add(void *p)
{
    pa_handle h    = 0;
    pa_handle slot = 0;
    pthread_mutex_lock(&g_token_lock);
    for (uint32_t i = 0; i < PAU_TOKEN_MAX; i++)
    {
        if (g_token[i] == p)
        {
            h = i + 1;
            break;
        }
        if (!g_token[i] && !slot)
            slot = i + 1;
    }
    if (!h && slot)
    {
        g_token[slot - 1] = p;
        h                 = slot;
    }
    pthread_mutex_unlock(&g_token_lock);
    return h;
}

static void *
tok_get(pa_handle h)
{
    if (h == 0 || h > PAU_TOKEN_MAX)
        return NULL;
    return g_token[h - 1];
}

static void
tok_clear(pa_handle h)
{
    if (h == 0 || h > PAU_TOKEN_MAX)
        return;
    pthread_mutex_lock(&g_token_lock);
    g_token[h - 1] = NULL;
    pthread_mutex_unlock(&g_token_lock);
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
    bool            half;           /* current ASIO double-buffer index */
    long            rt_deadline_ns; /* per-cycle reply budget           */

    /* RT/aux producer to PE pump bridge. */
    pthread_mutex_t prod_mutex;
    pthread_mutex_t mutex;
    pthread_cond_t  ready;
    pthread_cond_t  done;
    bool            sync_init;
    bool            rt_raised; /* pump SCHED_FIFO self-raise done (one-shot) */
    bool            installed;
    bool            pending;
    bool            delivered;
    bool            reply_ready;
    bool            shutdown;
    uint32_t        seq;
    pa_wait_params  evt;
    uint32_t        produced;
    int32_t         result;
} client_ctx;

static client_ctx *
cc_get(pa_handle h)
{
    return (client_ctx *)tok_get(h);
}

static audio_port_t *
port_get(pa_handle h)
{
    return (audio_port_t *)tok_get(h);
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
    cc->pending     = false;
    cc->delivered   = false;
    cc->reply_ready = false;
    cc->shutdown    = false;
    cc->rt_raised   = false;
    cc->produced    = 0;
    cc->result      = 0;
    pthread_mutex_unlock(&cc->mutex);
}

/* Wake the pump and any blocked producer during deactivate/close. */
static void
bridge_shutdown(client_ctx *cc)
{
    if (!cc->sync_init)
        return;
    pthread_mutex_lock(&cc->mutex);
    cc->shutdown    = true;
    cc->pending     = false;
    cc->reply_ready = true;
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

/* Post one bridge event and wait boundedly for the PE pump.  prod_mutex
 * serializes the RT producer with the aux callbacks (buffer_size / sample_rate
 * / latency, fired on PipeWire's main-loop thread): if an aux invoke is in
 * flight, the RT cycle blocks on prod_mutex for up to rt_deadline_ns.  Aux
 * events occur only on renegotiation, so this stall window is acceptable. */
static uint32_t
bridge_invoke(client_ctx *cc, uint32_t kind, uint32_t index, uint32_t nframes, uint64_t time_nsec,
              int32_t value)
{
    uint32_t        produced = 0;
    struct timespec deadline;

    if (!cc->sync_init || !cc->installed)
        return 0;

    pthread_mutex_lock(&cc->prod_mutex);
    pthread_mutex_lock(&cc->mutex);
    if (cc->shutdown)
    {
        pthread_mutex_unlock(&cc->mutex);
        pthread_mutex_unlock(&cc->prod_mutex);
        return 0;
    }

    memset(&cc->evt, 0, sizeof cc->evt);
    cc->evt.version   = PIPEASIO_UNIX_ABI_VERSION;
    cc->evt.seq       = ++cc->seq;
    cc->evt.kind      = kind;
    cc->evt.index     = index;
    cc->evt.nframes   = nframes;
    cc->evt.time_nsec = pa_i64_from(time_nsec);
    cc->evt.value     = value;
    cc->pending       = true;
    cc->delivered     = false;
    cc->reply_ready   = false;
    pthread_cond_signal(&cc->ready);

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += cc->rt_deadline_ns > 0 ? cc->rt_deadline_ns : PAU_RT_DEADLINE_FLOOR_NS;
    while (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
    }

    while (!cc->reply_ready && !cc->shutdown)
    {
        int rc = pthread_cond_timedwait(&cc->done, &cc->mutex, &deadline);
        if (rc != 0) /* ETIMEDOUT or error: drop this cycle, keep the stream */
            break;
    }
    if (cc->reply_ready && !cc->shutdown)
        produced = cc->produced;
    cc->pending = false; /* abandon: a late reply with the old seq is ignored */
    pthread_mutex_unlock(&cc->mutex);
    pthread_mutex_unlock(&cc->prod_mutex);
    return produced;
}

/* Backend callbacks. */

static int
wow64_rt_process(audio_nframes_t nframes, void *arg)
{
    client_ctx *cc = arg;
    bool        half;
    uint32_t    produced;

    if (!cc->buffer_base)
    {
        for (uint32_t i = 0; i < cc->n_out; i++)
            if (cc->out_active[i] && cc->out_port[i])
            {
                audio_sample_t *dst = audio_port_get_buffer(cc->out_port[i], nframes);
                if (dst)
                    memset(dst, 0, sizeof(audio_sample_t) * nframes);
            }
        return 0;
    }

    half = cc->half;

    /* Gather. */
    for (uint32_t i = 0; i < cc->n_in; i++)
        if (cc->in_active[i] && cc->in_port[i])
        {
            audio_sample_t *src = audio_port_get_buffer(cc->in_port[i], nframes);
            audio_sample_t *dst = cc->buffer_base
                                  + pipeasio_host_input_offset_samples(i, cc->buffer_size)
                                  + pipeasio_host_half_offset_samples(half, cc->buffer_size);
            audio_nframes_t avail = audio_port_buffer_avail_frames(cc->in_port[i]);
            audio_nframes_t n     = (src && avail < nframes) ? avail : (src ? nframes : 0);
            if (n)
                memcpy(dst, src, sizeof(audio_sample_t) * n);
            if (n < nframes)
                memset(dst + n, 0, sizeof(audio_sample_t) * (nframes - n));
        }

    /* Host callback. */
    produced = bridge_invoke(cc, PAU_CB_BUFFER_SWITCH, half, nframes,
                             audio_get_time_nsec(cc->client), 0);

    /* Scatter or silence. */
    for (uint32_t i = 0; i < cc->n_out; i++)
        if (cc->out_active[i] && cc->out_port[i])
        {
            audio_sample_t *dst = audio_port_get_buffer(cc->out_port[i], nframes);
            if (!dst)
                continue;
            if (produced)
            {
                audio_sample_t *src
                        = cc->buffer_base
                          + pipeasio_host_output_offset_samples(i, cc->n_in, cc->buffer_size)
                          + pipeasio_host_half_offset_samples(half, cc->buffer_size);
                memcpy(dst, src, sizeof(audio_sample_t) * nframes);
            }
            else
            {
                memset(dst, 0, sizeof(audio_sample_t) * nframes);
            }
        }

    cc->half = !half;
    return 0;
}

static int
wow64_buffer_size_cb(audio_nframes_t nframes, void *arg)
{
    bridge_invoke((client_ctx *)arg, PAU_CB_BUFFER_SIZE, 0, 0, 0, (int32_t)nframes);
    return 0;
}

static int
wow64_sample_rate_cb(audio_nframes_t nframes, void *arg)
{
    bridge_invoke((client_ctx *)arg, PAU_CB_SAMPLE_RATE, 0, 0, 0, (int32_t)nframes);
    return 0;
}

static void
wow64_latency_cb(audio_latency_mode_t mode, void *arg)
{
    bridge_invoke((client_ctx *)arg, PAU_CB_LATENCY, 0, 0, 0, (int32_t)mode);
}

/* Small helpers. */

static void
copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

#define PAU_CHECK(p)                                                                               \
    do                                                                                             \
    {                                                                                              \
        if (!(p) || ((const pa_open_params *)(p))->version != PIPEASIO_UNIX_ABI_VERSION)           \
            return STATUS_INVALID_PARAMETER;                                                       \
    } while (0)

/* Unix-call handlers. */

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
    p->client = tok_add(cc);
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
    if (cc)
    {
        bridge_shutdown(cc);
        audio_close(cc->client);
        bridge_destroy(cc);
        tok_clear(p->client);
        free(cc);
        p->result = 1;
    }
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
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    port = audio_port_register(cc->client, p->name, p->type, p->flags, p->channel);
    if (!port)
    {
        p->port = 0;
        return STATUS_SUCCESS;
    }
    /* Map channel index to the RT port arrays. */
    if (p->channel < PAU_RT_MAX_PORTS)
    {
        if (p->flags & AUDIO_PORT_IS_INPUT)
            cc->in_port[p->channel] = port;
        else if (p->flags & AUDIO_PORT_IS_OUTPUT)
            cc->out_port[p->channel] = port;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_port_unregister(void *args)
{
    pa_port_params *p = args;
    client_ctx     *cc;
    audio_port_t   *port;

    PAU_CHECK(p);
    cc   = cc_get(p->client);
    port = port_get(p->port);
    if (!cc || !port)
        return STATUS_INVALID_HANDLE;
    audio_port_unregister(cc->client, port);
    for (uint32_t i = 0; i < PAU_RT_MAX_PORTS; i++)
    {
        if (cc->in_port[i] == port)
            cc->in_port[i] = NULL;
        if (cc->out_port[i] == port)
            cc->out_port[i] = NULL;
    }
    tok_clear(p->port);
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_port_name(void *args)
{
    pa_port_params *p = args;
    audio_port_t   *port;

    PAU_CHECK(p);
    port = port_get(p->port);
    if (!port)
        return STATUS_INVALID_HANDLE;
    copy_string(p->name, sizeof p->name, audio_port_name(port));
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_port_type(void *args)
{
    pa_port_params *p = args;
    audio_port_t   *port;

    PAU_CHECK(p);
    port = port_get(p->port);
    if (!port)
        return STATUS_INVALID_HANDLE;
    copy_string(p->name, sizeof p->name, audio_port_type(port));
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_port_by_name(void *args)
{
    pa_port_params *p = args;
    client_ctx     *cc;
    audio_port_t   *port;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    port    = audio_port_by_name(cc->client, p->name);
    p->port = port ? tok_add(port) : 0;
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

/* Pack a NULL-terminated port list into the wire blob. */
static void
pack_ports(const char **ports, char *blob, size_t blob_size, uint32_t *out_count)
{
    size_t   off   = 0;
    uint32_t count = 0;

    if (ports)
    {
        for (uint32_t i = 0; ports[i]; i++)
        {
            size_t len = strlen(ports[i]) + 1;
            if (off + len > blob_size)
                break;
            memcpy(blob + off, ports[i], len);
            off += len;
            count++;
        }
    }
    *out_count = count;
}

static NTSTATUS
wow64_get_ports(void *args)
{
    pa_ports_params *p = args;
    client_ctx      *cc;
    const char     **ports;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    ports = audio_get_ports(cc->client, p->pattern[0] ? p->pattern : NULL,
                            p->type[0] ? p->type : NULL, p->flags);
    pack_ports(ports, p->names, sizeof p->names, &p->count);
    audio_free_ports(ports);
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_get_device_ports(void *args)
{
    pa_ports_params *p = args;
    client_ctx      *cc;
    const char     **ports;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    ports = audio_get_device_ports(cc->client, p->node[0] ? p->node : NULL, p->flags);
    pack_ports(ports, p->names, sizeof p->names, &p->count);
    audio_free_ports(ports);
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_connect(void *args)
{
    pa_connect_params *p = args;
    client_ctx        *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    p->ok = audio_connect(cc->client, p->src, p->dst) ? 1 : 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
wow64_get_client_name(void *args)
{
    pa_name_params *p = args;
    client_ctx     *cc;

    PAU_CHECK(p);
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;
    copy_string(p->name, sizeof p->name, audio_get_client_name(cc->client));
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
    p->result = audio_deactivate(cc->client) ? 1 : 0;
    bridge_shutdown(cc); /* let the pump's PAU_WAIT return and the PE join it */
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
    bridge_reset(cc); /* fresh activation: clear any prior shutdown/pending */
    audio_set_process_callback(cc->client, wow64_rt_process, cc);
    audio_set_buffer_size_callback(cc->client, wow64_buffer_size_cb, cc);
    audio_set_sample_rate_callback(cc->client, wow64_sample_rate_cb, cc);
    audio_set_latency_callback(cc->client, wow64_latency_cb, cc);
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
    cc = cc_get(p->client);
    if (!cc)
        return STATUS_INVALID_HANDLE;

    cc->buffer_base = (audio_sample_t *)(uintptr_t)p->buffer_base;
    cc->buffer_size = p->buffer_size;
    cc->n_in        = p->n_in < PAU_RT_MAX_PORTS ? p->n_in : PAU_RT_MAX_PORTS;
    cc->n_out       = p->n_out < PAU_RT_MAX_PORTS ? p->n_out : PAU_RT_MAX_PORTS;
    memcpy(cc->in_active, p->in_active, sizeof cc->in_active);
    memcpy(cc->out_active, p->out_active, sizeof cc->out_active);
    cc->half = false;

    /* max(2 * cycle period, 5 ms). */
    rate               = audio_get_sample_rate(cc->client);
    cc->rt_deadline_ns = PAU_RT_DEADLINE_FLOOR_NS;
    if (rate && cc->buffer_size)
    {
        long period = (long)((double)cc->buffer_size * 1.0e9 / (double)rate);
        if (2 * period > cc->rt_deadline_ns)
            cc->rt_deadline_ns = 2 * period;
    }
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
        cc->rt_raised = true; /* one attempt per pump-thread lifetime */
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
        *p              = cc->evt; /* copy seq/kind/index/nframes/time/value */
        p->client       = owner;
        p->shutdown     = 0;
        cc->delivered   = true; /* consume: do not redeliver until next event */
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
        cc->result      = p->result;
        cc->reply_ready = true;
        pthread_cond_signal(&cc->done);
    }
    pthread_mutex_unlock(&cc->mutex);
    return STATUS_SUCCESS;
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
    wow64_port_name,
    wow64_port_type,
    wow64_port_by_name,
    wow64_port_latency_range,
    wow64_get_ports,
    wow64_get_device_ports,
    wow64_connect,
    wow64_get_client_name,
    wow64_activate,
    wow64_deactivate,
    wow64_install_callbacks,
    wow64_bind_rt,
    wow64_load_config,
    wow64_config_fingerprint,
    wow64_wait_callback,
    wow64_reply_callback,
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
    wow64_port_name,
    wow64_port_type,
    wow64_port_by_name,
    wow64_port_latency_range,
    wow64_get_ports,
    wow64_get_device_ports,
    wow64_connect,
    wow64_get_client_name,
    wow64_activate,
    wow64_deactivate,
    wow64_install_callbacks,
    wow64_bind_rt,
    wow64_load_config,
    wow64_config_fingerprint,
    wow64_wait_callback,
    wow64_reply_callback,
};
_Static_assert(sizeof(__wine_unix_call_wow64_funcs) / sizeof(__wine_unix_call_wow64_funcs[0])
                       == PAU_CALL_COUNT,
               "wow64 unix call table size drift");
#endif

_Static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == PAU_CALL_COUNT,
               "unix call table size drift");
