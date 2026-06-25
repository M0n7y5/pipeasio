/*
 * pw_filter_probe.c - PipeWire-contract conformance test for PipeASIO.
 *
 * This replicates src/audio.c's PipeWire setup (custom spa_thread_utils,
 * pw_filter with MAP_BUFFERS DSP ports, FORCE_QUANTUM/RATE) WITHOUT Wine
 * or an ASIO host, so the contract the driver depends on can be checked
 * offline in ~1 second.  It pins the two invariants that were the root of
 * the FL Studio crash + "noise" saga:
 *
 *   1. THREADING.  The pw_filter process() callback - which in the real
 *      driver calls the ASIO host's COM bufferSwitch - MUST run on a thread
 *      our spa_thread_utils created (i.e. a Win32 CreateThread'd Wine thread
 *      with a real TEB).  This only happens if the context's data loop is
 *      stopped after our thread_utils is installed and then restarted
 *      through it, AND the filter is created on that data loop.  Skip either
 *      step and process() runs on a foreign pthread -> COM calls corrupt
 *      memory under Wine (the bug this project chased for weeks).
 *
 *   2. QUANTUM.  PW_KEY_NODE_FORCE_QUANTUM must pin spa_io_position.clock
 *      .duration to the ASIO buffer size, so the per-cycle copy length is
 *      correct.  (The driver assumes duration == buffer_size.)
 *
 * Default config mirrors the driver's fix (data loop + restart "dance" +
 * PW_FILTER_FLAG_NONE) and must PASS.  Flip --loop main / --no-dance to
 * reproduce the broken configuration (process on a non-bridged thread).
 *
 * Exit: 0 PASS, 1 FAIL, 77 SKIP (no PipeWire daemon).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors.  GPL v3+ (see COPYING).
 */
#define _GNU_SOURCE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/type.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define N_PORTS    2u
#define BSIZE      1024u
#define RATE       48000u
#define SAMPLE_SZ  sizeof(float)
#define RUN_USEC   (1200 * 1000)

static pid_t this_tid(void) { return (pid_t)syscall(SYS_gettid); }

/* ---- spa_thread_utils that records every tid it spawns ---------------- */

static atomic_int g_create_calls;
static pid_t      g_spawned[8];
static atomic_int g_n_spawned;
static pthread_t  g_handles[8];
static atomic_int g_n_handles;

struct tramp { void *(*entry)(void *); void *arg; };

static void *rt_trampoline(void *raw)
{
    struct tramp t = *(struct tramp *)raw;
    free(raw);
    int i = atomic_fetch_add(&g_n_spawned, 1);
    if (i < 8) g_spawned[i] = this_tid();
    return t.entry(t.arg);
}
static struct spa_thread *rt_create(void *data, const struct spa_dict *props,
                                    void *(*entry)(void *), void *arg)
{
    (void)data; (void)props;
    atomic_fetch_add(&g_create_calls, 1);
    struct tramp *t = malloc(sizeof *t);
    t->entry = entry; t->arg = arg;
    int h = atomic_fetch_add(&g_n_handles, 1);
    pthread_create(&g_handles[h], NULL, rt_trampoline, t);
    return (struct spa_thread *)&g_handles[h];
}
static int rt_join(void *data, struct spa_thread *thread, void **retval)
{
    (void)data;
    /* Only join handles we own; the default utils may pass us a foreign
     * one when the data loop is stopped before our first create(). */
    int n = atomic_load(&g_n_handles);
    for (int i = 0; i < n && i < 8; i++) {
        if ((struct spa_thread *)&g_handles[i] == thread) {
            void *r; pthread_join(g_handles[i], &r);
            if (retval) *retval = r;
            return 0;
        }
    }
    return 0;
}
static int rt_range(void *d, const struct spa_dict *p, int *mn, int *mx)
{ (void)d; (void)p; *mn = 0; *mx = 0; return 0; }
static int rt_acq(void *d, struct spa_thread *t, int prio)
{ (void)d; (void)t; (void)prio; return 0; }
static int rt_drop(void *d, struct spa_thread *t) { (void)d; (void)t; return 0; }

static const struct spa_thread_utils_methods rt_methods = {
    SPA_VERSION_THREAD_UTILS_METHODS,
    .create = rt_create, .join = rt_join,
    .get_rt_range = rt_range, .acquire_rt = rt_acq, .drop_rt = rt_drop,
};
static int spawned_by_us(pid_t tid)
{
    int n = atomic_load(&g_n_spawned);
    for (int i = 0; i < n && i < 8; i++)
        if (g_spawned[i] == tid) return 1;
    return 0;
}

