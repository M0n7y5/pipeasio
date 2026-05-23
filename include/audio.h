/*
 * audio.h — backend-agnostic audio API surface for WineASIO.
 *
 * Phase 1: opaque types and flag macros are aliases over the existing
 * jackbridge.h declarations, and src/audio.c forwards every call into
 * the libjack-backed jackbridge implementation.  asio.c sees only this
 * header — JACK terminology is encapsulated below this line.
 *
 * Phase 3 will replace the typedefs with native libpipewire-backed
 * structs and rewrite src/audio.c on top of pw_thread_loop + pw_filter,
 * without touching asio.c.  Phase 4 deletes jackbridge entirely.
 */
#pragma once

#include "jackbridge.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* --- Opaque types (transitional aliases) -------------------------------- */

typedef jack_client_t                audio_client_t;
typedef jack_port_t                  audio_port_t;
typedef jack_position_t              audio_position_t;
typedef jack_latency_range_t         audio_latency_range_t;
typedef jack_nframes_t               audio_nframes_t;
typedef jack_default_audio_sample_t  audio_sample_t;
typedef jack_latency_callback_mode_t audio_latency_mode_t;
typedef jack_transport_state_t       audio_transport_state_t;

/* --- Callback signatures ------------------------------------------------ */

typedef int  (*audio_process_cb)    (audio_nframes_t nframes, void *arg);
typedef int  (*audio_buffer_size_cb)(audio_nframes_t nframes, void *arg);
typedef int  (*audio_sample_rate_cb)(audio_nframes_t nframes, void *arg);
typedef void (*audio_latency_cb)    (audio_latency_mode_t mode, void *arg);
typedef int  (*audio_thread_creator)(pthread_t *thread, const pthread_attr_t *attr,
                                     void *(*start)(void *), void *arg);

/* --- Constants (transitional macros) ------------------------------------ */

#define AUDIO_DEFAULT_TYPE       JACK_DEFAULT_AUDIO_TYPE
#define AUDIO_PORT_IS_INPUT      JackPortIsInput
#define AUDIO_PORT_IS_OUTPUT     JackPortIsOutput
#define AUDIO_PORT_IS_PHYSICAL   JackPortIsPhysical
#define AUDIO_CAPTURE_LATENCY    JackCaptureLatency
#define AUDIO_PLAYBACK_LATENCY   JackPlaybackLatency
#define AUDIO_NULL_OPTION        JackNullOption
#define AUDIO_NO_START_SERVER    JackNoStartServer
#define AUDIO_TRANSPORT_ROLLING  JackTransportRolling

/* --- Lifecycle ---------------------------------------------------------- */

audio_client_t *audio_open (const char *client_name, uint32_t options, uint32_t *status);
bool            audio_close(audio_client_t *client);
bool            audio_activate  (audio_client_t *client);
bool            audio_deactivate(audio_client_t *client);
const char     *audio_get_client_name(audio_client_t *client);

/* --- Properties --------------------------------------------------------- */

audio_nframes_t audio_get_sample_rate(audio_client_t *client);
audio_nframes_t audio_get_buffer_size(audio_client_t *client);
bool            audio_set_buffer_size(audio_client_t *client, audio_nframes_t nframes);

/* --- Ports -------------------------------------------------------------- */

audio_port_t *audio_port_register  (audio_client_t *client,
                                    const char *port_name, const char *port_type,
                                    uint64_t flags, uint64_t buffer_size);
bool          audio_port_unregister(audio_client_t *client, audio_port_t *port);
void         *audio_port_get_buffer(audio_port_t *port, audio_nframes_t nframes);
const char   *audio_port_name      (const audio_port_t *port);
const char   *audio_port_type      (const audio_port_t *port);
audio_port_t *audio_port_by_name   (audio_client_t *client, const char *port_name);
const char  **audio_get_ports      (audio_client_t *client,
                                    const char *port_name_pattern,
                                    const char *type_name_pattern,
                                    uint64_t flags);
void          audio_port_get_latency_range(audio_port_t *port, uint32_t mode,
                                           audio_latency_range_t *range);

/* --- Callbacks ---------------------------------------------------------- */

bool audio_set_process_callback    (audio_client_t *client, audio_process_cb cb,     void *arg);
bool audio_set_buffer_size_callback(audio_client_t *client, audio_buffer_size_cb cb, void *arg);
bool audio_set_sample_rate_callback(audio_client_t *client, audio_sample_rate_cb cb, void *arg);
bool audio_set_latency_callback    (audio_client_t *client, audio_latency_cb cb,     void *arg);
void audio_set_thread_creator      (audio_thread_creator creator);

/* --- Connections / transport / memory ----------------------------------- */

bool     audio_connect        (audio_client_t *client, const char *src, const char *dst);
uint32_t audio_transport_query(const audio_client_t *client, audio_position_t *pos);
void     audio_free           (void *ptr);
