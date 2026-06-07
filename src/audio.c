/*
 * audio.c — native libpipewire-0.3 backend for PipeASIO.
 *
 * Stands up a pw_thread_loop and context, and overrides spa_thread_utils
 * on the data loop so PipeWire's RT thread is a Win32 CreateThread'd Wine
 * thread (proper TEB, able to call back into the ASIO host's COM methods).
 * Creates a duplex pw_filter with MAP_BUFFERS ports and walks the registry
 * for port discovery and audio_connect.
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 */

#define _GNU_SOURCE   /* SCHED_FIFO and friends */

#include "audio.h"
#include "pipeasio_offsets.h"

#define WIN32_LEAN_AND_MEAN
#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"

#include <stdlib.h>   /* getenv for PIPEASIO_DEBUG */

/* Raw write(STDERR_FILENO,…) logging: bypasses Wine's debug-channel
 * filtering and the stdio buffers discarded on abnormal exit, and works
 * from any thread (incl. the RT/data-loop threads that have no Wine
 * TEB).  Verbose TRACE is opt-in via the PIPEASIO_DEBUG env var;
 * WARN/ERR always emit. */
#undef TRACE
#undef WARN
#undef ERR
static inline int pipeasio_log_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("PIPEASIO_DEBUG") ? 1 : 0;
    return on;
}
#define PIPEASIO_LOG(pfx, fmt, ...) do {                                       \
    char _buf[1024];                                                           \
    int  _n = snprintf(_buf, sizeof _buf, pfx fmt, ##__VA_ARGS__);             \
    if (_n > 0)                                                                \
        (void)write(STDERR_FILENO, _buf,                                       \
                    (size_t)_n < sizeof _buf ? (size_t)_n : sizeof _buf - 1);  \
} while (0)
#define TRACE(fmt, ...) do { if (pipeasio_log_on()) PIPEASIO_LOG("[pipeasio] ", fmt, ##__VA_ARGS__); } while (0)
#define WARN(fmt, ...)  PIPEASIO_LOG("[pipeasio] WARN: ", fmt, ##__VA_ARGS__)
#define ERR(fmt, ...)   PIPEASIO_LOG("[pipeasio] ERR: ",  fmt, ##__VA_ARGS__)

/* Build identifier — stamped into the audio_open banner so we can verify
 * which build is actually loaded (no more staring at stale logs). */
#define PIPEASIO_BUILD_TAG  __DATE__ " " __TIME__

#include <pipewire/pipewire.h>
#include <pipewire/thread.h>
#include <spa/param/audio/format-utils.h>
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
#include <unistd.h>

WINE_DEFAULT_DEBUG_CHANNEL(asio);

/* Startup defaults.  The buffer size is overridden by the ASIO host's
 * negotiated size in CreateBuffers; the sample rate is overridden by
 * audio_on_io_changed when the graph runs at a different rate. */
#define AUDIO_DEFAULT_SAMPLE_RATE  48000u
#define AUDIO_DEFAULT_BUFFER_SIZE   1024u

/* SCHED_FIFO range offered to PipeWire's rt handling. */
#define AUDIO_RT_PRIO_MIN  1
#define AUDIO_RT_PRIO_MAX 80

/* ----------------------------------------------------------------------
 * Wine RT thread bridge — install custom spa_thread_utils on the data
 * loop so the audio thread is a CreateThread'd Wine thread, capable of
 * calling back into the ASIO host's COM methods.  PipeWire's pw_data_loop
 * spawns exactly one RT thread, so a single state slot is sufficient.
 * ---------------------------------------------------------------------- */

struct audio_rt_state {
    HANDLE       win_handle;                /* Win32 handle for join() */
    DWORD        win_tid;
    pthread_t    ptid;                      /* captured inside the spawned thread */
    int          rt_priority;               /* current SCHED_FIFO priority, 0 = none */
    atomic_bool  ready;                     /* released once ptid is captured */
    void      *(*user_entry)(void *);       /* PipeWire-provided entry */
    void        *user_arg;
    void        *user_ret;
};

static DWORD WINAPI audio_rt_trampoline(LPVOID raw)
{
    struct audio_rt_state *s = raw;

    s->ptid = pthread_self();
    atomic_store_explicit(&s->ready, true, memory_order_release);

    TRACE("rt thread entry: tid=%lx\n", (unsigned long)GetCurrentThreadId());

    s->user_ret = s->user_entry(s->user_arg);
    return 0;
}

static struct spa_thread *
audio_rt_create(void *data, const struct spa_dict *props,
                void *(*entry)(void *), void *arg)
{
    struct audio_rt_state *s = data;
    (void)props;

    TRACE("rt_create: ENTRY entry=%p arg=%p\n", (void *)entry, arg);
    s->user_entry = entry;
    s->user_arg   = arg;
    atomic_store_explicit(&s->ready, false, memory_order_relaxed);

    /* 8MB stack for the pw_filter → audio_on_process → ASIO host callback
     * chain; the Win32 1MB default is too small.
     *
     * STACK_SIZE_PARAM_IS_A_RESERVATION (0x00010000) — without this flag
     * dwStackSize sets only the *committed* size; the *reserved* size
     * still comes from the PE header (1MB default). With the flag, the
     * full 8MB is reserved, which is what we actually want here. */
    s->win_handle = CreateThread(NULL, 8 * 1024 * 1024,
                                 audio_rt_trampoline, s,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION,
                                 &s->win_tid);
    if (!s->win_handle) {
        ERR("CreateThread failed for PipeWire RT thread\n");
        return NULL;
    }

    while (!atomic_load_explicit(&s->ready, memory_order_acquire))
        sched_yield();

    return (struct spa_thread *)(uintptr_t)s->ptid;
}

static int audio_rt_join(void *data, struct spa_thread *thread, void **retval)
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

static int audio_rt_get_range(void *data, const struct spa_dict *props,
                              int *min, int *max)
{
    (void)data; (void)props;
    *min = AUDIO_RT_PRIO_MIN;
    *max = AUDIO_RT_PRIO_MAX;
    return 0;
}

static int audio_rt_acquire(void *data, struct spa_thread *thread, int priority)
{
    struct audio_rt_state *s = data;
    (void)thread;

    if (priority <= 0)
        return 0;

    int err = pthread_setschedparam(s->ptid, SCHED_FIFO,
                                    &(struct sched_param){.sched_priority = priority});
    if (err) {
        WARN("pthread_setschedparam(SCHED_FIFO, %d) failed: %s\n",
             priority, strerror(err));
        return -1;
    }
    s->rt_priority = priority;
    return 0;
}

