/*
 * PipeWireGraph.cpp - live PipeWire graph model (see PipeWireGraph.hpp).
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
#include "PipeWireGraph.hpp"

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <pipewire/extensions/profiler.h>
#include <spa/debug/types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/profiler.h>
#include <spa/pod/parser.h>

#include <QMutex>
#include <QPointer>
#include <QTimer>

namespace
{

/* One-time pw_init for the process. */
void
ensurePwInit()
{
    static QMutex m;
    static bool   done = false;
    QMutexLocker  lock(&m);
    if (!done)
    {
        pw_init(nullptr, nullptr);
        done = true;
    }
}

struct NodeRec
{
    QString name;         /* node.name */
    QString description;  /* node.description / node.nick fallback */
    QString mediaClass;   /* media.class */
    QString codec;        /* api.bluez5.codec */
    QString deviceApi;    /* device.api */
    QString state;        /* "running" / "idle" / ... (bound nodes only) */
    QString sampleFmt;    /* negotiated sample format, e.g. "F32P" */
    int     rate     = 0; /* negotiated rate (bound nodes only) */
    int     channels = 0; /* negotiated channels, else audio.channels prop */
    bool    isPipeAsio = false;
};

struct LinkRec
{
    uint32_t outNode = 0;
    uint32_t inNode  = 0;
};

QString
nodeStateString(enum pw_node_state st)
{
    switch (st)
    {
    case PW_NODE_STATE_ERROR: return QStringLiteral("error");
    case PW_NODE_STATE_CREATING: return QStringLiteral("creating");
    case PW_NODE_STATE_SUSPENDED: return QStringLiteral("suspended");
    case PW_NODE_STATE_IDLE: return QStringLiteral("idle");
    case PW_NODE_STATE_RUNNING: return QStringLiteral("running");
    }
    return {};
}

QString
prettyBtCodec(const QString &codec)
{
    static const QHash<QString, QString> names = {
        { QStringLiteral("sbc"), QStringLiteral("SBC") },
        { QStringLiteral("sbc_xq"), QStringLiteral("SBC-XQ") },
        { QStringLiteral("aac"), QStringLiteral("AAC") },
        { QStringLiteral("aptx"), QStringLiteral("aptX") },
        { QStringLiteral("aptx_hd"), QStringLiteral("aptX HD") },
        { QStringLiteral("aptx_ll"), QStringLiteral("aptX LL") },
        { QStringLiteral("aptx_ll_duplex"), QStringLiteral("aptX LL") },
        { QStringLiteral("ldac"), QStringLiteral("LDAC") },
        { QStringLiteral("lc3"), QStringLiteral("LC3") },
        { QStringLiteral("faststream"), QStringLiteral("FastStream") },
        { QStringLiteral("opus_05"), QStringLiteral("Opus") },
    };
    return names.value(codec.toLower(), codec.toUpper());
}

QString
dictStr(const spa_dict *props, const char *key)
{
    const char *v = props ? spa_dict_lookup(props, key) : nullptr;
    return v ? QString::fromUtf8(v) : QString();
}

/* Fill the registry-props-derived fields of rec. */
void
fillFromProps(NodeRec &rec, const spa_dict *props)
{
    rec.name        = dictStr(props, PW_KEY_NODE_NAME);
    rec.description = dictStr(props, PW_KEY_NODE_DESCRIPTION);
    if (rec.description.isEmpty())
        rec.description = dictStr(props, PW_KEY_NODE_NICK);
    if (rec.description.isEmpty())
        rec.description = rec.name;
    rec.mediaClass = dictStr(props, PW_KEY_MEDIA_CLASS);
    rec.codec      = dictStr(props, "api.bluez5.codec");
    rec.deviceApi  = dictStr(props, "device.api");
    const QString ch = dictStr(props, "audio.channels");
    if (!ch.isEmpty())
        rec.channels = ch.toInt();
    const QString marker = dictStr(props, "pipeasio.node");
    rec.isPipeAsio       = (marker == QLatin1String("1"));
}

} // namespace

/* Per-bound-node listener context: lets the pw_node events know which node
 * they fire for and owns the hook/proxy lifetime. */
struct NodeBinding
{
    PipeWireGraphImpl *impl;
    uint32_t             id;
    pw_proxy           *proxy;
    spa_hook             listener{};
};

