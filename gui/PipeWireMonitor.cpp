/*
 * PipeWireMonitor.cpp - implementation.
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
#include "PipeWireMonitor.hpp"
#include "ProfilerParse.hpp"

#include <QElapsedTimer>
#include <QLatin1Char>
#include <QMap>
#include <QSocketNotifier>
#include <QStringList>
#include <QTimer>

#include <pipewire/pipewire.h>
/* Both must follow pipewire.h: profiler.h uses spa_api_method_r from
 * spa/utils/hook.h without including it, and PW_EXTENSION_MODULE_PROFILER
 * expands to PIPEWIRE_MODULE_PREFIX from impl-module.h. */
#include <pipewire/impl-module.h>
#include <pipewire/extensions/profiler.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-types.h>
#include <spa/param/format-utils.h>
#include <spa/utils/string.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>

namespace
{

/* Flush accumulated cycles to the UI at 10 Hz. Fast enough to show the load
 * transients that precede an xrun, slow enough that a 375 Hz cycle rate (128
 * frames at 48 kHz) does not drive a repaint per cycle. */
constexpr int kEmitIntervalMs = 100;

/* A node with no profiler block for this long counts as not running. Matches
 * pw-top's refresh granularity and still spans several cycles at the slowest
 * usable quantum (8192 at 44.1 kHz is 186 ms/cycle). */
constexpr int kStaleMs = 1000;

constexpr int kReconnectMs = 2000;

const char *const kMarkerProp = "pipeasio.node";

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

/* pw-top's S column. */
QString
stateLetter(int state, uint32_t transport)
{
    switch (state)
    {
    case PW_NODE_STATE_ERROR:
        return QStringLiteral("E");
    case PW_NODE_STATE_CREATING:
        return QStringLiteral("C");
    case PW_NODE_STATE_SUSPENDED:
        return QStringLiteral("S");
    case PW_NODE_STATE_IDLE:
        return QStringLiteral("I");
    case PW_NODE_STATE_RUNNING:
        switch (transport)
        {
        case SPA_IO_POSITION_STATE_STARTING:
            return QStringLiteral("t");
        case SPA_IO_POSITION_STATE_RUNNING:
            return QStringLiteral("T");
        default:
            return QStringLiteral("R");
        }
    default:
        return QStringLiteral("!");
    }
}

QString
dictString(const spa_dict *props, const char *key)
{
    const char *v = props ? spa_dict_lookup(props, key) : nullptr;
    return v ? QString::fromUtf8(v) : QString();
}

/* pw_init() is refcounted but the panel never unloads PipeWire, so one
 * process-wide init is enough. */
void
ensurePwInit()
{
    static const bool done = []
    {
        pw_init(nullptr, nullptr);
        return true;
    }();
    (void)done;
}

} // namespace

void
describePeer(const PeerInfo &peer, QString *name, QString *detail)
{
    QString n = peer.description;
    if (n.isEmpty())
        n = peer.nick;
    if (n.isEmpty())
        n = peer.name;
    *name = n;

    QStringList attrs;
    if (!peer.btCodec.isEmpty())
        attrs << prettyBtCodec(peer.btCodec);
    else if (peer.bluetooth)
        attrs << QStringLiteral("Bluetooth");

    if (peer.rate > 0)
        attrs << QStringLiteral("%1 Hz").arg(peer.rate);
    if (peer.channels > 0)
    {
        QString ch = QStringLiteral("%1 ch").arg(peer.channels);
        if (!peer.sampleFormat.isEmpty())
            ch += QLatin1Char(' ') + peer.sampleFormat;
        attrs << ch;
    }
    if (!peer.state.isEmpty())
        attrs << peer.state;

    *detail = attrs.join(QStringLiteral(" \u00b7 "));
}

