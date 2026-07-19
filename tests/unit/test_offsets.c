/*
 * test_offsets.c - exhaustively exercise pipeasio_offsets.h math.
 *
 * The intent is to lock in the invariants of the host-side callback
 * buffer layout (asio.c::CreateBuffers, wow64/audio_unix.c):
 *   - inputs tile first, outputs after, each channel double-buffered
 *   - slices are strictly sequential with no gaps or overlaps
 *   - the two halves of a channel are packed back-to-back
 *
 * If the offset math ever regresses, this test fires before anyone
 * has to bisect a stack smash.
 */
#include "pipeasio_offsets.h"
#include "test_helpers.h"

static const size_t SAMPLE_BYTES = sizeof(float);   /* audio_sample_t */

static void test_host_callback_layout(void)
{
    /* Total bytes: (n_in + n_out) channels x 2 halves x bsize x sizeof. */
    TEST_GROUP("host callback total size") {
        const uint32_t inputs  = 16;
        const uint32_t outputs = 16;
        const size_t   bsize   = 1024;
        EXPECT_EQ(pipeasio_host_callback_size_bytes(inputs, outputs, bsize, SAMPLE_BYTES),
                  (size_t)(inputs + outputs) * 2 * bsize * SAMPLE_BYTES);
        /* No overflow at realistic ceiling: 64 channels x 8192 x 2 x 4 = 4 MiB. */
        EXPECT_EQ(pipeasio_host_callback_size_bytes(32, 32, 8192, SAMPLE_BYTES),
                  (size_t)4 * 1024 * 1024);
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
        EXPECT_EQ(pipeasio_host_callback_size_bytes(1, 1, 32, SAMPLE_BYTES),
                  (size_t)2 * 2 * 32 * 4);
        EXPECT_EQ(pipeasio_host_output_offset_samples(0, 1, 32), (size_t)2 * 32);
    }

    /* Single mono channel - common control-room monitor setup. */
    TEST_GROUP("single channel") {
        EXPECT_EQ(pipeasio_host_callback_size_bytes(1, 0, 1024, SAMPLE_BYTES),
                  (size_t)2 * 1024 * 4);
        EXPECT_EQ(pipeasio_host_input_offset_samples(0, 1024), 0u);
        EXPECT_EQ(pipeasio_host_output_offset_samples(0, 1, 1024), (size_t)2 * 1024);
    }
}

int main(void)
{
    test_host_callback_layout();
    test_edge_cases();
    return test_report();
}