struct PipeWireGraphImpl
{
    PipeWireGraph   *q;
    pw_thread_loop  *loop     = nullptr;
    pw_context      *context  = nullptr;
    pw_core         *core     = nullptr;
    pw_registry     *registry = nullptr;
    spa_hook         registryListener{};
    spa_hook         coreListener{};
    int              syncSeq  = 0;
    bool             syncDone = false;
    bool             running  = false;

    QHash<uint32_t, NodeRec>      nodes;
    QHash<uint32_t, LinkRec>      links;
    QHash<uint32_t, NodeBinding *> bindings; /* bound Audio/* and own nodes */

    /* Profiler (pw-top's data source): one proxy to the daemon's Profiler
     * object, latest measurement of our target node. */
    pw_proxy  *profilerProxy = nullptr;
    uint32_t   profilerId    = SPA_ID_INVALID;
    spa_hook   profilerHook{};
    PipeWireGraph::ProfilerStats prof;
    uint32_t   profId = SPA_ID_INVALID; /* node id the prof stats belong to */
    QString    profName;                /* manual node_name target (else marker) */

    void noteChanged();
};

/* Queue a coalesced changed() emission on the GUI thread.  Loop thread. */
void
PipeWireGraphImpl::noteChanged()
{
    QPointer<PipeWireGraph> guard(q);
    QMetaObject::invokeMethod(
            q, [guard]() { if (guard) guard->kickCoalesce(); }, Qt::QueuedConnection);
}

/* --- node proxy events (loop thread) ------------------------------------- */

static void
graph_node_info(void *data, const pw_node_info *info)
{
    auto    *binding = static_cast<NodeBinding *>(data);
    NodeRec &rec     = binding->impl->nodes[binding->id];

    rec.state = nodeStateString(info->state);
    if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS)
        fillFromProps(rec, info->props);

    /* (Re)read the negotiated format whenever the param set changes. */
    if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS)
    {
        for (uint32_t i = 0; i < info->n_params; i++)
        {
            const spa_param_info &p = info->params[i];
            if (p.id == SPA_PARAM_Format && (p.flags & SPA_PARAM_INFO_READ))
                pw_node_enum_params(reinterpret_cast<pw_node *>(binding->proxy), 0,
                                    SPA_PARAM_Format, 0, 1, nullptr);
        }
    }
    binding->impl->noteChanged();
}

static void
graph_node_param(void *data, int /*seq*/, uint32_t id, uint32_t /*index*/, uint32_t /*next*/,
                 const spa_pod *param)
{
    if (id != SPA_PARAM_Format || !param)
        return;
    auto *binding = static_cast<NodeBinding *>(data);

    spa_audio_info_raw raw{};
    if (spa_format_audio_raw_parse(param, &raw) < 0)
        return;
    NodeRec &rec  = binding->impl->nodes[binding->id];
    rec.rate      = (int)raw.rate;
    rec.channels  = (int)raw.channels;
    const char *fmt = spa_debug_type_find_short_name(spa_type_audio_format, raw.format);
    rec.sampleFmt = fmt ? QString::fromUtf8(fmt) : QString();
    binding->impl->noteChanged();
}

static const pw_node_events nodeEvents = {
        .version = PW_VERSION_NODE_EVENTS,
        .info    = graph_node_info,
        .param   = graph_node_param,
};

/* --- profiler events (loop thread): per-cycle driver measurements -------- */

namespace
{

/* Driver-level block of a Profiler object: clock + error counters. */
struct DriverInfo
{
    int64_t  duration  = 0; /* frames per cycle (the QUANT column) */
    uint32_t rateNum   = 0; /* sample rate (the RATE column) */
    uint32_t rateDenom = 1;
    uint32_t xrunCount = 0;
    bool     haveClock = false;
};

} // namespace

static void
prof_process_info(const spa_pod *pod, DriverInfo *info)
{
    float    load[3];
    int32_t  xruns = 0;
    int64_t  count = 0;
    if (spa_pod_parse_struct(pod, SPA_POD_Long(&count), SPA_POD_Float(&load[0]),
                             SPA_POD_Float(&load[1]), SPA_POD_Float(&load[2]),
                             SPA_POD_Int(&xruns)) >= 0)
        info->xrunCount = (uint32_t)xruns;
}

