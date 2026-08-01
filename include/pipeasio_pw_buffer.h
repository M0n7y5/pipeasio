/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <pipewire/pipewire.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

static inline bool
pipeasio_pw_validate_region(const struct pw_buffer *buffer, uint32_t frames, void **data_out,
                            struct spa_chunk **chunk_out)
{
    struct spa_data  *data;
    struct spa_chunk *chunk;
    size_t            bytes;
    if (data_out)
        *data_out = NULL;
    if (chunk_out)
        *chunk_out = NULL;
    if (!buffer || !buffer->buffer || buffer->buffer->n_datas < 1
        || frames > UINT32_MAX / sizeof(float))
        return false;
    data  = &buffer->buffer->datas[0];
    chunk = data->chunk;
    if (chunk_out)
        *chunk_out = chunk;
    bytes = (size_t)frames * sizeof(float);
    if (!chunk || !data->data || data->maxsize % sizeof(float) || chunk->offset > data->maxsize
        || chunk->size > data->maxsize - chunk->offset || chunk->size < bytes
        || data->maxsize - chunk->offset < bytes
        || (chunk->stride && chunk->stride != (int32_t)sizeof(float))
        || ((uintptr_t)data->data + chunk->offset) % _Alignof(float))
        return false;
    if (data_out)
        *data_out = (uint8_t *)data->data + chunk->offset;
    return true;
}

static inline bool
pipeasio_pw_publish_output(struct pw_buffer *buffer, const float *source, uint32_t frames,
                           bool admitted, bool active)
{
    struct spa_data  *data;
    struct spa_chunk *chunk = NULL;
    size_t            bytes;
    if (buffer && buffer->buffer && buffer->buffer->n_datas >= 1)
        chunk = buffer->buffer->datas[0].chunk;
    if (!buffer || !buffer->buffer || buffer->buffer->n_datas < 1
        || frames > UINT32_MAX / sizeof(float))
        goto malformed;
    data  = &buffer->buffer->datas[0];
    bytes = (size_t)frames * sizeof(float);
    if (!chunk || !data->data || data->maxsize % sizeof(float) || data->maxsize < bytes
        || (uintptr_t)data->data % _Alignof(float))
        goto malformed;
    chunk->offset = 0;
    chunk->stride = (int32_t)sizeof(float);
    chunk->flags  = 0;
    chunk->size   = (uint32_t)bytes;
    if (admitted && active && source && (uintptr_t)source % _Alignof(float) == 0)
        memcpy(data->data, source, bytes);
    else
        memset(data->data, 0, bytes);
    return true;
malformed:
    if (chunk)
        chunk->size = 0;
    return false;
}

typedef int (*pipeasio_pw_queue_fn)(void *context, struct pw_buffer *buffer);

static inline bool
pipeasio_pw_finish_output(_Atomic(struct pw_buffer *) *cycle_slot, const float *source,
                          uint32_t frames, bool admitted, bool active,
                          pipeasio_pw_queue_fn queue_fn, void *context)
{
    struct pw_buffer *buffer = atomic_exchange_explicit(cycle_slot, NULL, memory_order_acq_rel);
    bool              valid;
    int               queued;
    if (!buffer)
        return false;
    valid  = pipeasio_pw_publish_output(buffer, source, frames, admitted, active);
    queued = queue_fn ? queue_fn(context, buffer) : 0;
    return valid && queued >= 0;
}
