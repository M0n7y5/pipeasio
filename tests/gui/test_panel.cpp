/*
 * test_panel.cpp - unit tests for the Qt panel:
 *   - Config::serializeIni / parseIni round-trip
 *   - cross-language: panel-written INI parsed by the driver's C reader
 *   - DeviceEnumerator::parsePwDump (fixture)
 *   - the Profiler POD contract and its pw-top-compatible derivations
 *   - SettingsDialog behavior (device loading state, tooltip wrapping,
 *     monitor transient hold)
 *
 * Runnable headless: CTest sets QT_QPA_PLATFORM=offscreen.
 */
#include "Config.hpp"
#include "DeviceEnumerator.hpp"
#include "PipeWireMonitor.hpp"
#include "SettingsDialog.hpp"
#include "ProfilerParse.hpp"

#include <spa/node/io.h>
#include <spa/param/profiler.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTabWidget>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern "C"
{
#include "pipeasio_config.h"
#include "pipeasio_parse.h"
}

static int g_total = 0;
static int g_fail  = 0;

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        g_total++;                                                                                 \
        if (!(cond))                                                                               \
        {                                                                                          \
            g_fail++;                                                                              \
            std::fprintf(stderr, "  FAIL %s:%d CHECK(%s)\n", __FILE__, __LINE__, #cond);           \
        }                                                                                          \
    } while (0)

static void
test_config_roundtrip()
{
    pipeasio_config c     = Config::defaults();
    c.inputs              = 6;
    c.outputs             = 2;
    c.buffer_size         = 512;
    c.fixed_buffer_size   = false;
    c.sample_rate         = 96000;
    c.auto_connect        = false;
    c.follow_device_clock = true;
    c.realtime            = true;
    std::strcpy(c.output_device, "alsa_output.x");
    std::strcpy(c.input_device, "alsa_input.y");
    std::strcpy(c.node_name, "Foo");

    const pipeasio_config r = Config::parseIni(Config::serializeIni(c));
    CHECK(r.inputs == 6);
    CHECK(r.outputs == 2);
    CHECK(r.buffer_size == 512);
    CHECK(r.fixed_buffer_size == false);
    CHECK(r.sample_rate == 96000);
    CHECK(r.auto_connect == false);
    CHECK(r.follow_device_clock == true);
    CHECK(r.realtime == true);
    CHECK(std::strcmp(r.output_device, "alsa_output.x") == 0);
    CHECK(std::strcmp(r.input_device, "alsa_input.y") == 0);
    CHECK(std::strcmp(r.node_name, "Foo") == 0);
}

/* The panel writes the file. The driver's C reader must parse it identically. */
static void
test_cross_language()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    if (!tmp.isValid())
        return;
    QDir().mkpath(tmp.path() + "/" + QLatin1String(PIPEASIO_CONFIG_DIR));
    qputenv("XDG_CONFIG_HOME", tmp.path().toUtf8());

    pipeasio_config c     = Config::defaults();
    c.inputs              = 10;
    c.outputs             = 12;
    c.buffer_size         = 2048;
    c.fixed_buffer_size   = true;
    c.sample_rate         = 44100;
    c.auto_connect        = true;
    c.follow_device_clock = true;
    c.realtime            = true;
    std::strcpy(c.output_device, "sink.test");
    std::strcpy(c.node_name, "Bar");
    CHECK(Config::save(c)); /* writes $XDG_CONFIG_HOME/pipeasio/config.ini */

    pipeasio_config d;
    const bool      found = pipeasio_config_load(&d); /* driver-side C reader */
    CHECK(found);
    CHECK(d.inputs == 10);
    CHECK(d.outputs == 12);
    CHECK(d.buffer_size == 2048);
    CHECK(d.fixed_buffer_size == true);
    CHECK(d.sample_rate == 44100);
    CHECK(d.auto_connect == true);
    CHECK(d.follow_device_clock == true);
    CHECK(d.realtime == true);
    CHECK(std::strcmp(d.output_device, "sink.test") == 0);
    CHECK(d.input_device[0] == '\0');
    CHECK(std::strcmp(d.node_name, "Bar") == 0);

    /* Reject INI injection through discovered PipeWire names. */
    std::strcpy(c.output_device, "sink.test\nbuffer_size = 16");
    CHECK(!Config::save(c));
    CHECK(pipeasio_config_load(&d));
    CHECK(d.buffer_size == 2048);
    CHECK(std::strcmp(d.output_device, "sink.test") == 0);
}

/* The panel parser must draw the same max-line boundary as the driver's C
 * reader, or the two disagree on one file. */
