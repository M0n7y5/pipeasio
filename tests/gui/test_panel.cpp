/*
 * test_panel.cpp - unit tests for the Qt panel's pure functions:
 *   - Config::serializeIni / parseIni round-trip
 *   - cross-language: panel-written INI parsed by the driver's C reader
 *   - parsePwTop (fixture)
 *
 * No GUI is constructed; these are pure-data tests runnable headless.
 */
#include "Config.hpp"
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
    c.rt_priority         = 25;
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
    CHECK(r.rt_priority == 25);
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
    c.rt_priority         = 30;
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
    CHECK(d.rt_priority == 30);
    CHECK(std::strcmp(d.output_device, "sink.test") == 0);
    CHECK(d.input_device[0] == '\0');
    CHECK(std::strcmp(d.node_name, "Bar") == 0);
}

/* Keys under a foreign section header must be ignored (mirrors the driver's
 * C parser); keys before any [section] header are tolerated. */
static void
test_section_filter()
{
    const QString ini = QStringLiteral("buffer_size = 256\n"
                                       "[other]\n"
                                       "buffer_size = 8192\n"
                                       "inputs = 99\n"
                                       "[" PIPEASIO_CONFIG_SECTION "]\n"
                                       "buffer_size = 512\n");
    const pipeasio_config c = Config::parseIni(ini);
    CHECK(c.buffer_size == 512);
    CHECK(c.inputs == PIPEASIO_DEFAULT_INPUTS); /* [other] inputs ignored */
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

    /* Streaming regression: pw-top stdout is block-buffered, so the live
     * buffer usually ENDS with a fresh header whose rows have not arrived
     * yet (and possibly a truncated first row).  The parser must still find
     * the newest complete data row instead of reporting "waiting" forever. */
    const QByteArray streaming
            = "S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME\n"
              "R   54    256  48000  64.9us   1.1us  0.01  0.00    1    S24LE 1 48000 mic\n"
              "R   78    256  48000  11.0us   1.7ms  0.00  0.42    7                   = FL64\n"
              "S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME\n"
              "R   54    25"; /* truncated mid-row: not enough tokens */
    const NodeStats w = parsePwTop(streaming, QStringLiteral("FL64"));
    CHECK(w.found);
    CHECK(w.quantum == 256);
    CHECK(std::fabs(w.dspLoad - 0.42) < 1e-9);
    CHECK(w.xruns == 7);
}

int
main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pipeasio-settings"));

    test_config_roundtrip();
    test_cross_language();
    test_section_filter();
    test_parse_pwtop();

    std::fprintf(stderr, "[%s] %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_total, g_fail);
    return g_fail ? 1 : 0;
}
