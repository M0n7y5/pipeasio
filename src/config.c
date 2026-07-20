/*
 * config.c - flat-INI reader for the PipeASIO driver.
 *
 * The driver is a native ELF, so it reads its settings straight from
 * $XDG_CONFIG_HOME/pipeasio/config.ini (the file the Qt panel writes) rather
 * than the Windows registry. The format is deliberately trivial: an optional
 * "[pipeasio]" section header, "key = value" lines, "#"/";" comments.
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
#include "pipeasio_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h> /* _mkdir */
#include <io.h>      /* _unlink */
#define cfg_mkdir(d) (_mkdir(d))
#define cfg_unlink(f) (_unlink(f))
#else
#define cfg_mkdir(d) (mkdir(d, 0755))
#define cfg_unlink(f) (unlink(f))
#endif

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
    c->rt_priority         = PIPEASIO_DEFAULT_RT_PRIORITY;
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
    else if (!strcmp(key, PIPEASIO_KEY_RT_PRIORITY))
        c->rt_priority = atoi(val);
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
    if (c->inputs < 0 || c->inputs > PIPEASIO_MAX_CHANNELS)
        c->inputs = PIPEASIO_DEFAULT_INPUTS;
    if (c->outputs < 0 || c->outputs > PIPEASIO_MAX_CHANNELS)
        c->outputs = PIPEASIO_DEFAULT_OUTPUTS;

    if (!pipeasio_buffer_size_valid(c->buffer_size))
        c->buffer_size = PIPEASIO_DEFAULT_BUFFER_SIZE;

    if (c->sample_rate < 0)
        c->sample_rate = 0;

    if (c->rt_priority < PIPEASIO_MIN_RT_PRIORITY || c->rt_priority > PIPEASIO_MAX_RT_PRIORITY)
        c->rt_priority = PIPEASIO_DEFAULT_RT_PRIORITY;
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
    /* Section handling is mirrored by the settings panel (gui/Config.cpp);
     * keep both in sync when changing it.  Intentional differences are
     * documented there. */
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


/* The single writer for the panel's INI format; the Qt panel calls this too,
 * so the on-disk layout has exactly one implementation. */
bool
pipeasio_config_save(const struct pipeasio_config *c)
{
    char path[1024];
    if (!pipeasio_config_path(path, sizeof path))
        return false;

    /* Ensure the config directory exists (one level under the config home). */
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
    {
        *slash = '\0';
        if (cfg_mkdir(dir) != 0 && errno != EEXIST)
            return false;
    }

    /* tmp + rename: the driver's config watcher polls every second and must
     * never read a half-written file. */
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f)
        return false;

    fprintf(f, "# PipeASIO settings - written by pipeasio-settings\n");
    fprintf(f, "[" PIPEASIO_CONFIG_SECTION "]\n");
    fprintf(f, PIPEASIO_KEY_INPUTS " = %d\n", c->inputs);
    fprintf(f, PIPEASIO_KEY_OUTPUTS " = %d\n", c->outputs);
    fprintf(f, PIPEASIO_KEY_BUFFER_SIZE " = %d\n", c->buffer_size);
    fprintf(f, PIPEASIO_KEY_FIXED_BUFFER_SIZE " = %d\n", c->fixed_buffer_size ? 1 : 0);
    fprintf(f, PIPEASIO_KEY_SAMPLE_RATE " = %d\n", c->sample_rate);
    fprintf(f, PIPEASIO_KEY_AUTO_CONNECT " = %d\n", c->auto_connect ? 1 : 0);
    fprintf(f, PIPEASIO_KEY_FOLLOW_DEVICE_CLOCK " = %d\n", c->follow_device_clock ? 1 : 0);
    fprintf(f, PIPEASIO_KEY_RT_PRIORITY " = %d\n", c->rt_priority);
    fprintf(f, PIPEASIO_KEY_OUTPUT_DEVICE " = %s\n", c->output_device);
    fprintf(f, PIPEASIO_KEY_INPUT_DEVICE " = %s\n", c->input_device);
    fprintf(f, PIPEASIO_KEY_NODE_NAME " = %s\n", c->node_name);

    bool ok = fclose(f) == 0 && rename(tmp, path) == 0;
    if (!ok)
        cfg_unlink(tmp);
    return ok;
}