static void
test_line_length_boundary()
{
    /* src/config.c reads lines with fgets into a PIPEASIO_CONFIG_LINE_MAX
     * buffer and drops any line whose content plus its newline does not fit:
     * content >= LINE_MAX - 1 bytes is discarded, so the largest accepted line
     * is LINE_MAX - 2 content bytes + newline. ('realtime' defaults off.) */
    const QByteArray key = "realtime = 1";

    /* LINE_MAX - 2 content bytes: the largest line the driver keeps -> parsed. */
    const QString accepted
            = QString::fromLatin1(key + QByteArray(PIPEASIO_CONFIG_LINE_MAX - 2 - key.size(), ' '));
    CHECK(accepted.toUtf8().size() == PIPEASIO_CONFIG_LINE_MAX - 2);
    CHECK(Config::parseIni(accepted).realtime == true);

    /* LINE_MAX - 1 content bytes: content + newline no longer fits -> dropped,
     * so 'realtime' keeps its default value. */
    const QString dropped
            = QString::fromLatin1(key + QByteArray(PIPEASIO_CONFIG_LINE_MAX - 1 - key.size(), ' '));
    CHECK(dropped.toUtf8().size() == PIPEASIO_CONFIG_LINE_MAX - 1);
    CHECK(Config::parseIni(dropped).realtime == Config::defaults().realtime);
}

static void
test_parse_pwdump()
{
    const QByteArray json
            = "[\n"
              "  {\"id\":52,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
              "{\"media.class\":\"Audio/Sink\",\"node.name\":\"alsa_output.test\","
              "\"node.description\":\"Test Speakers\"}}},\n"
              "  {\"id\":53,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
              "{\"media.class\":\"Audio/Source\",\"node.name\":\"alsa_input.test\","
              "\"node.description\":\"Test Mic\"}}},\n"
              "  {\"id\":99,\"type\":\"PipeWire:Interface:Port\",\"info\":{\"props\":{}}},\n"
              "  {\"id\":100,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
              "{\"media.class\":\"Stream/Output/Audio\",\"node.name\":\"somestream\"}}}\n"
              "]\n";

    const auto parsed = DeviceEnumerator::parsePwDump(json);
    CHECK(parsed.has_value());
    const QList<DeviceEnumerator::Device> devs = parsed.value_or(QList<DeviceEnumerator::Device>{});

    int  sinks = 0, sources = 0;
    bool sawSink = false, sawSource = false;
    for (const auto &d : devs)
    {
        if (d.isSink)
        {
            sinks++;
            if (d.name == "alsa_output.test" && d.description == "Test Speakers")
                sawSink = true;
        }
        else
        {
            sources++;
            if (d.name == "alsa_input.test" && d.description == "Test Mic")
                sawSource = true;
        }
    }
    CHECK(sinks == 1);
    CHECK(sources == 1);
    CHECK(sawSink);
    CHECK(sawSource);
}

/* The latency readout in "Follow PipeWire" mode uses the settings metadata's
 * clock rate (issue #20): clock.force-rate wins when nonzero, else clock.rate;
 * values may be JSON numbers or strings; no settings object -> 0. */
static void
test_graph_clock_rate()
{
    const QByteArray followDefault
            = "[\n"
              "  {\"id\":31,\"type\":\"PipeWire:Interface:Metadata\",\"props\":"
              "{\"metadata.name\":\"settings\"},\"metadata\":[\n"
              "    {\"subject\":0,\"key\":\"log.level\",\"value\":2},\n"
              "    {\"subject\":0,\"key\":\"clock.rate\",\"value\":44100},\n"
              "    {\"subject\":0,\"key\":\"clock.force-rate\",\"value\":0}\n"
              "  ]}\n"
              "]\n";
    CHECK(DeviceEnumerator::graphClockRate(followDefault) == 44100);

    const QByteArray forced
            = "[\n"
              "  {\"id\":31,\"type\":\"PipeWire:Interface:Metadata\",\"props\":"
              "{\"metadata.name\":\"settings\"},\"metadata\":[\n"
              "    {\"subject\":0,\"key\":\"clock.rate\",\"value\":\"48000\"},\n"
              "    {\"subject\":0,\"key\":\"clock.force-rate\",\"value\":\"96000\"}\n"
              "  ]}\n"
              "]\n";
    CHECK(DeviceEnumerator::graphClockRate(forced) == 96000);

    /* Another metadata object (e.g. "default") must not match. */
    const QByteArray wrongObject
            = "[\n"
              "  {\"id\":30,\"type\":\"PipeWire:Interface:Metadata\",\"props\":"
              "{\"metadata.name\":\"default\"},\"metadata\":[\n"
              "    {\"subject\":0,\"key\":\"clock.rate\",\"value\":88200}\n"
              "  ]}\n"
              "]\n";
    CHECK(DeviceEnumerator::graphClockRate(wrongObject) == 0);

    CHECK(DeviceEnumerator::graphClockRate("[]") == 0);
    CHECK(DeviceEnumerator::graphClockRate("not json") == 0);
}