Connections
resolveConnections(uint32_t ownId, const QHash<uint32_t, PeerInfo> &nodes,
                   const QVector<GraphLink> &links)
{
    Connections conn;

    /* A link FROM our node lands on a sink we play to, a link TO our node comes
     * from a source we capture from. Distinct peers, link order kept. */
    QVector<uint32_t> outIds, inIds;
    for (const GraphLink &l : links)
    {
        if (l.outputNode == ownId && nodes.contains(l.inputNode) && !outIds.contains(l.inputNode))
            outIds.append(l.inputNode);
        if (l.inputNode == ownId && nodes.contains(l.outputNode) && !inIds.contains(l.outputNode))
            inIds.append(l.outputNode);
    }

    /* One peer per side is the norm (our N ports fan into a single device); show
     * its name and detail separately so the UI can stack them. Several distinct
     * peers (manual patching) collapse to a names-only list, no detail. */
    const auto fill = [&](const QVector<uint32_t> &ids, QString *name, QString *detail)
    {
        if (ids.isEmpty())
            return;
        if (ids.size() == 1)
        {
            describePeer(nodes.value(ids.first()), name, detail);
            return;
        }
        QStringList names;
        for (uint32_t id : ids)
        {
            QString n, d;
            describePeer(nodes.value(id), &n, &d);
            names << n;
        }
        *name = names.join(QStringLiteral(", "));
    };
    fill(outIds, &conn.output, &conn.outputDetail);
    fill(inIds, &conn.input, &conn.inputDetail);
    return conn;
}

struct NodeRecord
{
    uint32_t               id             = 0;
    bool                   isOurs         = false;
    int                    state          = PW_NODE_STATE_CREATING;
    int                    formatChannels = 0;
    int                    propChannels   = 0;
    PeerInfo               peer;
    pw_proxy              *proxy = nullptr;
    spa_hook               proxyListener{};
    spa_hook               objectListener{};
    PipeWireMonitor::Impl *impl = nullptr;
};

struct PipeWireMonitor::Impl
{
    PipeWireMonitor *q = nullptr;

    QString target;
    bool    autoDiscover = true;
    bool    running      = false;

    /* connection */
    pw_loop         *loop       = nullptr;
    pw_context      *context    = nullptr;
    pw_core         *core       = nullptr;
    pw_registry     *registry   = nullptr;
    pw_proxy        *profiler   = nullptr;
    uint32_t         profilerId = SPA_ID_INVALID;
    spa_hook         coreListener{};
    spa_hook         registryListener{};
    spa_hook         profilerListener{};
    QSocketNotifier *notifier         = nullptr;
    bool             entered          = false;
    int              syncSeq          = 0;
    bool             pendingReconnect = false;

    QTimer *emitTimer      = nullptr;
    QTimer *reconnectTimer = nullptr;

    /* graph - ordered so own-node selection and multi-peer lists are stable */
    QMap<uint32_t, NodeRecord *> nodes;
    QMap<uint32_t, GraphLink>    links;
    bool                         graphDirty = true;
    Connections                  conns;

    /* accumulated cycles */
    QElapsedTimer lastBlock;
    CycleTiming   lastTiming;
    double        windowPeak      = 0.0;
    bool          windowHasCycles = false;
    double        lastBusy        = 0.0;
    uint32_t      transportState  = 0;

    QString unavailable;

    void connectToDaemon();
    void teardown();
    void fail(const QString &reason);
    void drain();
    void tick();

    NodeRecord *findOwn() const;
    void        addNode(uint32_t id, const spa_dict *props);
    void        applyProps(NodeRecord *n, const spa_dict *props);
    void        refreshConnections();

    void onGlobal(uint32_t id, const char *type, const spa_dict *props);
    void onGlobalRemove(uint32_t id);
    void onProfile(const spa_pod *pod);
};

