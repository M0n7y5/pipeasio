/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pipeasio_handle_table.h"

#include <stdlib.h>
#include <string.h>

static pipeasio_handle
make_handle(uint32_t index, uint16_t generation)
{
    return ((uint32_t)generation << PIPEASIO_HANDLE_INDEX_BITS) | (index + 1u);
}

static bool
decode_handle(pipeasio_handle handle, uint32_t *index, uint16_t *generation)
{
    uint32_t encoded_index      = handle & PIPEASIO_HANDLE_INDEX_MASK;
    uint32_t encoded_generation = handle >> PIPEASIO_HANDLE_INDEX_BITS;
    if (!encoded_index || !encoded_generation)
        return false;
    *index      = encoded_index - 1u;
    *generation = (uint16_t)encoded_generation;
    return true;
}

bool
pipeasio_handle_table_init(pipeasio_handle_table *table)
{
    memset(table, 0, sizeof(*table));
    return pthread_mutex_init(&table->lock, NULL) == 0;
}

void
pipeasio_handle_table_destroy(pipeasio_handle_table *table)
{
    if (!table)
        return;
    free(table->entries);
    table->entries  = NULL;
    table->capacity = 0;
    table->live     = 0;
    pthread_mutex_destroy(&table->lock);
}

pipeasio_handle
pipeasio_handle_table_insert(pipeasio_handle_table *table, void *value)
{
    uint32_t        index;
    pipeasio_handle handle = 0;
    if (!table || !value)
        return 0;
    pthread_mutex_lock(&table->lock);
    for (index = 0; index < table->capacity; ++index)
        if (!table->entries[index].value
            && table->entries[index].generation < PIPEASIO_HANDLE_GENERATION_MAX)
            break;
    if (index == table->capacity)
    {
        uint32_t capacity = table->capacity ? table->capacity * 2u : 64u;
        if (capacity > PIPEASIO_HANDLE_INDEX_MASK)
            capacity = PIPEASIO_HANDLE_INDEX_MASK;
        if (capacity <= table->capacity)
            goto out;
        pipeasio_handle_entry *entries
                = realloc(table->entries, (size_t)capacity * sizeof(*entries));
        if (!entries)
            goto out;
        memset(entries + table->capacity, 0,
               (size_t)(capacity - table->capacity) * sizeof(*entries));
        table->entries  = entries;
        table->capacity = capacity;
    }
    if (!table->entries[index].generation)
        table->entries[index].generation = 1;
    table->entries[index].value = value;
    ++table->live;
    handle = make_handle(index, table->entries[index].generation);
out:
    pthread_mutex_unlock(&table->lock);
    return handle;
}

void *
pipeasio_handle_table_get(pipeasio_handle_table *table, pipeasio_handle handle)
{
    uint32_t index;
    uint16_t generation;
    void    *value = NULL;
    if (!table || !decode_handle(handle, &index, &generation))
        return NULL;
    pthread_mutex_lock(&table->lock);
    if (index < table->capacity && table->entries[index].generation == generation)
        value = table->entries[index].value;
    pthread_mutex_unlock(&table->lock);
    return value;
}

void *
pipeasio_handle_table_remove(pipeasio_handle_table *table, pipeasio_handle handle)
{
    uint32_t index;
    uint16_t generation;
    void    *value = NULL;
    if (!table || !decode_handle(handle, &index, &generation))
        return NULL;
    pthread_mutex_lock(&table->lock);
    if (index < table->capacity && table->entries[index].generation == generation
        && table->entries[index].value)
    {
        value                       = table->entries[index].value;
        table->entries[index].value = NULL;
        if (table->entries[index].generation < PIPEASIO_HANDLE_GENERATION_MAX)
            ++table->entries[index].generation;
        --table->live;
    }
    pthread_mutex_unlock(&table->lock);
    return value;
}

uint32_t
pipeasio_handle_table_live(pipeasio_handle_table *table)
{
    uint32_t live;
    if (!table)
        return 0;
    pthread_mutex_lock(&table->lock);
    live = table->live;
    pthread_mutex_unlock(&table->lock);
    return live;
}
