/*
 * Native libpipewire-0.3 backend for PipeASIO.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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

#define _GNU_SOURCE /* SCHED_FIFO and friends */

#include "audio.h"
#include "pipeasio_config.h"
#include "pipeasio_offsets.h"
#include "pipeasio_parse.h"
#include "pipeasio_pw_buffer.h"
#include "pipeasio_rt_priority.h"
#ifndef PIPEASIO_AUDIO_UNIXLIB
#define WIN32_LEAN_AND_MEAN
#include "windef.h"
#include "winbase.h"
#endif
#include "pipeasio_log.h"

/* Printed by audio_open to identify the loaded binary. */
#define PIPEASIO_BUILD_TAG __DATE__ " " __TIME__

#include <pipewire/pipewire.h>
#include <pipewire/thread.h>
#include <pipewire/extensions/metadata.h>
#include <spa/utils/json.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <pmmintrin.h> /* _MM_SET_DENORMALS_ZERO_MODE */
#include <xmmintrin.h> /* _MM_SET_FLUSH_ZERO_MODE */

/* RT/data-loop thread id for diagnostics. */
static unsigned long
audio_current_thread_id(void)
{
#ifdef PIPEASIO_AUDIO_UNIXLIB
    return (unsigned long)(uintptr_t)pthread_self();
#else
    return (unsigned long)GetCurrentThreadId();
#endif
}

/* Defaults before host negotiation and graph callbacks. */
#define AUDIO_DEFAULT_SAMPLE_RATE 48000u

/* Bound untrusted PipeWire registry data retained by the host. */
#define AUDIO_REGISTRY_MAX_NODES 1024u
#define AUDIO_REGISTRY_MAX_PORTS 4096u
#define AUDIO_REGISTRY_PROPERTY_MAX 1024u
#define AUDIO_REGISTRY_METADATA_MAX 4096u

/* Keep native and WoW64 scheduling values in sync. */
#define AUDIO_RT_PRIO_MIN PIPEASIO_RT_PRIO_MIN
#define AUDIO_RT_PRIO_MAX PIPEASIO_RT_PRIO_MAX
/* The thread-utils bridge bypasses module-rt, so resolve its default request here. */
#define AUDIO_RT_PRIO_DEFAULT PIPEASIO_RT_PRIO_DEFAULT

#ifndef PIPEASIO_AUDIO_UNIXLIB
/* Wine RT thread bridge for the PipeWire data loop. */

struct audio_rt_state
{
    HANDLE      win_handle; /* Win32 handle for join() */
    DWORD       win_tid;
    pthread_t   ptid;            /* captured inside the spawned thread */
    int         rt_priority;     /* current SCHED_FIFO priority, 0 = none */
    bool        want_realtime;   /* from config.ini, PIPEASIO_RT_PRIORITY wins */
    atomic_bool ready;           /* released once ptid is captured */
    void *(*user_entry)(void *); /* PipeWire-provided entry */
    void *user_arg;
    void *user_ret;
};

static DWORD WINAPI
audio_rt_trampoline(LPVOID raw)
{
    struct audio_rt_state *s = raw;

    s->ptid = pthread_self();
    atomic_store_explicit(&s->ready, true, memory_order_release);

    TRACE("rt thread entry: tid=%lx\n", (unsigned long)GetCurrentThreadId());

    /* Flush subnormal floats to zero on this RT thread: the ASIO host's DSP
     * runs here, and denormals can stall the CPU for hundreds of cycles. */
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    s->user_ret = s->user_entry(s->user_arg);
    return 0;
}

static struct spa_thread *
audio_rt_create(void *data, const struct spa_dict *props, void *(*entry)(void *), void *arg)
{
    struct audio_rt_state *s = data;
    (void)props;

    TRACE("rt_create: ENTRY entry=%p arg=%p\n", (void *)entry, arg);
    s->user_entry = entry;
    s->user_arg   = arg;
    atomic_store_explicit(&s->ready, false, memory_order_relaxed);

    /* The ASIO host callback chain can exceed Wine's 1 MB default stack. */
    s->win_handle = CreateThread(NULL, 8 * 1024 * 1024, audio_rt_trampoline, s,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, &s->win_tid);
    if (!s->win_handle)
    {
        ERR("CreateThread failed for PipeWire RT thread\n");
        return NULL;
    }

    while (!atomic_load_explicit(&s->ready, memory_order_acquire))
        sched_yield();

    return (struct spa_thread *)(uintptr_t)s->ptid;
}

static int
audio_rt_join(void *data, struct spa_thread *thread, void **retval)
{
    struct audio_rt_state *s = data;
    (void)thread;

    if (!s->win_handle)
        return -1;

    DWORD wait = WaitForSingleObject(s->win_handle, INFINITE);
    if (retval)
        *retval = s->user_ret;

    CloseHandle(s->win_handle);
    s->win_handle = NULL;
    return (wait == WAIT_OBJECT_0) ? 0 : -1;
}

static int
audio_rt_get_range(void *data, const struct spa_dict *props, int *min, int *max)
{
    (void)data;
    (void)props;
    *min = AUDIO_RT_PRIO_MIN;
    *max = AUDIO_RT_PRIO_MAX;
    return 0;
}

static int
audio_rt_acquire(void *data, struct spa_thread *thread, int priority)
{
    struct audio_rt_state *s = data;
    (void)thread;

    bool realtime = s->want_realtime;
    switch (pipeasio_rt_env_override())
    {
    case 0:
        realtime = false;
        break;
    case 1:
        realtime = true;
        break;
    default:
        break;
    }

    if (!realtime)
    {
        TRACE("rt thread left SCHED_OTHER (realtime disabled)\n");
        return 0;
    }

    /* SPA contract: priority <= 0 means "apply the configured default".
     * module-rt is bypassed by our thread-utils override, so map it here. */
    if (priority <= 0)
        priority = AUDIO_RT_PRIO_DEFAULT;
    if (priority > AUDIO_RT_PRIO_MAX)
        priority = AUDIO_RT_PRIO_MAX;

    int err = pthread_setschedparam(s->ptid, SCHED_FIFO,
                                    &(struct sched_param){ .sched_priority = priority });
    if (err == EPERM)
    {
        /* RLIMIT_RTPRIO may cap us below the default. Retry at the cap. */
        struct rlimit rl;
        if (getrlimit(RLIMIT_RTPRIO, &rl) == 0 && rl.rlim_cur > 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur < (rlim_t)priority)
        {
            priority = (int)rl.rlim_cur;
            err      = pthread_setschedparam(s->ptid, SCHED_FIFO,
                                             &(struct sched_param){ .sched_priority = priority });
        }
    }
    if (err)
    {
        WARN("pthread_setschedparam(SCHED_FIFO, %d) failed: %s\n", priority, strerror(err));
        return -1;
    }
    s->rt_priority = priority;
    TRACE("rt thread acquired SCHED_FIFO %d\n", priority);
    return 0;
}

static int
audio_rt_drop(void *data, struct spa_thread *thread)
{
    struct audio_rt_state *s = data;
    (void)thread;

    if (!s->rt_priority)
        return 0;

    int err = pthread_setschedparam(s->ptid, SCHED_OTHER,
                                    &(struct sched_param){ .sched_priority = 0 });
    if (err)
    {
        WARN("pthread_setschedparam(SCHED_OTHER) failed: %s\n", strerror(err));
        return -1;
    }
    s->rt_priority = 0;
    return 0;
}

static const struct spa_thread_utils_methods audio_rt_methods = {
    SPA_VERSION_THREAD_UTILS_METHODS,   .create = audio_rt_create,      .join = audio_rt_join,
    .get_rt_range = audio_rt_get_range, .acquire_rt = audio_rt_acquire, .drop_rt = audio_rt_drop,
};
#endif /* !PIPEASIO_AUDIO_UNIXLIB */

/* Opaque types backing audio.h. */

struct audio_client
{
    char                   *name;
    _Atomic audio_nframes_t sample_rate;
    audio_nframes_t         buffer_size;
    audio_nframes_t         forced_rate;      /* 0 = follow graph, else FORCE_RATE */
    bool                    follow_device;    /* skip FORCE_QUANTUM: follow target clock */
    _Atomic uint32_t        observed_quantum; /* last graph quantum seen while following */

    struct pw_thread_loop *loop;
    struct pw_context     *ctx;
    struct pw_core        *core;
    struct pw_data_loop   *data_loop;

#ifndef PIPEASIO_AUDIO_UNIXLIB
    struct audio_rt_state   rt;
    struct spa_thread_utils rt_iface;
#endif

    audio_process_cb     process_cb;
    void                *process_cb_arg;
    audio_sample_rate_cb sample_rate_cb;
    void                *sample_rate_cb_arg;

    bool active;

    /* pw_filter */

    struct pw_filter *filter;
    struct spa_hook   filter_listener;

    /* Mixer state from SPA_PARAM_Props; on the client because the node is
     * recreated per activation. */
    bool     muted;
    float    volume;
    float    channel_volumes[SPA_AUDIO_MAX_CHANNELS];
    uint32_t n_channel_volumes;

    /* Registered-port array.  audio_port_register appends; audio_activate
     * walks this to build the filter, audio_deactivate frees it. */
    audio_port_t **ports;
    uint32_t       n_ports;
    uint32_t       cap_ports;

    /* Last spa_io_position.clock.nsec - feeds audio_get_time_nsec. */
    _Atomic uint64_t   last_clock_nsec;
    bool               debug_enabled;
    bool               quantum_warned;
    bool               rate_announced; /* process announced a rate this activation */
    uint64_t           cycle_count;
    struct spa_source *diagnostic_event;
    _Atomic uint32_t   diagnostic_kind;
    _Atomic uint32_t   diagnostic_quantum;
    _Atomic uint32_t   diagnostic_buffer_size;
    _Atomic uint32_t   diagnostic_rate_num;
    _Atomic uint32_t   diagnostic_rate_denom;
    _Atomic uint64_t   diagnostic_cycle;
    _Atomic uint64_t   diagnostic_thread;
    _Atomic uint32_t   xrun_count;          /* cycles we missed, this activation */
    uint64_t           last_clock_position; /* previous cycle's clock.position */
    uint32_t           last_clock_id;       /* timeline the position belongs to */

