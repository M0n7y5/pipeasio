/*
 * ProfilerParse.hpp - decode PipeWire Profiler PODs into pw-top's figures.
 *
 * PURE (POD in, structs out) so it is unit testable against synthetic PODs
 * built with spa_pod_builder.
 *
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
#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

struct spa_pod;

/* Absent per-block xrun counter; deriveTiming() falls back to the driver's. */
constexpr uint32_t kXrunInvalid = UINT32_MAX;

/* pw-top's time sentinels: the node did not run this cycle ("---"), or was
 * signalled but had not finished when the point was taken ("+++"). */
constexpr int64_t kTimeUnknown = -1;
constexpr int64_t kTimePending = -2;

/* One node's timing block. Mirrors pw-top's `struct measurement`. */
struct ProfilerBlock
{
    uint32_t id = 0;
    QString  name;
    int64_t  prevSignal   = 0;
    int64_t  signal       = 0;
    int64_t  awake        = 0;
    int64_t  finish       = 0;
    int32_t  status       = 0;
    uint32_t latencyNum   = 0;
    uint32_t latencyDenom = 0;
    uint32_t xrunCount    = kXrunInvalid;
    bool     async        = false;
    bool     isDriver     = false;
};

/* The driver-global part of a profiler point. Mirrors pw-top's `struct driver`. */
struct ProfilerDriverInfo
{
    int64_t  count          = 0;
    float    cpuLoad[3]     = { 0.0f, 0.0f, 0.0f };
    uint32_t xrunCount      = 0;
    uint32_t transportState = 0;

    int64_t  clockNsec      = 0;
    uint32_t clockRateNum   = 0;
    uint32_t clockRateDenom = 0;
    int64_t  clockPosition  = 0;
    int64_t  clockDuration  = 0;
    int64_t  clockDelay     = 0;
    double   clockRateDiff  = 0.0;
};

/* One SPA_TYPE_OBJECT_Profiler object: a driver's cycle and everyone in it. */
struct ProfilePoint
{
    ProfilerDriverInfo     info;
    QVector<ProfilerBlock> blocks;
};

/* Decode a pw_profiler `profile` event: one object per driver in the graph.
 * An object that fails to parse is skipped whole, never partially applied. */
QVector<ProfilePoint> parseProfilePods(const spa_pod *pod);

/* Per-cycle figures derived from a block, mirroring pw-top's print_node(). */
struct CycleTiming
{
    int     quantum   = 0; /* frames per cycle - pw-top's QUANT column */
    int     rate      = 0; /* clock denominator - pw-top's RATE column */
    int64_t waitNs    = kTimeUnknown;
    int64_t busyNs    = kTimeUnknown;
    double  waitRatio = 0.0; /* W/Q; 0 when the time is a sentinel */
    double  busyRatio = 0.0; /* B/Q; 0 when the time is a sentinel */
    long    xruns     = 0;
};

CycleTiming deriveTiming(const ProfilerDriverInfo &info, const ProfilerBlock &block);