static int audio_rt_drop(void *data, struct spa_thread *thread)
{
    struct audio_rt_state *s = data;
    (void)thread;

    if (!s->rt_priority)
        return 0;

    int err = pthread_setschedparam(s->ptid, SCHED_OTHER,
                                    &(struct sched_param){.sched_priority = 0});
    if (err) {
        WARN("pthread_setschedparam(SCHED_OTHER) failed: %s\n", strerror(err));
        return -1;
    }
    s->rt_priority = 0;
    return 0;
}

static const struct spa_thread_utils_methods audio_rt_methods = {
    SPA_VERSION_THREAD_UTILS_METHODS,
    .create       = audio_rt_create,
    .join         = audio_rt_join,
    .get_rt_range = audio_rt_get_range,
    .acquire_rt   = audio_rt_acquire,
    .drop_rt      = audio_rt_drop,
};

/* ----------------------------------------------------------------------
 * Opaque types backing audio.h
 * ---------------------------------------------------------------------- */

struct audio_client {
    char                       *name;
    audio_nframes_t             sample_rate;
    audio_nframes_t             buffer_size;
    audio_nframes_t             forced_rate;   /* 0 = follow graph, else FORCE_RATE */

    struct pw_thread_loop      *loop;
    struct pw_context          *ctx;
    struct pw_core             *core;
    struct pw_data_loop        *data_loop;

    struct audio_rt_state       rt;
    struct spa_thread_utils     rt_iface;

    audio_process_cb            process_cb;
    void                       *process_cb_arg;
    audio_buffer_size_cb        buffer_size_cb;
    void                       *buffer_size_cb_arg;
    audio_sample_rate_cb        sample_rate_cb;
    void                       *sample_rate_cb_arg;
    audio_latency_cb            latency_cb;
    void                       *latency_cb_arg;

    bool                        active;

    /* --- pw_filter ------------------------------------------------ */

    struct pw_filter           *filter;
    struct spa_hook             filter_listener;

    /* Registered-port array.  audio_port_register appends; audio_activate
     * walks this to build the filter, audio_deactivate frees it. */
    audio_port_t              **ports;
    uint32_t                    n_ports;
    uint32_t                    cap_ports;

    /* Last spa_io_position.clock.nsec — used by audio_transport_query. */
    uint64_t                    last_clock_nsec;
    uint64_t                    last_clock_position;

    /* --- registry walker ----------------------------------------- */

    struct pw_registry         *registry;
    struct spa_hook             registry_listener;
    struct spa_hook             core_listener;
    int                         sync_seq;
    uint32_t                    our_node_id;

    /* Discovered remote nodes (hardware + apps) — cached for assembling
     * full port names ("node:port") and for audio_connect lookups. */
    struct audio_node_info    **nodes;
    uint32_t                    n_nodes;
    uint32_t                    cap_nodes;

    /* Discovered remote ports.  Each entry is a heap audio_port_t with
     * pw_node_id / pw_port_id / name / type / flags filled in; the
     * filter-side fields stay zero. */
    audio_port_t              **discovered;
    uint32_t                    n_discovered;
    uint32_t                    cap_discovered;
};

struct audio_node_info {
    uint32_t  id;
    char     *node_name;
    char     *display_name;
    char     *media_class;
};

struct audio_port {
    audio_client_t        *client;
    char                  *name;
    char                  *type;
    uint64_t               flags;
    audio_latency_range_t  latency[2];           /* [CAPTURE, PLAYBACK] */

    /* --- PipeWire port handle ------------------------------------- */

    enum pw_direction      direction;
    void                  *pw_filter_port;       /* returned by pw_filter_add_port */
    struct pw_buffer      *cycle_buffer;         /* this cycle's dequeued buffer; datas[0].data is the live mmap */

    /* --- PipeWire registry IDs (for audio_connect link-factory) -- */

    uint32_t               pw_node_id;
    uint32_t               pw_port_id;
};

/* per-port userdata block stored by pw_filter_add_port — holds a pointer
 * back to our audio_port so the filter events can find it. */
typedef audio_port_t *audio_port_ref_t;

/* --- Forward decls for filter events ------------------------------------ */

static void audio_on_state_changed(void *userdata, enum pw_filter_state old,
                                   enum pw_filter_state state, const char *error);
static void audio_on_io_changed(void *userdata, void *port_data, uint32_t id,
                                void *area, uint32_t size);

static void audio_on_process(void *userdata, struct spa_io_position *position);

static const struct pw_filter_events audio_filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = audio_on_state_changed,
    .io_changed    = audio_on_io_changed,

    .process       = audio_on_process,
};

/* --- Forward decls for core / registry events --------------------------- */

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