    /* Latency the connected device chain reports, per direction, in samples.
     * Written on the filter loop from a peer's SPA_PARAM_Latency, read by
     * GetLatencies on a COM thread, hence atomic.  Indexed
     * [AUDIO_CAPTURE_LATENCY, AUDIO_PLAYBACK_LATENCY]. */
    _Atomic uint32_t device_latency[2];
    _Atomic bool     latency_changed;

    /* Registry walker */

    struct pw_registry *registry;
    struct spa_hook     registry_listener;
    struct spa_hook     core_listener;
    int                 sync_seq;
    uint32_t            our_node_id;

    /* "default" metadata object -> effective default sink/source node names,
     * used to resolve the panel's "Follow default" device choice. */
    struct pw_metadata *default_metadata;
    struct spa_hook     default_metadata_listener;
    char                default_sink_name[256];
    char                default_source_name[256];
    _Atomic bool        default_changed; /* metadata cb set this on a real switch */

    uint32_t default_metadata_id;
    bool     defaults_baselined;
    uint64_t default_sink_fingerprint;
    uint64_t default_source_fingerprint;
    bool     default_sink_resolved;
    bool     default_source_resolved;

    /* "settings" metadata object -> the graph's clock rate, so a follow-graph
     * (forced_rate == 0) driver reports the real rate to the host before the
     * first process cycle instead of AUDIO_DEFAULT_SAMPLE_RATE (issue #20). */
    struct pw_metadata *settings_metadata;
    struct spa_hook     settings_metadata_listener;
    uint32_t            settings_metadata_id;
    _Atomic uint32_t    graph_rate;       /* settings clock.rate, 0 = unknown */
    _Atomic uint32_t    graph_force_rate; /* settings clock.force-rate, 0 = none */
    /* Discovered remote nodes (hardware + apps) - cached for assembling
     * full port names ("node:port") and for audio_connect lookups. */
    struct audio_node_info **nodes;
    uint32_t                 n_nodes;
    uint32_t                 cap_nodes;
    bool                     node_limit_warned;

    /* Discovered remote ports.  Each entry is a heap audio_port_t with
     * pw_node_id / pw_port_id / name / type / flags filled in. The
     * filter-side fields stay zero. */
    audio_port_t    **discovered;
    uint32_t          n_discovered;
    uint32_t          cap_discovered;
    bool              port_limit_warned;
    struct pw_proxy **links;
    uint32_t          n_links;
    uint32_t          cap_links;
};

struct audio_node_info
{
    uint32_t id;
    char    *node_name;
    char    *display_name;
    char    *media_class;
};

struct audio_port
{
    audio_client_t *client;
    char           *name;
    char           *port_name;
    char           *node_name;
    uint32_t        port_id;
    uint64_t        flags;

    /* PipeWire port handle */

    enum pw_direction           direction;
    void                       *pw_filter_port; /* returned by pw_filter_add_port */
    _Atomic(struct pw_buffer *) cycle_buffer;
    /* mute * volume * channelVolumes[i]; written on the filter loop, read by process. */
    _Atomic float gain;

    /* --- PipeWire registry IDs (for audio_connect link-factory) -- */

    uint32_t pw_node_id;
    uint32_t pw_port_id;
};

/* per-port userdata block stored by pw_filter_add_port - holds a pointer
 * back to our audio_port so the filter events can find it. */
typedef audio_port_t *audio_port_ref_t;

/* Filter event forward declarations. */

static void audio_on_state_changed(void *userdata, enum pw_filter_state old,
                                   enum pw_filter_state state, const char *error);

static void audio_on_process(void *userdata, struct spa_io_position *position);

static void audio_on_param_changed(void *userdata, void *port_data, uint32_t id,
                                   const struct spa_pod *param);

static const struct pw_filter_events audio_filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = audio_on_state_changed,
    .param_changed = audio_on_param_changed,
    .process       = audio_on_process,
};

/* Core and registry event forward declarations. */

static void audio_on_core_done(void *userdata, uint32_t id, int seq);
static void audio_on_registry_global(void *userdata, uint32_t id, uint32_t permissions,
                                     const char *type, uint32_t version,
                                     const struct spa_dict *props);
static void audio_on_registry_global_remove(void *userdata, uint32_t id);

static const struct pw_core_events audio_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = audio_on_core_done,
};

static const struct pw_registry_events audio_registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global        = audio_on_registry_global,
    .global_remove = audio_on_registry_global_remove,
};

static void audio_teardown_filter(audio_client_t *c);
static void audio_sync(audio_client_t *c);
static void audio_adopt_own_ports(audio_client_t *c);
static void audio_refresh_defaults(audio_client_t *c);
static void audio_adopt_graph_rate(audio_client_t *c);
static void audio_diagnostic_event(void *data, uint64_t count);

/* Lifecycle. */

audio_client_t *
audio_open(const char *client_name, uint32_t options, uint32_t *status)
{
    (void)options;
    uint32_t err = AUDIO_STATUS_ERROR;
    if (status)
        *status = AUDIO_STATUS_OK;
    audio_client_t *c = calloc(1, sizeof(*c));
    if (!c)
    {
        if (status)
            *status = AUDIO_STATUS_NO_MEMORY;
        return NULL;
    }
    c->name = strdup(client_name ? client_name : "PipeASIO");
    if (!c->name)
    {
        err = AUDIO_STATUS_NO_MEMORY;
        goto fail_alloc;
    }
    atomic_init(&c->sample_rate, AUDIO_DEFAULT_SAMPLE_RATE);
    c->volume = 1.0f;
    atomic_init(&c->last_clock_nsec, 0);
    atomic_init(&c->observed_quantum, 0);
    atomic_init(&c->graph_rate, 0);
    atomic_init(&c->graph_force_rate, 0);
    atomic_init(&c->default_changed, false);
    atomic_init(&c->diagnostic_kind, 0);
    atomic_init(&c->diagnostic_quantum, 0);
    atomic_init(&c->diagnostic_buffer_size, 0);
    atomic_init(&c->diagnostic_rate_num, 0);
    atomic_init(&c->diagnostic_rate_denom, 0);
    atomic_init(&c->diagnostic_cycle, 0);
    atomic_init(&c->diagnostic_thread, 0);
    c->debug_enabled        = getenv("PIPEASIO_DEBUG") != NULL;
    c->buffer_size          = PIPEASIO_DEFAULT_BUFFER_SIZE;
    c->our_node_id          = SPA_ID_INVALID;
    c->default_metadata_id  = SPA_ID_INVALID;
    c->settings_metadata_id = SPA_ID_INVALID;
#ifndef PIPEASIO_AUDIO_UNIXLIB
    atomic_init(&c->rt.ready, false);
#endif

    pw_init(NULL, NULL);

    c->loop = pw_thread_loop_new(c->name, NULL);
    if (!c->loop)
    {
        ERR("pw_thread_loop_new(%s) failed\n", c->name);
        err = AUDIO_STATUS_NO_CONTEXT;
        goto fail_alloc;
    }

    c->ctx = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
    if (!c->ctx)
    {
        ERR("pw_context_new failed\n");
        err = AUDIO_STATUS_NO_CONTEXT;
        goto fail_loop;
    }

    /* Native build: create the PipeWire RT thread through CreateThread so it
     * has a Wine TEB before it calls back into the ASIO host. */
    c->data_loop = pw_context_get_data_loop(c->ctx);

    /* The context's acquire started the data loop with default pthread utils.
     * Stop (and join) it through those SAME utils before installing the Wine
     * bridge. Joining through the bridge would see win_handle == NULL and leak
     * the original thread.  audio_activate restarts the loop through the
     * bridge, so the RT thread is CreateThread'd and has a Wine TEB. */
    pw_data_loop_stop(c->data_loop);
#ifndef PIPEASIO_AUDIO_UNIXLIB
    c->rt.want_realtime = PIPEASIO_DEFAULT_REALTIME;
    c->rt_iface.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_ThreadUtils, SPA_VERSION_THREAD_UTILS,
                                           &audio_rt_methods, &c->rt);
    pw_data_loop_set_thread_utils(c->data_loop, &c->rt_iface);
#endif

    if (pw_thread_loop_start(c->loop) < 0)
    {
        ERR("pw_thread_loop_start failed\n");
        err = AUDIO_STATUS_NO_CONTEXT;
        goto fail_ctx;
    }

    pw_thread_loop_lock(c->loop);
    c->core = pw_context_connect(c->ctx, NULL, 0);
    if (!c->core)
    {
        pw_thread_loop_unlock(c->loop);
        ERR("pw_context_connect failed (is the PipeWire daemon running?)\n");
        err = AUDIO_STATUS_NO_DAEMON;
        goto fail_started;
    }
    c->diagnostic_event
            = pw_loop_add_event(pw_thread_loop_get_loop(c->loop), audio_diagnostic_event, c);

    /* Bind the registry and add listeners so we can walk the graph for
     * audio_get_ports / audio_port_by_name / audio_connect.  Then sync
     * the core once so the initial _global emission completes before
     * audio_open returns. */
    pw_core_add_listener(c->core, &c->core_listener, &audio_core_events, c);
    c->registry = pw_core_get_registry(c->core, PW_VERSION_REGISTRY, 0);
    if (c->registry)
        pw_registry_add_listener(c->registry, &c->registry_listener, &audio_registry_events, c);

    pw_thread_loop_unlock(c->loop);
    audio_sync(c);
    /* A second sync drains the initial property burst of the "default" and
     * "settings" metadata objects: they are bound during the first sync's
     * global emission, so their values (default.audio.sink/source, the graph
     * clock rate) only land on the next round-trip. */
    audio_sync(c);
    audio_refresh_defaults(c);
    c->defaults_baselined = true;
    atomic_store_explicit(&c->default_changed, false, memory_order_release);

    TRACE("audio_open(%s) -> %p [build " PIPEASIO_BUILD_TAG "] "
          "[pipewire headers %s, library %s] "
          "(registry sync done: %u nodes, %u ports discovered)\n",
          c->name, c, pw_get_headers_version(), pw_get_library_version(), c->n_nodes,
          c->n_discovered);
    return c;

fail_started:
    pw_thread_loop_stop(c->loop);
fail_ctx:
    pw_context_destroy(c->ctx);
fail_loop:
    pw_thread_loop_destroy(c->loop);
fail_alloc:
    free(c->name);
    free(c);
    if (status)
        *status = err;
    return NULL;
}

