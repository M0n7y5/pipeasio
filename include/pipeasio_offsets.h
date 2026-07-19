/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * pipeasio_offsets.h - pure buffer-arithmetic helpers shared between the
 * Wine-side driver (src/asio.c, src/wow64/audio_unix.c) and the
 * Linux-native unit tests (tests/unit/).
 *
 * Why a header: the math is small, but tests need to call it WITHOUT
 * compiling audio.c (which drags in pipewire/Wine headers).  Putting it
 * here as static-inline keeps a single source of truth - if the formula
 * ever changes, both the driver and the tests pick it up.
 *
 * Host-side callback buffer layout (asio.c side):
 *
 *   [in0 half0][in0 half1][in1 half0]...[inI-1 half1]
 *   [out0 half0][out0 half1][out1 half0]...[outO-1 half1]
 *
 *   - one heap block holds every host channel's double buffer
 *   - inputs come first, outputs after
 */
#ifndef PIPEASIO_OFFSETS_H
#define PIPEASIO_OFFSETS_H

#include <stddef.h>
#include <stdint.h>

/* Number of buffer halves per channel (ASIO double-buffer). */
#define PIPEASIO_BUFFER_HALVES 2u

/* Total bytes for the host's callback_audio_buffer covering n_in inputs
 * plus n_out outputs, each double-buffered. */
static inline size_t
pipeasio_host_callback_size_bytes(uint32_t n_in, uint32_t n_out, size_t buffer_size_samples,
                                  size_t sample_size_bytes)
{
    return ((size_t)n_in + n_out) * PIPEASIO_BUFFER_HALVES * buffer_size_samples
           * sample_size_bytes;
}

/* Sample offset into callback_audio_buffer for input channel i. */
static inline size_t
pipeasio_host_input_offset_samples(uint32_t input_idx, size_t buffer_size_samples)
{
    return (size_t)input_idx * PIPEASIO_BUFFER_HALVES * buffer_size_samples;
}

/* Sample offset into callback_audio_buffer for output channel i, given
 * n_in inputs ahead of it. */
static inline size_t
pipeasio_host_output_offset_samples(uint32_t output_idx, uint32_t n_in, size_t buffer_size_samples)
{
    return ((size_t)n_in + output_idx) * PIPEASIO_BUFFER_HALVES * buffer_size_samples;
}

/* Sample offset for the current host_buffer_index'd half of a channel
 * slice (the [0 .. buffer_size) or [buffer_size .. 2*buffer_size) part). */
static inline size_t
pipeasio_host_half_offset_samples(uint32_t host_buffer_index, size_t buffer_size_samples)
{
    return (size_t)host_buffer_index * buffer_size_samples;
}

#endif /* PIPEASIO_OFFSETS_H */