/* ----------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

audio_client_t *audio_open(const char *client_name, uint32_t options, uint32_t *status)
{
    (void)options;   /* legacy option flags do not map onto PipeWire */
    if (status) *status = 0;

    audio_client_t *c = calloc(1, sizeof(*c));
    if (!c) {
        ERR("out of memory allocating audio_client\n");
        if (status) *status = 1;
        return NULL;
    }

    c->name         = strdup(client_name ? client_name : "PipeASIO");
    c->sample_rate  = AUDIO_DEFAULT_SAMPLE_RATE;
    c->buffer_size  = AUDIO_DEFAULT_BUFFER_SIZE;
    c->our_node_id  = SPA_ID_INVALID;
    atomic_init(&c->rt.ready, false);

    pw_init(NULL, NULL);

    c->loop = pw_thread_loop_new(c->name, NULL);
    if (!c->loop) {
        ERR("pw_thread_loop_new(%s) failed\n", c->name);
        goto fail_alloc;
    }

    c->ctx = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
    if (!c->ctx) {
        ERR("pw_context_new failed\n");
        goto fail_loop;
    }

    /* Wire our Wine-thread spa_thread_utils so PipeWire spawns its RT
     * thread via CreateThread (giving it a Wine TEB) — critical because
     * any callback that bridges into PE code via Wine's loader will
     * otherwise execute on a Wine-managed bridge stack with a
     * mismatched __stack_chk_guard, which smashes the canary on return.
     *
     * Set the override on the context's data loop only (like pwasio): the
     * data loop owns the RT process() thread, which must be a Win32 thread
     * with a Wine TEB.  The pw_thread_loop main loop stays a plain pthread
     * — it only runs registry/core events (libc allocation, no PE
     * bridging), so it needs no TEB and no Win32 thread. */
    c->rt_iface.iface = SPA_INTERFACE_INIT(
        SPA_TYPE_INTERFACE_ThreadUtils, SPA_VERSION_THREAD_UTILS,
        &audio_rt_methods, &c->rt);
    c->data_loop = pw_context_get_data_loop(c->ctx);
    pw_data_loop_set_thread_utils(c->data_loop, &c->rt_iface);

    /* pw_context_new already started the data loop with the DEFAULT
     * (plain-pthread) thread utils.  Installing our override now only
     * affects the NEXT start, so stop the data loop here and restart it
     * below — then its RT thread is allocated through audio_rt_create
     * (Win32 CreateThread) and has a real Wine TEB.  Without this dance
     * the override is dead: the process() callback runs on a foreign
     * pthread and the ASIO host's COM bufferSwitch corrupts memory.
     * (Mirrors pwasio's stop/restart sequence.) */
    pw_data_loop_stop(c->data_loop);

    if (pw_thread_loop_start(c->loop) < 0) {
        ERR("pw_thread_loop_start failed\n");
        goto fail_ctx;
    }

    pw_thread_loop_lock(c->loop);
    c->core = pw_context_connect(c->ctx, NULL, 0);
    if (!c->core) {
        pw_thread_loop_unlock(c->loop);
        ERR("pw_context_connect failed (is the PipeWire daemon running?)\n");
        goto fail_started;
    }

    /* Bind the registry and add listeners so we can walk the graph for
     * audio_get_ports / audio_port_by_name / audio_connect.  Then sync
     * the core once so the initial _global emission completes before
     * audio_open returns. */
    pw_core_add_listener(c->core, &c->core_listener, &audio_core_events, c);
    c->registry = pw_core_get_registry(c->core, PW_VERSION_REGISTRY, 0);
    if (c->registry)
        pw_registry_add_listener(c->registry, &c->registry_listener,
                                 &audio_registry_events, c);

    pw_thread_loop_unlock(c->loop);
    audio_sync(c);

    TRACE("audio_open(%s) -> %p [build " PIPEASIO_BUILD_TAG "] "
          "(registry sync done: %u nodes, %u ports discovered)\n",
          c->name, c, c->n_nodes, c->n_discovered);
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
    if (status) *status = 1;
    return NULL;
}

bool audio_close(audio_client_t *c)
{
    if (!c) return false;

    /* Tear down the filter first so its destruction sees a live
     * thread loop / core to deliver events on. */
    if (c->active)
        audio_teardown_filter(c);

    if (c->loop) {
        pw_thread_loop_lock(c->loop);
        if (c->registry) {
            spa_hook_remove(&c->registry_listener);
            pw_proxy_destroy((struct pw_proxy *)c->registry);
            c->registry = NULL;
        }
        if (c->core) {
            spa_hook_remove(&c->core_listener);
            pw_core_disconnect(c->core);
            c->core = NULL;
        }
        pw_thread_loop_unlock(c->loop);
        pw_thread_loop_stop(c->loop);
    }
    if (c->ctx)  pw_context_destroy(c->ctx);
    if (c->loop) pw_thread_loop_destroy(c->loop);

    /* Free discovered nodes / ports caches. */
    for (uint32_t i = 0; i < c->n_nodes; i++) {
        free(c->nodes[i]->node_name);
        free(c->nodes[i]->display_name);
        free(c->nodes[i]->media_class);
        free(c->nodes[i]);
    }
    free(c->nodes);
    for (uint32_t i = 0; i < c->n_discovered; i++) {
        free(c->discovered[i]->name);
        free(c->discovered[i]->type);
        free(c->discovered[i]);
    }
    free(c->discovered);

    /* Free port array and any still-registered audio_port_t.  asio.c is
     * supposed to call audio_port_unregister for each port before closing,
     * but defend against leaks. */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p = c->ports[i];
        free(p->name);
        free(p->type);
        free(p);
    }
    free(c->ports);

    free(c->name);
    free(c);
    return true;
}

/* Helper — tear down the pw_filter and per-port resources.
 * Safe to call multiple times; clears all state to "not active". */
static void audio_teardown_filter(audio_client_t *c)
{
    /* Stop the RT data loop before destroying the filter it processes.
     * Paired with the pw_data_loop_start in audio_activate so re-activation
     * (DisposeBuffers -> CreateBuffers) starts from a stopped loop, exactly
     * like the first activation. */
    if (c->data_loop && c->loop) {
        pw_thread_loop_lock(c->loop);
        pw_data_loop_stop(c->data_loop);
        pw_thread_loop_unlock(c->loop);
    }
    if (c->filter) {
        pw_thread_loop_lock(c->loop);
        pw_filter_destroy(c->filter);
        c->filter = NULL;
        pw_thread_loop_unlock(c->loop);
    }
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p = c->ports[i];
        p->pw_filter_port = NULL;
        p->cycle_buffer   = NULL;
    }
}