/* ---- engine ----------------------------------------------------------- */

struct probe_port {
    enum pw_direction dir;
    void             *fp;             /* pw_filter port */
    struct pw_buffer *buf[2];
    int               live_buffers;   /* current (add - remove) */
};
struct engine {
    struct probe_port    ports[N_PORTS];
    atomic_int           cycles;
    pid_t                process_tid;
    uint32_t             obs_duration;
    uint32_t             obs_rate;
    atomic_int           first_done;
};

static void on_io_changed(void *u, void *pd, uint32_t id, void *area, uint32_t sz)
{
    struct engine *e = u; (void)pd;
    if (id == SPA_IO_Position && area && sz >= sizeof(struct spa_io_position))
        e->obs_rate = ((struct spa_io_position *)area)->clock.rate.denom;
}
static void on_add_buffer(void *u, void *pd, struct pw_buffer *b)
{
    (void)u;
    struct probe_port *p = *(struct probe_port **)pd;
    int half = p->buf[0] ? 1 : 0;
    if (p->buf[half]) return;
    p->buf[half] = b;
    p->live_buffers++;
}
static void on_remove_buffer(void *u, void *pd, struct pw_buffer *b)
{
    (void)u;
    struct probe_port *p = *(struct probe_port **)pd;
    if (b == p->buf[0]) { p->buf[0] = NULL; p->live_buffers--; }
    if (b == p->buf[1]) { p->buf[1] = NULL; p->live_buffers--; }
}
static void on_process(void *u, struct spa_io_position *pos)
{
    struct engine *e = u;
    e->process_tid = this_tid();
    atomic_fetch_add(&e->cycles, 1);
    if (!atomic_exchange(&e->first_done, 1) && pos)
        e->obs_duration = pos->clock.duration;

    for (uint32_t i = 0; i < N_PORTS; i++) {
        struct pw_buffer *b = pw_filter_dequeue_buffer(e->ports[i].fp);
        if (!b) continue;
        struct spa_data *d = &b->buffer->datas[0];
        uint32_t bytes = (pos ? pos->clock.duration : BSIZE) * SAMPLE_SZ;
        /* MAP_BUFFERS: datas[0].data is the mmap'd shared buffer the daemon
         * reads.  Write silence so the test never blasts audio if linked. */
        if (d->data) memset(d->data, 0, bytes);
        d->chunk->offset = 0;
        d->chunk->size   = bytes;
        d->chunk->stride = SAMPLE_SZ;
        d->chunk->flags  = 0;
        pw_filter_queue_buffer(e->ports[i].fp, b);
    }
}
static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .io_changed = on_io_changed, .add_buffer = on_add_buffer,
    .remove_buffer = on_remove_buffer, .process = on_process,
};