namespace
{

/* Event vtables, built by lambda: C++17 has no designated initializers, and
 * positional init would silently shift a callback if upstream adds a field. */
const pw_proxy_events kProxyEvents = []
{
    pw_proxy_events e{};
    e.version = PW_VERSION_PROXY_EVENTS;
    e.removed = [](void *data)
    {
        auto *n = static_cast<NodeRecord *>(data);
        pw_proxy_destroy(n->proxy);
    };
    e.destroy = [](void *data)
    {
        auto *n  = static_cast<NodeRecord *>(data);
        n->proxy = nullptr;
        spa_hook_remove(&n->proxyListener);
        spa_hook_remove(&n->objectListener);
    };
    return e;
}();

const pw_node_events kNodeEvents = []
{
    pw_node_events e{};
    e.version = PW_VERSION_NODE_EVENTS;
    e.info    = [](void *data, const pw_node_info *info)
    {
        auto *n  = static_cast<NodeRecord *>(data);
        n->state = info->state;
        if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS)
            n->impl->applyProps(n, info->props);
        n->peer.state       = QString::fromUtf8(pw_node_state_as_string(info->state));
        n->impl->graphDirty = true;
    };
    e.param = [](void *data, int, uint32_t id, uint32_t, uint32_t, const spa_pod *param)
    {
        auto *n = static_cast<NodeRecord *>(data);
        if (id != SPA_PARAM_Format)
            return;
        n->peer.rate      = 0;
        n->formatChannels = 0;
        n->peer.sampleFormat.clear();
        uint32_t mediaType = 0, mediaSubtype = 0;
        if (param && spa_format_parse(param, &mediaType, &mediaSubtype) >= 0
            && mediaType == SPA_MEDIA_TYPE_audio && mediaSubtype == SPA_MEDIA_SUBTYPE_raw)
        {
            spa_audio_info_raw raw{};
            if (spa_format_audio_raw_parse(param, &raw) >= 0)
            {
                n->peer.rate      = static_cast<int>(raw.rate);
                n->formatChannels = static_cast<int>(raw.channels);
                n->peer.sampleFormat
                        = QString::fromUtf8(spa_type_audio_format_to_short_name(raw.format));
            }
        }
        /* A negotiated Format wins over the node's advertised channel count. */
        n->peer.channels    = n->formatChannels ? n->formatChannels : n->propChannels;
        n->impl->graphDirty = true;
    };
    return e;
}();

const pw_registry_events kRegistryEvents = []
{
    pw_registry_events e{};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global =
            [](void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props)
    { static_cast<PipeWireMonitor::Impl *>(data)->onGlobal(id, type, props); };
    e.global_remove = [](void *data, uint32_t id)
    { static_cast<PipeWireMonitor::Impl *>(data)->onGlobalRemove(id); };
    return e;
}();

const pw_core_events kCoreEvents = []
{
    pw_core_events e{};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done    = [](void *data, uint32_t id, int seq)
    {
        auto *d = static_cast<PipeWireMonitor::Impl *>(data);
        if (id != PW_ID_CORE || seq != d->syncSeq)
            return;
        /* The initial roundtrip has replayed every global. No Profiler means
         * module-profiler is not loaded and there is no telemetry to be had. */
        d->unavailable = d->profiler ? QString()
                                     : QStringLiteral("no Profiler interface "
                                                      "(load PipeWire's module-profiler)");
    };
    e.error = [](void *data, uint32_t id, int, int res, const char *)
    {
        /* -EPIPE on the core means the daemon went away; everything else is a
         * per-object complaint we cannot act on. */
        if (id == PW_ID_CORE && res == -EPIPE)
            static_cast<PipeWireMonitor::Impl *>(data)->pendingReconnect = true;
    };
    return e;
}();

const pw_profiler_events kProfilerEvents = []
{
    pw_profiler_events e{};
    e.version = PW_VERSION_PROFILER_EVENTS;
    e.profile = [](void *data, const spa_pod *pod)
    { static_cast<PipeWireMonitor::Impl *>(data)->onProfile(pod); };
    return e;
}();

} // namespace

