/*
 * test_offsets.c - exhaustively exercise pipeasio_offsets.h math.
 *
 * The intent is to lock in the invariants we manually verified during
 * the Phase 3 stabilization static review:
 *   - memfd is correctly sized for n_ports x 2 halves x bsize x sizeof
 *   - per-port mapoffsets partition that range with no gaps or overlaps
 *   - audio_port_get_buffer's sample-offset never points past the
 *     allocated memfd
 *   - host-side callback buffer (asio.c::CreateBuffers) tiles inputs
 *     then outputs identically
 *
 * If the offset math ever regresses, this test fires before anyone
 * has to bisect a stack smash.
 */
#include "pipeasio_offsets.h"
#include "test_helpers.h"

static const size_t SAMPLE_BYTES = sizeof(float);   /* audio_sample_t */

static void test_memfd_size(void)
{
    TEST_GROUP("memfd size") {
        EXPECT_EQ(pipeasio_memfd_size_bytes(0,  1024, SAMPLE_BYTES), 0u);
        EXPECT_EQ(pipeasio_memfd_size_bytes(1,  1024, SAMPLE_BYTES),
                  (size_t)1 * 2 * 1024 * 4);
        EXPECT_EQ(pipeasio_memfd_size_bytes(32, 1024, SAMPLE_BYTES),
                  (size_t)262144);                     /* "256 kB" probe.err */
        EXPECT_EQ(pipeasio_memfd_size_bytes(1,  256,  SAMPLE_BYTES),
                  (size_t)2048);
        /* No overflow at realistic ceiling: 64 ports x 8192 x 4 = 4 MiB. */
        EXPECT_EQ(pipeasio_memfd_size_bytes(64, 8192, SAMPLE_BYTES),
                  (size_t)4 * 1024 * 1024);
    }
}

static void test_mapoffset_partition(void)
{
    /* For every (n_ports, bsize) sweep, the mapoffsets for every
     * (channel, half) must:
     *   - start at 0
     *   - be strictly increasing
     *   - end exactly one buffer-half-byte below total memfd size  */
    const uint32_t n_ports_cases[] = {1, 2, 16, 32};
    const size_t   bsize_cases[]   = {64, 256, 1024, 4096};

    TEST_GROUP("mapoffset partition") {
        for (size_t pi = 0; pi < sizeof n_ports_cases / sizeof *n_ports_cases; pi++) {
            for (size_t bi = 0; bi < sizeof bsize_cases / sizeof *bsize_cases; bi++) {
                const uint32_t n_ports = n_ports_cases[pi];
                const size_t   bsize   = bsize_cases[bi];
                const size_t   half_bytes = bsize * SAMPLE_BYTES;
                const size_t   total = pipeasio_memfd_size_bytes(n_ports, bsize, SAMPLE_BYTES);

                size_t prev = 0;
                size_t last = 0;
                for (uint32_t i = 0; i < n_ports; i++) {
                    for (uint32_t h = 0; h < 2; h++) {
                        size_t off = pipeasio_mapoffset_bytes(i, h, bsize, SAMPLE_BYTES);
                        if (i == 0 && h == 0) {
                            EXPECT_EQ(off, 0u);
                        } else {
                            EXPECT_TRUE(off == prev + half_bytes);
                        }
                        EXPECT_TRUE(off + half_bytes <= total);
                        prev = off;
                        last = off + half_bytes;
                    }
                }
                /* The final byte covered must equal the allocated total. */
                EXPECT_EQ(last, total);
            }
        }
    }
}