bool audio_activate(audio_client_t *c)
{
    if (!c) return false;
    if (c->active) return true;
    if (!c->n_ports) {
        ERR("audio_activate called with no ports registered\n");
        return false;
    }

    const size_t bsize_samples = c->buffer_size;
    const size_t bsize_bytes   = bsize_samples * sizeof(audio_sample_t);

    /* Build the filter's node-level properties.  FORCE_QUANTUM locks the
     * PipeWire graph to the ASIO host's negotiated buffer size; FORCE_RATE
     * is applied only when the user pins a sample rate (forced_rate != 0),
     * otherwise the node follows the graph rate. */
    struct pw_properties *filter_props = pw_properties_new(
        PW_KEY_NODE_NAME,            c->name,
        PW_KEY_NODE_DESCRIPTION,     c->name,
        PW_KEY_MEDIA_TYPE,           "Audio",
        PW_KEY_MEDIA_CATEGORY,       "Duplex",
        PW_KEY_MEDIA_ROLE,           "DSP",
        PW_KEY_NODE_ALWAYS_PROCESS,  "true",
        PW_KEY_NODE_GROUP,           "group.dsp.0",
        "pipeasio.node",             "1",
        NULL);
    if (!filter_props) {
        ERR("pw_properties_new (filter) failed\n");
        goto fail;
    }
    pw_properties_setf(filter_props, PW_KEY_NODE_FORCE_QUANTUM, "%u", (unsigned)bsize_samples);
    if (c->forced_rate)
        pw_properties_setf(filter_props, PW_KEY_NODE_FORCE_RATE, "%u", (unsigned)c->forced_rate);
    pw_properties_setf(filter_props, PW_KEY_NODE_LATENCY,       "%u/%u",
                       (unsigned)bsize_samples, (unsigned)c->sample_rate);
    TRACE("audio_activate: forcing quantum=%u, forced_rate=%u (0=follow graph), "
          "latency=%u/%u\n",
          (unsigned)bsize_samples, (unsigned)c->forced_rate,
          (unsigned)bsize_samples, (unsigned)c->sample_rate);

    pw_thread_loop_lock(c->loop);

    c->filter = pw_filter_new_simple(pw_data_loop_get_loop(c->data_loop),
                                     c->name, filter_props,
                                     &audio_filter_events, c);
    if (!c->filter) {
        pw_thread_loop_unlock(c->loop);
        ERR("pw_filter_new_simple failed\n");
        goto fail;
    }

    /* Add every registered port to the filter.  The FORMAT_DSP property locks
     * each port to F32 DSP mono; the buffers param requests 2 buffers of one
     * ASIO period.  PW_FILTER_PORT_FLAG_MAP_BUFFERS makes pw_filter mmap the
     * daemon's shared buffer memory into datas[0].data, so the ASIO host
     * reads/writes the live buffer directly (see audio_on_process). */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p = c->ports[i];

        struct pw_properties *pp = pw_properties_new(NULL, NULL);
        if (!pp) {
            pw_thread_loop_unlock(c->loop);
            ERR("pw_properties_new (port %u) failed\n", i);
            goto fail;
        }
        pw_properties_set(pp, PW_KEY_FORMAT_DSP, "32 bit float mono audio");
        pw_properties_set(pp, PW_KEY_PORT_NAME, p->name);

        uint8_t param_buf[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(param_buf, sizeof param_buf);
        const struct spa_pod *params[] = {
            spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers,  SPA_POD_Int(2),
                SPA_PARAM_BUFFERS_size,     SPA_POD_Int((int)bsize_bytes),
                SPA_PARAM_BUFFERS_stride,   SPA_POD_Int(sizeof(audio_sample_t)),
                SPA_PARAM_BUFFERS_align,    SPA_POD_Int((int)bsize_bytes),
                SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)),
        };

        p->pw_filter_port = pw_filter_add_port(c->filter, p->direction,
                                               PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                               sizeof(audio_port_ref_t),
                                               pp, params, SPA_N_ELEMENTS(params));
        if (!p->pw_filter_port) {
            pw_thread_loop_unlock(c->loop);
            ERR("pw_filter_add_port failed for port %u (%s)\n", i, p->name);
            goto fail;
        }
        *(audio_port_ref_t *)p->pw_filter_port = p;
    }

    if (pw_filter_connect(c->filter, PW_FILTER_FLAG_NONE, NULL, 0) < 0) {
        pw_thread_loop_unlock(c->loop);
        ERR("pw_filter_connect failed\n");
        goto fail;
    }

    pw_thread_loop_unlock(c->loop);

    /* Start the data loop now — AFTER add_port/connect.  Those run in the
     * thread-loop context and fail "wrong context: not in loop" if the
     * filter's (data) loop is already running, so it stayed stopped since
     * audio_open.  Starting it here spawns the RT thread via
     * audio_rt_create (Wine TEB) and lets the node bind below and schedule
     * process() on that bridged thread.  (Mirrors pwasio: filter set up
     * with the data loop stopped, started in Start().) */
    pw_thread_loop_lock(c->loop);
    if (pw_data_loop_start(c->data_loop) < 0) {
        pw_thread_loop_unlock(c->loop);
        ERR("pw_data_loop_start failed\n");
        goto fail;
    }
    pw_thread_loop_unlock(c->loop);

    /* Wait for the filter to bind: pw_filter_connect is async, and the
     * node id only becomes valid once the daemon has BOUND the node
     * proxy.  A single pw_core_sync isn't always enough (the BOUND
     * event can be in flight when sync DONE is processed), so we poll
     * with a timed wait that gets kicked by audio_on_state_changed. */
    {
        pw_thread_loop_lock(c->loop);
        struct timespec abstime;
        pw_thread_loop_get_time(c->loop, &abstime,
                                5 * SPA_NSEC_PER_SEC);  /* 5 s budget */
        for (;;) {
            uint32_t nid = pw_filter_get_node_id(c->filter);
            enum pw_filter_state st = pw_filter_get_state(c->filter, NULL);
            if (nid != SPA_ID_INVALID) {
                c->our_node_id = nid;
                break;
            }
            if (st == PW_FILTER_STATE_ERROR) {
                pw_thread_loop_unlock(c->loop);
                ERR("audio_activate: pw_filter reached ERROR state before bind\n");
                goto fail;
            }
            if (pw_thread_loop_timed_wait_full(c->loop, &abstime) < 0) {
                /* Timed out — try one more sync as a last resort. */
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

    /* The filter is bound, but the daemon emits the globals for the
     * filter's own ports slightly later, as the node activates.  A single
     * sync isn't enough — poll sync+adopt until every registered port has
     * a PipeWire port id, so the connect-to-hardware links in CreateBuffers
     * can resolve them.  Bounded to ~1s; proceed with whatever adopted if
     * it overruns. */
    for (int attempt = 0; attempt < 50; attempt++) {
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

    /* Static latency: one buffer-period in either direction.  Refine
     * later via SPA_IO_Latency events if a host actually queries them. */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p = c->ports[i];
        p->latency[AUDIO_CAPTURE_LATENCY].min  = c->buffer_size;
        p->latency[AUDIO_CAPTURE_LATENCY].max  = c->buffer_size;
        p->latency[AUDIO_PLAYBACK_LATENCY].min = c->buffer_size;
        p->latency[AUDIO_PLAYBACK_LATENCY].max = c->buffer_size;
    }

    c->active = true;
    TRACE("audio_activate: %u ports, %u-sample buffers, %u Hz\n",
          c->n_ports, c->buffer_size, c->sample_rate);
    return true;

fail:
    audio_teardown_filter(c);
    return false;
}

bool audio_deactivate(audio_client_t *c)
{
    if (!c) return false;
    if (!c->active) return true;
    audio_teardown_filter(c);
    c->active = false;
    return true;
}

const char *audio_get_client_name(audio_client_t *c)
{
    return c ? c->name : NULL;
}

/* ----------------------------------------------------------------------
 * Properties
 * ---------------------------------------------------------------------- */

audio_nframes_t audio_get_sample_rate(audio_client_t *c)
{
    return c ? c->sample_rate : 0;
}

audio_nframes_t audio_get_buffer_size(audio_client_t *c)
{
    return c ? c->buffer_size : 0;
}

bool audio_set_buffer_size(audio_client_t *c, audio_nframes_t nframes)
{
    if (!c || !nframes) return false;
    c->buffer_size = nframes;
    if (c->buffer_size_cb)
        c->buffer_size_cb(nframes, c->buffer_size_cb_arg);
    /* Applied on the next audio_activate, which rebuilds the filter with
     * the new PW_KEY_NODE_FORCE_QUANTUM (host path: DisposeBuffers →
     * audio_deactivate, then CreateBuffers → audio_set_buffer_size →
     * audio_activate). */
    return true;
}

void audio_set_forced_rate(audio_client_t *c, audio_nframes_t rate)
{
    if (!c) return;
    c->forced_rate = rate;
    if (rate)
        c->sample_rate = rate;   /* report the pinned rate immediately */
}

/* ----------------------------------------------------------------------
 * Ports.  audio_port_register allocates an audio_port_t and appends it to
 * the client's port array; audio_activate turns each into a pw_filter
 * port.  During a process cycle audio_port_get_buffer returns the live
 * MAP_BUFFERS mmap (cycle_buffer->buffer->datas[0].data); outside a cycle
 * it returns NULL.
 * ---------------------------------------------------------------------- */

audio_port_t *audio_port_register(audio_client_t *c,
                                  const char *port_name, const char *port_type,
                                  uint64_t flags, uint64_t buffer_size)
{
    (void)buffer_size;   /* asio.c passes the channel index here, not a size */
    if (!c || !port_name) return NULL;

    audio_port_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->client      = c;
    p->name        = strdup(port_name);
    p->type        = strdup(port_type ? port_type : AUDIO_DEFAULT_TYPE);
    p->flags       = flags;
    p->direction   = (flags & AUDIO_PORT_IS_INPUT) ? PW_DIRECTION_INPUT
                                                   : PW_DIRECTION_OUTPUT;

    if (c->n_ports == c->cap_ports) {
        uint32_t new_cap = c->cap_ports ? c->cap_ports * 2 : 16;
        audio_port_t **grown = realloc(c->ports, new_cap * sizeof(*grown));
        if (!grown) { free(p->name); free(p->type); free(p); return NULL; }
        c->ports     = grown;
        c->cap_ports = new_cap;
    }
    c->ports[c->n_ports++] = p;
    return p;
}

bool audio_port_unregister(audio_client_t *c, audio_port_t *p)
{
    if (!c || !p) return false;
    /* Remove from the client's array (compact in place). */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        if (c->ports[i] == p) {
            memmove(&c->ports[i], &c->ports[i + 1],
                    (c->n_ports - i - 1) * sizeof(*c->ports));
            c->n_ports--;
            break;
        }
    }
    free(p->name);
    free(p->type);
    free(p);
    return true;
}

void *audio_port_get_buffer(audio_port_t *p, audio_nframes_t nframes)
{
    (void)nframes;   /* the filter always runs at client->buffer_size */
    if (!p || !p->cycle_buffer) return NULL;
    return p->cycle_buffer->buffer->datas[0].data;
}

const char *audio_port_name(const audio_port_t *p) { return p ? p->name : NULL; }
const char *audio_port_type(const audio_port_t *p) { return p ? p->type : NULL; }

audio_port_t *audio_port_by_name(audio_client_t *c, const char *port_name)
{
    if (!c || !port_name) return NULL;
    for (uint32_t i = 0; i < c->n_discovered; i++) {
        audio_port_t *p = c->discovered[i];
        if (p->name && !strcmp(p->name, port_name))
            return p;
    }
    /* Also check our own (filter) ports, since asio.c does pass our own
     * names through audio_port_by_name in some legacy paths. */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p = c->ports[i];
        if (p->name && !strcmp(p->name, port_name))
            return p;
    }
    return NULL;
}

