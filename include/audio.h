/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.  See COPYING.LIB for the full license text.
 */

/*
 * audio.h — backend-agnostic audio API surface for PipeASIO.
 *
 * Implemented natively on libpipewire-0.3 in src/audio.c: a single
 * pw_thread_loop drives a pw_filter, with custom spa_thread_utils
 * bridging the PipeWire RT thread to Wine via CreateThread.  asio.c
 * sees only this header — PipeWire terminology is encapsulated below
 * this line.
 */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* --- Opaque handle types ------------------------------------------------ */

typedef struct audio_client audio_client_t;
typedef struct audio_port   audio_port_t;

/* --- Concrete value types ----------------------------------------------- */

typedef uint32_t audio_nframes_t;
typedef float    audio_sample_t;

typedef struct
{
    audio_nframes_t min;
    audio_nframes_t max;
} audio_latency_range_t;

typedef struct
{
    audio_nframes_t frame;      /* current playback frame */
    uint64_t        usecs;      /* wall-clock microseconds */
    audio_nframes_t frame_rate; /* sample rate */
} audio_position_t;

typedef enum
{
    AUDIO_CAPTURE_LATENCY  = 0,
    AUDIO_PLAYBACK_LATENCY = 1,
} audio_latency_mode_t;

typedef enum
{
    AUDIO_TRANSPORT_STOPPED = 0,
    AUDIO_TRANSPORT_ROLLING = 1,
} audio_transport_state_t;

/* --- Callback signatures ------------------------------------------------ */

typedef int (*audio_process_cb)(audio_nframes_t nframes, void *arg);
typedef int (*audio_buffer_size_cb)(audio_nframes_t nframes, void *arg);
typedef int (*audio_sample_rate_cb)(audio_nframes_t nframes, void *arg);
typedef void (*audio_latency_cb)(audio_latency_mode_t mode, void *arg);

/* --- Constants ---------------------------------------------------------- */

#define AUDIO_DEFAULT_TYPE "32 bit float mono audio"

/* audio_open option flags */
#define AUDIO_NULL_OPTION 0x00u
#define AUDIO_NO_START_SERVER 0x01u

/* audio_port_register / audio_get_ports flag bits */
#define AUDIO_PORT_IS_INPUT 0x01u
#define AUDIO_PORT_IS_OUTPUT 0x02u
#define AUDIO_PORT_IS_PHYSICAL 0x04u

/* --- Lifecycle ---------------------------------------------------------- */

audio_client_t *audio_open(const char *client_name, uint32_t options, uint32_t *status);
bool            audio_close(audio_client_t *client);
bool            audio_activate(audio_client_t *client);
bool            audio_deactivate(audio_client_t *client);
const char     *audio_get_client_name(audio_client_t *client);

/* --- Properties --------------------------------------------------------- */

audio_nframes_t audio_get_sample_rate(audio_client_t *client);
audio_nframes_t audio_get_buffer_size(audio_client_t *client);
bool            audio_set_buffer_size(audio_client_t *client, audio_nframes_t nframes);

/* Pin the graph sample rate (FORCE_RATE) on the next audio_activate.
 * rate == 0 means follow the PipeWire graph (no FORCE_RATE). */
void audio_set_forced_rate(audio_client_t *client, audio_nframes_t rate);

/* When set, the next audio_activate does NOT pin the graph quantum
 * (PW_KEY_NODE_FORCE_QUANTUM): the filter becomes a follower so the target
 * device (e.g. a Bluetooth sink whose clock cannot be slaved) drives the cycle.
 * Default off — wired devices keep the forced low-latency quantum. */
void audio_set_follow_device(audio_client_t *client, bool follow);

/* Most recent graph quantum (clock.duration) the process callback observed
 * while following a device, or 0 if none seen yet. Lets the ASIO side settle
 * its buffer size to the device-dictated quantum. */
audio_nframes_t audio_observed_quantum(audio_client_t *client);

/* Nanosecond timestamp (PipeWire graph clock) of the most recent process cycle,
 * 0 before the first cycle. Feeds the ASIO host's systemTime. */
uint64_t audio_get_time_nsec(audio_client_t *client);

/* --- Ports -------------------------------------------------------------- */

audio_port_t *audio_port_register(audio_client_t *client, const char *port_name,
                                  const char *port_type, uint64_t flags, uint64_t buffer_size);
bool          audio_port_unregister(audio_client_t *client, audio_port_t *port);
void         *audio_port_get_buffer(audio_port_t *port, audio_nframes_t nframes);
const char   *audio_port_name(const audio_port_t *port);
const char   *audio_port_type(const audio_port_t *port);
audio_port_t *audio_port_by_name(audio_client_t *client, const char *port_name);
/* The returned NULL-terminated array and its name strings are duplicated
 * out of the discovered cache; free both with audio_free_ports (not audio_free). */
const char **audio_get_ports(audio_client_t *client, const char *port_name_pattern,
                             const char *type_name_pattern, uint64_t flags);
/* Like audio_get_ports, but restricted to the device whose PipeWire
 * node.name == node_name.  node_name NULL/"" falls back to the first
 * available device (same as audio_get_ports). Free with audio_free_ports. */
const char **audio_get_device_ports(audio_client_t *client, const char *node_name, uint64_t flags);
/* Returns and clears the "PipeWire default sink/source changed" flag (set when
 * the "default" metadata switches to a different node after the initial fill).
 * Lets the ASIO side trigger a reconnect when the user follows the default. */
bool audio_default_changed(audio_client_t *client);
void audio_port_get_latency_range(audio_port_t *port, uint32_t mode, audio_latency_range_t *range);

/* --- Callbacks ---------------------------------------------------------- */

bool audio_set_process_callback(audio_client_t *client, audio_process_cb cb, void *arg);
bool audio_set_buffer_size_callback(audio_client_t *client, audio_buffer_size_cb cb, void *arg);
bool audio_set_sample_rate_callback(audio_client_t *client, audio_sample_rate_cb cb, void *arg);
bool audio_set_latency_callback(audio_client_t *client, audio_latency_cb cb, void *arg);

/* --- Connections / transport / memory ----------------------------------- */

bool     audio_connect(audio_client_t *client, const char *src, const char *dst);
uint32_t audio_transport_query(const audio_client_t *client, audio_position_t *pos);
void     audio_free(void *ptr);
/* Frees an array returned by audio_get_ports / audio_get_device_ports,
 * including the duplicated name strings. */
void audio_free_ports(const char **ports);