void
PipeWireMonitor::Impl::connectToDaemon()
{
    ensurePwInit();

    loop = pw_loop_new(nullptr);
    if (!loop)
    {
        fail(QStringLiteral("cannot create a PipeWire loop"));
        return;
    }
    context = pw_context_new(loop, nullptr, 0);
    if (!context)
    {
        fail(QStringLiteral("cannot create a PipeWire context"));
        return;
    }
    /* The Profiler interface is an extension: its protocol-native marshal ships
     * in module-profiler, so a client that does not load it locally gets a NULL
     * proxy back from pw_registry_bind. */
    pw_context_load_module(context, PW_EXTENSION_MODULE_PROFILER, nullptr, nullptr);

    /* "manager" asks for the whole graph, not the subset a playback client
     * would see. */
    core = pw_context_connect(context,
                              pw_properties_new(PW_KEY_REMOTE_INTENTION, "manager", nullptr), 0);
    if (!core)
    {
        fail(QStringLiteral("no PipeWire daemon"));
        return;
    }
    pw_core_add_listener(core, &coreListener, &kCoreEvents, this);

    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!registry)
    {
        fail(QStringLiteral("cannot get the PipeWire registry"));
        return;
    }
    pw_registry_add_listener(registry, &registryListener, &kRegistryEvents, this);
    syncSeq = pw_core_sync(core, PW_ID_CORE, 0);

    pw_loop_enter(loop);
    entered = true;

    /* Drive the loop from Qt: one non-blocking iterate per readable fd. The
     * loop's epoll fd is level-triggered, so anything left over re-arms us. */
    notifier = new QSocketNotifier(pw_loop_get_fd(loop), QSocketNotifier::Read, q);
    QObject::connect(notifier, &QSocketNotifier::activated, q, [this] { drain(); });
}