bool
audio_close(audio_client_t *c)
{
    if (!c)
        return false;

    /* Tear down the filter first so its destruction sees a live
     * thread loop / core to deliver events on. */
    if (c->active)
        audio_teardown_filter(c);

    if (c->loop)
    {
        pw_thread_loop_lock(c->loop);
        if (c->diagnostic_event)
        {
            pw_loop_destroy_source(pw_thread_loop_get_loop(c->loop), c->diagnostic_event);
            c->diagnostic_event = NULL;
        }
        if (c->default_metadata)
        {
            spa_hook_remove(&c->default_metadata_listener);
            pw_proxy_destroy((struct pw_proxy *)c->default_metadata);
            c->default_metadata = NULL;
        }
        if (c->settings_metadata)
        {
            spa_hook_remove(&c->settings_metadata_listener);
            pw_proxy_destroy((struct pw_proxy *)c->settings_metadata);
            c->settings_metadata = NULL;
        }
        if (c->registry)
        {
            spa_hook_remove(&c->registry_listener);
            pw_proxy_destroy((struct pw_proxy *)c->registry);
            c->registry = NULL;
        }
        if (c->core)
        {
            spa_hook_remove(&c->core_listener);
            pw_core_disconnect(c->core);
            c->core = NULL;
        }
        pw_thread_loop_unlock(c->loop);
        pw_thread_loop_stop(c->loop);
    }
    if (c->ctx)
        pw_context_destroy(c->ctx);
    if (c->loop)
        pw_thread_loop_destroy(c->loop);

    for (uint32_t i = 0; i < c->n_nodes; i++)
    {
        free(c->nodes[i]->node_name);
        free(c->nodes[i]->display_name);
        free(c->nodes[i]->media_class);
        free(c->nodes[i]);
    }
    free(c->nodes);
    for (uint32_t i = 0; i < c->n_discovered; i++)
    {
        free(c->discovered[i]->name);
        free(c->discovered[i]->port_name);
        free(c->discovered[i]->node_name);
        free(c->discovered[i]);
    }
    free(c->discovered);
    for (uint32_t i = 0; i < c->n_ports; i++)
    {
        audio_port_t *p = c->ports[i];
        free(p->name);
        free(p);
    }
    free(c->ports);
    free(c->links);

    free(c->name);
    free(c);
    return true;
}

/* Helper - tear down the pw_filter and per-port resources.
 * Safe to call multiple times. Clears all state to "not active". */
static void
audio_teardown_filter(audio_client_t *c)
{
    if (c->data_loop && c->loop)
    {
        pw_thread_loop_lock(c->loop);
        pw_data_loop_stop(c->data_loop);
        pw_thread_loop_unlock(c->loop);
    }
    if (c->n_links)
    {
        pw_thread_loop_lock(c->loop);
        while (c->n_links)
            pw_proxy_destroy(c->links[--c->n_links]);
        pw_thread_loop_unlock(c->loop);
    }
    if (c->filter)
    {
        pw_thread_loop_lock(c->loop);
        pw_filter_destroy(c->filter);
        c->filter = NULL;
        pw_thread_loop_unlock(c->loop);
    }
    for (uint32_t i = 0; i < c->n_ports; ++i)
    {
        audio_port_t *port   = c->ports[i];
        port->pw_filter_port = NULL;
        atomic_store_explicit(&port->cycle_buffer, NULL, memory_order_release);
    }
}

static float
audio_channel_volume(const audio_client_t *c, uint32_t channel)
{
    if (!c->n_channel_volumes)
        return 1.0f;
    if (channel >= c->n_channel_volumes)
        channel = c->n_channel_volumes - 1;
    return c->channel_volumes[channel];
}

static void
audio_apply_volume(audio_client_t *c)
{
    uint32_t channel = 0;
    for (uint32_t i = 0; i < c->n_ports; ++i)
    {
        audio_port_t *p = c->ports[i];
        if (p->direction != PW_DIRECTION_OUTPUT)
            continue;
        const float gain = c->muted ? 0.0f : c->volume * audio_channel_volume(c, channel++);
        atomic_store_explicit(&p->gain, gain, memory_order_relaxed);
    }
}

static uint32_t
audio_output_layout(const audio_client_t *c, float *volumes, uint32_t *positions)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < c->n_ports && n < SPA_AUDIO_MAX_CHANNELS; ++i)
    {
        if (c->ports[i]->direction != PW_DIRECTION_OUTPUT)
            continue;
        volumes[n] = audio_channel_volume(c, n);
        ++n;
    }
    for (uint32_t i = 0; i < n; ++i)
        positions[i] = n == 1   ? SPA_AUDIO_CHANNEL_MONO
                       : n == 2 ? (i == 0 ? SPA_AUDIO_CHANNEL_FL : SPA_AUDIO_CHANNEL_FR)
                                : SPA_AUDIO_CHANNEL_AUX0 + i;
    return n;
}

/* pw_filter forwards a set_param without storing it; what mixers read back
 * is whatever we publish. */
static struct spa_pod *
audio_build_props(const audio_client_t *c, struct spa_pod_builder *b)
{
    float          volumes[SPA_AUDIO_MAX_CHANNELS];
    uint32_t       positions[SPA_AUDIO_MAX_CHANNELS];
    const uint32_t n = audio_output_layout(c, volumes, positions);
    return spa_pod_builder_add_object(
            b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute, SPA_POD_Bool(c->muted),
            SPA_PROP_volume, SPA_POD_Float(c->volume), SPA_PROP_channelVolumes,
            SPA_POD_Array(sizeof(float), SPA_TYPE_Float, n, volumes), SPA_PROP_channelMap,
            SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id, n, positions));
}

/* pipewire-pulse lists a stream only with a node-level Format (collect.c,
 * validate_device_info); a filter has none of its own. */
static struct spa_pod *
audio_build_format(const audio_client_t *c, struct spa_pod_builder *b)
{
    float                     volumes[SPA_AUDIO_MAX_CHANNELS];
    struct spa_audio_info_raw info
            = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32P,
                                      .rate   = atomic_load_explicit(&c->sample_rate,
                                                                     memory_order_relaxed));
    info.channels = audio_output_layout(c, volumes, info.position);
    return spa_format_audio_raw_build(b, SPA_PARAM_Format, &info);
}