static void test_port_buffer_offset_samples(void)
{
    /* audio_port_get_buffer math: indexed in samples (float pointer
     * arithmetic), not bytes.  Round-trip via SAMPLE_BYTES must equal
     * the mapoffset_bytes. */
    TEST_GROUP("port_buffer_offset matches mapoffset_bytes") {
        for (uint32_t i = 0; i < 8; i++) {
            for (uint32_t h = 0; h < 2; h++) {
                size_t samples = pipeasio_port_buffer_offset_samples(i, h, 1024);
                size_t bytes_via_samples = samples * SAMPLE_BYTES;
                size_t bytes_direct = pipeasio_mapoffset_bytes(i, h, 1024, SAMPLE_BYTES);
                EXPECT_EQ(bytes_via_samples, bytes_direct);
            }
        }
    }

    /* Each (channel, half) maps to a unique offset.  No two distinct
     * (i, h) pairs collide - important because aliased offsets would
     * be cross-channel buffer corruption. */
    TEST_GROUP("port_buffer_offset uniqueness") {
        const uint32_t n_ports = 32;
        const size_t   bsize   = 1024;
        size_t seen[64] = {0};
        for (uint32_t i = 0; i < n_ports; i++) {
            for (uint32_t h = 0; h < 2; h++) {
                size_t off = pipeasio_port_buffer_offset_samples(i, h, bsize);
                size_t slot = i * 2 + h;
                seen[slot] = off;
            }
        }
        /* Verify strict ordering 0, bsize, 2*bsize, ... */
        for (size_t k = 0; k < (size_t)n_ports * 2; k++) {
            EXPECT_EQ(seen[k], k * bsize);
        }
    }
}

static void test_host_callback_layout(void)
{
    /* Host's callback_audio_buffer has the same total size as the memfd
     * when n_in + n_out == n_ports - confirming the two sides agree on
     * the allocation footprint. */
    TEST_GROUP("host callback total size") {
        const uint32_t inputs  = 16;
        const uint32_t outputs = 16;
        EXPECT_EQ(
            pipeasio_host_callback_size_bytes(inputs, outputs, 1024, SAMPLE_BYTES),
            pipeasio_memfd_size_bytes(inputs + outputs, 1024, SAMPLE_BYTES));
    }

    /* Inputs occupy [0, inputs*2*bsize), outputs occupy
     * [inputs*2*bsize, (inputs+outputs)*2*bsize).  Strict, non-overlapping. */
    TEST_GROUP("input/output slice layout") {
        const uint32_t inputs  = 16;
        const uint32_t outputs = 16;
        const size_t   bsize   = 1024;
        const size_t   half_samples = bsize;

        /* Last input ends where first output begins. */
        size_t last_input_end =
            pipeasio_host_input_offset_samples(inputs - 1, bsize)
            + 2 * half_samples;
        size_t first_output =
            pipeasio_host_output_offset_samples(0, inputs, bsize);
        EXPECT_EQ(last_input_end, first_output);

        /* Outputs fully fit within the buffer. */
        size_t last_output_end =
            pipeasio_host_output_offset_samples(outputs - 1, inputs, bsize)
            + 2 * half_samples;
        size_t total_samples =
            pipeasio_host_callback_size_bytes(inputs, outputs, bsize, SAMPLE_BYTES)
            / SAMPLE_BYTES;
        EXPECT_EQ(last_output_end, total_samples);
    }

    /* Within one channel's slice, the two halves are sequential - the
     * host process callback does &audio_buffer[host_buffer_index * bsize]
     * with host_buffer_index in {0, 1}, so the two halves must be packed
     * with no gap.  This is implicit in pipeasio_host_half_offset_samples
     * but worth pinning. */
    TEST_GROUP("host half offset") {
        EXPECT_EQ(pipeasio_host_half_offset_samples(0, 1024), 0u);
        EXPECT_EQ(pipeasio_host_half_offset_samples(1, 1024), 1024u);
    }
}

static void test_edge_cases(void)
{
    /* Tiny ASIO buffer (RME prefers 32).  Should still align cleanly. */
    TEST_GROUP("tiny buffer size") {
        EXPECT_EQ(pipeasio_memfd_size_bytes(2, 32, SAMPLE_BYTES),
                  (size_t)2 * 2 * 32 * 4);
        EXPECT_EQ(pipeasio_mapoffset_bytes(1, 1, 32, SAMPLE_BYTES),
                  (size_t)3 * 32 * 4);
    }

    /* Single mono channel - common control-room monitor setup. */
    TEST_GROUP("single channel") {
        EXPECT_EQ(pipeasio_memfd_size_bytes(1, 1024, SAMPLE_BYTES),
                  (size_t)2 * 1024 * 4);
        EXPECT_EQ(pipeasio_port_buffer_offset_samples(0, 0, 1024), 0u);
        EXPECT_EQ(pipeasio_port_buffer_offset_samples(0, 1, 1024), 1024u);
    }
}

int main(void)
{
    test_memfd_size();
    test_mapoffset_partition();
    test_port_buffer_offset_samples();
    test_host_callback_layout();
    test_edge_cases();
    return test_report();
}