/* Append one driver/follower timing block to an in-progress Profiler object.
 * `haveXrun`/`haveAsync` exercise the trailing optional fields. */
static void
build_block(spa_pod_builder *b, uint32_t key, int32_t id, const char *name, int64_t prev,
            int64_t signal, int64_t awake, int64_t finish, int32_t xrun, bool haveXrun,
            bool haveAsync, bool async)
{
    spa_pod_frame f;
    spa_pod_builder_prop(b, key, 0);
    spa_pod_builder_push_struct(b, &f);
    spa_pod_builder_int(b, id);
    spa_pod_builder_string(b, name);
    spa_pod_builder_long(b, prev);
    spa_pod_builder_long(b, signal);
    spa_pod_builder_long(b, awake);
    spa_pod_builder_long(b, finish);
    spa_pod_builder_int(b, 0); /* status */
    spa_pod_builder_fraction(b, 1024u, 48000u);
    if (haveXrun)
        spa_pod_builder_int(b, xrun);
    if (haveAsync)
        spa_pod_builder_bool(b, async);
    spa_pod_builder_pop(b, &f);
}

/* The Profiler POD is the panel's only telemetry source, so its layout is a
 * hard contract with module-profiler. */
static void
test_profiler_pod()
{
    uint8_t         buf[8192];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    spa_pod_frame   f[2];
    spa_fraction    rate = SPA_FRACTION(1u, 48000u);

    spa_pod_builder_push_struct(&b, &f[0]);

    spa_pod_builder_push_object(&b, &f[1], SPA_TYPE_OBJECT_Profiler, 0);
    spa_pod_builder_prop(&b, SPA_PROFILER_info, 0);
    spa_pod_builder_add_struct(&b, SPA_POD_Long((int64_t)7), SPA_POD_Float(0.1f),
                               SPA_POD_Float(0.2f), SPA_POD_Float(0.3f), SPA_POD_Int((int32_t)9));
    spa_pod_builder_prop(&b, SPA_PROFILER_clock, 0);
    spa_pod_builder_add_struct(
            &b, SPA_POD_Int(0), SPA_POD_Int(1), SPA_POD_String("clk"), SPA_POD_Long((int64_t)123),
            SPA_POD_Fraction(&rate), SPA_POD_Long((int64_t)4096), SPA_POD_Long((int64_t)1024),
            SPA_POD_Long((int64_t)0), SPA_POD_Double(1.0), SPA_POD_Long((int64_t)456),
            SPA_POD_Int(SPA_IO_POSITION_STATE_RUNNING));
    build_block(&b, SPA_PROFILER_driverBlock, 40, "alsa", 1000000, 2000000, 2500000, 6000000, 5,
                true, false, false);
    build_block(&b, SPA_PROFILER_followerBlock, 61, "FL64", 1000000, 2100000, 2200000, 2900000, 0,
                false, false, false);
    build_block(&b, SPA_PROFILER_followerBlock, 62, "Other", 1000, 2000, 3000, 4000, 3, true, true,
                true);
    /* Never signalled this cycle -> both times unknown ("---" in pw-top). */
    build_block(&b, SPA_PROFILER_followerBlock, 63, "Silent", 5000, 4000, 3000, 2000, 0, false,
                false, false);
    /* Woken but not finished -> busy is pending ("+++"). */
    build_block(&b, SPA_PROFILER_followerBlock, 64, "Late", 1000, 2000, 3000, 2500, 0, false, false,
                false);
    spa_pod_builder_pop(&b, &f[1]);

    /* A profiler object with a short info struct must be dropped whole, and a
     * non-Profiler object must not be mistaken for a point. */
    spa_pod_builder_push_object(&b, &f[1], SPA_TYPE_OBJECT_Profiler, 0);
    spa_pod_builder_prop(&b, SPA_PROFILER_info, 0);
    spa_pod_builder_add_struct(&b, SPA_POD_Long((int64_t)1));
    spa_pod_builder_pop(&b, &f[1]);
    spa_pod_builder_push_object(&b, &f[1], SPA_TYPE_OBJECT_Props, 0);
    spa_pod_builder_pop(&b, &f[1]);

    const spa_pod *pod = static_cast<const spa_pod *>(spa_pod_builder_pop(&b, &f[0]));

    const QVector<ProfilePoint> points = parseProfilePods(pod);
    CHECK(points.size() == 1);
    if (points.isEmpty())
        return;
    const ProfilePoint &p = points.first();

    CHECK(p.info.count == 7);
    CHECK(p.info.xrunCount == 9);
    CHECK(p.info.clockDuration == 1024);
    CHECK(p.info.clockRateNum == 1);
    CHECK(p.info.clockRateDenom == 48000);
    CHECK(p.info.transportState == SPA_IO_POSITION_STATE_RUNNING);
    CHECK(p.blocks.size() == 5);
    if (p.blocks.size() != 5)
        return;

    /* Driver: QUANT/RATE come from the clock, W/Q and B/Q from its timestamps. */
    const CycleTiming drv = deriveTiming(p.info, p.blocks.at(0));
    CHECK(p.blocks.at(0).isDriver);
    CHECK(p.blocks.at(0).name == QStringLiteral("alsa"));
    CHECK(drv.quantum == 1024);
    CHECK(drv.rate == 48000);
    CHECK(drv.waitNs == 500000);
    CHECK(drv.busyNs == 3500000);
    CHECK(std::fabs(drv.busyRatio - 0.1640625) < 1e-9);
    CHECK(std::fabs(drv.waitRatio - 0.0234375) < 1e-9);
    CHECK(drv.xruns == 5); /* the block's own counter wins */

    /* Follower: QUANT/RATE come from its latency fraction; an absent xrun
     * counter falls back to the driver-wide one. */
    const CycleTiming own = deriveTiming(p.info, p.blocks.at(1));
    CHECK(!p.blocks.at(1).isDriver);
    CHECK(p.blocks.at(1).id == 61);
    CHECK(own.quantum == 1024);
    CHECK(own.rate == 48000);
    CHECK(own.busyNs == 700000);
    CHECK(std::fabs(own.busyRatio - 0.0328125) < 1e-9);
    CHECK(own.xruns == 9);

    CHECK(p.blocks.at(2).async);
    CHECK(deriveTiming(p.info, p.blocks.at(2)).xruns == 3);

    const CycleTiming silent = deriveTiming(p.info, p.blocks.at(3));
    CHECK(silent.waitNs == kTimeUnknown);
    CHECK(silent.busyNs == kTimeUnknown);
    CHECK(silent.busyRatio == 0.0); /* sentinels never leak into the ratio */

    const CycleTiming late = deriveTiming(p.info, p.blocks.at(4));
    CHECK(late.waitNs == 1000);
    CHECK(late.busyNs == kTimePending);
    CHECK(late.busyRatio == 0.0);

    CHECK(parseProfilePods(nullptr).isEmpty());
}