void
PipeWireMonitor::Impl::teardown()
{
    if (notifier)
    {
        notifier->setEnabled(false);
        notifier->deleteLater();
        notifier = nullptr;
    }
    for (NodeRecord *n : nodes)
    {
        if (n->proxy)
            pw_proxy_destroy(n->proxy);
        delete n;
    }
    nodes.clear();
    links.clear();
    conns      = Connections();
    graphDirty = true;

    if (profiler)
    {
        pw_proxy_destroy(profiler);
        profiler = nullptr;
    }
    profilerId = SPA_ID_INVALID;
    if (registry)
    {
        spa_hook_remove(&registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
        registry = nullptr;
    }
    if (core)
    {
        spa_hook_remove(&coreListener);
        pw_core_disconnect(core);
        core = nullptr;
    }
    if (context)
    {
        pw_context_destroy(context);
        context = nullptr;
    }
    if (loop)
    {
        if (entered)
        {
            pw_loop_leave(loop);
            entered = false;
        }
        pw_loop_destroy(loop);
        loop = nullptr;
    }

    lastBlock.invalidate();
    lastTiming       = CycleTiming();
    windowPeak       = 0.0;
    windowHasCycles  = false;
    lastBusy         = 0.0;
    transportState   = 0;
    pendingReconnect = false;
}

void
PipeWireMonitor::Impl::fail(const QString &reason)
{
    teardown();
    unavailable = reason;
    if (running)
        reconnectTimer->start(kReconnectMs);
}

void
PipeWireMonitor::Impl::drain()
{
    if (!loop)
        return;
    const int res = pw_loop_iterate(loop, 0);
    if (res < 0 && res != -EINTR)
        pendingReconnect = true;

    /* Never tear the connection down inside a PipeWire callback or inside the
     * notifier that is dispatching it - unwind to the Qt loop first. */
    if (pendingReconnect)
    {
        pendingReconnect = false;
        QTimer::singleShot(0, q, [this] { fail(QStringLiteral("PipeWire daemon went away")); });
    }
}

NodeRecord *
PipeWireMonitor::Impl::findOwn() const
{
    for (NodeRecord *n : nodes)
    {
        if (autoDiscover)
        {
            if (n->isOurs)
                return n;
        }
        else if (!target.isEmpty() && n->peer.name.contains(target))
            return n;
    }
    return nullptr;
}

void
PipeWireMonitor::Impl::applyProps(NodeRecord *n, const spa_dict *props)
{
    if (!props)
        return;
    n->peer.name        = dictString(props, PW_KEY_NODE_NAME);
    n->peer.description = dictString(props, PW_KEY_NODE_DESCRIPTION);
    n->peer.nick        = dictString(props, PW_KEY_NODE_NICK);
    n->peer.btCodec     = dictString(props, "api.bluez5.codec");
    n->peer.bluetooth   = dictString(props, PW_KEY_DEVICE_API) == QLatin1String("bluez5");
    n->propChannels     = dictString(props, "audio.channels").toInt();
    n->peer.channels    = n->formatChannels ? n->formatChannels : n->propChannels;

    /* The driver tags its own filter node so the panel finds it no matter what
     * the ASIO host named the process. */
    const char *marker = spa_dict_lookup(props, kMarkerProp);
    n->isOurs          = marker && spa_streq(marker, "1");
}

void
PipeWireMonitor::Impl::addNode(uint32_t id, const spa_dict *props)
{
    if (nodes.contains(id))
        return;

    auto *n = new NodeRecord;
    n->id   = id;
    n->impl = this;
    applyProps(n, props);

    n->proxy = static_cast<pw_proxy *>(
            pw_registry_bind(registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (n->proxy)
    {
        pw_proxy_add_listener(n->proxy, &n->proxyListener, &kProxyEvents, n);
        pw_proxy_add_object_listener(n->proxy, &n->objectListener, &kNodeEvents, n);
        uint32_t ids[1] = { SPA_PARAM_Format };
        pw_node_subscribe_params(reinterpret_cast<pw_node *>(n->proxy), ids, 1);
    }
    nodes.insert(id, n);
    graphDirty = true;
}

void
PipeWireMonitor::Impl::onGlobal(uint32_t id, const char *type, const spa_dict *props)
{
    if (spa_streq(type, PW_TYPE_INTERFACE_Node))
    {
        addNode(id, props);
        return;
    }
    if (spa_streq(type, PW_TYPE_INTERFACE_Link))
    {
        const char *out = props ? spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE) : nullptr;
        const char *in  = props ? spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE) : nullptr;
        if (!out || !in)
            return;
        GraphLink l;
        l.outputNode = static_cast<uint32_t>(std::strtoul(out, nullptr, 10));
        l.inputNode  = static_cast<uint32_t>(std::strtoul(in, nullptr, 10));
        links.insert(id, l);
        graphDirty = true;
        return;
    }
    if (spa_streq(type, PW_TYPE_INTERFACE_Profiler) && !profiler)
    {
        profiler = static_cast<pw_proxy *>(
                pw_registry_bind(registry, id, type, PW_VERSION_PROFILER, 0));
        if (!profiler)
            return;
        profilerId = id;
        pw_proxy_add_object_listener(profiler, &profilerListener, &kProfilerEvents, this);
        unavailable.clear();
    }
}

void
PipeWireMonitor::Impl::onGlobalRemove(uint32_t id)
{
    if (auto it = nodes.find(id); it != nodes.end())
    {
        NodeRecord *n = it.value();
        if (n->proxy)
            pw_proxy_destroy(n->proxy);
        delete n;
        nodes.erase(it);
        graphDirty = true;
    }
    if (links.remove(id) > 0)
        graphDirty = true;
    if (id == profilerId)
    {
        pw_proxy_destroy(profiler);
        profiler    = nullptr;
        profilerId  = SPA_ID_INVALID;
        unavailable = QStringLiteral("no Profiler interface "
                                     "(load PipeWire's module-profiler)");
    }
}

void
PipeWireMonitor::Impl::onProfile(const spa_pod *pod)
{
    const NodeRecord *own = findOwn();
    if (!own)
        return;

    for (const ProfilePoint &point : parseProfilePods(pod))
    {
        for (const ProfilerBlock &block : point.blocks)
        {
            if (block.id != own->id)
                continue;
            lastTiming     = deriveTiming(point.info, block);
            transportState = point.info.transportState;
            lastBlock.start();
            if (lastTiming.busyNs >= 0)
            {
                lastBusy        = lastTiming.busyRatio;
                windowPeak      = std::max(windowPeak, lastTiming.busyRatio);
                windowHasCycles = true;
            }
            break;
        }
    }
}

void
PipeWireMonitor::Impl::refreshConnections()
{
    graphDirty = false;
    conns      = Connections();

    const NodeRecord *own = findOwn();
    if (!own)
        return;

    QHash<uint32_t, PeerInfo> peers;
    peers.reserve(nodes.size());
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it)
        peers.insert(it.key(), it.value()->peer);

    QVector<GraphLink> ls;
    ls.reserve(links.size());
    for (const GraphLink &l : links)
        ls.append(l);

    conns = resolveConnections(own->id, peers, ls);
}

