/*
 * test_panel.cpp - unit tests for the Qt panel's pure functions:
 *   - Config::serializeIni / parseIni round-trip
 *   - cross-language: panel-written INI parsed by the driver's C reader
 *
 * No GUI is constructed; these are pure-data tests runnable headless.
 */
#include "Config.hpp"

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

int
main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pipeasio-settings"));

    test_config_roundtrip();
    test_cross_language();
    test_section_filter();

    std::fprintf(stderr, "[%s] %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_total, g_fail);
    return g_fail ? 1 : 0;
}