static void
test_describe_peer()
{
    PeerInfo bt;
    bt.description  = QStringLiteral("FiiO UTWS5");
    bt.name         = QStringLiteral("bluez_output.xx");
    bt.btCodec      = QStringLiteral("aptx");
    bt.bluetooth    = true;
    bt.rate         = 44100;
    bt.channels     = 2;
    bt.sampleFormat = QStringLiteral("S16LE");
    bt.state        = QStringLiteral("running");

    QString name, detail;
    describePeer(bt, &name, &detail);
    CHECK(name == QStringLiteral("FiiO UTWS5"));
    CHECK(detail.contains(QStringLiteral("aptX")));
    CHECK(detail.contains(QStringLiteral("44100 Hz")));
    CHECK(detail.contains(QStringLiteral("2 ch S16LE")));
    CHECK(detail.contains(QStringLiteral("running")));

    /* Name falls back description -> nick -> node.name. */
    PeerInfo nick;
    nick.nick = QStringLiteral("Nick");
    nick.name = QStringLiteral("node.name");
    describePeer(nick, &name, &detail);
    CHECK(name == QStringLiteral("Nick"));
    nick.nick.clear();
    describePeer(nick, &name, &detail);
    CHECK(name == QStringLiteral("node.name"));

    /* A suspended device has no negotiated Format: drop those attributes
     * instead of printing "0 Hz". */
    PeerInfo suspended;
    suspended.name  = QStringLiteral("alsa_output.test");
    suspended.state = QStringLiteral("suspended");
    describePeer(suspended, &name, &detail);
    CHECK(detail == QStringLiteral("suspended"));
}