const char **audio_get_ports(audio_client_t *c,
                             const char *port_name_pattern,
                             const char *type_name_pattern,
                             uint64_t flags)
{
    if (!c) return NULL;
    (void)port_name_pattern;   /* asio.c always passes NULL */
    (void)type_name_pattern;   /* same */

    /* Collect indices of matching discovered ports.  asio.c asks for:
     *   PHYSICAL|OUTPUT — hardware sources (mic-like) we'll read FROM
     *   PHYSICAL|INPUT  — hardware sinks (speaker-like) we'll write TO
     */
    uint32_t *match_idx = NULL;
    uint32_t  n_match  = 0;
    uint32_t  cap      = 0;

    /* Restrict the result to a SINGLE device (the node of the first
     * matching port).  asio.c's connect-to-hardware then wires our ports
     * to exactly one sink/source instead of spanning every device in
     * discovery order — a single filter node linked across multiple
     * hardware clocks (analog + HDMI + ...) is forced async by PipeWire,
     * which yields 1 buffer/port + constant renegotiation = buzzing.
     * TODO: prefer the PipeWire default sink/source over "first seen". */
    uint32_t target_node = SPA_ID_INVALID;
    for (uint32_t i = 0; i < c->n_discovered; i++) {
        audio_port_t *p = c->discovered[i];
        if ((p->flags & flags) != flags) continue;
        if (target_node == SPA_ID_INVALID)
            target_node = p->pw_node_id;
        else if (p->pw_node_id != target_node)
            continue;

        if (n_match == cap) {
            cap = cap ? cap * 2 : 16;
            uint32_t *grown = realloc(match_idx, cap * sizeof(*grown));
            if (!grown) { free(match_idx); return NULL; }
            match_idx = grown;
        }
        match_idx[n_match++] = i;
    }

    /* Allocate the NULL-terminated array.  Caller calls audio_free() on
     * the array itself (not the strings).  We keep ownership of the
     * strings — they live in the discovered cache. */
    const char **result = calloc(n_match + 1, sizeof(*result));
    if (!result) { free(match_idx); return NULL; }
    for (uint32_t i = 0; i < n_match; i++)
        result[i] = c->discovered[match_idx[i]]->name;
    result[n_match] = NULL;

    free(match_idx);
    return result;
}

const char **audio_get_device_ports(audio_client_t *c, const char *node_name,
                                     uint64_t flags)
{
    if (!c) return NULL;
    if (!node_name || !node_name[0])
        return audio_get_ports(c, NULL, NULL, flags);

    uint32_t target = SPA_ID_INVALID;
    for (uint32_t i = 0; i < c->n_nodes; i++)
        if (c->nodes[i]->node_name && !strcmp(c->nodes[i]->node_name, node_name)) {
            target = c->nodes[i]->id;
            break;
        }
    if (target == SPA_ID_INVALID) {
        WARN("audio_get_device_ports: node '%s' not in discovery; "
             "falling back to first available\n", node_name);
        return audio_get_ports(c, NULL, NULL, flags);
    }

    uint32_t *idx = NULL, n = 0, cap = 0;
    for (uint32_t i = 0; i < c->n_discovered; i++) {
        audio_port_t *p = c->discovered[i];
        if (p->pw_node_id != target) continue;
        if ((p->flags & flags) != flags) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            uint32_t *grown = realloc(idx, cap * sizeof(*grown));
            if (!grown) { free(idx); return NULL; }
            idx = grown;
        }
        idx[n++] = i;
    }

    const char **result = calloc(n + 1, sizeof(*result));
    if (!result) { free(idx); return NULL; }
    for (uint32_t i = 0; i < n; i++)
        result[i] = c->discovered[idx[i]]->name;
    result[n] = NULL;
    free(idx);
    return result;
}

