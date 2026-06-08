/*
 * pw_default_probe — PipeWire-contract test for src/audio.c's "Follow default"
 * device resolution.
 *
 * The driver's "Follow default" choice depends on a chain of PipeWire facts:
 *   1. a metadata object named "default" exists and is bindable,
 *   2. it publishes default.audio.sink / default.audio.source as the JSON
 *      object {"name":"<node.name>"} (parsed with spa_json_str_object_find),
 *   3. that node appears in the registry and carries ports in the direction
 *      the driver needs — a sink we WRITE to has "in" (playback) ports, a
 *      source we READ from has "out" (capture) ports.
 *
 * audio_preferred_default_node() encodes exactly this; here we replicate it
 * against the live daemon so a PipeWire change (renamed key, reshaped JSON)
 * that would silently break "Follow default" fails the suite instead.
 *
 * Needs a running PipeWire daemon; exits 77 (CTest SKIP) when none is
 * reachable, or when the session reports no default sink AND no default
 * source (nothing to verify).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors.  LGPL v2.1+ (see COPYING.LIB).
 */
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/utils/json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct node
{
    uint32_t id;
    char     name[256];
    char     mclass[64];
    int      n_in, n_out;
};

struct data
{
    struct pw_main_loop *loop;
    struct pw_context   *context;
    struct pw_core      *core;
    struct pw_registry  *registry;
    struct spa_hook      core_listener, registry_listener;
    struct pw_metadata  *meta;
    struct spa_hook      meta_listener;
    int                  pending;
    char                 sink[256], source[256];
    struct node          nodes[512];
    int                  n_nodes;
};

static int
on_prop(void *userdata, uint32_t subject, const char *key, const char *type, const char *value)
{
    struct data *x = userdata;
    (void)subject;
    (void)type;
    if (!key || !value)
        return 0;
    if (!strcmp(key, "default.audio.sink"))
        spa_json_str_object_find(value, strlen(value), "name", x->sink, sizeof x->sink);
    else if (!strcmp(key, "default.audio.source"))
        spa_json_str_object_find(value, strlen(value), "name", x->source, sizeof x->source);
    return 0;
}
static const struct pw_metadata_events meta_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_prop,
};

static void
on_global(void *userdata, uint32_t id, uint32_t perm, const char *type, uint32_t ver,
          const struct spa_dict *props)
{
    struct data *x = userdata;
    (void)perm;
    if (!type || !props)
        return;

    if (!strcmp(type, PW_TYPE_INTERFACE_Node))
    {
        const char *nm = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (nm && x->n_nodes < 512)
        {
            x->nodes[x->n_nodes].id = id;
            snprintf(x->nodes[x->n_nodes].name, sizeof x->nodes[x->n_nodes].name, "%s", nm);
            snprintf(x->nodes[x->n_nodes].mclass, sizeof x->nodes[x->n_nodes].mclass, "%s",
                     mc ? mc : "");
            x->n_nodes++;
        }
    }
    else if (!strcmp(type, PW_TYPE_INTERFACE_Port))
    {
        const char *nid = spa_dict_lookup(props, PW_KEY_NODE_ID);
        const char *dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        if (nid && dir)
        {
            uint32_t n = (uint32_t)strtoul(nid, NULL, 10);
            for (int i = 0; i < x->n_nodes; i++)
                if (x->nodes[i].id == n)
                {
                    if (!strcmp(dir, "in"))
                        x->nodes[i].n_in++;
                    else
                        x->nodes[i].n_out++;
                }
        }
    }
    else if (!strcmp(type, PW_TYPE_INTERFACE_Metadata))
    {
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name && !strcmp(name, "default") && !x->meta)
        {
            x->meta = pw_registry_bind(x->registry, id, PW_TYPE_INTERFACE_Metadata, ver, 0);
            if (x->meta)
                pw_metadata_add_listener(x->meta, &x->meta_listener, &meta_events, x);
        }
    }
}
static const struct pw_registry_events reg_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_global,
};

static void
on_done(void *userdata, uint32_t id, int seq)
{
    struct data *x = userdata;
    if (id == PW_ID_CORE && seq == x->pending)
        pw_main_loop_quit(x->loop);
}
static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_done,
};

/* Returns 1 if the named node is in the registry with >= 1 port in the wanted
 * direction ("in" for a sink, "out" for a source); 0 otherwise. */
static int
node_has_port(const struct data *x, const char *name, int want_in)
{
    for (int i = 0; i < x->n_nodes; i++)
        if (!strcmp(x->nodes[i].name, name))
        {
            printf("  %s '%s' (id=%u class='%s' in=%d out=%d)\n", want_in ? "sink" : "source", name,
                   x->nodes[i].id, x->nodes[i].mclass, x->nodes[i].n_in, x->nodes[i].n_out);
            return want_in ? x->nodes[i].n_in > 0 : x->nodes[i].n_out > 0;
        }
    printf("  node '%s' NOT found in registry\n", name);
    return 0;
}

int
main(int argc, char **argv)
{
    pw_init(&argc, &argv);

    struct data x = { 0 };
    x.loop        = pw_main_loop_new(NULL);
    if (!x.loop)
    {
        fprintf(stderr, "pw_main_loop_new failed\n");
        return 1;
    }
    x.context = pw_context_new(pw_main_loop_get_loop(x.loop), NULL, 0);
    if (!x.context)
    {
        fprintf(stderr, "pw_context_new failed\n");
        return 1;
    }
    x.core = pw_context_connect(x.context, NULL, 0);
    if (!x.core)
    {
        printf("SKIP: no PipeWire daemon reachable\n");
        return 77;
    }
    pw_core_add_listener(x.core, &x.core_listener, &core_events, &x);
    x.registry = pw_core_get_registry(x.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(x.registry, &x.registry_listener, &reg_events, &x);

    /* Two syncs, exactly like audio_open(): registry globals settle on the
     * first, the bound metadata's initial property burst on the second. */
    x.pending = pw_core_sync(x.core, PW_ID_CORE, 0);
    pw_main_loop_run(x.loop);
    x.pending = pw_core_sync(x.core, PW_ID_CORE, x.pending);
    pw_main_loop_run(x.loop);

    printf("default.audio.sink   = '%s'\n", x.sink);
    printf("default.audio.source = '%s'\n", x.source);

    if (!x.sink[0] && !x.source[0])
    {
        printf("SKIP: session reports no default sink or source\n");
        return 77;
    }

    int ok = 1;
    if (x.sink[0] && !node_has_port(&x, x.sink, 1))
        ok = 0;
    if (x.source[0] && !node_has_port(&x, x.source, 0))
        ok = 0;

    printf("%s\n", ok ? "PASS: default device(s) resolve to nodes with matching ports"
                      : "FAIL: a reported default did not resolve to usable ports");
    return ok ? 0 : 1;
}