int main(int argc, char **argv)
{
    int use_data = 1, dance = 1, rt = 0;   /* default = the driver's fix */
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--loop") && i + 1 < argc)
            use_data = !strcmp(argv[++i], "data");
        else if (!strcmp(argv[i], "--no-dance")) dance = 0;
        else if (!strcmp(argv[i], "--rt"))       rt = 1;
    }
    fprintf(stderr, "[pw_probe] config: filter-loop=%s data-loop-dance=%d rt-flag=%d\n",
            use_data ? "data" : "main", dance, rt);

    pw_init(NULL, NULL);
    struct engine e;
    memset(&e, 0, sizeof e);

    struct pw_thread_loop *tl = pw_thread_loop_new("pw_probe", NULL);
    if (!tl) {
        fprintf(stderr, "[pw_probe] SKIP: pw_thread_loop_new failed (no SPA support plugins?)\n");
        return 77;
    }
    struct pw_context     *ctx = pw_context_new(pw_thread_loop_get_loop(tl), NULL, 0);
    struct pw_data_loop   *dl = pw_context_get_data_loop(ctx);

    struct spa_thread_utils iface;
    iface.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_ThreadUtils,
                                     SPA_VERSION_THREAD_UTILS, &rt_methods, NULL);
    pw_context_set_object(ctx, SPA_TYPE_INTERFACE_ThreadUtils, &iface);
    pw_data_loop_set_thread_utils(dl, &iface);
    if (dance) pw_data_loop_stop(dl);

    pw_thread_loop_start(tl);
    pw_thread_loop_lock(tl);
    struct pw_core *core = pw_context_connect(ctx, NULL, 0);
    if (!core) {
        pw_thread_loop_unlock(tl);
        fprintf(stderr, "[pw_probe] SKIP: cannot connect to PipeWire daemon\n");
        return 77;
    }
    pw_thread_loop_unlock(tl);

    for (uint32_t i = 0; i < N_PORTS; i++)
        e.ports[i].dir = PW_DIRECTION_OUTPUT;

    struct pw_properties *fprops = pw_properties_new(
        PW_KEY_NODE_NAME, "pw_probe", PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_NODE_ALWAYS_PROCESS, "true", NULL);
    pw_properties_setf(fprops, PW_KEY_NODE_FORCE_QUANTUM, "%u", BSIZE);
    pw_properties_setf(fprops, PW_KEY_NODE_FORCE_RATE,    "%u", RATE);

    struct pw_loop *floop = use_data ? pw_data_loop_get_loop(dl)
                                     : pw_thread_loop_get_loop(tl);

    pw_thread_loop_lock(tl);
    struct pw_filter *filter =
        pw_filter_new_simple(floop, "pw_probe", fprops, &filter_events, &e);
    for (uint32_t i = 0; i < N_PORTS; i++) {
        struct pw_properties *pp = pw_properties_new(NULL, NULL);
        pw_properties_set(pp, PW_KEY_FORMAT_DSP, "32 bit float mono audio");
        pw_properties_setf(pp, PW_KEY_PORT_NAME, "out_%u", i + 1);
        uint8_t pod_buf[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof pod_buf);
        const struct spa_pod *params[] = {
            spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers,  SPA_POD_Int(2),
                SPA_PARAM_BUFFERS_size,     SPA_POD_Int((int)(BSIZE * SAMPLE_SZ)),
                SPA_PARAM_BUFFERS_stride,   SPA_POD_Int((int)SAMPLE_SZ),
                SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)),
        };
        e.ports[i].fp = pw_filter_add_port(filter, e.ports[i].dir,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(struct probe_port *),
            pp, params, 1);
        *(struct probe_port **)e.ports[i].fp = &e.ports[i];
    }
    pw_filter_connect(filter, rt ? PW_FILTER_FLAG_RT_PROCESS : PW_FILTER_FLAG_NONE,
                      NULL, 0);
    pw_thread_loop_unlock(tl);

    /* Start the data loop AFTER the filter is connected.  pwasio keeps it
     * stopped through add_port/connect (starting earlier makes those calls
     * fail "wrong context: not in loop"); once started, the node is
     * scheduled on this Wine-bridged data loop and binds. */
    if (dance) {
        pw_thread_loop_lock(tl);
        pw_data_loop_start(dl);
        pw_thread_loop_unlock(tl);
    }

    usleep(RUN_USEC);

    /* ---- evaluate ---- */
    int cycles     = atomic_load(&e.cycles);
    int on_bridged = e.process_tid > 0 && spawned_by_us(e.process_tid);
    int quantum_ok = e.obs_duration == BSIZE;
    uint32_t node_id = pw_filter_get_node_id(filter);
    int bound      = node_id != SPA_ID_INVALID;

    fprintf(stderr, "\n[pw_probe] ==== report ====\n");
    fprintf(stderr, "[pw_probe] thread_utils.create() fired : %d\n", atomic_load(&g_create_calls));
    fprintf(stderr, "[pw_probe] process() cycles            : %d\n", cycles);
    fprintf(stderr, "[pw_probe] process() on bridged thread : %s\n",
            on_bridged ? "YES" : "NO (COM bufferSwitch would corrupt under Wine)");
    fprintf(stderr, "[pw_probe] FORCE_QUANTUM %u -> duration : %u (%s)\n",
            BSIZE, e.obs_duration, quantum_ok ? "locked" : "MISMATCH");
    fprintf(stderr, "[pw_probe] clock.rate                  : %u\n", e.obs_rate);
    fprintf(stderr, "[pw_probe] filter bound (node id)       : %s (%u)\n",
            bound ? "YES" : "NO", node_id);
    fprintf(stderr, "[pw_probe] live buffers/port           : %d, %d (0 = unlinked, informational)\n",
            e.ports[0].live_buffers, e.ports[1].live_buffers);

    /* Teardown (under the loop lock - the filter lives on a loop thread). */
    pw_thread_loop_lock(tl);
    pw_filter_destroy(filter);
    pw_thread_loop_unlock(tl);
    pw_thread_loop_lock(tl);
    pw_core_disconnect(core);
    pw_thread_loop_unlock(tl);
    pw_thread_loop_stop(tl);
    pw_context_destroy(ctx);
    pw_thread_loop_destroy(tl);

    int pass = cycles > 0 && on_bridged && quantum_ok && bound;
    fprintf(stderr, "[pw_probe] RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