bool
audio_activate(audio_client_t *c)
{
    if (!c)
        return false;
    if (c->active)
        return true;
    if (!c->n_ports)
    {
        ERR("audio_activate called with no ports registered\n");
        return false;
    }
    c->quantum_warned = false;
    c->rate_announced = false;
    c->cycle_count    = 0;
    atomic_store_explicit(&c->diagnostic_kind, 0, memory_order_release);

    const size_t bsize_samples = c->buffer_size;
    const size_t bsize_bytes   = bsize_samples * sizeof(audio_sample_t);

    /* FORCE_QUANTUM is skipped only when following the device clock.
     * Stream/Output/Audio puts the host in volume mixers (#25) without
     * WirePlumber auto-linking it (that needs node.autoconnect=true);
     * Audio/Duplex would make it a default-sink candidate. */
    struct pw_properties *filter_props = pw_properties_new(
            PW_KEY_NODE_NAME, c->name, PW_KEY_NODE_DESCRIPTION, c->name, PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Duplex", PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_MEDIA_CLASS,
            "Stream/Output/Audio", PW_KEY_NODE_ALWAYS_PROCESS, "true", PW_KEY_NODE_GROUP,
            "group.dsp.0", "pipeasio.node", "1", NULL);
    if (!filter_props)
    {
        ERR("pw_properties_new (filter) failed\n");
        goto fail;
    }
    /* Force pins the value we want, lock stops another client moving it while we
     * run.  We are scheduled synchronously, so a quantum we did not ask for is a
     * glitch we cannot absorb.  Follow-device mode takes the device's quantum on
     * purpose and locks neither. */
    if (!c->follow_device)
    {
        pw_properties_setf(filter_props, PW_KEY_NODE_FORCE_QUANTUM, "%u", (unsigned)bsize_samples);
        pw_properties_set(filter_props, PW_KEY_NODE_LOCK_QUANTUM, "true");
    }
    if (c->forced_rate)
    {
        pw_properties_setf(filter_props, PW_KEY_NODE_FORCE_RATE, "%u", (unsigned)c->forced_rate);
        pw_properties_set(filter_props, PW_KEY_NODE_LOCK_RATE, "true");
    }
    audio_nframes_t sample_rate = atomic_load_explicit(&c->sample_rate, memory_order_acquire);
    pw_properties_setf(filter_props, PW_KEY_NODE_LATENCY, "%u/%u", (unsigned)bsize_samples,
                       (unsigned)sample_rate);
    TRACE("audio_activate: follow_device=%d quantum=%u forced_rate=%u (0=follow graph) "
          "latency=%u/%u\n",
          (int)c->follow_device, (unsigned)bsize_samples, (unsigned)c->forced_rate,
          (unsigned)bsize_samples, (unsigned)sample_rate);

    pw_thread_loop_lock(c->loop);

    c->filter = pw_filter_new_simple(pw_data_loop_get_loop(c->data_loop), c->name, filter_props,
                                     &audio_filter_events, c);
    if (!c->filter)
    {
        pw_thread_loop_unlock(c->loop);
        ERR("pw_filter_new_simple failed\n");
        goto fail;
    }

    /* Add every registered port to the filter.  The FORMAT_DSP property locks
     * each port to F32 DSP mono. The buffers param requests 2 buffers of one
     * ASIO period.  PW_FILTER_PORT_FLAG_MAP_BUFFERS makes pw_filter mmap the
     * daemon's shared buffer memory into datas[0].data, so the ASIO host
     * reads/writes the live buffer directly (see audio_on_process). */
    for (uint32_t i = 0; i < c->n_ports; i++)
    {
        audio_port_t *p = c->ports[i];

        struct pw_properties *pp = pw_properties_new(NULL, NULL);
        if (!pp)
        {
            pw_thread_loop_unlock(c->loop);
            ERR("pw_properties_new (port %u) failed\n", i);
            goto fail;
        }
        pw_properties_set(pp, PW_KEY_FORMAT_DSP, "32 bit float mono audio");
        pw_properties_set(pp, PW_KEY_PORT_NAME, p->name);

        uint8_t                param_buf[1024];
        struct spa_pod_builder b        = SPA_POD_BUILDER_INIT(param_buf, sizeof param_buf);
        const struct spa_pod  *params[] = {
            spa_pod_builder_add_object(
                    &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
                    SPA_POD_Int(2), SPA_PARAM_BUFFERS_size, SPA_POD_Int((int)bsize_bytes),
                    SPA_PARAM_BUFFERS_stride, SPA_POD_Int(sizeof(audio_sample_t)),
                    SPA_PARAM_BUFFERS_align, SPA_POD_Int((int)bsize_bytes),
                    SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)),
        };

        p->pw_filter_port
                = pw_filter_add_port(c->filter, p->direction, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                     sizeof(audio_port_ref_t), pp, params, SPA_N_ELEMENTS(params));
        if (!p->pw_filter_port)
        {
            pw_thread_loop_unlock(c->loop);
            ERR("pw_filter_add_port failed for port %u (%s)\n", i, p->name);
            goto fail;
        }
        *(audio_port_ref_t *)p->pw_filter_port = p;
    }

    /* CUSTOM_LATENCY is the only way a filter is told what its peers report.
     * Without it pw_filter swallows every SPA_PARAM_Latency (filter.c sets
     * emit=false) and runs default_latency(), which only recombines what we
     * published ourselves.  With the flag the values reach
     * audio_on_param_changed and GetLatencies can report the real device
     * delay. */
    audio_apply_volume(c);
    uint8_t                props_buf[1024];
    struct spa_pod_builder props_b = SPA_POD_BUILDER_INIT(props_buf, sizeof props_buf);
    const struct spa_pod  *props[]
            = { audio_build_props(c, &props_b), audio_build_format(c, &props_b) };
    if (pw_filter_connect(c->filter, PW_FILTER_FLAG_CUSTOM_LATENCY, props, 2) < 0)
    {
        pw_thread_loop_unlock(c->loop);
        ERR("pw_filter_connect failed\n");
        goto fail;
    }

    /* Connecting without PW_FILTER_FLAG_RT_PROCESS sets both
     * node.loop.class=main and node.async=true.  Keep the first: it schedules
     * this node on the loop passed to pw_filter_new_simple, our Wine-bridged
     * data loop, so process() can call the host's COM bufferSwitch on a thread
     * that has a TEB.  RT_PROCESS drops it for a PipeWire pool data-loop
     * thread that has none, which segfaults in ntdll.  Drop the second: it
     * makes every link carry an extra graph quantum, one buffer period of
     * round trip.  Clear it before any link exists, because connect always
     * sets it and impl-link latches link->async when a link is created.  A
     * follower keeps async so a device-driven quantum cannot stall the graph. */
    if (!c->follow_device)
    {
        struct spa_dict_item async_off[] = {
            SPA_DICT_ITEM_INIT(PW_KEY_NODE_ASYNC, "false"),
        };
        struct spa_dict dict = SPA_DICT_INIT(async_off, 1);
        pw_filter_update_properties(c->filter, NULL, &dict);
    }

    pw_thread_loop_unlock(c->loop);

    /* Start the data loop after add_port/connect; those need the thread-loop
     * context while the data loop is stopped. */
    pw_thread_loop_lock(c->loop);
    if (pw_data_loop_start(c->data_loop) < 0)
    {
        pw_thread_loop_unlock(c->loop);
        ERR("pw_data_loop_start failed\n");
        goto fail;
    }
    pw_thread_loop_unlock(c->loop);

    /* Wait until the async pw_filter_connect bind publishes a node id. */
    {
        pw_thread_loop_lock(c->loop);
        struct timespec abstime;
        pw_thread_loop_get_time(c->loop, &abstime, 5 * SPA_NSEC_PER_SEC); /* 5 s budget */
        for (;;)
        {
            uint32_t             nid = pw_filter_get_node_id(c->filter);
            enum pw_filter_state st  = pw_filter_get_state(c->filter, NULL);
            if (nid != SPA_ID_INVALID)
            {
                c->our_node_id = nid;
                break;
            }
            if (st == PW_FILTER_STATE_ERROR)
            {
                pw_thread_loop_unlock(c->loop);
                ERR("audio_activate: pw_filter reached ERROR state before bind\n");
                goto fail;
            }
            if (pw_thread_loop_timed_wait_full(c->loop, &abstime) < 0)
            {
                /* Last sync before giving up on the bind wait. */
                pw_thread_loop_unlock(c->loop);
                WARN("audio_activate: filter bind timed out, forcing sync\n");
                audio_sync(c);
                pw_thread_loop_lock(c->loop);
                c->our_node_id = pw_filter_get_node_id(c->filter);
                break;
            }
        }
        pw_thread_loop_unlock(c->loop);
    }

    TRACE("audio_activate: filter bound, our_node_id=%u\n", c->our_node_id);

    /* Port globals may arrive after the filter node is bound; adopt them with
     * a bounded poll so CreateBuffers can connect to hardware. */
    for (int attempt = 0; attempt < 50; attempt++)
    {
        audio_sync(c);
        audio_adopt_own_ports(c);
        uint32_t have = 0;
        for (uint32_t i = 0; i < c->n_ports; i++)
            if (c->ports[i]->pw_port_id != 0)
                have++;
        if (have == c->n_ports)
            break;
        usleep(20 * 1000);
    }

    /* Device latency starts unknown, so GetLatencies reports our own buffer
     * period alone until the first peer SPA_PARAM_Latency lands.  Clear it per
     * activation: the previous run's device may be gone. */
    atomic_store_explicit(&c->device_latency[AUDIO_CAPTURE_LATENCY], 0, memory_order_relaxed);
    atomic_store_explicit(&c->device_latency[AUDIO_PLAYBACK_LATENCY], 0, memory_order_relaxed);
    atomic_store_explicit(&c->latency_changed, false, memory_order_relaxed);
    atomic_store_explicit(&c->xrun_count, 0, memory_order_relaxed);
    c->last_clock_position = 0;
    c->last_clock_id       = 0;

    c->active = true;
    TRACE("audio_activate: %u ports, %u-sample buffers, %u Hz\n", c->n_ports, c->buffer_size,
          atomic_load_explicit(&c->sample_rate, memory_order_acquire));
    return true;

fail:
    audio_teardown_filter(c);
    return false;
}

bool
audio_deactivate(audio_client_t *c)
{
    if (!c)
        return false;
    if (!c->active)
        return true;
    audio_teardown_filter(c);
    c->active = false;
    return true;
}

const char *
audio_get_client_name(audio_client_t *c)
{
    return c ? c->name : NULL;
}

/* Properties. */

audio_nframes_t
audio_get_sample_rate(audio_client_t *c)
{
    return c ? atomic_load_explicit(&c->sample_rate, memory_order_acquire) : 0;
}

audio_nframes_t
audio_get_buffer_size(audio_client_t *c)
{
    return c ? c->buffer_size : 0;
}

bool
audio_set_buffer_size(audio_client_t *c, audio_nframes_t nframes)
{
    if (!c || !nframes)
        return false;
    c->buffer_size = nframes;
    /* Applied by the next audio_activate. */
    return true;
}

/* Follow-graph mode: adopt the settings metadata's clock rate while the
 * filter is inactive, so audio_get_sample_rate reports the real graph rate
 * before the first process cycle.  A running filter learns the authoritative
 * rate from the position io in audio_on_process, which also notifies the
 * host through sample_rate_cb - never override that path from here. */
static void
audio_adopt_graph_rate(audio_client_t *c)
{
    if (c->forced_rate || c->active)
        return;
    uint32_t rate = atomic_load_explicit(&c->graph_force_rate, memory_order_acquire);
    if (!rate)
        rate = atomic_load_explicit(&c->graph_rate, memory_order_acquire);
    if (rate)
        atomic_store_explicit(&c->sample_rate, rate, memory_order_release);
}

void
audio_set_forced_rate(audio_client_t *c, audio_nframes_t rate)
{
    if (!c)
        return;
    c->forced_rate = rate;
    if (rate)
        atomic_store_explicit(&c->sample_rate, rate, memory_order_release);
    else
        audio_adopt_graph_rate(c);
}

void
audio_set_follow_device(audio_client_t *c, bool follow)
{
    if (!c)
        return;
    c->follow_device = follow;
}

void
audio_set_realtime(audio_client_t *c, bool realtime)
{
    if (!c)
        return;
#ifndef PIPEASIO_AUDIO_UNIXLIB
    c->rt.want_realtime = realtime;
#else
    (void)realtime;
#endif
}

audio_nframes_t
audio_observed_quantum(audio_client_t *c)
{
    return c ? atomic_load(&c->observed_quantum) : 0;
}

uint64_t
audio_get_time_nsec(audio_client_t *c)
{
    return c ? atomic_load_explicit(&c->last_clock_nsec, memory_order_acquire) : 0;
}

/* Ports. */

audio_port_t *
audio_port_register(audio_client_t *c, const char *port_name, uint64_t flags, uint32_t channel)
{
    audio_port_t *port;
    char         *owned_name;
    (void)channel;
    if (!c || !port_name || !port_name[0] || strlen(port_name) >= 32
        || (!(flags & AUDIO_PORT_IS_INPUT) == !(flags & AUDIO_PORT_IS_OUTPUT)))
        return NULL;
    owned_name = strdup(port_name);
    if (!owned_name)
        return NULL;
    port = calloc(1, sizeof(*port));
    if (!port)
    {
        free(owned_name);
        return NULL;
    }
    if (c->n_ports == c->cap_ports)
    {
        uint32_t       capacity = c->cap_ports ? c->cap_ports * 2u : 16u;
        audio_port_t **ports    = realloc(c->ports, (size_t)capacity * sizeof(*ports));
        if (!ports)
        {
            free(port);
            free(owned_name);
            return NULL;
        }
        c->ports     = ports;
        c->cap_ports = capacity;
    }
    port->client    = c;
    port->name      = owned_name;
    port->flags     = flags;
    port->direction = (flags & AUDIO_PORT_IS_INPUT) ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT;
    atomic_init(&port->cycle_buffer, NULL);
    atomic_init(&port->gain, 1.0f);
    c->ports[c->n_ports++] = port;
    return port;
}

bool
audio_port_unregister(audio_client_t *c, audio_port_t *port)
{
    if (!c || !port)
        return false;
    uint32_t index;
    for (index = 0; index < c->n_ports; ++index)
        if (c->ports[index] == port)
            break;
    if (index == c->n_ports)
        return false;
    memmove(&c->ports[index], &c->ports[index + 1],
            (size_t)(c->n_ports - index - 1) * sizeof(*c->ports));
    --c->n_ports;
    free(port->name);
    free(port);
    return true;
}