void audio_port_get_latency_range(audio_port_t *p, uint32_t mode,
                                  audio_latency_range_t *range)
{
    if (!range) return;
    if (!p) { range->min = range->max = 0; return; }
    *range = p->latency[mode == AUDIO_PLAYBACK_LATENCY ? 1 : 0];
}

/* ----------------------------------------------------------------------
 * Callbacks
 * ---------------------------------------------------------------------- */

bool audio_set_process_callback(audio_client_t *c, audio_process_cb cb, void *arg)
{
    if (!c) return false;
    c->process_cb = cb;
    c->process_cb_arg = arg;
    return true;
}

bool audio_set_buffer_size_callback(audio_client_t *c, audio_buffer_size_cb cb, void *arg)
{
    if (!c) return false;
    c->buffer_size_cb = cb;
    c->buffer_size_cb_arg = arg;
    return true;
}

bool audio_set_sample_rate_callback(audio_client_t *c, audio_sample_rate_cb cb, void *arg)
{
    if (!c) return false;
    c->sample_rate_cb = cb;
    c->sample_rate_cb_arg = arg;
    return true;
}

bool audio_set_latency_callback(audio_client_t *c, audio_latency_cb cb, void *arg)
{
    if (!c) return false;
    c->latency_cb = cb;
    c->latency_cb_arg = arg;
    return true;
}

/* ----------------------------------------------------------------------
 * Connections / transport / memory
 * ---------------------------------------------------------------------- */

/* Look up a port by full name.  Returns the matching audio_port_t* and
 * fills *node_id_out.  Searches the discovered cache and our own ports. */
static audio_port_t *
audio_lookup_port(audio_client_t *c, const char *name, uint32_t *node_id_out)
{
    for (uint32_t i = 0; i < c->n_discovered; i++) {
        audio_port_t *p = c->discovered[i];
        if (p->name && !strcmp(p->name, name)) {
            if (node_id_out) *node_id_out = p->pw_node_id;
            return p;
        }
    }
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p = c->ports[i];
        if (p->name && !strcmp(p->name, name)) {
            if (node_id_out) *node_id_out = c->our_node_id;
            return p;
        }
    }
    return NULL;
}

bool audio_connect(audio_client_t *c, const char *src, const char *dst)
{
    if (!c || !c->core || !src || !dst) return false;

    uint32_t src_node = SPA_ID_INVALID, dst_node = SPA_ID_INVALID;
    audio_port_t *sp = audio_lookup_port(c, src, &src_node);
    audio_port_t *dp = audio_lookup_port(c, dst, &dst_node);

    if (!sp || !dp || sp->pw_port_id == 0 || dp->pw_port_id == 0
                   || src_node == SPA_ID_INVALID || dst_node == SPA_ID_INVALID) {
        WARN("audio_connect: cannot resolve PipeWire IDs for %s -> %s "
             "(sp=%p sp.pw_port_id=%u src_node=%u | dp=%p dp.pw_port_id=%u dst_node=%u | %u nodes, %u ext-ports in cache)\n",
             src, dst, sp, sp ? sp->pw_port_id : 0, src_node,
             dp, dp ? dp->pw_port_id : 0, dst_node,
             c->n_nodes, c->n_discovered);
        return false;
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_OBJECT_LINGER, "false",
        NULL);
    if (!props) return false;
    pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", src_node);
    pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", sp->pw_port_id);
    pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE,  "%u", dst_node);
    pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT,  "%u", dp->pw_port_id);

    pw_thread_loop_lock(c->loop);
    struct pw_proxy *link = pw_core_create_object(c->core, "link-factory",
                                                  PW_TYPE_INTERFACE_Link,
                                                  PW_VERSION_LINK,
                                                  &props->dict, 0);
    pw_thread_loop_unlock(c->loop);
    pw_properties_free(props);

    if (!link) {
        WARN("audio_connect: pw_core_create_object(link-factory) failed for %s -> %s\n",
             src, dst);
        return false;
    }
    TRACE("audio_connect: %s -> %s (link proxy %p)\n", src, dst, link);
    return true;
}

uint32_t audio_transport_query(const audio_client_t *c, audio_position_t *pos)
{
    if (pos) {
        pos->frame      = 0;
        pos->usecs      = 0;
        pos->frame_rate = c ? c->sample_rate : 0;
    }
    return AUDIO_TRANSPORT_STOPPED;
}

void audio_free(void *ptr)
{
    free(ptr);
}

/* ----------------------------------------------------------------------
 * Filter event callbacks — fire on the PipeWire data thread.  Because the
 * data thread was spawned by our audio_rt_create, it is a real Wine
 * thread; calling back into asio.c's process_cb (which then calls the
 * host's ASIO COM bufferSwitch) is safe.
 * ---------------------------------------------------------------------- */

static void audio_on_state_changed(void *userdata, enum pw_filter_state old,
                                   enum pw_filter_state state, const char *error)
{
    audio_client_t *c = userdata;
    uint32_t nid = c->filter ? pw_filter_get_node_id(c->filter) : SPA_ID_INVALID;
    TRACE("pw_filter state: %s -> %s (node_id=%u)\n",
          pw_filter_state_as_string(old),
          pw_filter_state_as_string(state),
          nid);

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

static void audio_on_io_changed(void *userdata, void *port_data, uint32_t id,
                                void *area, uint32_t size)
{
    audio_client_t *c = userdata;
    (void)port_data;
    if (id == SPA_IO_Position && area && size >= sizeof(struct spa_io_position)) {
        const struct spa_io_position *pos = area;
        /* spa_fraction (num, denom): for an audio graph num=1 and denom is
         * the sample rate in Hz, so we just take denom. */
        audio_nframes_t new_rate = pos->clock.rate.denom
            ? pos->clock.rate.denom : c->sample_rate;
        if (new_rate && new_rate != c->sample_rate) {
            c->sample_rate = new_rate;
            if (c->sample_rate_cb)
                c->sample_rate_cb(new_rate, c->sample_rate_cb_arg);
        }
    }
}

static void audio_on_process(void *userdata, struct spa_io_position *position)
{
    audio_client_t *c = userdata;

    if (position) {
        c->last_clock_nsec     = position->clock.nsec;
        c->last_clock_position = position->clock.position;
    }

    /* Dequeue this cycle's buffer for every port.  Because the ports use
     * PW_FILTER_PORT_FLAG_MAP_BUFFERS, pw_filter has already mmap'd each
     * buffer's shared memory into datas[0].data — the SAME memory the
     * daemon plays (outputs) or just captured into (inputs).  The ASIO
     * host therefore writes/reads straight into the live buffer via
     * audio_port_get_buffer, and we queue back exactly the buffer we
     * dequeued (the standard pw_filter contract). */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p = c->ports[i];
        p->cycle_buffer = p->pw_filter_port
            ? pw_filter_dequeue_buffer(p->pw_filter_port) : NULL;
    }

