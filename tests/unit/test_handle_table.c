/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pipeasio_handle_table.h"
#include "test_helpers.h"

#include <stdint.h>

static void
fill_table(pipeasio_handle_table *table, pipeasio_handle *handles, uintptr_t base, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        handles[i] = pipeasio_handle_table_insert(table, (void *)(base + i));
        EXPECT_TRUE(handles[i] != 0);
        EXPECT_TRUE(pipeasio_handle_table_get(table, handles[i]) == (void *)(base + i));
    }
}

int
main(void)
{
    pipeasio_handle_table first;
    pipeasio_handle_table second;
    pipeasio_handle       first_handles[513];
    pipeasio_handle       second_handles[513];

    TEST_GROUP("growth and simultaneous clients")
    {
        EXPECT_TRUE(pipeasio_handle_table_init(&first));
        EXPECT_TRUE(pipeasio_handle_table_init(&second));
        fill_table(&first, first_handles, 0x1000, 513);
        fill_table(&second, second_handles, 0x100000, 513);
        EXPECT_EQ(pipeasio_handle_table_live(&first), 513);
        EXPECT_EQ(pipeasio_handle_table_live(&second), 513);
    }

    TEST_GROUP("generation rejects stale handles")
    {
        pipeasio_handle stale = first_handles[256];
        EXPECT_TRUE(pipeasio_handle_table_remove(&first, stale) == (void *)(0x1000 + 256));
        EXPECT_TRUE(pipeasio_handle_table_get(&first, stale) == NULL);
        pipeasio_handle replacement = pipeasio_handle_table_insert(&first, (void *)0xabcdef);
        EXPECT_TRUE(replacement != 0);
        EXPECT_TRUE(replacement != stale);
        EXPECT_TRUE(pipeasio_handle_table_get(&first, stale) == NULL);
        EXPECT_TRUE(pipeasio_handle_table_get(&first, replacement) == (void *)0xabcdef);
        EXPECT_TRUE(pipeasio_handle_table_remove(&first, stale) == NULL);
        EXPECT_TRUE(pipeasio_handle_table_remove(&first, replacement) == (void *)0xabcdef);
    }

    TEST_GROUP("complete removal")
    {
        for (uint32_t i = 0; i < 513; ++i)
            if (i != 256)
                EXPECT_TRUE(pipeasio_handle_table_remove(&first, first_handles[i])
                            == (void *)(uintptr_t)(0x1000 + i));
        for (uint32_t i = 0; i < 513; ++i)
            EXPECT_TRUE(pipeasio_handle_table_remove(&second, second_handles[i])
                        == (void *)(uintptr_t)(0x100000 + i));
        EXPECT_EQ(pipeasio_handle_table_live(&first), 0);
        EXPECT_EQ(pipeasio_handle_table_live(&second), 0);
        pipeasio_handle_table_destroy(&first);
        pipeasio_handle_table_destroy(&second);
    }

    TEST_GROUP("generation exhaustion retires slot")
    {
        pipeasio_handle_table rollover;
        pipeasio_handle       first_stale = 0;
        pipeasio_handle       last_stale  = 0;
        EXPECT_TRUE(pipeasio_handle_table_init(&rollover));
        for (uint32_t generation = 1; generation <= PIPEASIO_HANDLE_GENERATION_MAX; ++generation)
        {
            pipeasio_handle handle = pipeasio_handle_table_insert(&rollover, (void *)1);
            EXPECT_TRUE(handle != 0);
            if (generation == 1)
                first_stale = handle;
            last_stale = handle;
            EXPECT_TRUE(pipeasio_handle_table_remove(&rollover, handle) == (void *)1);
        }
        pipeasio_handle next = pipeasio_handle_table_insert(&rollover, (void *)2);
        EXPECT_TRUE(next != 0);
        EXPECT_TRUE((next & PIPEASIO_HANDLE_INDEX_MASK)
                    != (first_stale & PIPEASIO_HANDLE_INDEX_MASK));
        EXPECT_TRUE(pipeasio_handle_table_get(&rollover, first_stale) == NULL);
        EXPECT_TRUE(pipeasio_handle_table_get(&rollover, last_stale) == NULL);
        EXPECT_TRUE(pipeasio_handle_table_remove(&rollover, next) == (void *)2);
        pipeasio_handle_table_destroy(&rollover);
    }

    return test_report();
}
