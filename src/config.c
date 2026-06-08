/*
 * config.c — flat-INI reader for the PipeASIO driver.
 *
 * The driver is a native ELF, so it reads its settings straight from
 * $XDG_CONFIG_HOME/pipeasio/config.ini (the file the Qt panel writes) rather
 * than the Windows registry. The format is deliberately trivial: an optional
 * "[pipeasio]" section header, "key = value" lines, "#"/";" comments.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 */
#include "pipeasio_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

bool
pipeasio_config_path(char *buf, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
    {
        snprintf(buf, n, "%s/%s/%s", xdg, PIPEASIO_CONFIG_DIR, PIPEASIO_CONFIG_FILE);
        return true;
    }
    const char *home = getenv("HOME");
    if (home && home[0])
    {
        snprintf(buf, n, "%s/.config/%s/%s", home, PIPEASIO_CONFIG_DIR, PIPEASIO_CONFIG_FILE);
        return true;
    }
    return false;
}

static char *
trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

static bool
parse_bool(const char *v)
{
    return v[0] == '1' || !strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcasecmp(v, "yes");
}

static void
copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0)
        return;
    size_t i = 0;
    for (; src[i] && i < cap - 1; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void
set_defaults(struct pipeasio_config *c)
{
    c->inputs              = PIPEASIO_DEFAULT_INPUTS;
    c->outputs             = PIPEASIO_DEFAULT_OUTPUTS;
    c->buffer_size         = PIPEASIO_DEFAULT_BUFFER_SIZE;
    c->fixed_buffer_size   = PIPEASIO_DEFAULT_FIXED_BUFFER_SIZE;
    c->sample_rate         = PIPEASIO_DEFAULT_SAMPLE_RATE;
    c->auto_connect        = PIPEASIO_DEFAULT_AUTO_CONNECT;
    c->follow_device_clock = PIPEASIO_DEFAULT_FOLLOW_DEVICE_CLOCK;
    c->output_device[0]    = '\0';
    c->input_device[0]     = '\0';
    c->node_name[0]        = '\0';
}

static void
apply_kv(struct pipeasio_config *c, const char *key, const char *val)
{
    if (!strcmp(key, PIPEASIO_KEY_INPUTS))
        c->inputs = atoi(val);
    else if (!strcmp(key, PIPEASIO_KEY_OUTPUTS))
        c->outputs = atoi(val);
    else if (!strcmp(key, PIPEASIO_KEY_BUFFER_SIZE))
        c->buffer_size = atoi(val);
    else if (!strcmp(key, PIPEASIO_KEY_FIXED_BUFFER_SIZE))
        c->fixed_buffer_size = parse_bool(val);
    else if (!strcmp(key, PIPEASIO_KEY_SAMPLE_RATE))
        c->sample_rate = atoi(val);
    else if (!strcmp(key, PIPEASIO_KEY_AUTO_CONNECT))
        c->auto_connect = parse_bool(val);
    else if (!strcmp(key, PIPEASIO_KEY_FOLLOW_DEVICE_CLOCK))
        c->follow_device_clock = parse_bool(val);
    else if (!strcmp(key, PIPEASIO_KEY_OUTPUT_DEVICE))
        copy_str(c->output_device, sizeof c->output_device, val);
    else if (!strcmp(key, PIPEASIO_KEY_INPUT_DEVICE))
        copy_str(c->input_device, sizeof c->input_device, val);
    else if (!strcmp(key, PIPEASIO_KEY_NODE_NAME))
        copy_str(c->node_name, sizeof c->node_name, val);
    /* unknown keys are ignored */
}

static void
validate(struct pipeasio_config *c)
{
    if (c->inputs < 0)
        c->inputs = PIPEASIO_DEFAULT_INPUTS;
    if (c->outputs < 0)
        c->outputs = PIPEASIO_DEFAULT_OUTPUTS;

    const int b = c->buffer_size;
    if (!(b > 0 && (b & (b - 1)) == 0 && b >= PIPEASIO_MIN_BUFFER_SIZE
          && b <= PIPEASIO_MAX_BUFFER_SIZE))
        c->buffer_size = PIPEASIO_DEFAULT_BUFFER_SIZE;

    if (c->sample_rate < 0)
        c->sample_rate = 0;
}

bool
pipeasio_config_load(struct pipeasio_config *out)
{
    set_defaults(out);

    char path[1024];
    if (!pipeasio_config_path(path, sizeof path))
        return false;

    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    char line[1024];
    bool in_section = true; /* tolerate keys before any [section] header */
    while (fgets(line, sizeof line, f))
    {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        if (*s == '[')
        {
            char *close = strchr(s, ']');
            if (close)
            {
                *close     = '\0';
                in_section = (strcmp(s + 1, PIPEASIO_CONFIG_SECTION) == 0);
            }
            continue;
        }
        if (!in_section)
            continue;

        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        apply_kv(out, trim(s), trim(eq + 1));
    }

    fclose(f);
    validate(out);
    return true;
}
