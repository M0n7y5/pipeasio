/*
 * pw_delivery_probe.c - PipeWire buffer-DELIVERY conformance test for PipeASIO.
 *
 * The threading probe (pw_filter_probe.c) checks that process() runs on the
 * right thread, but it never verifies that the bytes the driver writes into a
 * buffer are the bytes the daemon reads back.  That gap let a real bug ship:
 * the driver aliased a private memfd into each buffer (ALLOC_BUFFERS + fd
 * override), the host wrote into it, but the daemon never read it and played
 * its own uninitialised buffer instead - broadband garbage layered over the
 * audio (the "buzz").
 *
 * This test pins the delivery contract end-to-end, offline, in ~1 second:
 *   - a PRODUCER pw_filter (1 output port, MAP_BUFFERS) writes a contiguous
 *     integer ramp into datas[0].data every cycle,
 *   - a CONSUMER pw_filter (1 input port, MAP_BUFFERS) is linked to it and
 *     checks the bytes it receives are that exact ramp (diff == 1, non-zero,
 *     finite).
 * With the broken memfd-alias path the consumer sees zeros/garbage and the
 * test FAILS; with MAP_BUFFERS it sees the ramp and PASSES.
 *
 * Exit: 0 PASS, 1 FAIL, 77 SKIP (no PipeWire daemon / never scheduled).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors.  GPL v3+ (see COPYING).
 */
#define _GNU_SOURCE
#include <pipewire/pipewire.h>
#include <spa/pod/builder.h>
#include <spa/param/param.h>

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BSIZE      1024u
#define RATE       48000u
#define SAMPLE_SZ  sizeof(float)
#define RUN_USEC   (800 * 1000)

/* ---- producer: writes an integer ramp into its output buffer ---------- */
struct producer {
    struct pw_filter *filter;
    void             *port;
    double            counter;     /* next ramp value */
    atomic_int        cycles;
};
static void prod_process(void *u, struct spa_io_position *pos)
{
    struct producer *pr = u;
    uint32_t n = pos ? pos->clock.duration : BSIZE;
    struct pw_buffer *b = pw_filter_dequeue_buffer(pr->port);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    float *dst = d->data;
    uint32_t m = n; if (m * SAMPLE_SZ > d->maxsize) m = d->maxsize / SAMPLE_SZ;
    if (dst)
        for (uint32_t i = 0; i < m; i++)
            dst[i] = (float)(pr->counter + i);
    pr->counter += m;
    d->chunk->offset = 0;
    d->chunk->size   = m * SAMPLE_SZ;
    d->chunk->stride = SAMPLE_SZ;
    d->chunk->flags  = 0;
    pw_filter_queue_buffer(pr->port, b);
    atomic_fetch_add(&pr->cycles, 1);
}
static const struct pw_filter_events producer_events = {
    PW_VERSION_FILTER_EVENTS, .process = prod_process,
};

/* ---- consumer: verifies the ramp arrived intact ----------------------- */
struct consumer {
    struct pw_filter *filter;
    void             *port;
    atomic_int        ramp_ok;     /* buffers that are the exact +1 ramp */
    atomic_int        silence;     /* all-zero buffers (legit at startup) */
    atomic_int        bad;         /* non-finite or corrupt (the bug signature) */
    atomic_int        seen;        /* buffers with data */
};
static void cons_process(void *u, struct spa_io_position *pos)
{
    struct consumer *co = u;
    (void)pos;
    struct pw_buffer *b = pw_filter_dequeue_buffer(co->port);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    const float *src = d->data;
    uint32_t m = d->chunk ? d->chunk->size / SAMPLE_SZ : 0;
    if (src && m >= 2) {
        atomic_fetch_add(&co->seen, 1);
        int ramp = 1, nonzero = 0, finite = 1;
        for (uint32_t i = 0; i < m; i++) {
            if (!isfinite(src[i])) { finite = 0; break; }
            if (src[i] != 0.0f) nonzero = 1;
            if (i && src[i] - src[i - 1] != 1.0f) ramp = 0;
        }
        if (!finite)              atomic_fetch_add(&co->bad, 1);     /* NaN/inf = the memfd-alias bug */
        else if (!nonzero)        atomic_fetch_add(&co->silence, 1); /* startup zeros, tolerated */
        else if (ramp)            atomic_fetch_add(&co->ramp_ok, 1); /* exact ramp delivered */
        else                      atomic_fetch_add(&co->bad, 1);     /* finite but corrupt */
    }
    pw_filter_queue_buffer(co->port, b);
}
static const struct pw_filter_events consumer_events = {
    PW_VERSION_FILTER_EVENTS, .process = cons_process,
};

