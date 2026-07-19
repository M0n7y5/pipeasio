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

/* Single source of truth for the PipeASIO COM class id
 * (used by asio.c's QueryInterface and main.c's DllGetClassObject).
 * Requires the Wine/Win32 headers defining GUID. */
#pragma once

/* {2d3ca9e2-1193-4c5d-b5fd-38798f3dc074} */
static GUID const CLSID_PipeASIO
        = { 0x2d3ca9e2, 0x1193, 0x4c5d, { 0xb5, 0xfd, 0x38, 0x79, 0x8f, 0x3d, 0xc0, 0x74 } };