void *
audio_port_get_buffer(audio_port_t *port, audio_nframes_t frames)
{
    struct pw_buffer *buffer;
    void             *region = NULL;
    if (!port)
        return NULL;
    buffer = atomic_load_explicit(&port->cycle_buffer, memory_order_acquire);
    return pipeasio_pw_validate_region(buffer, frames, &region, NULL) ? region : NULL;
}

audio_nframes_t
audio_port_buffer_avail_frames(const audio_port_t *port)
{
    struct pw_buffer *buffer;
    struct spa_data  *data;
    if (!port || !(buffer = atomic_load_explicit(&port->cycle_buffer, memory_order_acquire))
        || !buffer->buffer || buffer->buffer->n_datas < 1)
        return 0;
    data = &buffer->buffer->datas[0];
    if (!data->chunk || data->maxsize % sizeof(audio_sample_t)
        || data->chunk->offset > data->maxsize)
        return 0;
    return (data->maxsize - data->chunk->offset) / sizeof(audio_sample_t);
}

bool
audio_port_get_name(const audio_port_t *port, char *out, size_t size)
{
    if (!out || !size)
        return false;
    out[0] = '\0';
    if (!port || !port->name || strlen(port->name) >= size)
        return false;
    memcpy(out, port->name, strlen(port->name) + 1);
    return true;
}

static int
audio_queue_output(void *context, struct pw_buffer *buffer)
{
    audio_port_t *port = context;
    return port->pw_filter_port ? pw_filter_queue_buffer(port->pw_filter_port, buffer) : -1;
}

bool
audio_port_publish_output(audio_port_t *port, const audio_sample_t *source, audio_nframes_t frames,
                          bool admitted, bool active)
{
    if (!port || port->direction != PW_DIRECTION_OUTPUT)
        return false;
    return pipeasio_pw_finish_output(&port->cycle_buffer, source, frames,
                                     atomic_load_explicit(&port->gain, memory_order_relaxed),
                                     admitted, active, audio_queue_output, port);
}
static bool
audio_endpoint_key(const audio_port_t *port, char *out, size_t size)
{
    int written;
    if (!port || !out || !size)
        return false;
    written = snprintf(out, size, "pw:%u:%u", port->pw_node_id, port->pw_port_id);
    return written > 0 && (size_t)written < size;
}
static int
audio_compare_port_identity(const void *left, const void *right)
{
    const audio_port_t *a = *(audio_port_t *const *)left;
    const audio_port_t *b = *(audio_port_t *const *)right;
    if (a->port_id != b->port_id)
        return a->port_id < b->port_id ? -1 : 1;
    if (a->pw_port_id != b->pw_port_id)
        return a->pw_port_id < b->pw_port_id ? -1 : 1;
    return 0;
}

static uint32_t
audio_find_node_id(audio_client_t *client, const char *node_name)
{
    for (uint32_t i = 0; i < client->n_nodes; ++i)
        if (client->nodes[i]->node_name && !strcmp(client->nodes[i]->node_name, node_name))
            return client->nodes[i]->id;
    return SPA_ID_INVALID;
}

static audio_port_t **
audio_collect_device_port_objects(audio_client_t *client, const char *node_name, uint64_t flags,
                                  uint32_t *count_out)
{
    audio_port_t **ports  = NULL;
    uint32_t       count  = 0;
    uint32_t       target = SPA_ID_INVALID;
    *count_out            = 0;
    if (node_name && node_name[0])
        target = audio_find_node_id(client, node_name);
    else
    {
        const char *default_name = flags & AUDIO_PORT_IS_INPUT ? client->default_sink_name
                                                               : client->default_source_name;
        if (default_name[0])
            target = audio_find_node_id(client, default_name);
        else
            for (uint32_t i = 0; i < client->n_discovered; ++i)
                if ((client->discovered[i]->flags & flags) == flags
                    && (target == SPA_ID_INVALID || client->discovered[i]->pw_node_id < target))
                    target = client->discovered[i]->pw_node_id;
    }
    if (target == SPA_ID_INVALID)
        return NULL;
    for (uint32_t i = 0; i < client->n_discovered; ++i)
        if (client->discovered[i]->pw_node_id == target
            && (client->discovered[i]->flags & flags) == flags)
            ++count;
    if (!count)
        return NULL;
    ports = malloc((size_t)count * sizeof(*ports));
    if (!ports)
        return NULL;
    uint32_t out = 0;
    for (uint32_t i = 0; i < client->n_discovered; ++i)
        if (client->discovered[i]->pw_node_id == target
            && (client->discovered[i]->flags & flags) == flags)
            ports[out++] = client->discovered[i];
    qsort(ports, count, sizeof(*ports), audio_compare_port_identity);
    *count_out = count;
    return ports;
}

const char **
audio_get_device_ports(audio_client_t *client, const char *node_name, uint64_t flags)
{
    audio_port_t **objects;
    const char   **result = NULL;
    uint32_t       count  = 0;
    if (!client)
        return NULL;
    pw_thread_loop_lock(client->loop);
    objects = audio_collect_device_port_objects(client, node_name, flags, &count);
    result  = calloc((size_t)count + 1, sizeof(*result));
    if (result)
        for (uint32_t i = 0; i < count; ++i)
        {
            char key[256];
            if (!audio_endpoint_key(objects[i], key, sizeof(key)))
            {
                audio_free_ports(result);
                result = NULL;
                break;
            }
            result[i] = strdup(key);
            if (!result[i])
            {
                audio_free_ports(result);
                result = NULL;
                break;
            }
        }
    free(objects);
    pw_thread_loop_unlock(client->loop);
    return result;
}

bool
audio_get_device_endpoints(audio_client_t *client, const char *node_name, uint64_t flags,
                           audio_endpoint_t *endpoints, uint32_t capacity, uint32_t *count)
{
    audio_port_t **objects;
    uint32_t       found = 0;
    bool           valid = true;
    if (!client || !endpoints || !count)
        return false;
    *count = 0;
    pw_thread_loop_lock(client->loop);
    objects = audio_collect_device_port_objects(client, node_name, flags, &found);
    if (found > capacity)
        found = capacity;
    for (uint32_t i = 0; valid && i < found; ++i)
        valid = objects[i]->node_name && objects[i]->port_name
                && strlen(objects[i]->node_name) < sizeof(endpoints[i].node_name)
                && strlen(objects[i]->port_name) < sizeof(endpoints[i].port_name);
    if (valid)
        for (uint32_t i = 0; i < found; ++i)
        {
            memset(&endpoints[i], 0, sizeof(endpoints[i]));
            memcpy(endpoints[i].node_name, objects[i]->node_name,
                   strlen(objects[i]->node_name) + 1);
            memcpy(endpoints[i].port_name, objects[i]->port_name,
                   strlen(objects[i]->port_name) + 1);
            audio_endpoint_key(objects[i], endpoints[i].key, sizeof(endpoints[i].key));
            endpoints[i].node_id        = objects[i]->pw_node_id;
            endpoints[i].port_id        = objects[i]->port_id;
            endpoints[i].direction      = objects[i]->direction;
            endpoints[i].global_port_id = objects[i]->pw_port_id;
        }
    if (valid)
        *count = found;
    free(objects);
    pw_thread_loop_unlock(client->loop);
    return valid;
}

void
audio_port_get_latency_range(audio_port_t *p, uint32_t mode, audio_latency_range_t *range)
{
    if (!range)
        return;
    if (!p || !p->client)
    {
        range->min = range->max = 0;
        return;
    }
    /* One buffer period is ours: a captured frame is that old by the time the
     * host sees it, and a played frame waits that long before we hand it on.
     * Everything past our ports is what the device chain reported. */
    const uint32_t idx
            = mode == AUDIO_PLAYBACK_LATENCY ? AUDIO_PLAYBACK_LATENCY : AUDIO_CAPTURE_LATENCY;
    const uint32_t total
            = p->client->buffer_size
              + atomic_load_explicit(&p->client->device_latency[idx], memory_order_acquire);
    range->min = range->max = total;
}

bool
audio_latency_changed(audio_client_t *client)
{
    if (!client)
        return false;
    return atomic_exchange_explicit(&client->latency_changed, false, memory_order_acq_rel);
}

/* Callbacks. */

bool
audio_set_process_callback(audio_client_t *c, audio_process_cb cb, void *arg)
{
    if (!c)
        return false;
    c->process_cb     = cb;
    c->process_cb_arg = arg;
    return true;
}

bool
audio_set_sample_rate_callback(audio_client_t *c, audio_sample_rate_cb cb, void *arg)
{
    if (!c)
        return false;
    c->sample_rate_cb     = cb;
    c->sample_rate_cb_arg = arg;
    return true;
}

/* Connections, transport, and memory. */

static audio_port_t *
audio_lookup_port(audio_client_t *client, const char *name, uint32_t *node_id)
{
    char key[256];
    for (uint32_t i = 0; i < client->n_discovered; ++i)
        if ((!audio_endpoint_key(client->discovered[i], key, sizeof(key)) ? false
                                                                          : !strcmp(key, name))
            || !strcmp(client->discovered[i]->name, name))
        {
            *node_id = client->discovered[i]->pw_node_id;
            return client->discovered[i];
        }
    for (uint32_t i = 0; i < client->n_ports; ++i)
        if (!strcmp(client->ports[i]->name, name))
        {
            *node_id = client->our_node_id;
            return client->ports[i];
        }
    return NULL;
}

