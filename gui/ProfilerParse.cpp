/*
 * ProfilerParse.cpp - implementation.
 *
 * The struct layouts decoded here are the documented payloads of
 * enum spa_profiler (spa/param/profiler.h). Trailing fields PipeWire may add
 * later are simply not read: spa_pod_parse_struct stops at the end of the
 * requested format.
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
#include "ProfilerParse.hpp"

#include <spa/param/profiler.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/defs.h>

namespace
{

int
parseInfo(const spa_pod *pod, ProfilerDriverInfo *info)
{
    int32_t   xruns = 0;
    const int res   = spa_pod_parse_struct(pod, SPA_POD_Long(&info->count),
                                           SPA_POD_Float(&info->cpuLoad[0]),
                                           SPA_POD_Float(&info->cpuLoad[1]),
                                           SPA_POD_Float(&info->cpuLoad[2]), SPA_POD_Int(&xruns));
    if (res < 0)
        return res;
    info->xrunCount = static_cast<uint32_t>(xruns);
    return res;
}

int
parseClock(const spa_pod *pod, ProfilerDriverInfo *info)
{
    int32_t             flags     = 0;
    int32_t             id        = 0;
    char                name[64]  = { 0 };
    struct spa_fraction rate      = { 0, 0 };
    int64_t             nextNsec  = 0;
    int32_t             transport = 0;

    const int res = spa_pod_parse_struct(
            pod, SPA_POD_Int(&flags), SPA_POD_Int(&id), SPA_POD_Stringn(name, sizeof(name)),
            SPA_POD_Long(&info->clockNsec), SPA_POD_Fraction(&rate),
            SPA_POD_Long(&info->clockPosition), SPA_POD_Long(&info->clockDuration),
            SPA_POD_Long(&info->clockDelay), SPA_POD_Double(&info->clockRateDiff),
            SPA_POD_Long(&nextNsec), SPA_POD_OPT_Int(&transport));
    if (res < 0)
        return res;
    info->clockRateNum   = rate.num;
    info->clockRateDenom = rate.denom;
    info->transportState = static_cast<uint32_t>(transport);
    return res;
}

/* Driver and follower blocks share a layout; only the follower carries the
 * trailing `async` flag. */
int
parseBlock(const spa_pod *pod, bool isDriver, ProfilerBlock *out)
{
    int32_t             id     = 0;
    const char         *name   = nullptr;
    int32_t             status = 0;
    struct spa_fraction lat    = { 0, 0 };
    int32_t             xruns  = static_cast<int32_t>(kXrunInvalid);
    bool                async  = false;

    ProfilerBlock b;
    const int     res = spa_pod_parse_struct(pod, SPA_POD_Int(&id), SPA_POD_String(&name),
                                             SPA_POD_Long(&b.prevSignal), SPA_POD_Long(&b.signal),
                                             SPA_POD_Long(&b.awake), SPA_POD_Long(&b.finish),
                                             SPA_POD_Int(&status), SPA_POD_Fraction(&lat),
                                             SPA_POD_OPT_Int(&xruns), SPA_POD_OPT_Bool(&async));
    if (res < 0)
        return res;

    b.id           = static_cast<uint32_t>(id);
    b.name         = QString::fromUtf8(name ? name : "");
    b.status       = status;
    b.latencyNum   = lat.num;
    b.latencyDenom = lat.denom;
    b.xrunCount    = static_cast<uint32_t>(xruns);
    b.async        = async;
    b.isDriver     = isDriver;
    *out           = std::move(b);
    return res;
}

} // namespace

QVector<ProfilePoint>
parseProfilePods(const spa_pod *pod)
{
    QVector<ProfilePoint> points;
    if (!pod)
        return points;

    struct spa_pod *obj = nullptr;
    SPA_POD_STRUCT_FOREACH(pod, obj)
    {
        if (!spa_pod_is_object_type(obj, SPA_TYPE_OBJECT_Profiler))
            continue;

        ProfilePoint         point;
        bool                 broken = false;
        struct spa_pod_prop *prop   = nullptr;
        SPA_POD_OBJECT_FOREACH(reinterpret_cast<struct spa_pod_object *>(obj), prop)
        {
            ProfilerBlock block;
            switch (prop->key)
            {
            case SPA_PROFILER_info:
                broken = parseInfo(&prop->value, &point.info) < 0;
                break;
            case SPA_PROFILER_clock:
                broken = parseClock(&prop->value, &point.info) < 0;
                break;
            case SPA_PROFILER_driverBlock:
                if (parseBlock(&prop->value, true, &block) < 0)
                    broken = true;
                else
                    point.blocks.append(std::move(block));
                break;
            case SPA_PROFILER_followerBlock:
                /* A malformed follower costs that node's row, not the whole
                 * cycle: the clock and the other blocks are still good. */
                if (parseBlock(&prop->value, false, &block) >= 0)
                    point.blocks.append(std::move(block));
                break;
            default:
                break;
            }
            if (broken)
                break;
        }
        if (!broken)
            points.append(std::move(point));
    }
    return points;
}

CycleTiming
deriveTiming(const ProfilerDriverInfo &info, const ProfilerBlock &block)
{
    CycleTiming t;

    /* QUANT/RATE: a driver reports its own clock, a follower the latency
     * fraction it was scheduled with (normally the same numbers). */
    if (block.isDriver)
    {
        t.quantum = static_cast<int>(info.clockDuration * info.clockRateNum);
        t.rate    = static_cast<int>(info.clockRateDenom);
    }
    else
    {
        t.quantum = static_cast<int>(block.latencyNum);
        t.rate    = static_cast<int>(block.latencyDenom);
    }

    /* Cycle length in seconds - the denominator of the W/Q and B/Q ratios. */
    const double cycle = info.clockRateDenom ? static_cast<double>(info.clockDuration)
                                                       * info.clockRateNum / info.clockRateDenom
                                             : 0.0;

    if (block.awake >= block.signal)
        t.waitNs = block.awake - block.signal;
    else
        t.waitNs = block.signal > block.prevSignal ? kTimePending : kTimeUnknown;

    if (block.finish >= block.awake)
        t.busyNs = block.finish - block.awake;
    else
        t.busyNs = block.awake > block.prevSignal ? kTimePending : kTimeUnknown;

    if (cycle > 0.0)
    {
        if (t.waitNs >= 0)
            t.waitRatio = static_cast<double>(t.waitNs) / 1e9 / cycle;
        if (t.busyNs >= 0)
            t.busyRatio = static_cast<double>(t.busyNs) / 1e9 / cycle;
    }

    t.xruns = block.xrunCount == kXrunInvalid ? static_cast<long>(info.xrunCount)
                                              : static_cast<long>(block.xrunCount);
    return t;
}
