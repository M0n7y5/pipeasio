/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pipeasio_admission_gate.h"
#include "test_helpers.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

struct carrier
{
    pipeasio_admission_gate *gate;
    atomic_bool              entered;
    atomic_bool              release;
};

static void *
carry_gate(void *arg)
{
    struct carrier *carrier = arg;
    if (!pipeasio_gate_try_enter(carrier->gate))
        return NULL;
    atomic_store_explicit(&carrier->entered, true, memory_order_release);
    while (!atomic_load_explicit(&carrier->release, memory_order_acquire))
        nanosleep(&(struct timespec){ .tv_nsec = 1000000 }, NULL);
    pipeasio_gate_leave(carrier->gate);
    return NULL;
}

static bool
wait_for_bool(atomic_bool *value)
{
    for (unsigned int i = 0; i < 2000; ++i)
    {
        if (atomic_load_explicit(value, memory_order_acquire))
            return true;
        nanosleep(&(struct timespec){ .tv_nsec = 1000000 }, NULL);
    }
    return false;
}

int
main(void)
{
    pipeasio_admission_gate method;
    pipeasio_admission_gate host;

    TEST_GROUP("transient close and identity admission")
    {
        pipeasio_gate_init(&method, true);
        EXPECT_TRUE(pipeasio_gate_try_enter(&method));
        EXPECT_EQ(pipeasio_gate_count(&method), 1);
        EXPECT_TRUE(pipeasio_gate_close(&method, 1));
        EXPECT_TRUE(!pipeasio_gate_try_enter(&method));
        EXPECT_TRUE(pipeasio_gate_try_enter_identity(&method));
        EXPECT_EQ(pipeasio_gate_count(&method), 2);
        EXPECT_EQ(pipeasio_gate_leave(&method), 1);
        EXPECT_EQ(pipeasio_gate_leave(&method), 0);
        EXPECT_TRUE(pipeasio_gate_reopen(&method, 1));
        EXPECT_TRUE(pipeasio_gate_try_enter(&method));
        EXPECT_EQ(pipeasio_gate_leave(&method), 0);
    }

    TEST_GROUP("owner handoff")
    {
        pipeasio_gate_init(&host, true);
        EXPECT_TRUE(pipeasio_gate_close(&host, 11));
        EXPECT_TRUE(pipeasio_gate_handoff(&host, 11, 12));
        EXPECT_TRUE(!pipeasio_gate_handoff(&host, 11, 13));
        EXPECT_TRUE(!pipeasio_gate_reopen(&host, 11));
        EXPECT_TRUE(pipeasio_gate_reopen(&host, 12));
        EXPECT_TRUE(pipeasio_gate_close(&host, 14));
        EXPECT_TRUE(!pipeasio_gate_close(&host, 11));
        EXPECT_TRUE(!pipeasio_gate_reopen(&host, 11));
        EXPECT_TRUE(pipeasio_gate_reopen(&host, 14));
    }

    TEST_GROUP("independent permanent destruction")
    {
        pipeasio_gate_close_permanently(&method);
        EXPECT_TRUE(pipeasio_gate_is_permanent(&method));
        EXPECT_TRUE(!pipeasio_gate_try_enter(&method));
        EXPECT_TRUE(!pipeasio_gate_try_enter_identity(&method));
        EXPECT_TRUE(!pipeasio_gate_reopen(&method, 1));
        EXPECT_TRUE(pipeasio_gate_try_enter(&host));
        EXPECT_EQ(pipeasio_gate_leave(&host), 0);
        pipeasio_gate_close_permanently(&host);
        EXPECT_TRUE(!pipeasio_gate_reopen(&host, 12));
        for (pipeasio_gate_owner owner = 1; owner < 8; ++owner)
        {
            EXPECT_TRUE(!pipeasio_gate_close(&host, owner));
            EXPECT_TRUE(!pipeasio_gate_handoff(&host, 0, owner));
            EXPECT_TRUE(!pipeasio_gate_reopen(&host, owner));
        }
    }

    TEST_GROUP("in-flight final decrement")
    {
        pthread_t      thread;
        struct carrier carrier;
        pipeasio_gate_init(&method, true);
        carrier.gate = &method;
        atomic_init(&carrier.entered, false);
        atomic_init(&carrier.release, false);
        EXPECT_EQ(pthread_create(&thread, NULL, carry_gate, &carrier), 0);
        EXPECT_TRUE(wait_for_bool(&carrier.entered));
        EXPECT_TRUE(pipeasio_gate_close(&method, 21));
        EXPECT_EQ(pipeasio_gate_count(&method), 1);
        atomic_store_explicit(&carrier.release, true, memory_order_release);
        EXPECT_EQ(pthread_join(thread, NULL), 0);
        EXPECT_EQ(pipeasio_gate_count(&method), 0);
        EXPECT_TRUE(pipeasio_gate_reopen(&method, 21));
    }

    return test_report();
}