    const uint32_t quantum = position ? (uint32_t)position->clock.duration : 0;

    /* Decisive #3 signal: the host's bufferSwitch runs once per process() for
     * c->buffer_size frames, but we emit `quantum` frames to the graph — so
     * playback speed scales as buffer_size/quantum.  They MUST be equal; if
     * the daemon clamped our forced quantum (pinned clock.min/max-quantum)
     * they diverge and audio runs slow/pitched-down.  Warn once (the data
     * loop is single-threaded here), unconditionally so it shows without
     * PIPEASIO_DEBUG. */
    if (quantum && quantum != c->buffer_size) {
        static bool quantum_warned;
        if (!quantum_warned) {
            quantum_warned = true;
            WARN("PipeWire quantum %u != host buffer_size %u: graph clamped our "
                 "forced quantum, playback runs at %u/%u speed. Raise the host "
                 "buffer size or relax clock.min-quantum/clock.max-quantum "
                 "(pw-metadata -n settings).\n",
                 (unsigned)quantum, (unsigned)c->buffer_size,
                 (unsigned)c->buffer_size, (unsigned)quantum);
        }
    }

    if (pipeasio_log_on()) {
        static uint64_t cycle_count;
        if (++cycle_count <= 8
            || (cycle_count <  100 && cycle_count %  10 == 0)
            || (cycle_count >= 100 && cycle_count % 100 == 0))
            TRACE("process: cycle=%lu tid=%lx buffer_size=%u quantum=%u rate=%u/%u\n",
                  (unsigned long)cycle_count, (unsigned long)GetCurrentThreadId(),
                  (unsigned)c->buffer_size, (unsigned)quantum,
                  position ? (unsigned)position->clock.rate.num : 0u,
                  position ? (unsigned)position->clock.rate.denom : 0u);
    }

    /* Run the ASIO host's process callback; audio_port_get_buffer returns
     * each port's dequeued-buffer data pointer. */
    if (c->process_cb)
        c->process_cb(c->buffer_size, c->process_cb_arg);

    const uint32_t bytes_this_cycle =
        (position ? position->clock.duration : c->buffer_size) * sizeof(audio_sample_t);

    /* Queue every dequeued buffer back to the daemon. */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t     *p = c->ports[i];
        struct pw_buffer *b = p->cycle_buffer;
        if (!b || !p->pw_filter_port)
            continue;
        if (p->direction == PW_DIRECTION_OUTPUT) {
            struct spa_data *d = &b->buffer->datas[0];
            d->chunk->offset = 0;
            d->chunk->size   = bytes_this_cycle;
            d->chunk->stride = sizeof(audio_sample_t);
            d->chunk->flags  = 0;
        }
        pw_filter_queue_buffer(p->pw_filter_port, b);
        p->cycle_buffer = NULL;
    }
}

/* ----------------------------------------------------------------------
 * Core sync — block until the daemon acknowledges that every prior
 * request has been processed.  Used after registry binding (initial
 * port enumeration) and after pw_filter_connect (filter port discovery).
 * ---------------------------------------------------------------------- */

static void audio_on_core_done(void *userdata, uint32_t id, int seq)
{
    audio_client_t *c = userdata;
    if (id != PW_ID_CORE) return;
    if (seq == c->sync_seq)
        pw_thread_loop_signal(c->loop, false);
}