static void
prof_process_clock(const spa_pod *pod, DriverInfo *info)
{
    spa_io_clock clock;
    if (spa_pod_parse_struct(pod, SPA_POD_Int(&clock.flags), SPA_POD_Int(&clock.id),
                             SPA_POD_Stringn(clock.name, sizeof(clock.name)),
                             SPA_POD_Long(&clock.nsec), SPA_POD_Fraction(&clock.rate),
                             SPA_POD_Long(&clock.position), SPA_POD_Long(&clock.duration),
                             SPA_POD_Long(&clock.delay), SPA_POD_Double(&clock.rate_diff),
                             SPA_POD_Long(&clock.next_nsec)) >= 0)
    {
        info->duration  = clock.duration;
        info->rateNum   = clock.rate.num;
        info->rateDenom = clock.rate.denom ? clock.rate.denom : 1;
        info->haveClock = true;
    }
}

static void
prof_process_block(PipeWireGraphImpl *impl, const spa_pod *pod, const DriverInfo &info)
{
    uint32_t            id     = 0;
    char               *name   = nullptr;
    int64_t             prev_signal = 0, signal = 0, awake = 0, finish = 0;
    int32_t             status = 0;
    struct spa_fraction latency = SPA_FRACTION(0, 1);
    uint32_t            xruns  = (uint32_t)-1;
    bool                async  = false;

    if (spa_pod_parse_struct(pod, SPA_POD_Int(&id), SPA_POD_String(&name),
                             SPA_POD_Long(&prev_signal), SPA_POD_Long(&signal),
                             SPA_POD_Long(&awake), SPA_POD_Long(&finish),
                             SPA_POD_Int(&status), SPA_POD_Fraction(&latency),
                             SPA_POD_OPT_Int(&xruns), SPA_POD_OPT_Bool(&async)) < 0)
        return;

    const NodeRec rec = impl->nodes.value(id);
    const bool    isTarget
            = rec.isPipeAsio || (!impl->profName.isEmpty() && rec.name == impl->profName);
    if (!isTarget)
        return;

    impl->profId       = id;
    impl->prof.found   = true;
    impl->prof.quantum = info.haveClock ? (int)info.duration : 0;
    /* spa_io_clock.rate is the PERIOD fraction (1/48000), so the Hz rate is
     * its denominator; pw-top prints them as QUANT/RATE = duration/denom. */
    impl->prof.rate    = info.haveClock ? (int)info.rateDenom : 0;
    /* B/Q of pw-top: busy (finish - awake) over the cycle period. */
    const double busyNs   = finish >= awake ? (double)(finish - awake) : 0.0;
    const double periodNs = info.haveClock && info.rateDenom
                                    ? 1e9 * (double)info.duration * info.rateNum / info.rateDenom
                                    : 0.0;
    impl->prof.dspLoad = periodNs > 0 ? busyNs / periodNs : 0.0;
    impl->prof.xruns   = xruns != (uint32_t)-1 ? (long)xruns : (long)info.xrunCount;
    impl->noteChanged();
}

static void
graph_profiler_profile(void *data, const spa_pod *pod)
{
    auto               *impl = static_cast<PipeWireGraphImpl *>(data);
    struct spa_pod     *o;
    struct spa_pod_prop *p;

    SPA_POD_STRUCT_FOREACH(pod, o)
    {
        if (!spa_pod_is_object_type(o, SPA_TYPE_OBJECT_Profiler))
            continue;

        DriverInfo info;
        SPA_POD_OBJECT_FOREACH((struct spa_pod_object *)o, p)
        {
            switch (p->key)
            {
            case SPA_PROFILER_info:
                prof_process_info(&p->value, &info);
                break;
            case SPA_PROFILER_clock:
                prof_process_clock(&p->value, &info);
                break;
            case SPA_PROFILER_driverBlock:
            case SPA_PROFILER_followerBlock:
                prof_process_block(impl, &p->value, info);
                break;
            default:
                break;
            }
        }
    }
}

static const pw_profiler_events profilerEvents = {
        PW_VERSION_PROFILER_EVENTS,
        graph_profiler_profile,
};

/* --- registry events (loop thread) --------------------------------------- */