static void
test_resolve_connections()
{
    PeerInfo sink;
    sink.description = QStringLiteral("FiiO UTWS5");
    sink.btCodec     = QStringLiteral("aptx");
    sink.rate        = 44100;
    PeerInfo source;
    source.description = QStringLiteral("Built-in Mic");
    PeerInfo sink2;
    sink2.description = QStringLiteral("Second Sink");

    const QHash<uint32_t, PeerInfo> nodes
            = { { 61, PeerInfo() }, { 89, sink }, { 55, source }, { 90, sink2 } };

    /* Our node (61) plays to 89 and captures from 55. */
    QVector<GraphLink> links = { { 61, 89 }, { 61, 89 }, { 55, 61 } };
    Connections        c     = resolveConnections(61, nodes, links);
    CHECK(c.output == QStringLiteral("FiiO UTWS5")); /* duplicate links coalesce */
    CHECK(c.outputDetail.contains(QStringLiteral("aptX")));
    CHECK(c.outputDetail.contains(QStringLiteral("44100 Hz")));
    CHECK(c.input == QStringLiteral("Built-in Mic"));
    CHECK(c.inputDetail.isEmpty()); /* no format/state known for this peer */

    /* Several distinct peers on one side collapse to a names-only list. */
    links = { { 61, 89 }, { 61, 90 } };
    c     = resolveConnections(61, nodes, links);
    CHECK(c.output == QStringLiteral("FiiO UTWS5, Second Sink"));
    CHECK(c.outputDetail.isEmpty());

    /* Links to nodes we never saw, and an unconnected node, resolve to empty. */
    c = resolveConnections(61, nodes, { { 61, 777 } });
    CHECK(c.output.isEmpty());
    c = resolveConnections(61, nodes, {});
    CHECK(c.output.isEmpty());
    CHECK(c.input.isEmpty());
}

static int
helper_mode(const QStringList &arguments)
{
    if (arguments.size() < 2)
        return -1;
    const QString mode = arguments.at(1);
    if (mode == QStringLiteral("--devices"))
    {
        std::fputs("[{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
                   "{\"media.class\":\"Audio/Sink\",\"node.name\":\"saved.sink\","
                   "\"node.description\":\"Saved Sink\"}}},"
                   "{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
                   "{\"media.class\":\"Audio/Source\",\"node.name\":\"saved.source\","
                   "\"node.description\":\"Saved Source\"}}}]",
                   stdout);
        return 0;
    }
    if (mode == QStringLiteral("--devices-44k"))
    {
        std::fputs("[{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
                   "{\"media.class\":\"Audio/Sink\",\"node.name\":\"saved.sink\","
                   "\"node.description\":\"Saved Sink\"}}},"
                   "{\"id\":31,\"type\":\"PipeWire:Interface:Metadata\",\"props\":"
                   "{\"metadata.name\":\"settings\"},\"metadata\":["
                   "{\"subject\":0,\"key\":\"clock.rate\",\"type\":\"\",\"value\":44100},"
                   "{\"subject\":0,\"key\":\"clock.force-rate\",\"type\":\"\",\"value\":0}]}]",
                   stdout);
        return 0;
    }
    if (mode == QStringLiteral("--empty"))
    {
        std::fputs("[]", stdout);
        return 0;
    }
    if (mode == QStringLiteral("--malformed"))
    {
        std::fputs("{not-json", stdout);
        return 0;
    }
    if (mode == QStringLiteral("--fail"))
        return 5;
    if (mode == QStringLiteral("--abnormal"))
        std::abort();
    if (mode == QStringLiteral("--sleep"))
    {
        usleep(500000);
        std::fputs("[]", stdout);
        return 0;
    }
    return -1;
}

struct RequestResult
{
    int                             count   = 0;
    bool                            success = false;
    QList<DeviceEnumerator::Device> devices;
};

static RequestResult
run_request(const QString &mode, int timeoutMs)
{
    DeviceEnumerator::RequestOptions options;
    options.program   = QCoreApplication::applicationFilePath();
    options.arguments = { mode };
    options.timeoutMs = timeoutMs;
    DeviceEnumerator::Request request(options);
    RequestResult             result;
    QEventLoop                loop;
    QTimer                    deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(
            &request, &DeviceEnumerator::Request::finished, &loop,
            [&](bool success, const QList<DeviceEnumerator::Device> &devices, const QString &)
            {
                ++result.count;
                result.success = success;
                result.devices = devices;
                loop.quit();
            });
    request.start();
    deadline.start(2500);
    loop.exec();
    CHECK(deadline.isActive());
    QCoreApplication::processEvents();
    return result;
}

