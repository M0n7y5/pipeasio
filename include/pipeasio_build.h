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

/* Build identity for the debug log: version, commit, and the architecture
 * this half of the driver was compiled for. */
#pragma once

#include "pipeasio_build_info.h" /* generated: PIPEASIO_GIT_COMMIT */

/* arm64ec also defines __x86_64__, so it is tested first. */
#if defined(__arm64ec__)
#define PIPEASIO_BUILD_ARCH "arm64ec"
#elif defined(__aarch64__)
#define PIPEASIO_BUILD_ARCH "aarch64"
#elif defined(__x86_64__)
#define PIPEASIO_BUILD_ARCH "x86_64"
#elif defined(__i386__)
#define PIPEASIO_BUILD_ARCH "i386"
#else
#define PIPEASIO_BUILD_ARCH "unknown"
#endif