bool
audio_connect(audio_client_t *client, const char *source, const char *destination)
{
    struct pw_proxy *link;
    uint32_t         source_node      = SPA_ID_INVALID;
    uint32_t         destination_node = SPA_ID_INVALID;
    if (!client || !client->core || !source || !destination)
        return false;
    pw_thread_loop_lock(client->loop);
    audio_port_t *source_port      = audio_lookup_port(client, source, &source_node);
    audio_port_t *destination_port = audio_lookup_port(client, destination, &destination_node);
    if (!source_port || !destination_port || !source_port->pw_port_id
        || !destination_port->pw_port_id || source_node == SPA_ID_INVALID
        || destination_node == SPA_ID_INVALID)
        goto fail;
    if (client->n_links == client->cap_links)
    {
        uint32_t          capacity = client->cap_links ? client->cap_links * 2u : 16u;
        struct pw_proxy **links    = realloc(client->links, (size_t)capacity * sizeof(*links));
        if (!links)
            goto fail;
        client->links     = links;
        client->cap_links = capacity;
    }
    struct pw_properties *properties = pw_properties_new(PW_KEY_OBJECT_LINGER, "false", NULL);
    if (!properties)
        goto fail;
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u", source_node);
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u", source_port->pw_port_id);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u", destination_node);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u", destination_port->pw_port_id);
    link = pw_core_create_object(client->core, "link-factory", PW_TYPE_INTERFACE_Link,
                                 PW_VERSION_LINK, &properties->dict, 0);
    pw_properties_free(properties);
    if (!link)
        goto fail;
    client->links[client->n_links++] = link;
    pw_thread_loop_unlock(client->loop);
    return true;
fail:
    pw_thread_loop_unlock(client->loop);
    WARN("audio_connect: cannot connect %s -> %s\n", source, destination);
    return false;
}

void
audio_free(void *ptr)
{
    free(ptr);
}

void
audio_free_ports(const char **ports)
{
    if (!ports)
        return;
    for (size_t i = 0; ports[i]; i++)
        free((void *)ports[i]);
    free((void *)ports);
}

/* Filter callbacks run on the bridged data thread in native builds. */

static void
audio_on_state_changed(void *userdata, enum pw_filter_state old, enum pw_filter_state state,
                       const char *error)
{
    audio_client_t *c   = userdata;
    uint32_t        nid = c->filter ? pw_filter_get_node_id(c->filter) : SPA_ID_INVALID;
    TRACE("pw_filter state: %s -> %s (node_id=%u)\n", pw_filter_state_as_string(old),
          pw_filter_state_as_string(state), nid);

    /* Capture our node id as soon as the daemon binds the filter, so
     * that any port-global events the loop dispatches NEXT (still on
     * this same thread, before audio_activate's waiter resumes) can be
     * routed to the local-port backfill path in audio_cache_port. */
    if (nid != SPA_ID_INVALID && c->our_node_id == SPA_ID_INVALID)
        c->our_node_id = nid;

    if (state == PW_FILTER_STATE_ERROR && error)
        ERR("pw_filter entered ERROR state: %s\n", error);
    /* Wake any thread waiting in audio_activate for the node-id binding. */
    if (c->loop)
        pw_thread_loop_signal(c->loop, false);
}

/* Convert one direction of a spa_latency_info to samples.  The three terms are
 * additive (spa/param/latency.h): whole graph quanta, a raw sample count, and
 * nanoseconds.  Take the max end of each, which is the figure an ASIO host
 * needs for delay compensation. */
static uint32_t
audio_latency_info_samples(const struct spa_latency_info *info, uint32_t quantum, uint32_t rate)
{
    double samples = (double)info->max_quantum * (double)quantum;
    samples += (double)info->max_rate;
    if (rate)
        samples += (double)info->max_ns * (double)rate / 1000000000.0;
    if (!(samples > 0.0))
        return 0;
    if (samples > (double)UINT32_MAX)
        return UINT32_MAX;
    return (uint32_t)(samples + 0.5);
}

static void
audio_on_props(audio_client_t *c, const struct spa_pod *param)
{
    const struct spa_pod_prop *prop;
    if (!spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props))
        return;
    SPA_POD_OBJECT_FOREACH((const struct spa_pod_object *)param, prop)
    {
        switch (prop->key)
        {
        case SPA_PROP_mute:
            spa_pod_get_bool(&prop->value, &c->muted);
            break;
        case SPA_PROP_volume:
            spa_pod_get_float(&prop->value, &c->volume);
            break;
        case SPA_PROP_channelVolumes:
            c->n_channel_volumes = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
                                                      c->channel_volumes, SPA_AUDIO_MAX_CHANNELS);
            break;
        }
    }
    audio_apply_volume(c);
    uint8_t                buf[1024];
    struct spa_pod_builder b        = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod  *params[] = { audio_build_props(c, &b) };
    pw_filter_update_params(c->filter, NULL, params, 1);
    TRACE("props: mute=%d volume=%.3f channels=%u\n", (int)c->muted, (double)c->volume,
          (unsigned)c->n_channel_volumes);
}

static void
audio_on_param_changed(void *userdata, void *port_data, uint32_t id, const struct spa_pod *param)
{
    audio_client_t         *c = userdata;
    struct spa_latency_info info;

    if (!param)
        return;
    if (id == SPA_PARAM_Props && !port_data)
    {
        audio_on_props(c, param);
        return;
    }
    /* Global (port_data == NULL) params carry no port latency. */
    if (id != SPA_PARAM_Latency || !port_data)
        return;
    if (spa_latency_parse(param, &info) < 0)
        return;

    /* pw_filter forwards the direction opposite to the port's own, so an
     * OUTPUT-direction info is the capture chain feeding our inputs and an
     * INPUT-direction one is the playback chain behind our outputs.  Ports on
     * the same side report the same figure; keep the worst case. */
    const uint32_t idx     = info.direction == SPA_DIRECTION_OUTPUT ? AUDIO_CAPTURE_LATENCY
                                                                    : AUDIO_PLAYBACK_LATENCY;
    const uint32_t rate    = atomic_load_explicit(&c->sample_rate, memory_order_acquire);
    const uint32_t quantum = c->buffer_size;
    const uint32_t samples = audio_latency_info_samples(&info, quantum, rate);

    uint32_t previous = atomic_load_explicit(&c->device_latency[idx], memory_order_relaxed);
    if (samples <= previous)
        return;
    atomic_store_explicit(&c->device_latency[idx], samples, memory_order_release);
    atomic_store_explicit(&c->latency_changed, true, memory_order_release);
    TRACE("latency: %s device chain %u samples (was %u)\n",
          idx == AUDIO_CAPTURE_LATENCY ? "capture" : "playback", samples, previous);
}

enum
{
    AUDIO_DIAGNOSTIC_QUANTUM = 1u,
    AUDIO_DIAGNOSTIC_TRACE   = 2u,
    AUDIO_DIAGNOSTIC_XRUN    = 4u
};

static void
audio_diagnostic_event(void *data, uint64_t count)
{
    audio_client_t *client = data;
    uint32_t kind    = atomic_exchange_explicit(&client->diagnostic_kind, 0, memory_order_acq_rel);
    uint32_t quantum = atomic_load_explicit(&client->diagnostic_quantum, memory_order_acquire);
    uint32_t buffer_size
            = atomic_load_explicit(&client->diagnostic_buffer_size, memory_order_acquire);
    (void)count;
    if (kind & AUDIO_DIAGNOSTIC_QUANTUM)
        WARN("PipeWire quantum %u differs from host buffer size %u\n", quantum, buffer_size);
    if (kind & AUDIO_DIAGNOSTIC_XRUN)
        WARN("xrun: missed the cycle deadline (%u this activation, buffer size %u)\n",
             atomic_load_explicit(&client->xrun_count, memory_order_acquire), buffer_size);
    if (kind & AUDIO_DIAGNOSTIC_TRACE)
        TRACE("process: cycle=%lu tid=%lx buffer_size=%u quantum=%u rate=%u/%u\n",
              (unsigned long)atomic_load_explicit(&client->diagnostic_cycle, memory_order_acquire),
              (unsigned long)atomic_load_explicit(&client->diagnostic_thread, memory_order_acquire),
              buffer_size, quantum,
              atomic_load_explicit(&client->diagnostic_rate_num, memory_order_acquire),
              atomic_load_explicit(&client->diagnostic_rate_denom, memory_order_acquire));
}

static void
audio_signal_diagnostic(audio_client_t *client, uint32_t kind, uint32_t quantum,
                        const struct spa_io_position *position)
{
    if (!client->diagnostic_event)
        return;
    atomic_store_explicit(&client->diagnostic_quantum, quantum, memory_order_relaxed);
    atomic_store_explicit(&client->diagnostic_buffer_size, client->buffer_size,
                          memory_order_relaxed);
    atomic_store_explicit(&client->diagnostic_rate_num, position ? position->clock.rate.num : 0,
                          memory_order_relaxed);
    atomic_store_explicit(&client->diagnostic_rate_denom, position ? position->clock.rate.denom : 0,
                          memory_order_relaxed);
    atomic_store_explicit(&client->diagnostic_cycle, client->cycle_count, memory_order_relaxed);
    atomic_store_explicit(&client->diagnostic_thread, audio_current_thread_id(),
                          memory_order_relaxed);
    atomic_fetch_or_explicit(&client->diagnostic_kind, kind, memory_order_release);
    pw_loop_signal_event(pw_thread_loop_get_loop(client->loop), client->diagnostic_event);
}

