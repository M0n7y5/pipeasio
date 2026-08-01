/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pipeasio_pw_buffer.h"
#include "test_helpers.h"

#include <stdalign.h>

struct fixture
{
    struct pw_buffer  pw;
    struct spa_buffer spa;
    struct spa_data   data;
    struct spa_chunk  chunk;
    alignas(float) unsigned char storage[32];
};

static void
fixture_init(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->pw.buffer    = &f->spa;
    f->spa.n_datas  = 1;
    f->spa.datas    = &f->data;
    f->data.data    = f->storage;
    f->data.maxsize = sizeof(f->storage);
    f->data.chunk   = &f->chunk;
    f->chunk.size   = sizeof(f->storage);
    f->chunk.stride = sizeof(float);
}

struct queue_state
{
    unsigned          calls;
    struct pw_buffer *buffer;
    int               result;
};

static int
fake_queue(void *context, struct pw_buffer *buffer)
{
    struct queue_state *state = context;
    ++state->calls;
    state->buffer = buffer;
    return state->result;
}

int
main(void)
{
    struct fixture    f;
    void             *region   = (void *)1;
    struct spa_chunk *chunk    = NULL;
    float             source[] = { 1.0f, -2.0f, 3.0f, -4.0f };

    TEST_GROUP("region validation")
    {
        fixture_init(&f);
        EXPECT_TRUE(pipeasio_pw_validate_region(&f.pw, 4, &region, &chunk));
        EXPECT_TRUE(region == f.storage);
        EXPECT_TRUE(chunk == &f.chunk);
        f.chunk.offset = sizeof(float);
        f.chunk.size   = 4 * sizeof(float);
        EXPECT_TRUE(pipeasio_pw_validate_region(&f.pw, 4, &region, NULL));
        EXPECT_TRUE(region == f.storage + sizeof(float));
        f.chunk.offset = sizeof(f.storage) - sizeof(float);
        EXPECT_TRUE(!pipeasio_pw_validate_region(&f.pw, 2, &region, NULL));
        EXPECT_TRUE(region == NULL);
        f.chunk.offset = 0;
        f.data.maxsize = sizeof(f.storage) - 1;
        EXPECT_TRUE(!pipeasio_pw_validate_region(&f.pw, 1, NULL, NULL));
        fixture_init(&f);
        f.data.data = f.storage + 1;
        EXPECT_TRUE(!pipeasio_pw_validate_region(&f.pw, 1, NULL, NULL));
        fixture_init(&f);
        f.data.chunk = NULL;
        EXPECT_TRUE(!pipeasio_pw_validate_region(&f.pw, 1, NULL, NULL));
    }

    TEST_GROUP("copy and silence publication")
    {
        fixture_init(&f);
        EXPECT_TRUE(pipeasio_pw_publish_output(&f.pw, source, 4, true, true));
        EXPECT_TRUE(!memcmp(f.storage, source, sizeof(source)));
        EXPECT_EQ(f.chunk.offset, 0);
        EXPECT_EQ(f.chunk.size, sizeof(source));
        EXPECT_EQ(f.chunk.stride, sizeof(float));
        memset(f.storage, 0xff, sizeof(source));
        EXPECT_TRUE(pipeasio_pw_publish_output(&f.pw, source, 4, false, true));
        for (size_t i = 0; i < sizeof(source); ++i)
            EXPECT_EQ(f.storage[i], 0);
    }

    TEST_GROUP("malformed output queues once and clears slot")
    {
        struct queue_state          queue = { 0 };
        _Atomic(struct pw_buffer *) slot;
        fixture_init(&f);
        f.data.maxsize = 3;
        atomic_init(&slot, &f.pw);
        EXPECT_TRUE(!pipeasio_pw_finish_output(&slot, source, 4, true, true, fake_queue, &queue));
        EXPECT_TRUE(atomic_load(&slot) == NULL);
        EXPECT_EQ(queue.calls, 1);
        EXPECT_TRUE(queue.buffer == &f.pw);
        EXPECT_EQ(f.chunk.size, 0);
        EXPECT_TRUE(!pipeasio_pw_finish_output(&slot, source, 4, true, true, fake_queue, &queue));
        EXPECT_EQ(queue.calls, 1);
    }

    return test_report();
}