static bool
wait_until_enabled(QComboBox *combo)
{
    QElapsedTimer deadline;
    deadline.start();
    while (!combo->isEnabled() && deadline.elapsed() < 2500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return combo->isEnabled();
}

static void
test_async_enumerator()
{
    RequestResult result = run_request(QStringLiteral("--devices"), 1000);
    CHECK(result.count == 1);
    CHECK(result.success);
    CHECK(result.devices.size() == 2);

    result = run_request(QStringLiteral("--empty"), 1000);
    CHECK(result.count == 1);
    CHECK(result.success);
    CHECK(result.devices.isEmpty());

    for (const QString &mode :
         { QStringLiteral("--malformed"), QStringLiteral("--fail"), QStringLiteral("--abnormal") })
    {
        result = run_request(mode, 1000);
        CHECK(result.count == 1);
        CHECK(!result.success);
    }
    result = run_request(QStringLiteral("--sleep"), 20);
    CHECK(result.count == 1);
    CHECK(!result.success);

    int                              emissions = 0;
    DeviceEnumerator::RequestOptions options;
    options.program   = QCoreApplication::applicationFilePath();
    options.arguments = { QStringLiteral("--sleep") };
    options.timeoutMs = 1000;
    auto *request     = new DeviceEnumerator::Request(options);
    QObject::connect(request, &DeviceEnumerator::Request::finished, [&] { ++emissions; });
    request->start();
    delete request;
    QEventLoop loop;
    QTimer::singleShot(100, &loop, &QEventLoop::quit);
    loop.exec();
    CHECK(emissions == 0);
}

static void
test_dialog_loading_state()
{
    pipeasio_config config = Config::defaults();
    std::strcpy(config.output_device, "saved.sink");
    std::strcpy(config.input_device, "saved.source");
    config.realtime = true;
    CHECK(Config::save(config));

    SettingsDialogOptions options;
    options.monitorEnabled          = false;
    options.deviceRequest.program   = QCoreApplication::applicationFilePath();
    options.deviceRequest.arguments = { QStringLiteral("--sleep") };
    options.deviceRequest.timeoutMs = 1000;
    {
        SettingsDialog dialog(nullptr, options);
        auto          *output = dialog.findChild<QComboBox *>(QStringLiteral("outputDevice"));
        auto          *input  = dialog.findChild<QComboBox *>(QStringLiteral("inputDevice"));
        CHECK(output && input);
        CHECK(!output->isEnabled() && !input->isEnabled());
        CHECK(output->currentData().toString() == QStringLiteral("saved.sink"));
        auto *realtime = dialog.findChild<QCheckBox *>(QStringLiteral("realtime"));
        CHECK(realtime && realtime->isChecked());
        dialog.findChild<QPushButton *>(QStringLiteral("restoreDefaultsButton"))->click();
        dialog.findChild<QPushButton *>(QStringLiteral("applyButton"))->click();
        const pipeasio_config saved = Config::load();
        CHECK(saved.output_device[0] == '\0');
        CHECK(saved.input_device[0] == '\0');
        CHECK(saved.realtime == false);
    }

    std::strcpy(config.output_device, "saved.sink");
    std::strcpy(config.input_device, "saved.source");
    CHECK(Config::save(config));
    options.deviceRequest.arguments = { QStringLiteral("--devices") };
    {
        SettingsDialog dialog(nullptr, options);
        auto          *output = dialog.findChild<QComboBox *>(QStringLiteral("outputDevice"));
        auto          *input  = dialog.findChild<QComboBox *>(QStringLiteral("inputDevice"));
        CHECK(wait_until_enabled(output));
        CHECK(input->isEnabled());
        CHECK(output->currentData().toString() == QStringLiteral("saved.sink"));
        CHECK(input->currentData().toString() == QStringLiteral("saved.source"));
    }

    CHECK(Config::save(Config::defaults()));
    options.deviceRequest.arguments = { QStringLiteral("--malformed") };
    {
        SettingsDialog dialog(nullptr, options);
        auto          *output = dialog.findChild<QComboBox *>(QStringLiteral("outputDevice"));
        CHECK(wait_until_enabled(output));
        CHECK(output->currentText() == QStringLiteral("PipeWire devices unavailable"));
        CHECK(output->currentData().toString().isEmpty());
    }
}

/* Issue #20 follow-up: with "Follow PipeWire" selected the latency readout
 * must use the graph clock rate from the pw-dump snapshot, not 48 kHz. */
static void
test_latency_follows_graph_rate()
{
    CHECK(Config::save(Config::defaults())); /* sample_rate=0, buffer 1024 */

    SettingsDialogOptions options;
    options.monitorEnabled          = false;
    options.deviceRequest.program   = QCoreApplication::applicationFilePath();
    options.deviceRequest.arguments = { QStringLiteral("--devices-44k") };
    options.deviceRequest.timeoutMs = 1000;

    SettingsDialog dialog(nullptr, options);
    auto          *output  = dialog.findChild<QComboBox *>(QStringLiteral("outputDevice"));
    auto          *latency = dialog.findChild<QLabel *>(QStringLiteral("latency"));
    auto          *rate    = dialog.findChild<QComboBox *>(QStringLiteral("sampleRate"));
    CHECK(output && latency && rate);
    CHECK(rate->currentData().toInt() == 0); /* "Follow PipeWire" */
    CHECK(wait_until_enabled(output));
    /* 1024 * 1000 / 44100 = 23.2 ms; the pre-fix label showed 21.3 (48 kHz). */
    CHECK(latency->text() == QStringLiteral("23.2 ms"));

    /* A fixed rate still wins over the graph rate. */
    rate->setCurrentIndex(rate->findData(96000));
    CHECK(latency->text() == QStringLiteral("10.7 ms"));
}

/* The follow-device checkbox also flips the node between synchronous and
 * asynchronous scheduling; the derived row names the mode and its cost. */
static void
test_scheduling_row_follows_checkbox()
{
    CHECK(Config::save(Config::defaults())); /* follow off, buffer 1024, 48 kHz */

    SettingsDialogOptions options;
    options.monitorEnabled          = false;
    options.deviceRequest.program   = QCoreApplication::applicationFilePath();
    options.deviceRequest.arguments = { QStringLiteral("--devices") };
    options.deviceRequest.timeoutMs = 1000;

    SettingsDialog dialog(nullptr, options);
    auto          *scheduling = dialog.findChild<QLabel *>(QStringLiteral("scheduling"));
    auto          *follow     = dialog.findChild<QCheckBox *>(QStringLiteral("followDeviceClock"));
    auto          *buffer     = dialog.findChild<QComboBox *>(QStringLiteral("bufferSize"));
    CHECK(scheduling && follow && buffer);
    CHECK(!follow->isChecked());
    CHECK(scheduling->text() == QStringLiteral("synchronous"));

    /* A shown window does not grow, so the longer reading must fit up front. */
    const int hintBefore = dialog.sizeHint().width();

    follow->setChecked(true);
    CHECK(scheduling->text() == QStringLiteral("asynchronous (+1 period, 21.3 ms)"));
    CHECK(dialog.sizeHint().width() == hintBefore);
    CHECK(scheduling->sizeHint().width() <= scheduling->minimumWidth());

    buffer->setCurrentIndex(buffer->findData(128));
    CHECK(scheduling->text() == QStringLiteral("asynchronous (+1 period, 2.7 ms)"));

    follow->setChecked(false);
    CHECK(scheduling->text() == QStringLiteral("synchronous"));
}

static void
test_tooltip_wrapping()
{
    SettingsDialogOptions options;
    options.monitorEnabled          = false;
    options.deviceRequest.program   = QCoreApplication::applicationFilePath();
    options.deviceRequest.arguments = { QStringLiteral("--sleep") };
    options.deviceRequest.timeoutMs = 200;

    SettingsDialog dialog(nullptr, options);

    /* Long tooltips must be word-wrapped to <= 72 chars per line so they do
     * not render as one unwrapped line across the screen. */
    const QList<QWidget *> fields
            = { dialog.findChild<QCheckBox *>(QStringLiteral("realtime")),
                dialog.findChild<QCheckBox *>(QStringLiteral("followDeviceClock")),
                dialog.findChild<QComboBox *>(QStringLiteral("sampleRate")) };
    for (const QWidget *f : fields)
    {
        CHECK(f != nullptr);
        if (!f)
            continue;
        const QString tip = f->toolTip();
        CHECK(tip.contains(QLatin1Char('\n')));
        const QStringList lines = tip.split(QLatin1Char('\n'));
        for (const QString &l : lines)
            CHECK(l.size() <= 72);
    }
}

static void
test_monitor_transient_hold()
{
    qRegisterMetaType<NodeStats>("NodeStats");

    SettingsDialogOptions options;
    options.monitorEnabled          = false;
    options.deviceRequest.program   = QCoreApplication::applicationFilePath();
    options.deviceRequest.arguments = { QStringLiteral("--sleep") };
    options.deviceRequest.timeoutMs = 200;

    SettingsDialog dialog(nullptr, options);
    auto          *quantum = dialog.findChild<QLabel *>(QStringLiteral("monQuantum"));
    auto          *state   = dialog.findChild<QLabel *>(QStringLiteral("monState"));
    auto          *output  = dialog.findChild<QLabel *>(QStringLiteral("monOutput"));
    CHECK(quantum != nullptr);
    CHECK(state != nullptr);
    CHECK(output != nullptr);

    const auto send = [&dialog](const NodeStats &st)
    {
        CHECK(QMetaObject::invokeMethod(&dialog, "onMonitorUpdated", Qt::DirectConnection,
                                        Q_ARG(NodeStats, st)));
    };

    /* Before the first good sample a miss shows the waiting state. */
    send(NodeStats());
    CHECK(quantum->text() == QStringLiteral("waiting for audio..."));

    NodeStats good;
    good.found        = true;
    good.quantum      = 64;
    good.rate         = 48000;
    good.xruns        = 7;
    good.state        = QStringLiteral("R");
    good.outputDevice = QStringLiteral("Sink A");
    send(good);
    CHECK(quantum->text() == QStringLiteral("64"));
    CHECK(output->text() == QStringLiteral("Sink A"));

    /* One miss (the node vanished for a single frame) holds the last
     * good frame. A second consecutive miss reverts to waiting and clears the
     * device rows. */
    send(NodeStats());
    CHECK(quantum->text() == QStringLiteral("64"));
    CHECK(output->text() == QStringLiteral("Sink A"));
    send(NodeStats());
    CHECK(quantum->text() == QStringLiteral("waiting for audio..."));
    CHECK(output->text() == QStringLiteral("—"));

    send(good);
    CHECK(quantum->text() == QStringLiteral("64"));

    /* An idle/suspended frame (real row, quantum/rate 0) is held once; a
     * persistent idle renders the actual I/S state, not "waiting". */
    NodeStats idle;
    idle.found = true;
    idle.state = QStringLiteral("I");
    send(idle);
    CHECK(quantum->text() == QStringLiteral("64"));
    send(idle);
    CHECK(quantum->text() == QStringLiteral("0"));
    CHECK(state->text() == QStringLiteral("I"));

    /* Recovery renders fresh values immediately. */
    good.quantum = 128;
    send(good);
    CHECK(quantum->text() == QStringLiteral("128"));
}

static void
spin(int ms)
{
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

/* The Profiler pushes a point per audio cycle, so the panel must not hold the
 * connection while the tab that consumes it is hidden. A remote that cannot
 * exist makes a delivered frame observable: no frame leaves the labels at
 * their construction-time text, one writes the connection error. */
static void
test_monitor_tab_gating()
{
    const QByteArray savedRemote = qgetenv("PIPEWIRE_REMOTE");
    qputenv("PIPEWIRE_REMOTE", "pipeasio-test-no-such-remote");

    SettingsDialogOptions options; /* monitorEnabled stays at its default */
    options.deviceRequest.program   = QCoreApplication::applicationFilePath();
    options.deviceRequest.arguments = { QStringLiteral("--sleep") };
    options.deviceRequest.timeoutMs = 200;

    {
        SettingsDialog dialog(nullptr, options);
        auto          *tabs    = dialog.findChild<QTabWidget *>();
        auto          *quantum = dialog.findChild<QLabel *>(QStringLiteral("monQuantum"));
        CHECK(tabs != nullptr);
        CHECK(quantum != nullptr);
        if (tabs && quantum)
        {
            CHECK(tabs->currentIndex() == 0); /* opens on Settings, not Monitor */

            /* Hidden: nothing is sampled, so no frame ever reaches the labels. */
            spin(500);
            CHECK(quantum->text() == QStringLiteral("waiting for audio..."));

            /* Showing: frames arrive within a few emit intervals. */
            tabs->setCurrentIndex(1);
            spin(500);
            CHECK(quantum->text() == QStringLiteral("no PipeWire daemon"));

            /* Hidden again: delivery stops, so the sentinel survives. */
            tabs->setCurrentIndex(0);
            quantum->setText(QStringLiteral("sentinel"));
            spin(500);
            CHECK(quantum->text() == QStringLiteral("sentinel"));
        }
    }

    if (savedRemote.isEmpty())
        qunsetenv("PIPEWIRE_REMOTE");
    else
        qputenv("PIPEWIRE_REMOTE", savedRemote);
}

int
main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pipeasio-settings"));
    const int helper = helper_mode(app.arguments());
    if (helper >= 0)
        return helper;

    test_config_roundtrip();
    test_cross_language();
    test_line_length_boundary();
    test_parse_pwdump();
    test_graph_clock_rate();
    test_profiler_pod();
    test_describe_peer();
    test_resolve_connections();
    test_async_enumerator();
    test_dialog_loading_state();
    test_latency_follows_graph_rate();
    test_scheduling_row_follows_checkbox();
    test_tooltip_wrapping();
    test_monitor_transient_hold();
    test_monitor_tab_gating();

    std::fprintf(stderr, "[%s] %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_total, g_fail);
    return g_fail ? 1 : 0;
}