static void
audio_on_process(void *userdata, struct spa_io_position *position)
{
    audio_client_t *c = userdata;

    if (position)
        atomic_store_explicit(&c->last_clock_nsec, position->clock.nsec, memory_order_release);
    for (uint32_t i = 0; i < c->n_ports; ++i)
    {
        audio_port_t     *port = c->ports[i];
        struct pw_buffer *buffer
                = port->pw_filter_port ? pw_filter_dequeue_buffer(port->pw_filter_port) : NULL;
        atomic_store_explicit(&port->cycle_buffer, buffer, memory_order_release);
    }

    const uint32_t quantum = position ? (uint32_t)position->clock.duration : 0;

    /* Follow-device mode: remember the device-dictated quantum so the ASIO
     * side can settle its buffer size to it (read by audio_observed_quantum). */
    if (c->follow_device && pipeasio_buffer_size_supported(quantum))
        atomic_store(&c->observed_quantum, quantum);

    if (quantum && quantum != c->buffer_size && !c->quantum_warned)
    {
        c->quantum_warned = true;
        audio_signal_diagnostic(c, AUDIO_DIAGNOSTIC_QUANTUM, quantum, position);
    }

    /* The running position io is the authoritative sample rate: settings
     * metadata is only a pre-activation estimate the daemon may refuse to
     * honor (clock.allowed-rates, another node pinning the graph).  Announce
     * on the FIRST cycle of every activation - not just on change - so a
     * correction that lands before the host publishes its callbacks or
     * reaches Running is never lost (issue #20).  This thread is the bridged
     * RT thread, the only one allowed to call back into the ASIO host. */
    if (position && position->clock.rate.denom)
    {
        const audio_nframes_t rate = position->clock.rate.denom;
        if (!c->rate_announced
            || rate != atomic_load_explicit(&c->sample_rate, memory_order_relaxed))
        {
            c->rate_announced = true;
            atomic_store_explicit(&c->sample_rate, rate, memory_order_release);
            if (c->sample_rate_cb)
                c->sample_rate_cb(rate, c->sample_rate_cb_arg);
        }
    }
    if (c->debug_enabled)
    {
        ++c->cycle_count;
        if (c->cycle_count <= 8 || (c->cycle_count < 100 && c->cycle_count % 10 == 0)
            || (c->cycle_count >= 100 && c->cycle_count % 100 == 0))
            audio_signal_diagnostic(c, AUDIO_DIAGNOSTIC_TRACE, quantum, position);
    }

    const bool delivered
            = c->process_cb ? c->process_cb(c->buffer_size, c->process_cb_arg) == 0 : false;

    /* Count cycles the host was due but did not get.  SPA_IO_CLOCK_FLAG_XRUN_RECOVER
     * cannot serve here: impl-node.c only sets it around the driver node's own
     * process_node, and we are deliberately a follower.  clock.position instead
     * advances one duration per cycle, so a larger gap is the cycles our
     * bufferSwitch was too slow for.  Only judge that at our steady quantum
     * (renegotiation is not a dropout), across one clock.id (a new driver
     * rebases the timeline), and when the host consumed both cycles (a
     * deliberate Stop is idle, not late).  Report the first, then every 64th. */
    if (delivered && position && quantum && quantum == c->buffer_size)
    {
        const uint64_t pos = position->clock.position;
        if (position->clock.id != c->last_clock_id)
        {
            c->last_clock_id       = position->clock.id;
            c->last_clock_position = 0;
        }
        if (c->last_clock_position && pos > c->last_clock_position)
        {
            const uint64_t cycles = (pos - c->last_clock_position) / quantum;
            const uint32_t missed = cycles > 1 ? (uint32_t)(cycles - 1) : 0;
            if (missed)
            {
                uint32_t n = atomic_fetch_add_explicit(&c->xrun_count, missed, memory_order_relaxed)
                             + missed;
                if (n == missed || (n % 64) < missed)
                    audio_signal_diagnostic(c, AUDIO_DIAGNOSTIC_XRUN, quantum, position);
            }
        }
        c->last_clock_position = pos;
    }
    else
        c->last_clock_position = 0;
    for (uint32_t i = 0; i < c->n_ports; ++i)
    {
        audio_port_t *port = c->ports[i];
        if (port->direction == PW_DIRECTION_OUTPUT)
            audio_port_publish_output(port, NULL, c->buffer_size, false, false);
        else
        {
            struct pw_buffer *buffer
                    = atomic_exchange_explicit(&port->cycle_buffer, NULL, memory_order_acq_rel);
            if (buffer && port->pw_filter_port)
                pw_filter_queue_buffer(port->pw_filter_port, buffer);
        }
    }
}

/* Core sync completion. */

static void
audio_on_core_done(void *userdata, uint32_t id, int seq)
{
    audio_client_t *c = userdata;
    if (id != PW_ID_CORE)
        return;
    if (seq == c->sync_seq)
        pw_thread_loop_signal(c->loop, false);
}

static void
audio_sync(audio_client_t *c)
{
    if (!c->core || !c->loop)
        return;
    pw_thread_loop_lock(c->loop);
    c->sync_seq = pw_core_sync(c->core, PW_ID_CORE, c->sync_seq);
    pw_thread_loop_wait(c->loop);
    pw_thread_loop_unlock(c->loop);
}

/* Walk c->discovered after our filter's node id is known, and migrate
 * any entries belonging to our filter into c->ports[].  Necessary because
 * pw_filter_connect is asynchronous: the port globals arrive during the
 * sync round-trip, before pw_filter_get_node_id can return a valid id,
 * so audio_cache_port treats them as external on the first pass. */
static void
audio_adopt_own_ports(audio_client_t *c)
{
    if (!c || c->our_node_id == SPA_ID_INVALID)
        return;

    pw_thread_loop_lock(c->loop);

    uint32_t kept    = 0;
    uint32_t adopted = 0;
    for (uint32_t i = 0; i < c->n_discovered; i++)
    {
        audio_port_t *d = c->discovered[i];
        if (d->pw_node_id != c->our_node_id)
        {
            c->discovered[kept++] = d;
            continue;
        }

        for (uint32_t j = 0; j < c->n_ports; j++)
        {
            if (c->ports[j]->name && !strcmp(c->ports[j]->name, d->port_name))
            {
                c->ports[j]->pw_node_id = c->our_node_id;
                c->ports[j]->pw_port_id = d->pw_port_id;
                c->ports[j]->port_id    = d->port_id;
                adopted++;
                break;
            }
        }
        free(d->name);
        free(d->port_name);
        free(d->node_name);
        free(d);
    }
    c->n_discovered = kept;
    TRACE("audio_adopt_own_ports: our_node_id=%u, adopted=%u, %u ext-ports remain\n",
          c->our_node_id, adopted, c->n_discovered);
    pw_thread_loop_unlock(c->loop);
}

/* Registry walker. */

static struct audio_node_info *
audio_find_node(audio_client_t *c, uint32_t id)
{
    for (uint32_t i = 0; i < c->n_nodes; i++)
        if (c->nodes[i]->id == id)
            return c->nodes[i];
    return NULL;
}

static char *
audio_dup_or_null(const char *s)
{
    return s && strnlen(s, AUDIO_REGISTRY_PROPERTY_MAX + 1) <= AUDIO_REGISTRY_PROPERTY_MAX
                   ? strdup(s)
                   : NULL;
}

static bool
audio_registry_property_valid(const char *value)
{
    return !value || strnlen(value, AUDIO_REGISTRY_PROPERTY_MAX + 1) <= AUDIO_REGISTRY_PROPERTY_MAX;
}

static void
audio_cache_node(audio_client_t *c, uint32_t id, const struct spa_dict *props)
{
    const char *node_name   = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char *nick        = spa_dict_lookup(props, PW_KEY_NODE_NICK);
    if (!node_name || !audio_registry_property_valid(node_name)
        || !audio_registry_property_valid(media_class)
        || !audio_registry_property_valid(description) || !audio_registry_property_valid(nick))
        return;

    /* Cache our own node by identity, whatever class it carries. */
    int is_ours = (c->our_node_id != SPA_ID_INVALID && id == c->our_node_id)
                  || (c->name && !strcmp(node_name, c->name));

    /* Otherwise only cache audio nodes - saves us from holding refs to
     * every stream/module/etc. */
    if (!is_ours)
    {
        if (!media_class || !strstr(media_class, "Audio"))
            return;
        if (strstr(media_class, "Internal"))
            return;
    }

    /* Skip duplicates (shouldn't happen but be defensive). */
    if (audio_find_node(c, id))
        return;
    if (c->n_nodes >= AUDIO_REGISTRY_MAX_NODES)
    {
        if (!c->node_limit_warned)
        {
            WARN("registry node cache limit reached (%u); ignoring excess nodes\n",
                 AUDIO_REGISTRY_MAX_NODES);
            c->node_limit_warned = true;
        }
        return;
    }

    struct audio_node_info *n = calloc(1, sizeof(*n));
    if (!n)
        return;
    n->id           = id;
    n->node_name    = strdup(node_name);
    n->media_class  = audio_dup_or_null(media_class);
    n->display_name = strdup(description ? description : (nick ? nick : node_name));
    if (!n->node_name || !n->display_name || (media_class && !n->media_class))
    {
        free(n->node_name);
        free(n->display_name);
        free(n->media_class);
        free(n);
        return;
    }

    if (c->n_nodes == c->cap_nodes)
    {
        uint32_t new_cap = c->cap_nodes ? c->cap_nodes * 2 : 16;
        if (new_cap > AUDIO_REGISTRY_MAX_NODES)
            new_cap = AUDIO_REGISTRY_MAX_NODES;
        struct audio_node_info **grown = realloc(c->nodes, new_cap * sizeof(*grown));
        if (!grown)
        {
            free(n->node_name);
            free(n->display_name);
            free(n->media_class);
            free(n);
            return;
        }
        c->nodes     = grown;
        c->cap_nodes = new_cap;
    }
    c->nodes[c->n_nodes++] = n;
    audio_refresh_defaults(c);
    TRACE("registry: +node id=%u name=\"%s\" class=\"%s\" desc=\"%s\"\n", id, n->node_name,
          n->media_class ? n->media_class : "", n->display_name);
}

static bool
audio_parse_id(const char *value, uint32_t *result)
{
    char         *end;
    unsigned long parsed;
    if (!value || !value[0])
        return false;
    errno  = 0;
    parsed = strtoul(value, &end, 10);
    if (errno || *end || parsed > UINT32_MAX)
        return false;
    *result = (uint32_t)parsed;
    return true;
}

