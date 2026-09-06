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

/* ABI layout guard for include/pipeasio_unix_abi.h. */
#include <stddef.h>

#include "audio.h"
#include "pipeasio_unix_abi.h"

#ifndef EXPECTED_POINTER_SIZE
#error EXPECTED_POINTER_SIZE must be defined by the test runner
#endif

#define CHECK(expr, msg) _Static_assert((expr), msg)

CHECK(sizeof(void *) == EXPECTED_POINTER_SIZE, "unexpected compiler bitness");
CHECK(PIPEASIO_UNIX_ABI_VERSION == 4, "ABI version changed - bump both halves");
CHECK(PAU_RT_MAX_PORTS == 256, "RT channel cap changed");
CHECK(PAU_ENDPOINT_MAX == 256, "endpoint cap changed");

/* Call-code order indexes __wine_unix_call_funcs[]. */
CHECK(PAU_OPEN == 0, "call enum drift");
CHECK(PAU_CLOSE == 1, "call enum drift");
CHECK(PAU_BIND_RT == 23, "call enum drift");
CHECK(PAU_LOAD_CONFIG == 24, "call enum drift");
CHECK(PAU_WAIT_CALLBACK == 26, "call enum drift");
CHECK(PAU_REPLY_CALLBACK == 27, "call enum drift");
CHECK(PAU_SET_REALTIME == 28, "call enum drift - PAU_SET_REALTIME must stay appended");
CHECK(PAU_CALL_COUNT == 29, "call count drift - update both unix call tables");
CHECK(PAU_CB_BUFFER_SWITCH == 0, "callback kind drift");
CHECK(PAU_CB_RESERVED_3 == 3, "callback kind drift");

/* audio_open status values cross the PE/unixlib boundary in pa_open_params. */
CHECK(AUDIO_STATUS_OK == 0, "audio status ABI drift");
CHECK(AUDIO_STATUS_ERROR == 1, "audio status ABI drift");
CHECK(AUDIO_STATUS_NO_MEMORY == 2, "audio status ABI drift");
CHECK(AUDIO_STATUS_NO_CONTEXT == 3, "audio status ABI drift");
CHECK(AUDIO_STATUS_NO_DAEMON == 4, "audio status ABI drift");
CHECK(AUDIO_STATUS_NO_UNIXLIB == 5, "audio status ABI drift");

CHECK(sizeof(pa_i64) == 8, "pa_i64 size");
CHECK(offsetof(pa_i64, lo) == 0, "pa_i64 lo offset");
CHECK(offsetof(pa_i64, hi) == 4, "pa_i64 hi offset");

CHECK(sizeof(pa_open_params) == 48, "pa_open_params size");
CHECK(offsetof(pa_open_params, version) == 0, "version must lead every param struct");
CHECK(offsetof(pa_open_params, client) == 40, "pa_open_params client offset");
CHECK(offsetof(pa_open_params, status) == 44, "pa_open_params status offset");

CHECK(sizeof(pa_simple_params) == 12, "pa_simple_params size");
CHECK(offsetof(pa_simple_params, client) == 4, "pa_simple_params client offset");
CHECK(offsetof(pa_simple_params, result) == 8, "pa_simple_params result offset");

CHECK(sizeof(pa_set_u32_params) == 16, "pa_set_u32_params size");

CHECK(sizeof(pa_time_params) == 16, "pa_time_params size");
CHECK(offsetof(pa_time_params, nsec) == 8, "pa_time_params nsec offset");

CHECK(sizeof(pa_port_register_params) == 52, "pa_port_register_params size");
CHECK(offsetof(pa_port_register_params, port) == 48, "pa_port_register_params port offset");

CHECK(sizeof(pa_port_params) == 28, "pa_port_params size");
CHECK(offsetof(pa_port_params, lat_min) == 16, "pa_port_params lat_min offset");
CHECK(offsetof(pa_port_params, result) == 24, "pa_port_params result offset");

CHECK(sizeof(pa_endpoint_record) == 784, "pa_endpoint_record size");
CHECK(offsetof(pa_endpoint_record, node_id) == 768, "endpoint identity offset");
CHECK(sizeof(pa_ports_params) == 200992, "pa_ports_params size");
CHECK(offsetof(pa_ports_params, count) == 280, "pa_ports_params count offset");
CHECK(offsetof(pa_ports_params, endpoints) == 288, "pa_ports_params endpoints offset");

CHECK(sizeof(pa_connect_params) == 524, "pa_connect_params size");

CHECK(sizeof(pa_config_params) == 800, "pa_config_params size");
CHECK(offsetof(pa_config_params, cfg) == 4, "pa_config_params cfg offset");
CHECK(offsetof(pa_config_params, found) == 796, "pa_config_params found offset");
/* The struct size is unchanged, so pin the fields added or shifted by ABI 2. */
CHECK(sizeof(struct pipeasio_config) == 792, "pipeasio_config size");
CHECK(offsetof(struct pipeasio_config, realtime) == 22, "pipeasio_config realtime offset");
CHECK(offsetof(struct pipeasio_config, output_device) == 23,
      "pipeasio_config output_device offset");
CHECK(offsetof(struct pipeasio_config, input_device) == 279, "pipeasio_config input_device offset");
CHECK(offsetof(struct pipeasio_config, node_name) == 535, "pipeasio_config node_name offset");

CHECK(sizeof(pa_fingerprint_params) == 12, "pa_fingerprint_params size");
CHECK(offsetof(pa_fingerprint_params, fp) == 4, "pa_fingerprint_params fp offset");

CHECK(sizeof(pa_bind_params) == 544, "pa_bind_params size");
CHECK(offsetof(pa_bind_params, buffer_base) == 8, "pa_bind_params buffer_base offset");
CHECK(offsetof(pa_bind_params, in_active) == 28, "pa_bind_params in_active offset");
CHECK(offsetof(pa_bind_params, out_active) == 284, "pa_bind_params out_active offset");
CHECK(offsetof(pa_bind_params, result) == 540, "pa_bind_params result offset");

CHECK(sizeof(pa_wait_params) == 56, "pa_wait_params size");
CHECK(offsetof(pa_wait_params, seq) == 8, "pa_wait_params seq offset");
CHECK(offsetof(pa_wait_params, time_nsec) == 24, "pa_wait_params time_nsec offset");
CHECK(offsetof(pa_wait_params, admitted) == 40, "pa_wait_params admitted offset");
CHECK(offsetof(pa_wait_params, shutdown) == 52, "pa_wait_params shutdown offset");

CHECK(sizeof(pa_reply_params) == 32, "pa_reply_params size");
CHECK(offsetof(pa_reply_params, seq) == 8, "pa_reply_params seq offset");
CHECK(offsetof(pa_reply_params, admitted) == 12, "pa_reply_params admitted offset");
CHECK(offsetof(pa_reply_params, result) == 28, "pa_reply_params result offset");

/* The embedded config struct must itself be pointer-free / arch-stable. */
CHECK(sizeof(struct pipeasio_config) == 792, "pipeasio_config layout changed");

int
pipeasio_unix_abi_probe(void)
{
    return 0;
}