static void
graph_registry_global(void *data, uint32_t id, uint32_t /*permissions*/, const char *type,
                      uint32_t /*version*/, const spa_dict *props)
{
    auto *impl = static_cast<PipeWireGraphImpl *>(data);

    if (!strcmp(type, PW_TYPE_INTERFACE_Node))
    {
        NodeRec rec;
        fillFromProps(rec, props);
        impl->nodes.insert(id, rec);

        /* Bind EVERY node: the registry global carries only a generic prop
         * subset for client nodes (7 generic keys for our flatpak FL64), so
         * media.class and the driver's "pipeasio.node" marker are only known
         * from the node's own info->props.  Binding also brings the state and
         * the negotiated format.  (pw-dump does the same per dump.) */
        auto *proxy = static_cast<pw_proxy *>(
                pw_registry_bind(impl->registry, id, PW_TYPE_INTERFACE_Node,
                                 PW_VERSION_NODE, 0));
        if (proxy)
        {
            auto *binding = new NodeBinding{ impl, id, proxy, {} };
            impl->bindings.insert(id, binding);
            pw_node_add_listener(reinterpret_cast<pw_node *>(proxy), &binding->listener,
                                 &nodeEvents, binding);
        }
        impl->noteChanged();
    }
    else if (!strcmp(type, PW_TYPE_INTERFACE_Link))
    {
        LinkRec rec;
        rec.outNode = dictStr(props, PW_KEY_LINK_OUTPUT_NODE).toUInt();
        rec.inNode  = dictStr(props, PW_KEY_LINK_INPUT_NODE).toUInt();
        impl->links.insert(id, rec);
        impl->noteChanged();
    }
    else if (!strcmp(type, PW_TYPE_INTERFACE_Profiler) && !impl->profilerProxy)
    {
        /* The daemon loads module-profiler by default; bind its Profiler
         * object for per-cycle driver measurements (pw-top's data). */
        impl->profilerProxy = static_cast<pw_proxy *>(
                pw_registry_bind(impl->registry, id, type, PW_VERSION_PROFILER, 0));
        if (impl->profilerProxy)
        {
            impl->profilerId = id;
            pw_profiler_add_listener(reinterpret_cast<pw_profiler *>(impl->profilerProxy),
                                     &impl->profilerHook, &profilerEvents, impl);
        }
    }
}

static void
graph_registry_global_remove(void *data, uint32_t id)
{
    auto *impl = static_cast<PipeWireGraphImpl *>(data);
    impl->nodes.remove(id);
    impl->links.remove(id);
    if (id == impl->profilerId)
    {
        pw_proxy_destroy(impl->profilerProxy);
        impl->profilerProxy = nullptr;
        impl->profilerId    = SPA_ID_INVALID;
        impl->prof          = {};
        impl->profId        = SPA_ID_INVALID;
    }
    if (NodeBinding *binding = impl->bindings.take(id))
    {
        pw_proxy_destroy(binding->proxy);
        delete binding;
    }
    impl->noteChanged();
}

static const pw_registry_events registryEvents = {
        .version        = PW_VERSION_REGISTRY_EVENTS,
        .global         = graph_registry_global,
        .global_remove  = graph_registry_global_remove,
};

/* --- core events: initial registry roundtrip ------------------------------ */

static void
graph_core_done(void *data, uint32_t id, int seq)
{
    auto *impl = static_cast<PipeWireGraphImpl *>(data);
    if (id == PW_ID_CORE && seq == impl->syncSeq)
    {
        impl->syncDone = true;
        pw_thread_loop_signal(impl->loop, false);
    }
}

static const pw_core_events coreEvents = {
        .version = PW_VERSION_CORE_EVENTS,
        .done    = graph_core_done,
};

/* --- public API ------------------------------------------------------------ */

PipeWireGraph::PipeWireGraph(QObject *parent) : QObject(parent), m_impl(new PipeWireGraphImpl)
{
    m_impl->q = this;

    /* Coalesce timer lives on the GUI thread; kicked by Impl::noteChanged. */
    m_coalesce = new QTimer(this);
    m_coalesce->setSingleShot(true);
    m_coalesce->setInterval(150);
    connect(m_coalesce, &QTimer::timeout, this, &PipeWireGraph::changed);
}

void
PipeWireGraph::kickCoalesce()
{
    /* Fire-and-cap, NOT restart: profiler events arrive per audio cycle
     * (~200/s) and a restarting timer would never fire.  One kick starts the
     * timer; further kicks while it runs are coalesced away. */
    if (!m_coalesce->isActive())
        m_coalesce->start();
}

PipeWireGraph::~PipeWireGraph()
{
    stop();
    delete m_impl;
}