static uint64_t
audio_mix_identity(uint64_t hash, uint32_t node_id, uint32_t port_id)
{
    hash ^= ((uint64_t)node_id << 32) | port_id;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static uint64_t
audio_default_fingerprint(audio_client_t *client, uint64_t flags, const char *name, bool *resolved)
{
    uint32_t target = name[0] ? audio_find_node_id(client, name) : SPA_ID_INVALID;
    uint32_t count  = 0;
    uint64_t sum    = 0;
    uint64_t mixed  = 0;
    if (!name[0])
        for (uint32_t i = 0; i < client->n_discovered; ++i)
            if ((client->discovered[i]->flags & flags) == flags
                && (target == SPA_ID_INVALID || client->discovered[i]->pw_node_id < target))
                target = client->discovered[i]->pw_node_id;
    *resolved = target != SPA_ID_INVALID;
    if (!*resolved)
        return 0;
    for (uint32_t i = 0; i < client->n_discovered; ++i)
    {
        audio_port_t *port = client->discovered[i];
        if (port->pw_node_id != target || (port->flags & flags) != flags)
            continue;
        uint64_t identity = ((uint64_t)port->port_id << 32) | port->pw_port_id;
        sum += identity;
        mixed ^= audio_mix_identity(UINT64_C(1469598103934665603), port->port_id, port->pw_port_id);
        ++count;
    }
    *resolved = count != 0;
    return *resolved ? audio_mix_identity(sum ^ mixed, target, count) : 0;
}

static void
audio_refresh_defaults(audio_client_t *client)
{
    bool     sink_resolved;
    bool     source_resolved;
    uint64_t sink    = audio_default_fingerprint(client, AUDIO_PORT_IS_INPUT,
                                                 client->default_sink_name, &sink_resolved);
    uint64_t source  = audio_default_fingerprint(client, AUDIO_PORT_IS_OUTPUT,
                                                 client->default_source_name, &source_resolved);
    bool     changed = sink != client->default_sink_fingerprint
                       || source != client->default_source_fingerprint
                       || sink_resolved != client->default_sink_resolved
                       || source_resolved != client->default_source_resolved;
    client->default_sink_fingerprint   = sink;
    client->default_source_fingerprint = source;
    client->default_sink_resolved      = sink_resolved;
    client->default_source_resolved    = source_resolved;
    if (client->defaults_baselined && changed)
        atomic_store_explicit(&client->default_changed, true, memory_order_release);
}

static void
audio_free_discovered_port(audio_port_t *port)
{
    free(port->name);
    free(port->port_name);
    free(port->node_name);
    free(port);
}

static void
audio_cache_port(audio_client_t *client, uint32_t id, const struct spa_dict *props)
{
    const char *node_id_value = spa_dict_lookup(props, PW_KEY_NODE_ID);
    const char *port_id_value = spa_dict_lookup(props, PW_KEY_PORT_ID);
    const char *port_name     = spa_dict_lookup(props, PW_KEY_PORT_NAME);
    const char *direction     = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
    const char *monitor       = spa_dict_lookup(props, PW_KEY_PORT_MONITOR);
    uint32_t    node_id;
    uint32_t    port_id;
    if (!audio_registry_property_valid(node_id_value)
        || !audio_registry_property_valid(port_id_value)
        || !audio_registry_property_valid(port_name) || !audio_registry_property_valid(direction)
        || !audio_registry_property_valid(monitor) || !audio_parse_id(node_id_value, &node_id)
        || !audio_parse_id(port_id_value, &port_id) || !port_name || !direction)
        return;
    if (client->our_node_id != SPA_ID_INVALID && node_id == client->our_node_id)
    {
        for (uint32_t i = 0; i < client->n_ports; ++i)
            if (!strcmp(client->ports[i]->name, port_name))
            {
                client->ports[i]->pw_node_id = node_id;
                client->ports[i]->pw_port_id = id;
                client->ports[i]->port_id    = port_id;
                return;
            }
        return;
    }
    if ((monitor && !strcmp(monitor, "true"))
        || (strcmp(direction, "in") && strcmp(direction, "out")))
        return;
    struct audio_node_info *node = audio_find_node(client, node_id);
    if (!node)
        return;
    if (client->n_discovered >= AUDIO_REGISTRY_MAX_PORTS)
    {
        if (!client->port_limit_warned)
        {
            WARN("registry port cache limit reached (%u); ignoring excess ports\n",
                 AUDIO_REGISTRY_MAX_PORTS);
            client->port_limit_warned = true;
        }
        return;
    }
    audio_port_t *port = calloc(1, sizeof(*port));
    if (!port)
        return;
    char identity[64];
    int  length     = snprintf(identity, sizeof(identity), "pw:%u:%u", node_id, id);
    port->name      = length > 0 && (size_t)length < sizeof(identity) ? strdup(identity) : NULL;
    port->port_name = strdup(port_name);
    port->node_name = strdup(node->node_name);
    if (!port->name || !port->port_name || !port->node_name)
    {
        audio_free_discovered_port(port);
        return;
    }
    port->direction  = !strcmp(direction, "in") ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT;
    port->pw_node_id = node_id;
    port->pw_port_id = id;
    port->port_id    = port_id;
    port->flags      = AUDIO_PORT_IS_PHYSICAL
                       | (port->direction == PW_DIRECTION_OUTPUT ? AUDIO_PORT_IS_OUTPUT
                                                                 : AUDIO_PORT_IS_INPUT);
    atomic_init(&port->cycle_buffer, NULL);
    if (client->n_discovered == client->cap_discovered)
    {
        uint32_t capacity = client->cap_discovered ? client->cap_discovered * 2u : 32u;
        if (capacity > AUDIO_REGISTRY_MAX_PORTS)
            capacity = AUDIO_REGISTRY_MAX_PORTS;
        audio_port_t **ports = realloc(client->discovered, (size_t)capacity * sizeof(*ports));
        if (!ports)
        {
            audio_free_discovered_port(port);
            return;
        }
        client->discovered     = ports;
        client->cap_discovered = capacity;
    }
    client->discovered[client->n_discovered++] = port;
    audio_refresh_defaults(client);
}

static int
audio_on_metadata_property(void *userdata, uint32_t subject, const char *key, const char *type,
                           const char *value)
{
    audio_client_t *client = userdata;
    char           *destination;
    char            name[256] = "";
    (void)subject;
    (void)type;
    if (!key || strnlen(key, AUDIO_REGISTRY_PROPERTY_MAX + 1) > AUDIO_REGISTRY_PROPERTY_MAX)
        return 0;
    if (!strcmp(key, "clock.rate") || !strcmp(key, "clock.force-rate"))
    {
        /* "settings" metadata: plain integer values ("48000"); a removed
         * key (value == NULL) means "no rate forced" / unknown. */
        int parsed = 0;
        if (value && !pipeasio_parse_int(value, 0, INT_MAX, &parsed))
            return 0;
        if (!strcmp(key, "clock.rate"))
            atomic_store_explicit(&client->graph_rate, (uint32_t)parsed, memory_order_release);
        else
            atomic_store_explicit(&client->graph_force_rate, (uint32_t)parsed,
                                  memory_order_release);
        audio_adopt_graph_rate(client);
        return 0;
    }
    if (!strcmp(key, "default.audio.sink"))
        destination = client->default_sink_name;
    else if (!strcmp(key, "default.audio.source"))
        destination = client->default_source_name;
    else
        return 0;
    if (value)
    {
        size_t value_length = strnlen(value, AUDIO_REGISTRY_METADATA_MAX + 1);
        if (value_length > AUDIO_REGISTRY_METADATA_MAX)
            return 0;
        spa_json_str_object_find(value, value_length, "name", name, sizeof(name));
    }
    memcpy(destination, name, sizeof(name));
    audio_refresh_defaults(client);
    return 0;
}

static const struct pw_metadata_events audio_metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = audio_on_metadata_property,
};

static void
audio_cache_metadata(audio_client_t *client, uint32_t id, uint32_t version,
                     const struct spa_dict *props)
{
    const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
    if (!name || !audio_registry_property_valid(name))
        return;
    if (!strcmp(name, "default") && !client->default_metadata)
    {
        client->default_metadata
                = pw_registry_bind(client->registry, id, PW_TYPE_INTERFACE_Metadata, version, 0);
        if (!client->default_metadata)
            return;
        client->default_metadata_id = id;
        pw_metadata_add_listener(client->default_metadata, &client->default_metadata_listener,
                                 &audio_metadata_events, client);
    }
    else if (!strcmp(name, "settings") && !client->settings_metadata)
    {
        client->settings_metadata
                = pw_registry_bind(client->registry, id, PW_TYPE_INTERFACE_Metadata, version, 0);
        if (!client->settings_metadata)
            return;
        client->settings_metadata_id = id;
        pw_metadata_add_listener(client->settings_metadata, &client->settings_metadata_listener,
                                 &audio_metadata_events, client);
    }
}

bool
audio_default_changed(audio_client_t *client)
{
    return client ? atomic_exchange_explicit(&client->default_changed, false, memory_order_acq_rel)
                  : false;
}

static void
audio_on_registry_global(void *userdata, uint32_t id, uint32_t permissions, const char *type,
                         uint32_t version, const struct spa_dict *props)
{
    audio_client_t *client = userdata;
    (void)permissions;
    if (!type || !props)
        return;
    if (!strcmp(type, PW_TYPE_INTERFACE_Node))
        audio_cache_node(client, id, props);
    else if (!strcmp(type, PW_TYPE_INTERFACE_Port))
        audio_cache_port(client, id, props);
    else if (!strcmp(type, PW_TYPE_INTERFACE_Metadata))
        audio_cache_metadata(client, id, version, props);
}

static void
audio_on_registry_global_remove(void *userdata, uint32_t id)
{
    audio_client_t *client = userdata;
    if (id == client->default_metadata_id)
    {
        spa_hook_remove(&client->default_metadata_listener);
        pw_proxy_destroy((struct pw_proxy *)client->default_metadata);
        client->default_metadata       = NULL;
        client->default_metadata_id    = SPA_ID_INVALID;
        client->default_sink_name[0]   = '\0';
        client->default_source_name[0] = '\0';
        audio_refresh_defaults(client);
        return;
    }
    if (id == client->settings_metadata_id)
    {
        spa_hook_remove(&client->settings_metadata_listener);
        pw_proxy_destroy((struct pw_proxy *)client->settings_metadata);
        client->settings_metadata    = NULL;
        client->settings_metadata_id = SPA_ID_INVALID;
        /* Keep the last known graph rate - better than snapping back to the
         * 48 kHz default while the daemon restarts its metadata object. */
        return;
    }
    for (uint32_t i = 0; i < client->n_ports; ++i)
        if (client->ports[i]->pw_port_id == id)
        {
            client->ports[i]->pw_port_id = 0;
            client->ports[i]->port_id    = 0;
            return;
        }
    for (uint32_t i = 0; i < client->n_discovered; ++i)
        if (client->discovered[i]->pw_port_id == id)
        {
            audio_free_discovered_port(client->discovered[i]);
            memmove(&client->discovered[i], &client->discovered[i + 1],
                    (size_t)(client->n_discovered - i - 1) * sizeof(*client->discovered));
            --client->n_discovered;
            audio_refresh_defaults(client);
            return;
        }
    for (uint32_t i = 0; i < client->n_nodes; ++i)
        if (client->nodes[i]->id == id)
        {
            struct audio_node_info *node = client->nodes[i];
            free(node->node_name);
            free(node->display_name);
            free(node->media_class);
            free(node);
            memmove(&client->nodes[i], &client->nodes[i + 1],
                    (size_t)(client->n_nodes - i - 1) * sizeof(*client->nodes));
            --client->n_nodes;
            audio_refresh_defaults(client);
            return;
        }
}
