/*
 * test_panel.cpp - unit tests for the Qt panel:
 *   - Config::serializeIni / parseIni round-trip
 *   - cross-language: panel-written INI parsed by the driver's C reader
 *   - DeviceEnumerator::parsePwDump (fixture)
 *   - parsePwTop (fixture)
 *   - SettingsDialog behavior (device loading state, tooltip wrapping,
 *     monitor transient hold)
 *
 * Runnable headless: CTest sets QT_QPA_PLATFORM=offscreen.
 */
#include "Config.hpp"
#include "DeviceEnumerator.hpp"
#include "PipeWireMonitor.hpp"
#include "SettingsDialog.hpp"

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

static void
test_parse_pwtop()
{
    /* Single-iteration sanity (period decimals, multi-token FORMAT column). */
    const QByteArray one
            = "S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT        NAME\n"
              "R   45    1024  48000  10.0us  20.0us 0.01  0.25    2  F32P 2 48000 PipeASIO\n"
              "C   52       0      0    ---     ---   ---   ---     0               "
              "alsa_output.test\n";
    const NodeStats s = parsePwTop(one, QStringLiteral("PipeASIO"));
    CHECK(s.found);
    CHECK(s.quantum == 1024);
    CHECK(s.rate == 48000);
    CHECK(std::fabs(s.dspLoad - 0.25) < 1e-9);
    CHECK(s.xruns == 2);
    CHECK(!parsePwTop(one, QStringLiteral("NoSuchNode")).found);

    /* Realistic batch output: pw-top emits an all-zero baseline first, then a
     * measured iteration.  The parser must read the LAST iteration, cope with
     * comma decimals (non-C locale), and handle a driver row whose FORMAT
     * column is empty (only a "=" link marker before the NAME). */
    const QByteArray two
            = "S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME\n"
              "C   54      0      0    ---     ---   ---   ---     0                  mic\n"
              "C   78      0      0    ---     ---   ---   ---     0                  FL64\n"
              "S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME\n"
              "R   54    256  48000  64,9us   1,1us  0,01  0,00    1    S24LE 1 48000 mic\n"
              "R   78    256  48000  11,0us   1,7ms  0,00  0,17    3                   = FL64\n";
    const NodeStats m = parsePwTop(two, QStringLiteral("FL64"));
    CHECK(m.found);
    CHECK(m.state == QStringLiteral("R")); /* measured iteration, not baseline "C" */
    CHECK(m.quantum == 256);
    CHECK(m.rate == 48000);
    CHECK(std::fabs(m.dspLoad - 0.17) < 1e-9); /* comma decimal parsed */
    CHECK(m.xruns == 3);

    /* An empty target must not match a random row (panel shows "waiting"). */
    CHECK(!parsePwTop(two, QString()).found);
}

static void
test_find_own_node()
{
    const QByteArray json
            = "[\n"
              "  {\"id\":40,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
              "{\"media.class\":\"Audio/Sink\",\"node.name\":\"alsa_output.test\"}}},\n"
              "  {\"id\":61,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
              "{\"node.name\":\"FL64\",\"media.role\":\"DSP\",\"pipeasio.node\":\"1\"}}}\n"
              "]\n";
    CHECK(DeviceEnumerator::findOwnNode(json) == QStringLiteral("FL64"));

    /* Numeric marker form: pw-dump serialises the property "1" as a JSON
     * number, which is what actually appears at runtime - the panel must still
     * recognise it (regression: toString() is empty for JSON numbers). */
    const QByteArray numForm
            = "[\n"
              "  {\"id\":112,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
              "{\"node.name\":\"FL64\",\"pipeasio.node\":1}}}\n"
              "]\n";
    CHECK(DeviceEnumerator::findOwnNode(numForm) == QStringLiteral("FL64"));

    const QByteArray none = "[ {\"id\":40,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
                            "{\"node.name\":\"alsa_output.test\"}}} ]";
    CHECK(DeviceEnumerator::findOwnNode(none).isEmpty());
}

static void
test_resolve_connections()
{
    /* Our filter node (61, marked) plays to a sink (89, Bluetooth/aptX) and
     * captures from a source (53). Links reference node ids directly. Each peer
     * carries state + negotiated Format so we can assert the enriched line. */
    const QByteArray json
            = "[\n"
              "  {\"id\":89,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"state\":\"running\","
              "\"props\":{\"media.class\":\"Audio/Sink\",\"node.name\":\"bluez_output.x\","
              "\"node.description\":\"FiiO UTWS5\",\"api.bluez5.codec\":\"aptx\","
              "\"device.api\":\"bluez5\"},"
              "\"params\":{\"Format\":[{\"rate\":44100,\"channels\":2,\"format\":\"S24LE\"}]}}},\n"
              "  {\"id\":53,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"state\":\"running\","
              "\"props\":{\"media.class\":\"Audio/Source\",\"node.name\":\"alsa_input.mic\","
              "\"node.description\":\"USB Mic\"},"
              "\"params\":{\"Format\":[{\"rate\":48000,\"channels\":1,\"format\":\"S24LE\"}]}}},\n"
              "  {\"id\":61,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
              "{\"node.name\":\"FL64\",\"pipeasio.node\":1}}},\n"
              "  {\"id\":201,\"type\":\"PipeWire:Interface:Link\",\"info\":{\"props\":"
              "{\"link.output.node\":61,\"link.input.node\":89}}},\n"
              "  {\"id\":202,\"type\":\"PipeWire:Interface:Link\",\"info\":{\"props\":"
              "{\"link.output.node\":53,\"link.input.node\":61}}}\n"
              "]\n";
    const DeviceEnumerator::Connections c = DeviceEnumerator::resolveConnections(json);
    CHECK(c.output == QStringLiteral("FiiO UTWS5"));
    CHECK(c.outputDetail.contains(QStringLiteral("aptX")));
    CHECK(c.outputDetail.contains(QStringLiteral("44100 Hz")));
    CHECK(c.outputDetail.contains(QStringLiteral("2 ch S24LE")));
    CHECK(c.outputDetail.contains(QStringLiteral("running")));
    CHECK(c.input == QStringLiteral("USB Mic"));
    CHECK(c.inputDetail.contains(QStringLiteral("48000 Hz")));
    CHECK(c.inputDetail.contains(QStringLiteral("1 ch S24LE")));

    /* No pipeasio-marked node present -> both sides empty. */
    const QByteArray none = "[ {\"id\":89,\"type\":\"PipeWire:Interface:Node\",\"info\":"
                            "{\"props\":{\"media.class\":\"Audio/Sink\",\"node.name\":\"x\"}}} ]";
    const DeviceEnumerator::Connections e = DeviceEnumerator::resolveConnections(none);
    CHECK(e.output.isEmpty());
    CHECK(e.input.isEmpty());
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

    /* One miss (node vanished from a single pw-top iteration) holds the last
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
    test_parse_pwtop();
    test_find_own_node();
    test_resolve_connections();
    test_async_enumerator();
    test_dialog_loading_state();
    test_tooltip_wrapping();
    test_monitor_transient_hold();

    std::fprintf(stderr, "[%s] %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_total, g_fail);
    return g_fail ? 1 : 0;
}
