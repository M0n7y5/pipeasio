/*
 * test_panel.cpp — unit tests for the Qt panel's pure functions:
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
#include <unistd.h>   /* getpid */

extern "C" {
#include "pipeasio_config.h"
}

static int g_total = 0;
static int g_fail = 0;

#define CHECK(cond) do {                                                    \
    g_total++;                                                              \
    if (!(cond)) {                                                          \
        g_fail++;                                                           \
        std::fprintf(stderr, "  FAIL %s:%d CHECK(%s)\n",                    \
                     __FILE__, __LINE__, #cond);                            \
    }                                                                       \
} while (0)

static void test_config_roundtrip()
{
    pipeasio_config c = Config::defaults();
    c.inputs = 6;
    c.outputs = 2;
    c.buffer_size = 512;
    c.fixed_buffer_size = false;
    c.sample_rate = 96000;
    c.auto_connect = false;
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
    CHECK(std::strcmp(r.output_device, "alsa_output.x") == 0);
    CHECK(std::strcmp(r.input_device, "alsa_input.y") == 0);
    CHECK(std::strcmp(r.node_name, "Foo") == 0);
}

/* The panel writes the file; the driver's C reader must parse it identically. */
static void test_cross_language()
{
    QString tmp = QStringLiteral("/tmp/pipeasio_paneltest_%1").arg(getpid());
    QDir().mkpath(tmp + "/" + QLatin1String(PIPEASIO_CONFIG_DIR));
    qputenv("XDG_CONFIG_HOME", tmp.toUtf8());

    pipeasio_config c = Config::defaults();
    c.inputs = 10;
    c.outputs = 12;
    c.buffer_size = 2048;
    c.fixed_buffer_size = true;
    c.sample_rate = 44100;
    c.auto_connect = true;
    std::strcpy(c.output_device, "sink.test");
    std::strcpy(c.node_name, "Bar");
    CHECK(Config::save(c));   /* writes $XDG_CONFIG_HOME/pipeasio/config.ini */

    pipeasio_config d;
    const bool found = pipeasio_config_load(&d);   /* driver-side C reader */
    CHECK(found);
    CHECK(d.inputs == 10);
    CHECK(d.outputs == 12);
    CHECK(d.buffer_size == 2048);
    CHECK(d.fixed_buffer_size == true);
    CHECK(d.sample_rate == 44100);
    CHECK(d.auto_connect == true);
    CHECK(std::strcmp(d.output_device, "sink.test") == 0);
    CHECK(d.input_device[0] == '\0');
    CHECK(std::strcmp(d.node_name, "Bar") == 0);
}

static void test_parse_pwdump()
{
    const QByteArray json =
        "[\n"
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

    int sinks = 0, sources = 0;
    bool sawSink = false, sawSource = false;
    for (const auto &d : devs) {
        if (d.isSink) {
            sinks++;
            if (d.name == "alsa_output.test" && d.description == "Test Speakers")
                sawSink = true;
        } else {
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

static void test_parse_pwtop()
{
    const QByteArray out =
        "S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT        NAME\n"
        "R   45    1024  48000  10.0us  20.0us 0.01  0.25    2  F32P 2 48000 PipeASIO\n"
        "C   52       0      0    ---     ---   ---   ---     0               alsa_output.test\n";

    const NodeStats s = parsePwTop(out, QStringLiteral("PipeASIO"));
    CHECK(s.found);
    CHECK(s.quantum == 1024);
    CHECK(s.rate == 48000);
    CHECK(std::fabs(s.dspLoad - 0.25) < 1e-9);
    CHECK(s.xruns == 2);

    const NodeStats miss = parsePwTop(out, QStringLiteral("NoSuchNode"));
    CHECK(!miss.found);
}

static void test_find_own_node()
{
    const QByteArray json =
        "[\n"
        "  {\"id\":40,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
        "{\"media.class\":\"Audio/Sink\",\"node.name\":\"alsa_output.test\"}}},\n"
        "  {\"id\":61,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
        "{\"node.name\":\"FL64\",\"media.role\":\"DSP\",\"pipeasio.node\":\"1\"}}}\n"
        "]\n";
    CHECK(DeviceEnumerator::findOwnNode(json) == QStringLiteral("FL64"));

    const QByteArray none =
        "[ {\"id\":40,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":"
        "{\"node.name\":\"alsa_output.test\"}}} ]";
    CHECK(DeviceEnumerator::findOwnNode(none).isEmpty());
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pipeasio-settings"));

    test_config_roundtrip();
    test_cross_language();
    test_parse_pwdump();
    test_parse_pwtop();
    test_find_own_node();

    std::fprintf(stderr, "[%s] %d checks, %d failed\n",
                 g_fail ? "FAIL" : "PASS", g_total, g_fail);
    return g_fail ? 1 : 0;
}
