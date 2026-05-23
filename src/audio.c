/*
 * audio.c — Phase 1 adapter forwarding the backend-agnostic audio_* API to
 * the legacy libjack-backed jackbridge implementation.
 *
 * Each function is a thin pass-through.  At -O2 the compiler inlines these
 * into the call sites in src/asio.c; in debug builds the frames remain
 * visible so step-into works.
 *
 * Phase 3 replaces this file's body with a native libpipewire-0.3
 * implementation (pw_thread_loop + pw_filter), and Phase 4 deletes
 * jackbridge.{c,h}.  asio.c is unaffected by either step.
 */

#include "audio.h"

/* --- Lifecycle ---------------------------------------------------------- */

audio_client_t *audio_open(const char *client_name, uint32_t options, uint32_t *status)
{
    return jackbridge_client_open(client_name, options, (jack_status_t *)status);
}

bool audio_close(audio_client_t *client)
{
    return jackbridge_client_close(client);
}

bool audio_activate(audio_client_t *client)
{
    return jackbridge_activate(client);
}

bool audio_deactivate(audio_client_t *client)
{
    return jackbridge_deactivate(client);
}

const char *audio_get_client_name(audio_client_t *client)
{
    return jackbridge_get_client_name(client);
}

/* --- Properties --------------------------------------------------------- */

audio_nframes_t audio_get_sample_rate(audio_client_t *client)
{
    return jackbridge_get_sample_rate(client);
}

audio_nframes_t audio_get_buffer_size(audio_client_t *client)
{
    return jackbridge_get_buffer_size(client);
}

bool audio_set_buffer_size(audio_client_t *client, audio_nframes_t nframes)
{
    return jackbridge_set_buffer_size(client, nframes);
}

/* --- Ports -------------------------------------------------------------- */

audio_port_t *audio_port_register(audio_client_t *client,
                                  const char *port_name, const char *port_type,
                                  uint64_t flags, uint64_t buffer_size)
{
    return jackbridge_port_register(client, port_name, port_type, flags, buffer_size);
}

bool audio_port_unregister(audio_client_t *client, audio_port_t *port)
{
    return jackbridge_port_unregister(client, port);
}

void *audio_port_get_buffer(audio_port_t *port, audio_nframes_t nframes)
{
    return jackbridge_port_get_buffer(port, nframes);
}

const char *audio_port_name(const audio_port_t *port)
{
    return jackbridge_port_name(port);
}

const char *audio_port_type(const audio_port_t *port)
{
    return jackbridge_port_type(port);
}

audio_port_t *audio_port_by_name(audio_client_t *client, const char *port_name)
{
    return jackbridge_port_by_name(client, port_name);
}

const char **audio_get_ports(audio_client_t *client,
                             const char *port_name_pattern,
                             const char *type_name_pattern,
                             uint64_t flags)
{
    return jackbridge_get_ports(client, port_name_pattern, type_name_pattern, flags);
}

void audio_port_get_latency_range(audio_port_t *port, uint32_t mode,
                                  audio_latency_range_t *range)
{
    jackbridge_port_get_latency_range(port, mode, range);
}

/* --- Callbacks ---------------------------------------------------------- */

bool audio_set_process_callback(audio_client_t *client, audio_process_cb cb, void *arg)
{
    return jackbridge_set_process_callback(client, (JackProcessCallback)cb, arg);
}

bool audio_set_buffer_size_callback(audio_client_t *client, audio_buffer_size_cb cb, void *arg)
{
    return jackbridge_set_buffer_size_callback(client, (JackBufferSizeCallback)cb, arg);
}

bool audio_set_sample_rate_callback(audio_client_t *client, audio_sample_rate_cb cb, void *arg)
{
    return jackbridge_set_sample_rate_callback(client, (JackSampleRateCallback)cb, arg);
}

bool audio_set_latency_callback(audio_client_t *client, audio_latency_cb cb, void *arg)
{
    return jackbridge_set_latency_callback(client, (JackLatencyCallback)cb, arg);
}

void audio_set_thread_creator(audio_thread_creator creator)
{
    jackbridge_set_thread_creator((JackThreadCreator)creator);
}

/* --- Connections / transport / memory ----------------------------------- */

bool audio_connect(audio_client_t *client, const char *src, const char *dst)
{
    return jackbridge_connect(client, src, dst);
}

uint32_t audio_transport_query(const audio_client_t *client, audio_position_t *pos)
{
    return jackbridge_transport_query(client, pos);
}

void audio_free(void *ptr)
{
    jackbridge_free(ptr);
}
