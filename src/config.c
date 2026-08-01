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
#include "pipeasio_parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool
pipeasio_config_path(char *buf, size_t n)
{
    const char *base   = getenv("XDG_CONFIG_HOME");
    const char *suffix = "";
    int         length;
    if (!buf || !n)
        return false;
    if (!base || !base[0])
    {
        base   = getenv("HOME");
        suffix = "/.config";
    }
    if (!base || !base[0])
        return false;
    length = snprintf(buf, n, "%s%s/%s/%s", base, suffix, PIPEASIO_CONFIG_DIR,
                      PIPEASIO_CONFIG_FILE);
    if (length < 0 || (size_t)length >= n)
    {
        buf[0] = '\0';
        return false;
    }
    return true;
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

void
pipeasio_config_defaults(struct pipeasio_config *c)
{
    if (!c)
        return;
    c->inputs              = PIPEASIO_DEFAULT_INPUTS;
    c->outputs             = PIPEASIO_DEFAULT_OUTPUTS;
    c->buffer_size         = PIPEASIO_DEFAULT_BUFFER_SIZE;
    c->fixed_buffer_size   = PIPEASIO_DEFAULT_FIXED_BUFFER_SIZE;
    c->sample_rate         = PIPEASIO_DEFAULT_SAMPLE_RATE;
    c->auto_connect        = PIPEASIO_DEFAULT_AUTO_CONNECT;
    c->follow_device_clock = PIPEASIO_DEFAULT_FOLLOW_DEVICE_CLOCK;
    c->realtime            = PIPEASIO_DEFAULT_REALTIME;
    c->output_device[0]    = '\0';
    c->input_device[0]     = '\0';
    c->node_name[0]        = '\0';
}

static bool
copy_str(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source);
    if (!capacity || length >= capacity)
        return false;
    memcpy(destination, source, length + 1);
    return true;
}

static void
apply_kv(struct pipeasio_config *config, const char *key, const char *value)
{
    int  parsed;
    bool flag;
    if (!strcmp(key, PIPEASIO_KEY_INPUTS))
    {
        if (pipeasio_parse_int(value, 0, PIPEASIO_MAX_CHANNELS, &parsed))
            config->inputs = parsed;
    }
    else if (!strcmp(key, PIPEASIO_KEY_OUTPUTS))
    {
        if (pipeasio_parse_int(value, 0, PIPEASIO_MAX_CHANNELS, &parsed))
            config->outputs = parsed;
    }
    else if (!strcmp(key, PIPEASIO_KEY_BUFFER_SIZE))
    {
        if (pipeasio_parse_int(value, PIPEASIO_MIN_BUFFER_SIZE, PIPEASIO_MAX_BUFFER_SIZE, &parsed)
            && !(parsed & (parsed - 1)))
            config->buffer_size = parsed;
    }
    else if (!strcmp(key, PIPEASIO_KEY_FIXED_BUFFER_SIZE))
    {
        if (pipeasio_parse_bool(value, &flag))
            config->fixed_buffer_size = flag;
    }
    else if (!strcmp(key, PIPEASIO_KEY_SAMPLE_RATE))
    {
        if (pipeasio_parse_int(value, 0, INT_MAX, &parsed))
            config->sample_rate = parsed;
    }
    else if (!strcmp(key, PIPEASIO_KEY_AUTO_CONNECT))
    {
        if (pipeasio_parse_bool(value, &flag))
            config->auto_connect = flag;
    }
    else if (!strcmp(key, PIPEASIO_KEY_FOLLOW_DEVICE_CLOCK))
    {
        if (pipeasio_parse_bool(value, &flag))
            config->follow_device_clock = flag;
    }
    else if (!strcmp(key, PIPEASIO_KEY_REALTIME))
    {
        if (pipeasio_parse_bool(value, &flag))
            config->realtime = flag;
    }
    else if (!strcmp(key, PIPEASIO_KEY_OUTPUT_DEVICE))
        copy_str(config->output_device, sizeof(config->output_device), value);
    else if (!strcmp(key, PIPEASIO_KEY_INPUT_DEVICE))
        copy_str(config->input_device, sizeof(config->input_device), value);
    else if (!strcmp(key, PIPEASIO_KEY_NODE_NAME))
        copy_str(config->node_name, sizeof(config->node_name), value);
}

bool
pipeasio_config_load(struct pipeasio_config *out)
{
    char path[PIPEASIO_CONFIG_LINE_MAX];
    char line[PIPEASIO_CONFIG_LINE_MAX];
    bool in_section = true;
    if (!out)
        return false;
    pipeasio_config_defaults(out);
    if (!pipeasio_config_path(path, sizeof(path)))
        return false;
    FILE *file = fopen(path, "r");
    if (!file)
        return false;
    while (fgets(line, sizeof(line), file))
    {
        size_t length  = strlen(line);
        bool   newline = length && line[length - 1] == '\n';
        if (!newline && length == sizeof(line) - 1)
        {
            int ch;
            while ((ch = fgetc(file)) != '\n' && ch != EOF)
                ;
            continue;
        }
        char *text = trim(line);
        if (!*text || *text == '#' || *text == ';')
            continue;
        if (*text == '[')
        {
            char *close = strchr(text, ']');
            if (close)
            {
                *close     = '\0';
                in_section = !strcmp(text + 1, PIPEASIO_CONFIG_SECTION);
            }
            continue;
        }
        if (!in_section)
            continue;
        char *equals = strchr(text, '=');
        if (!equals)
            continue;
        *equals = '\0';
        apply_kv(out, trim(text), trim(equals + 1));
    }
    fclose(file);
    return true;
}