/* ---- registry: capture every port (node id, direction, object id) so we can
 * resolve our producer/consumer ports once their node ids are known. ------- */
struct reg {
    struct { uint32_t node, id; int is_out; } ports[128];
    int n;
};
static void reg_global(void *u, uint32_t id, uint32_t perm, const char *type,
                       uint32_t ver, const struct spa_dict *props)
{
    struct reg *r = u; (void)perm; (void)ver;
    if (!props || !type || strcmp(type, PW_TYPE_INTERFACE_Port)) return;
    const char *nid = spa_dict_lookup(props, PW_KEY_NODE_ID);
    const char *dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
    if (!nid || !dir || r->n >= 128) return;
    r->ports[r->n].node   = (uint32_t)strtoul(nid, NULL, 10);
    r->ports[r->n].id     = id;
    r->ports[r->n].is_out = !strcmp(dir, "out");
    r->n++;
}
static uint32_t reg_find(struct reg *r, uint32_t node, int is_out)
{
    for (int i = 0; i < r->n; i++)
        if (r->ports[i].node == node && r->ports[i].is_out == is_out)
            return r->ports[i].id;
    return 0;
}
static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS, .global = reg_global,
};

static void *add_dsp_port(struct pw_filter *f, enum pw_direction dir,
                          const char *name, size_t user_sz)
{
    struct pw_properties *pp = pw_properties_new(NULL, NULL);
    pw_properties_set(pp, PW_KEY_FORMAT_DSP, "32 bit float mono audio");
    pw_properties_set(pp, PW_KEY_PORT_NAME, name);
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *params[] = {
        spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers,  SPA_POD_Int(2),
            SPA_PARAM_BUFFERS_size,     SPA_POD_Int((int)(BSIZE * SAMPLE_SZ)),
            SPA_PARAM_BUFFERS_stride,   SPA_POD_Int((int)SAMPLE_SZ),
            SPA_PARAM_BUFFERS_align,    SPA_POD_Int((int)(BSIZE * SAMPLE_SZ)),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)),
    };
    return pw_filter_add_port(f, dir, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                              user_sz, pp, params, 1);
}

static struct pw_properties *node_props(const char *name, const char *cat)
{
    struct pw_properties *p = pw_properties_new(
        PW_KEY_NODE_NAME, name, PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, cat, PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_NODE_ALWAYS_PROCESS, "true", PW_KEY_NODE_GROUP, "delivery.probe",
        NULL);
    pw_properties_setf(p, PW_KEY_NODE_FORCE_QUANTUM, "%u", BSIZE);
    pw_properties_setf(p, PW_KEY_NODE_FORCE_RATE, "%u", RATE);
    return p;
}