void
PipeWireMonitor::Impl::tick()
{
    if (graphDirty)
        refreshConnections();

    NodeStats st;
    st.unavailable = unavailable;

    if (const NodeRecord *own = findOwn())
    {
        st.found = true;
        st.name  = own->peer.name;
        st.state = stateLetter(own->state, transportState);
        st.xruns = lastTiming.xruns;
        /* Stale measurements read as an idle node (quantum/rate 0), which is
         * what the panel's idle branch expects. */
        if (lastBlock.isValid() && lastBlock.elapsed() < kStaleMs)
        {
            st.quantum = lastTiming.quantum;
            st.rate    = lastTiming.rate;
            /* Peak over the window, not a mean: a 100 ms average hides the
             * load spikes that precede an xrun. Windows with no cycle at all
             * (very large quanta) repeat the last known load. */
            st.dspLoad = windowHasCycles ? windowPeak : lastBusy;
        }
        st.outputDevice       = conns.output;
        st.outputDeviceDetail = conns.outputDetail;
        st.inputDevice        = conns.input;
        st.inputDeviceDetail  = conns.inputDetail;
    }

    windowPeak      = 0.0;
    windowHasCycles = false;
    emit q->updated(st);
}

PipeWireMonitor::PipeWireMonitor(QObject *parent) : QObject(parent), d(std::make_unique<Impl>())
{
    d->q         = this;
    d->emitTimer = new QTimer(this);
    d->emitTimer->setInterval(kEmitIntervalMs);
    connect(d->emitTimer, &QTimer::timeout, this, [this] { d->tick(); });

    d->reconnectTimer = new QTimer(this);
    d->reconnectTimer->setSingleShot(true);
    connect(d->reconnectTimer, &QTimer::timeout, this,
            [this]
            {
                if (d->running && !d->core)
                    d->connectToDaemon();
            });
}

PipeWireMonitor::~PipeWireMonitor()
{
    d->running = false;
    d->reconnectTimer->stop();
    d->teardown();
}

void
PipeWireMonitor::setTarget(const QString &nodeNameSubstr)
{
    d->target       = nodeNameSubstr;
    d->autoDiscover = nodeNameSubstr.isEmpty();
    d->graphDirty   = true;
}

void
PipeWireMonitor::start()
{
    if (d->running)
        return;
    d->running = true;
    d->connectToDaemon();
    d->emitTimer->start();
}

void
PipeWireMonitor::stop()
{
    d->running = false;
    d->emitTimer->stop();
    d->reconnectTimer->stop();
    d->teardown();
}