void
PipeWireGraph::start()
{
    if (m_impl->running)
        return;
    ensurePwInit();

    m_impl->loop = pw_thread_loop_new("pipeasio-panel", nullptr);
    if (!m_impl->loop)
        return;
    m_impl->context = pw_context_new(pw_thread_loop_get_loop(m_impl->loop), nullptr, 0);
    if (!m_impl->context)
    {
        pw_thread_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
        return;
    }
    /* Registers the Profiler interface client-side; without it binding the
     * daemon's Profiler object fails (unknown interface type). */
    pw_context_load_module(m_impl->context, PW_EXTENSION_MODULE_PROFILER, NULL, NULL);

    pw_thread_loop_lock(m_impl->loop);
    /* The daemon's Profiler object lives on the manager socket: plain clients
     * can see the global but binding it returns NULL.  Connect with manager
     * intention (what pw-top does); fall back to a plain connection. */
    m_impl->core = pw_context_connect(
            m_impl->context,
            pw_properties_new(PW_KEY_REMOTE_INTENTION, "manager", PW_KEY_REMOTE_NAME,
                              "[" PW_DEFAULT_REMOTE "-manager," PW_DEFAULT_REMOTE "]", NULL),
            0);
    if (!m_impl->core)
        m_impl->core = pw_context_connect(m_impl->context, nullptr, 0);
    if (m_impl->core)
    {
        m_impl->registry = pw_core_get_registry(m_impl->core, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(m_impl->registry, &m_impl->registryListener,
                                 &registryEvents, m_impl);
        pw_core_add_listener(m_impl->core, &m_impl->coreListener, &coreEvents,
                             m_impl);
        m_impl->running = true;

        /* Wait (bounded) for the initial registry fill so early callers like
         * the Settings tab combos see a complete graph, not an empty one. */
        m_impl->syncDone = false;
        m_impl->syncSeq  = pw_core_sync(m_impl->core, PW_ID_CORE, 0);
        pw_thread_loop_start(m_impl->loop);
        int tries = 4; /* 4 x 2 s worst case, normally milliseconds */
        while (!m_impl->syncDone && tries-- > 0)
            pw_thread_loop_timed_wait(m_impl->loop, 2);
        spa_hook_remove(&m_impl->coreListener);
    }
    pw_thread_loop_unlock(m_impl->loop);

    if (!m_impl->running)
    {
        pw_context_destroy(m_impl->context);
        m_impl->context = nullptr;
        pw_thread_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
    }
}

void
PipeWireGraph::stop()
{
    if (!m_impl->loop)
        return;
    if (m_impl->running)
    {
        pw_thread_loop_stop(m_impl->loop);
        m_impl->running = false;
    }

    pw_thread_loop_lock(m_impl->loop);
    spa_hook_remove(&m_impl->registryListener);
    if (m_impl->profilerProxy)
    {
        pw_proxy_destroy(m_impl->profilerProxy);
        m_impl->profilerProxy = nullptr;
        m_impl->profilerId    = SPA_ID_INVALID;
    }
    const auto bindings = m_impl->bindings;
    m_impl->bindings.clear();
    for (NodeBinding *binding : bindings)
    {
        pw_proxy_destroy(binding->proxy);
        delete binding;
    }
    if (m_impl->core)
        pw_core_disconnect(m_impl->core);
    m_impl->core     = nullptr;
    m_impl->registry = nullptr;
    m_impl->nodes.clear();
    m_impl->links.clear();
    pw_thread_loop_unlock(m_impl->loop);

    pw_context_destroy(m_impl->context);
    m_impl->context = nullptr;
    pw_thread_loop_destroy(m_impl->loop);
    m_impl->loop = nullptr;
}

QList<PipeWireGraph::Device>
PipeWireGraph::audioDevices()
{
    QList<Device> out;
    if (!m_impl->running)
        return out;

    pw_thread_loop_lock(m_impl->loop);
    for (const NodeRec &rec : m_impl->nodes)
    {
        Device d;
        if (rec.mediaClass.startsWith(QLatin1String("Audio/Sink")))
            d.isSink = true;
        else if (rec.mediaClass.startsWith(QLatin1String("Audio/Source")))
            d.isSink = false;
        else
            continue;
        d.name        = rec.name;
        d.description = rec.description;
        out.append(d);
    }
    pw_thread_loop_unlock(m_impl->loop);

    std::sort(out.begin(), out.end(),
              [](const Device &a, const Device &b) { return a.name < b.name; });
    return out;
}

QString
PipeWireGraph::ownNodeName()
{
    QString out;
    if (!m_impl->running)
        return out;

    pw_thread_loop_lock(m_impl->loop);
    for (const NodeRec &rec : m_impl->nodes)
        if (rec.isPipeAsio)
        {
            out = rec.name;
            break;
        }
    pw_thread_loop_unlock(m_impl->loop);
    return out;
}

/* Split a peer node into a display name and a detail line (codec / negotiated
 * rate / channels+format / state), like the old describePeer() did. */
static void
describePeer(const NodeRec &rec, QString *name, QString *detail)
{
    *name = rec.description;

    QStringList attrs;
    if (!rec.codec.isEmpty())
        attrs << prettyBtCodec(rec.codec);
    else if (rec.deviceApi == QLatin1String("bluez5"))
        attrs << QStringLiteral("Bluetooth");
    if (rec.rate > 0)
        attrs << QStringLiteral("%1 Hz").arg(rec.rate);
    if (rec.channels > 0)
    {
        QString ch = QStringLiteral("%1 ch").arg(rec.channels);
        if (!rec.sampleFmt.isEmpty())
            ch += QLatin1Char(' ') + rec.sampleFmt;
        attrs << ch;
    }
    if (!rec.state.isEmpty())
        attrs << rec.state;
    *detail = attrs.join(QStringLiteral(" · "));
}

PipeWireGraph::Connections
PipeWireGraph::ownConnections()
{
    Connections conn;
    if (!m_impl->running)
        return conn;

    pw_thread_loop_lock(m_impl->loop);

    uint32_t ownId = SPA_ID_INVALID;
    for (auto it = m_impl->nodes.constBegin(); it != m_impl->nodes.constEnd(); ++it)
        if (it->isPipeAsio)
        {
            ownId = it.key();
            break;
        }
    if (ownId != SPA_ID_INVALID)
    {
        /* A link FROM our node lands on a sink we play to; a link TO our node
         * comes from a source we capture from. */
        QList<uint32_t> outIds, inIds;
        for (const LinkRec &link : m_impl->links)
        {
            if (link.outNode == ownId && m_impl->nodes.contains(link.inNode)
                && !outIds.contains(link.inNode))
                outIds.append(link.inNode);
            if (link.inNode == ownId && m_impl->nodes.contains(link.outNode)
                && !inIds.contains(link.outNode))
                inIds.append(link.outNode);
        }

        const auto fill = [&](const QList<uint32_t> &ids, QString *name, QString *detail) {
            if (ids.isEmpty())
                return;
            if (ids.size() == 1)
            {
                describePeer(m_impl->nodes.value(ids.first()), name, detail);
                return;
            }
            QStringList names;
            for (uint32_t id : ids)
            {
                QString n, d;
                describePeer(m_impl->nodes.value(id), &n, &d);
                names << n;
            }
            *name = names.join(QStringLiteral(", "));
        };
        fill(outIds, &conn.output, &conn.outputDetail);
        fill(inIds, &conn.input, &conn.inputDetail);
    }

    pw_thread_loop_unlock(m_impl->loop);
    return conn;
}

/* pw-top's S column letter for a node state string. */
static QString
stateLetter(const QString &state)
{
    if (state == QLatin1String("running"))
        return QStringLiteral("R");
    if (state == QLatin1String("idle"))
        return QStringLiteral("I");
    if (state == QLatin1String("suspended"))
        return QStringLiteral("S");
    if (state == QLatin1String("error"))
        return QStringLiteral("E");
    if (state == QLatin1String("creating"))
        return QStringLiteral("C");
    return state;
}

PipeWireGraph::ProfilerStats
PipeWireGraph::profilerStats(const QString &nodeName)
{
    ProfilerStats out;
    if (!m_impl->running)
        return out;

    pw_thread_loop_lock(m_impl->loop);
    m_impl->profName = nodeName; /* manual target for the profile matcher */

    uint32_t target = SPA_ID_INVALID;
    for (auto it = m_impl->nodes.constBegin(); it != m_impl->nodes.constEnd(); ++it)
        if (nodeName.isEmpty() ? it->isPipeAsio : it->name == nodeName)
        {
            target = it.key();
            break;
        }
    if (target != SPA_ID_INVALID && target == m_impl->profId && m_impl->prof.found)
    {
        out       = m_impl->prof;
        out.state = stateLetter(m_impl->nodes.value(target).state);
    }
    pw_thread_loop_unlock(m_impl->loop);
    return out;
}
