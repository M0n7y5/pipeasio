/*
 * test_panel.cpp - integration checks for the Qt panel's Config wrapper:
 * defaults sanity and a cross-language round-trip (the panel writes via
 * Config::save -> the driver's C writer; the driver's C reader parses it
 * back).  Headless - no GUI is constructed - so it runs under CTest.
 */
#include "Config.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QString>

#include <cstdio>
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
test_defaults()
{
    const pipeasio_config c = Config::defaults();
    CHECK(c.inputs == PIPEASIO_DEFAULT_INPUTS);
    CHECK(c.outputs == PIPEASIO_DEFAULT_OUTPUTS);
    CHECK(c.buffer_size == PIPEASIO_DEFAULT_BUFFER_SIZE);
    CHECK(c.rt_priority == PIPEASIO_DEFAULT_RT_PRIORITY);
}

/* The panel writes the file (via the shared C writer); the driver's C
 * reader must parse it identically. */
static void
test_cross_language()
{
    QString tmp = QStringLiteral("/tmp/pipeasio_paneltest_%1").arg(getpid());
    QDir().mkpath(tmp);
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

int
main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pipeasio-settings"));

    test_defaults();
    test_cross_language();

    std::fprintf(stderr, "[%s] %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_total, g_fail);
    return g_fail ? 1 : 0;
}
