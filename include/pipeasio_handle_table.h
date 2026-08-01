/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define PIPEASIO_HANDLE_INDEX_BITS 20u
#define PIPEASIO_HANDLE_INDEX_MASK ((UINT32_C(1) << PIPEASIO_HANDLE_INDEX_BITS) - 1u)
#define PIPEASIO_HANDLE_GENERATION_MAX ((UINT32_C(1) << (32u - PIPEASIO_HANDLE_INDEX_BITS)) - 1u)

typedef uint32_t pipeasio_handle;

typedef struct pipeasio_handle_entry
{
    void    *value;
    uint16_t generation;
} pipeasio_handle_entry;

typedef struct pipeasio_handle_table
{
    pthread_mutex_t        lock;
    pipeasio_handle_entry *entries;
    uint32_t               capacity;
    uint32_t               live;
} pipeasio_handle_table;

bool            pipeasio_handle_table_init(pipeasio_handle_table *table);
void            pipeasio_handle_table_destroy(pipeasio_handle_table *table);
pipeasio_handle pipeasio_handle_table_insert(pipeasio_handle_table *table, void *value);
void           *pipeasio_handle_table_get(pipeasio_handle_table *table, pipeasio_handle handle);
void           *pipeasio_handle_table_remove(pipeasio_handle_table *table, pipeasio_handle handle);
uint32_t        pipeasio_handle_table_live(pipeasio_handle_table *table);
