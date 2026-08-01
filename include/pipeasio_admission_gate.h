/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/*
 * One atomic word carries the number of admitted users and the close owner.
 * Owner zero is reserved for an open gate. Destruction is permanent and
 * supersedes a transient owner.
 */
typedef struct pipeasio_admission_gate
{
    _Atomic uint64_t word;
} pipeasio_admission_gate;

typedef uint32_t pipeasio_gate_owner;

#define PIPEASIO_GATE_COUNT_MASK UINT64_C(0xffffffff)
#define PIPEASIO_GATE_CLOSED_BIT (UINT64_C(1) << 32)
#define PIPEASIO_GATE_PERMANENT_BIT (UINT64_C(1) << 33)
#define PIPEASIO_GATE_OWNER_SHIFT 34
#define PIPEASIO_GATE_OWNER_MASK (UINT64_C(0x3fffffff) << PIPEASIO_GATE_OWNER_SHIFT)
#define PIPEASIO_GATE_OWNER_MAX UINT32_C(0x3fffffff)

static inline uint64_t
pipeasio_gate_owner_bits(pipeasio_gate_owner owner)
{
    return ((uint64_t)(owner & PIPEASIO_GATE_OWNER_MAX)) << PIPEASIO_GATE_OWNER_SHIFT;
}

static inline pipeasio_gate_owner
pipeasio_gate_word_owner(uint64_t word)
{
    return (pipeasio_gate_owner)((word & PIPEASIO_GATE_OWNER_MASK) >> PIPEASIO_GATE_OWNER_SHIFT);
}

static inline void
pipeasio_gate_init(pipeasio_admission_gate *gate, bool open)
{
    atomic_init(&gate->word, open ? 0 : PIPEASIO_GATE_CLOSED_BIT);
}

static inline bool
pipeasio_gate_try_enter(pipeasio_admission_gate *gate)
{
    uint64_t word = atomic_load_explicit(&gate->word, memory_order_acquire);
    for (;;)
    {
        if ((word & PIPEASIO_GATE_CLOSED_BIT)
            || (word & PIPEASIO_GATE_COUNT_MASK) == PIPEASIO_GATE_COUNT_MASK)
            return false;
        if (atomic_compare_exchange_weak_explicit(&gate->word, &word, word + 1,
                                                  memory_order_acquire, memory_order_relaxed))
            return true;
    }
}

/* Identity methods may cross a transient close but never destruction. */
static inline bool
pipeasio_gate_try_enter_identity(pipeasio_admission_gate *gate)
{
    uint64_t word = atomic_load_explicit(&gate->word, memory_order_acquire);
    for (;;)
    {
        if ((word & PIPEASIO_GATE_PERMANENT_BIT)
            || (word & PIPEASIO_GATE_COUNT_MASK) == PIPEASIO_GATE_COUNT_MASK)
            return false;
        if (atomic_compare_exchange_weak_explicit(&gate->word, &word, word + 1,
                                                  memory_order_acquire, memory_order_relaxed))
            return true;
    }
}

static inline uint32_t
pipeasio_gate_leave(pipeasio_admission_gate *gate)
{
    uint64_t word = atomic_load_explicit(&gate->word, memory_order_relaxed);
    for (;;)
    {
        uint32_t count = (uint32_t)(word & PIPEASIO_GATE_COUNT_MASK);
        if (!count)
            return 0;
        if (atomic_compare_exchange_weak_explicit(&gate->word, &word, word - 1,
                                                  memory_order_release, memory_order_relaxed))
            return count - 1;
    }
}

/* Close an open gate, or refresh a close already owned by this owner. */
static inline bool
pipeasio_gate_close(pipeasio_admission_gate *gate, pipeasio_gate_owner owner)
{
    uint64_t word;
    uint64_t next;
    if (!owner || owner > PIPEASIO_GATE_OWNER_MAX)
        return false;
    word = atomic_load_explicit(&gate->word, memory_order_acquire);
    for (;;)
    {
        if (word & PIPEASIO_GATE_PERMANENT_BIT)
            return false;
        if ((word & PIPEASIO_GATE_CLOSED_BIT) && pipeasio_gate_word_owner(word) != owner)
            return false;
        next = (word & PIPEASIO_GATE_COUNT_MASK) | PIPEASIO_GATE_CLOSED_BIT
               | pipeasio_gate_owner_bits(owner);
        if (atomic_compare_exchange_weak_explicit(&gate->word, &word, next, memory_order_acq_rel,
                                                  memory_order_acquire))
            return true;
    }
}

/* Transfer a transient close only when the expected owner still owns it. */
static inline bool
pipeasio_gate_handoff(pipeasio_admission_gate *gate, pipeasio_gate_owner from,
                      pipeasio_gate_owner to)
{
    uint64_t word;
    uint64_t next;
    if (!from || !to || from > PIPEASIO_GATE_OWNER_MAX || to > PIPEASIO_GATE_OWNER_MAX)
        return false;
    word = atomic_load_explicit(&gate->word, memory_order_acquire);
    for (;;)
    {
        if (!(word & PIPEASIO_GATE_CLOSED_BIT) || (word & PIPEASIO_GATE_PERMANENT_BIT)
            || pipeasio_gate_word_owner(word) != from)
            return false;
        next = (word & (PIPEASIO_GATE_COUNT_MASK | PIPEASIO_GATE_CLOSED_BIT))
               | pipeasio_gate_owner_bits(to);
        if (atomic_compare_exchange_weak_explicit(&gate->word, &word, next, memory_order_acq_rel,
                                                  memory_order_acquire))
            return true;
    }
}

static inline bool
pipeasio_gate_reopen(pipeasio_admission_gate *gate, pipeasio_gate_owner owner)
{
    uint64_t word = atomic_load_explicit(&gate->word, memory_order_acquire);
    for (;;)
    {
        uint64_t next;
        if (!(word & PIPEASIO_GATE_CLOSED_BIT) || (word & PIPEASIO_GATE_PERMANENT_BIT)
            || pipeasio_gate_word_owner(word) != owner)
            return false;
        next = word & PIPEASIO_GATE_COUNT_MASK;
        if (atomic_compare_exchange_weak_explicit(&gate->word, &word, next, memory_order_release,
                                                  memory_order_acquire))
            return true;
    }
}

static inline void
pipeasio_gate_close_permanently(pipeasio_admission_gate *gate)
{
    uint64_t word = atomic_load_explicit(&gate->word, memory_order_acquire);
    for (;;)
    {
        uint64_t next = (word & PIPEASIO_GATE_COUNT_MASK) | PIPEASIO_GATE_CLOSED_BIT
                        | PIPEASIO_GATE_PERMANENT_BIT;
        if (atomic_compare_exchange_weak_explicit(&gate->word, &word, next, memory_order_acq_rel,
                                                  memory_order_acquire))
            return;
    }
}

static inline uint32_t
pipeasio_gate_count(const pipeasio_admission_gate *gate)
{
    return (uint32_t)(atomic_load_explicit(&gate->word, memory_order_acquire)
                      & PIPEASIO_GATE_COUNT_MASK);
}

static inline bool
pipeasio_gate_is_closed(const pipeasio_admission_gate *gate)
{
    return (atomic_load_explicit(&gate->word, memory_order_acquire) & PIPEASIO_GATE_CLOSED_BIT)
           != 0;
}

static inline bool
pipeasio_gate_is_permanent(const pipeasio_admission_gate *gate)
{
    return (atomic_load_explicit(&gate->word, memory_order_acquire) & PIPEASIO_GATE_PERMANENT_BIT)
           != 0;
}