int main(void)
{
    pw_init(NULL, NULL);
    struct producer pr; memset(&pr, 0, sizeof pr); pr.counter = 1.0;
    struct consumer co; memset(&co, 0, sizeof co);
    struct reg reg; memset(&reg, 0, sizeof reg);

    struct pw_thread_loop *tl = pw_thread_loop_new("delivery", NULL);
    if (!tl) {
        fprintf(stderr, "[delivery] SKIP: pw_thread_loop_new failed (no SPA support plugins?)\n");
        return 77;
    }
    struct pw_context *ctx = pw_context_new(pw_thread_loop_get_loop(tl), NULL, 0);
    pw_thread_loop_start(tl);

    pw_thread_loop_lock(tl);
    struct pw_core *core = pw_context_connect(ctx, NULL, 0);
    if (!core) {
        pw_thread_loop_unlock(tl);
        fprintf(stderr, "[delivery] SKIP: no PipeWire daemon\n");
        return 77;
    }
    struct pw_registry *registry =
        pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    struct spa_hook rhook;
    spa_zero(rhook);
    pw_registry_add_listener(registry, &rhook, &registry_events, &reg);

    pr.filter = pw_filter_new_simple(pw_thread_loop_get_loop(tl), "delivery_src",
                                     node_props("delivery_src", "Playback"),
                                     &producer_events, &pr);
    pr.port = add_dsp_port(pr.filter, PW_DIRECTION_OUTPUT, "out_1", sizeof(void *));
    co.filter = pw_filter_new_simple(pw_thread_loop_get_loop(tl), "delivery_sink",
                                     node_props("delivery_sink", "Capture"),
                                     &consumer_events, &co);
    co.port = add_dsp_port(co.filter, PW_DIRECTION_INPUT, "in_1", sizeof(void *));

    pw_filter_connect(pr.filter, PW_FILTER_FLAG_NONE, NULL, 0);
    pw_filter_connect(co.filter, PW_FILTER_FLAG_NONE, NULL, 0);
    pw_thread_loop_unlock(tl);

    /* Resolve our two node ids, then find their ports in the registry. */
    uint32_t prod_node = 0, cons_node = 0, prod_out = 0, cons_in = 0;
    for (int i = 0; i < 100; i++) {
        usleep(20 * 1000);
        pw_thread_loop_lock(tl);
        if (!prod_node) prod_node = pw_filter_get_node_id(pr.filter);
        if (!cons_node) cons_node = pw_filter_get_node_id(co.filter);
        if (prod_node) prod_out = reg_find(&reg, prod_node, 1);
        if (cons_node) cons_in  = reg_find(&reg, cons_node, 0);
        int have = prod_out && cons_in;
        pw_thread_loop_unlock(tl);
        if (have) break;
    }
    if (!prod_out || !cons_in) {
        fprintf(stderr, "[delivery] SKIP: ports never bound (prod=%u/%u cons=%u/%u)\n",
                prod_node, prod_out, cons_node, cons_in);
        return 77;
    }

    /* Link producer.out_1 -> consumer.in_1 via the link factory. */
    struct pw_properties *lp = pw_properties_new(NULL, NULL);
    pw_properties_setf(lp, PW_KEY_LINK_OUTPUT_NODE, "%u", prod_node);
    pw_properties_setf(lp, PW_KEY_LINK_OUTPUT_PORT, "%u", prod_out);
    pw_properties_setf(lp, PW_KEY_LINK_INPUT_NODE,  "%u", cons_node);
    pw_properties_setf(lp, PW_KEY_LINK_INPUT_PORT,  "%u", cons_in);
    pw_thread_loop_lock(tl);
    struct pw_proxy *link = pw_core_create_object(core, "link-factory",
        PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &lp->dict, 0);
    pw_thread_loop_unlock(tl);
    if (!link) {
        fprintf(stderr, "[delivery] SKIP: link-factory failed\n");
        return 77;
    }

    usleep(RUN_USEC);

    int prod_cycles = atomic_load(&pr.cycles);
    int seen = atomic_load(&co.seen);
    int ramp_ok = atomic_load(&co.ramp_ok);
    int bad = atomic_load(&co.bad);
    int silence = atomic_load(&co.silence);

    pw_thread_loop_lock(tl);
    pw_filter_destroy(pr.filter);
    pw_filter_destroy(co.filter);
    pw_proxy_destroy(link);
    pw_core_disconnect(core);
    pw_thread_loop_unlock(tl);
    pw_thread_loop_stop(tl);
    pw_context_destroy(ctx);
    pw_thread_loop_destroy(tl);

    fprintf(stderr, "[delivery] producer cycles=%d | consumer seen=%d ramp_ok=%d "
            "silence=%d bad=%d\n", prod_cycles, seen, ramp_ok, silence, bad);

    if (seen < 4) {
        fprintf(stderr, "[delivery] SKIP: graph never scheduled (no driver/clock)\n");
        return 77;
    }
    if (ramp_ok == 0) {
        /* The graph ran but the consumer never saw the producer's ramp - the
         * exact memfd-alias failure mode (host wrote a buffer the daemon does
         * not read). */
        fprintf(stderr, "[delivery] RESULT: FAIL (no ramp delivered)\n");
        return 1;
    }
    /* PASS: once data flowed, every buffer was the producer's exact ramp and
     * none was NaN/garbage or corrupt.  Leading all-zero (silence) buffers are
     * tolerated as startup.  The memfd-alias bug produced NaN/garbage and never
     * a single ramp -> bad>0, ramp_ok==0 -> FAIL. */
    int pass = ramp_ok > 0 && bad == 0;
    fprintf(stderr, "[delivery] RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