static void audio_sync(audio_client_t *c)
{
    if (!c->core || !c->loop) return;
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
static void audio_adopt_own_ports(audio_client_t *c)
{
    if (!c || c->our_node_id == SPA_ID_INVALID) return;

    uint32_t kept = 0;
    uint32_t adopted = 0;
    for (uint32_t i = 0; i < c->n_discovered; i++) {
        audio_port_t *d = c->discovered[i];
        if (d->pw_node_id != c->our_node_id) {
            c->discovered[kept++] = d;
            continue;
        }

        /* Port belongs to our filter.  Match by port_name (after the
         * "node:" prefix) against c->ports[] and backfill IDs. */
        const char *colon = strrchr(d->name, ':');
        const char *port_name = colon ? colon + 1 : d->name;
        for (uint32_t j = 0; j < c->n_ports; j++) {
            if (c->ports[j]->name && !strcmp(c->ports[j]->name, port_name)) {
                c->ports[j]->pw_node_id = c->our_node_id;
                c->ports[j]->pw_port_id = d->pw_port_id;
                adopted++;
                break;
            }
        }

        free(d->name);
        free(d->type);
        free(d);
    }
    c->n_discovered = kept;
    TRACE("audio_adopt_own_ports: our_node_id=%u, adopted=%u, %u ext-ports remain\n",
          c->our_node_id, adopted, c->n_discovered);
}

/* ----------------------------------------------------------------------
 * Registry walker — caches PipeWire Nodes and Ports so audio_get_ports
 * and audio_connect can resolve names ↔ IDs.  Filter rules adapted from
 * pwasio's design (we ignore monitor ports, Internal media classes, and
 * non-audio media types).
 * ---------------------------------------------------------------------- */

static struct audio_node_info *
audio_find_node(audio_client_t *c, uint32_t id)
{
    for (uint32_t i = 0; i < c->n_nodes; i++)
        if (c->nodes[i]->id == id) return c->nodes[i];
    return NULL;
}

static char *audio_dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

static void audio_cache_node(audio_client_t *c, uint32_t id,
                             const struct spa_dict *props)
{
    const char *node_name   = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char *nick        = spa_dict_lookup(props, PW_KEY_NODE_NICK);
    if (!node_name) return;

    /* Always cache our OWN filter node — matched by the node name we set,
     * or by its id once bound.  A Duplex/DSP filter carries no "Audio"
     * media.class, so without this audio_find_node() can't resolve it and
     * the registry globals for our own ports get dropped as orphans in
     * audio_cache_port (below), leaving audio_connect unable to link them
     * -> no sound. */
    int is_ours = (c->our_node_id != SPA_ID_INVALID && id == c->our_node_id)
               || (c->name && !strcmp(node_name, c->name));

    /* Otherwise only cache audio nodes — saves us from holding refs to
     * every stream/module/etc. */
    if (!is_ours) {
        if (!media_class || !strstr(media_class, "Audio"))
            return;
        if (strstr(media_class, "Internal"))
            return;
    }

    /* Skip duplicates (shouldn't happen but be defensive). */
    if (audio_find_node(c, id)) return;

    struct audio_node_info *n = calloc(1, sizeof(*n));
    if (!n) return;
    n->id            = id;
    n->node_name     = strdup(node_name);
    n->media_class   = audio_dup_or_null(media_class);
    n->display_name  = strdup(description ? description : (nick ? nick : node_name));

    if (c->n_nodes == c->cap_nodes) {
        uint32_t new_cap = c->cap_nodes ? c->cap_nodes * 2 : 16;
        struct audio_node_info **grown = realloc(c->nodes, new_cap * sizeof(*grown));
        if (!grown) {
            free(n->node_name); free(n->display_name); free(n->media_class); free(n);
            return;
        }
        c->nodes = grown;
        c->cap_nodes = new_cap;
    }
    c->nodes[c->n_nodes++] = n;
    TRACE("registry: +node id=%u name=\"%s\" class=\"%s\" desc=\"%s\"\n",
          id, n->node_name, n->media_class ? n->media_class : "",
          n->display_name);
}

static void audio_cache_port(audio_client_t *c, uint32_t id,
                             const struct spa_dict *props)
{
    const char *node_id_s = spa_dict_lookup(props, PW_KEY_NODE_ID);
    const char *port_name = spa_dict_lookup(props, PW_KEY_PORT_NAME);
    const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
    const char *monitor   = spa_dict_lookup(props, PW_KEY_PORT_MONITOR);
    if (!node_id_s || !port_name || !direction) return;

    uint32_t node_id = (uint32_t)strtoul(node_id_s, NULL, 10);

    /* If this port belongs to our own filter node, backfill the matching
     * c->ports[i]->pw_port_id and don't add it to the discovered list. */
    if (c->our_node_id != SPA_ID_INVALID && node_id == c->our_node_id) {
        for (uint32_t i = 0; i < c->n_ports; i++) {
            if (!strcmp(c->ports[i]->name, port_name)) {
                c->ports[i]->pw_node_id = node_id;
                c->ports[i]->pw_port_id = id;
                TRACE("registry: backfill local port \"%s\" -> node=%u port=%u\n",
                      port_name, node_id, id);
                return;
            }
        }
        TRACE("registry: own-node port \"%s\" (id=%u) not in c->ports[]\n",
              port_name, id);
        return;
    }

    /* External port.  Skip monitor-of-sink (avoid feedback loops). */
    if (monitor && !strcmp(monitor, "true")) return;

    /* Look up the node to confirm it's audio, and to build the full name. */
    struct audio_node_info *n = audio_find_node(c, node_id);
    if (!n) {
        TRACE("registry: skip orphan port id=%u node_id=%u (node not cached)\n",
              id, node_id);
        return;
    }

    enum pw_direction dir = (!strcmp(direction, "in")) ? PW_DIRECTION_INPUT
                                                       : PW_DIRECTION_OUTPUT;

    audio_port_t *p = calloc(1, sizeof(*p));
    if (!p) return;

    size_t namelen = strlen(n->display_name) + 1 + strlen(port_name) + 1;
    p->name = malloc(namelen);
    if (!p->name) { free(p); return; }
    snprintf(p->name, namelen, "%s:%s", n->display_name, port_name);
    p->type        = strdup("32 bit float mono audio");
    p->direction   = dir;
    p->pw_node_id  = node_id;
    p->pw_port_id  = id;

    /* asio.c asks for PHYSICAL|OUTPUT to list capture sources and
     * PHYSICAL|INPUT to list playback sinks.  Map PW "out" → OUTPUT,
     * "in" → INPUT. */
    p->flags = AUDIO_PORT_IS_PHYSICAL
             | ((dir == PW_DIRECTION_OUTPUT) ? AUDIO_PORT_IS_OUTPUT
                                             : AUDIO_PORT_IS_INPUT);

    if (c->n_discovered == c->cap_discovered) {
        uint32_t new_cap = c->cap_discovered ? c->cap_discovered * 2 : 32;
        audio_port_t **grown = realloc(c->discovered, new_cap * sizeof(*grown));
        if (!grown) { free(p->name); free(p->type); free(p); return; }
        c->discovered = grown;
        c->cap_discovered = new_cap;
    }
    c->discovered[c->n_discovered++] = p;
    TRACE("registry: +port id=%u node_id=%u name=\"%s\" dir=%s\n",
          id, node_id, p->name, dir == PW_DIRECTION_INPUT ? "in" : "out");
}

static void audio_on_registry_global(void *userdata, uint32_t id, uint32_t permissions,
                                     const char *type, uint32_t version,
                                     const struct spa_dict *props)
{
    audio_client_t *c = userdata;
    (void)permissions; (void)version;

    if (!type || !props) return;

    if (!strcmp(type, PW_TYPE_INTERFACE_Node))
        audio_cache_node(c, id, props);
    else if (!strcmp(type, PW_TYPE_INTERFACE_Port))
        audio_cache_port(c, id, props);
}

static void audio_on_registry_global_remove(void *userdata, uint32_t id)
{
    audio_client_t *c = userdata;

    /* Drop the port from our discovered list if present. */
    for (uint32_t i = 0; i < c->n_discovered; i++) {
        if (c->discovered[i]->pw_port_id == id) {
            audio_port_t *p = c->discovered[i];
            free(p->name); free(p->type); free(p);
            memmove(&c->discovered[i], &c->discovered[i + 1],
                    (c->n_discovered - i - 1) * sizeof(*c->discovered));
            c->n_discovered--;
            return;
        }
    }
    /* Or drop a node entry. */
    for (uint32_t i = 0; i < c->n_nodes; i++) {
        if (c->nodes[i]->id == id) {
            struct audio_node_info *n = c->nodes[i];
            free(n->node_name); free(n->display_name); free(n->media_class); free(n);
            memmove(&c->nodes[i], &c->nodes[i + 1],
                    (c->n_nodes - i - 1) * sizeof(*c->nodes));
            c->n_nodes--;
            return;
        }
    }
}
