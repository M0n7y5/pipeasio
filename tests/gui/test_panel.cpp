/*
 * test_panel.cpp - unit tests for the Qt panel's pure functions:
 *   - Config::serializeIni / parseIni round-trip
 *   - cross-language: panel-written INI parsed by the driver's C reader
 *   - DeviceEnumerator::parsePwDump (fixture)
 *   - parsePwTop (fixture)
 *
 * No GUI is constructed; these are pure-data tests runnable headless.
 */
#include "Config.hpp"
#include "DeviceEnumerator.hpp"
#include "PipeWireMonitor.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QString>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h> /* getpid */

extern "C"
{
#include "pipeasio_config.h"
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
    CHECK(std::strcmp(r.output_device, "alsa_output.x") == 0);
    CHECK(std::strcmp(r.input_device, "alsa_input.y") == 0);
    CHECK(std::strcmp(r.node_name, "Foo") == 0);
}

/* The panel writes the file; the driver's C reader must parse it identically. */
static void
test_cross_language()
{
    QString tmp = QStringLiteral("/tmp/pipeasio_paneltest_%1").arg(getpid());
    QDir().mkpath(tmp + "/" + QLatin1String(PIPEASIO_CONFIG_DIR));
    qputenv("XDG_CONFIG_HOME", tmp.toUtf8());

    pipeasio_config c     = Config::defaults();
    c.inputs              = 10;
    c.outputs             = 12;
    c.buffer_size         = 2048;
    c.fixed_buffer_size   = true;
    c.sample_rate         = 44100;
    c.auto_connect        = true;
    c.follow_device_clock = true;
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
    CHECK(std::strcmp(d.output_device, "sink.test") == 0);
    CHECK(d.input_device[0] == '\0');
    CHECK(std::strcmp(d.node_name, "Bar") == 0);
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

    const QList<DeviceEnumerator::Device> devs = DeviceEnumerator::parsePwDump(json);
    CHECK(devs.size() == 2);

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
     * captures from a source (53). Links reference node ids directly; each peer
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
    CHECK(c.output.contains(QStringLiteral("FiiO UTWS5")));
    CHECK(c.output.contains(QStringLiteral("aptX")));
    CHECK(c.output.contains(QStringLiteral("44100 Hz")));
    CHECK(c.output.contains(QStringLiteral("2 ch")));
    CHECK(c.output.contains(QStringLiteral("running")));
    CHECK(c.input.contains(QStringLiteral("USB Mic")));
    CHECK(c.input.contains(QStringLiteral("48000 Hz")));
    CHECK(c.input.contains(QStringLiteral("1 ch")));

    /* No pipeasio-marked node present -> both sides empty. */
    const QByteArray none = "[ {\"id\":89,\"type\":\"PipeWire:Interface:Node\",\"info\":"
                            "{\"props\":{\"media.class\":\"Audio/Sink\",\"node.name\":\"x\"}}} ]";
    const DeviceEnumerator::Connections e = DeviceEnumerator::resolveConnections(none);
    CHECK(e.output.isEmpty());
    CHECK(e.input.isEmpty());
}

int
main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pipeasio-settings"));

    test_config_roundtrip();
    test_cross_language();
    test_parse_pwdump();
    test_parse_pwtop();
    test_find_own_node();
    test_resolve_connections();

    std::fprintf(stderr, "[%s] %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_total, g_fail);
    return g_fail ? 1 : 0;
}
