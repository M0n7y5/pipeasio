/*
 * pipeasio_config.h - flat-INI settings contract shared by the native driver
 * (src/config.c, src/asio.c) and the Qt settings panel (gui/).
 *
 * The driver reads $XDG_CONFIG_HOME/pipeasio/config.ini directly (it is a
 * native ELF, so it does not need the Windows registry); the panel writes the
 * same file. Keeping the key names and defaults in one header is what keeps the
 * two sides from drifting.
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
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* --- Product version (driver log + settings-panel title) ------------------ */
#define PIPEASIO_VERSION "1.1.0"

/* --- File location (relative to $XDG_CONFIG_HOME, else $HOME/.config) ----- */
#define PIPEASIO_CONFIG_DIR "pipeasio"
#define PIPEASIO_CONFIG_FILE "config.ini"
#define PIPEASIO_CONFIG_SECTION "pipeasio"

/* --- INI keys ------------------------------------------------------------- */
#define PIPEASIO_KEY_INPUTS "inputs"
#define PIPEASIO_KEY_OUTPUTS "outputs"
#define PIPEASIO_KEY_BUFFER_SIZE "buffer_size"
#define PIPEASIO_KEY_FIXED_BUFFER_SIZE "fixed_buffer_size"
#define PIPEASIO_KEY_SAMPLE_RATE "sample_rate"
#define PIPEASIO_KEY_AUTO_CONNECT "auto_connect"
#define PIPEASIO_KEY_OUTPUT_DEVICE "output_device"
#define PIPEASIO_KEY_INPUT_DEVICE "input_device"
#define PIPEASIO_KEY_NODE_NAME "node_name"
#define PIPEASIO_KEY_FOLLOW_DEVICE_CLOCK "follow_device_clock"

/* --- Defaults ------------------------------------------------------------- */
#define PIPEASIO_DEFAULT_INPUTS 2
#define PIPEASIO_DEFAULT_OUTPUTS 2
#define PIPEASIO_DEFAULT_BUFFER_SIZE 1024
#define PIPEASIO_DEFAULT_FIXED_BUFFER_SIZE true
#define PIPEASIO_DEFAULT_SAMPLE_RATE 0 /* 0 = follow the PipeWire graph */
#define PIPEASIO_DEFAULT_AUTO_CONNECT true
#define PIPEASIO_DEFAULT_FOLLOW_DEVICE_CLOCK false

/* --- Bounds (mirror src/asio.c's PIPEASIO_{MINIMUM,MAXIMUM}_BUFFERSIZE) --- */
#define PIPEASIO_MIN_BUFFER_SIZE 16
#define PIPEASIO_MAX_BUFFER_SIZE 8192

/* Upper bound on channel counts (matches the GUI spinbox range); guards the
 * IOChannel and callback-buffer sizing against absurd config/env values. */
#define PIPEASIO_MAX_CHANNELS 256

#define PIPEASIO_DEVICE_NAME_MAX 256
#define PIPEASIO_NODE_NAME_MAX 256

/* --- Parsed configuration ------------------------------------------------- */
struct pipeasio_config
{
    int  inputs;
    int  outputs;
    int  buffer_size;                             /* preferred size, power-of-two */
    bool fixed_buffer_size;                       /* lock the host to buffer_size  */
    int  sample_rate;                             /* 0 = follow graph, else FORCE_RATE */
    bool auto_connect;                            /* 0 = manual patching           */
    bool follow_device_clock;                     /* follow target device quantum (BT) */
    char output_device[PIPEASIO_DEVICE_NAME_MAX]; /* node.name; "" = default sink   */
    char input_device[PIPEASIO_DEVICE_NAME_MAX];  /* node.name; "" = default source */
    char node_name[PIPEASIO_NODE_NAME_MAX];       /* "" = derive from app name      */
};

#ifdef __cplusplus
extern "C"
{
#endif

    /*
 * Populate `out` with defaults, then overlay any values found in the INI at
 * $XDG_CONFIG_HOME/pipeasio/config.ini (fallback $HOME/.config/pipeasio/...).
 * Returns true if a config file was found and parsed, false if none existed
 * (in which case `out` is still fully populated with defaults). Malformed
 * lines and unknown keys are skipped; out-of-range numeric values fall back to
 * their default.
 */
    bool pipeasio_config_load(struct pipeasio_config *out);

    /*
 * Resolve the absolute config-file path into `buf` (capacity `n`). Returns
 * false if neither XDG_CONFIG_HOME nor HOME is set. Creates nothing.
 */
    bool pipeasio_config_path(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
