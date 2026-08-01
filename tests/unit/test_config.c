/*
 * test_config.c - exercises the driver-side flat-INI reader (src/config.c)
 * against the exact format the Qt panel writes.  Runs from CTest.
 *
 * Strategy: point XDG_CONFIG_HOME at a temp dir, drop a config.ini into
 * <tmp>/pipeasio/, call pipeasio_config_load(), assert the parsed fields.
 */
#define _GNU_SOURCE
#include "pipeasio_config.h"
#include "pipeasio_parse.h"
#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_tmp[256];

static void
setup_tmpdir(void)
{
    snprintf(g_tmp, sizeof g_tmp, "/tmp/pipeasio_cfgtest_%d", (int)getpid());
    mkdir(g_tmp, 0700);
    char sub[320];
    snprintf(sub, sizeof sub, "%s/%s", g_tmp, PIPEASIO_CONFIG_DIR);
    mkdir(sub, 0700);
    setenv("XDG_CONFIG_HOME", g_tmp, 1);
}

static void
cfg_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/%s/%s", g_tmp, PIPEASIO_CONFIG_DIR, PIPEASIO_CONFIG_FILE);
}

static void
write_cfg(const char *body)
{
    char path[400];
    cfg_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f)
    {
        fputs(body, f);
        fclose(f);
    }
}

static void
remove_cfg(void)
{
    char path[400];
    cfg_path(path, sizeof path);
    unlink(path);
}

int
main(void)
{
    setup_tmpdir();

    /* No file => defaults, and the loader reports "not found". */
    remove_cfg();
    TEST_GROUP("missing file -> defaults")
    {
        struct pipeasio_config c;
        bool                   found = pipeasio_config_load(&c);
        EXPECT_TRUE(!found);
        EXPECT_EQ(c.inputs, PIPEASIO_DEFAULT_INPUTS);
        EXPECT_EQ(c.outputs, PIPEASIO_DEFAULT_OUTPUTS);
        EXPECT_EQ(c.buffer_size, PIPEASIO_DEFAULT_BUFFER_SIZE);
        EXPECT_EQ(c.fixed_buffer_size, 1);
        EXPECT_EQ(c.sample_rate, 0);
        EXPECT_EQ(c.auto_connect, 1);
        EXPECT_EQ(c.follow_device_clock, 0);
        /* Literal, not the macro: a future flip of the shipped default must
         * fail here rather than follow silently. */
        EXPECT_EQ(c.realtime, 0);
        EXPECT_TRUE(c.output_device[0] == '\0');
        EXPECT_TRUE(c.input_device[0] == '\0');
        EXPECT_TRUE(c.node_name[0] == '\0');
    }

    /* A full config in the exact panel format round-trips to the struct. */
    TEST_GROUP("full config parsed")
    {
        write_cfg("# PipeASIO settings - written by pipeasio-settings\n"
                  "[pipeasio]\n"
                  "inputs = 8\n"
                  "outputs = 4\n"
                  "buffer_size = 256\n"
                  "fixed_buffer_size = 0\n"
                  "sample_rate = 48000\n"
                  "auto_connect = 0\n"
                  "follow_device_clock = 1\n"
                  "output_device = alsa_output.pci-0000_12_00.6.analog-stereo\n"
                  "input_device = alsa_input.usb-mic\n"
                  "node_name = MyDAW\n");
        struct pipeasio_config c;
        bool                   found = pipeasio_config_load(&c);
        EXPECT_TRUE(found);
        EXPECT_EQ(c.inputs, 8);
        EXPECT_EQ(c.outputs, 4);
        EXPECT_EQ(c.buffer_size, 256);
        EXPECT_EQ(c.fixed_buffer_size, 0);
        EXPECT_EQ(c.sample_rate, 48000);
        EXPECT_EQ(c.auto_connect, 0);
        EXPECT_EQ(c.follow_device_clock, 1);
        EXPECT_TRUE(strcmp(c.output_device, "alsa_output.pci-0000_12_00.6.analog-stereo") == 0);
        EXPECT_TRUE(strcmp(c.input_device, "alsa_input.usb-mic") == 0);
        EXPECT_TRUE(strcmp(c.node_name, "MyDAW") == 0);
    }

    /* Out-of-range / malformed numerics fall back to defaults. */
    TEST_GROUP("validation")
    {
        write_cfg("[pipeasio]\n"
                  "inputs = -3\n"
                  "buffer_size = 1000\n" /* not a power of two */
                  "sample_rate = -5\n");
        struct pipeasio_config c;
        pipeasio_config_load(&c);
        EXPECT_EQ(c.inputs, PIPEASIO_DEFAULT_INPUTS);
        EXPECT_EQ(c.buffer_size, PIPEASIO_DEFAULT_BUFFER_SIZE);
        EXPECT_EQ(c.sample_rate, 0);
    }

    /* Comments, stray whitespace, unknown keys, bool spellings, no header. */
    TEST_GROUP("lenient parse")
    {
        write_cfg("; a comment\n"
                  "  inputs   =   2  \n"
                  "unknown_key = whatever\n"
                  "fixed_buffer_size = true\n"
                  "auto_connect = off\n");
        struct pipeasio_config c;
        pipeasio_config_load(&c);
        EXPECT_EQ(c.inputs, 2);
        EXPECT_EQ(c.fixed_buffer_size, 1); /* "true"  */
        EXPECT_EQ(c.auto_connect, 0);      /* "off"   */
        EXPECT_EQ(c.outputs, PIPEASIO_DEFAULT_OUTPUTS);
    }

    /* Keys under a foreign section header are ignored. */
    TEST_GROUP("foreign section ignored")
    {
        write_cfg("[other]\n"
                  "inputs = 99\n"
                  "[pipeasio]\n"
                  "outputs = 3\n");
        struct pipeasio_config c;
        pipeasio_config_load(&c);
        EXPECT_EQ(c.inputs, PIPEASIO_DEFAULT_INPUTS);
        EXPECT_EQ(c.outputs, 3);
    }

    /* Valid nondefaults expose omitted assignments without invalid _Bool values. */
    TEST_GROUP("defaults fill every field")
    {
        struct pipeasio_config c;
        c.inputs              = PIPEASIO_DEFAULT_INPUTS + 7;
        c.outputs             = PIPEASIO_DEFAULT_OUTPUTS + 7;
        c.buffer_size         = PIPEASIO_DEFAULT_BUFFER_SIZE / 2;
        c.fixed_buffer_size   = !PIPEASIO_DEFAULT_FIXED_BUFFER_SIZE;
        c.sample_rate         = PIPEASIO_DEFAULT_SAMPLE_RATE + 44100;
        c.auto_connect        = !PIPEASIO_DEFAULT_AUTO_CONNECT;
        c.follow_device_clock = !PIPEASIO_DEFAULT_FOLLOW_DEVICE_CLOCK;
        c.realtime            = !PIPEASIO_DEFAULT_REALTIME;
        c.output_device[0]    = 'X';
        c.input_device[0]     = 'X';
        c.node_name[0]        = 'X';

        pipeasio_config_defaults(&c);

        EXPECT_EQ(c.inputs, PIPEASIO_DEFAULT_INPUTS);
        EXPECT_EQ(c.outputs, PIPEASIO_DEFAULT_OUTPUTS);
        EXPECT_EQ(c.buffer_size, PIPEASIO_DEFAULT_BUFFER_SIZE);
        EXPECT_EQ(c.fixed_buffer_size, PIPEASIO_DEFAULT_FIXED_BUFFER_SIZE);
        EXPECT_EQ(c.sample_rate, PIPEASIO_DEFAULT_SAMPLE_RATE);
        EXPECT_EQ(c.auto_connect, PIPEASIO_DEFAULT_AUTO_CONNECT);
        EXPECT_EQ(c.follow_device_clock, PIPEASIO_DEFAULT_FOLLOW_DEVICE_CLOCK);
        EXPECT_EQ(c.realtime, PIPEASIO_DEFAULT_REALTIME);
        EXPECT_EQ(c.output_device[0], '\0');
        EXPECT_EQ(c.input_device[0], '\0');
        EXPECT_EQ(c.node_name[0], '\0');
    }

    TEST_GROUP("realtime key parses")
    {
        write_cfg("[pipeasio]\nrealtime = 0\n");
        struct pipeasio_config c;
        pipeasio_config_load(&c);
        EXPECT_EQ(c.realtime, 0);

        write_cfg("[pipeasio]\nrealtime = on\n");
        pipeasio_config_load(&c);
        EXPECT_EQ(c.realtime, 1);

        write_cfg("[pipeasio]\ninputs = 2\n");
        pipeasio_config_load(&c);
        EXPECT_EQ(c.realtime, PIPEASIO_DEFAULT_REALTIME);
    }

    TEST_GROUP("numeric junk overflow duplicates and directions")
    {
        write_cfg("[pipeasio]\n"
                  "inputs = 4junk\n"
                  "outputs = 999999999999999999999999\n"
                  "buffer_size = 256 trailing\n");
        struct pipeasio_config c;
        pipeasio_config_load(&c);
        EXPECT_EQ(c.inputs, PIPEASIO_DEFAULT_INPUTS);
        EXPECT_EQ(c.outputs, PIPEASIO_DEFAULT_OUTPUTS);
        EXPECT_EQ(c.buffer_size, PIPEASIO_DEFAULT_BUFFER_SIZE);

        write_cfg("[pipeasio]\ninputs = 1\ninputs = 3\noutputs = 0\n");
        pipeasio_config_load(&c);
        EXPECT_EQ(c.inputs, 3);
        EXPECT_EQ(c.outputs, 0);

        write_cfg("[pipeasio]\ninputs = 0\noutputs = 2\n");
        pipeasio_config_load(&c);
        EXPECT_EQ(c.inputs, 0);
        EXPECT_EQ(c.outputs, 2);

        write_cfg("[pipeasio]\ninputs = 0\noutputs = 0\n");
        EXPECT_TRUE(pipeasio_config_load(&c));
        EXPECT_EQ(c.inputs, 0);
        EXPECT_EQ(c.outputs, 0);
    }

    TEST_GROUP("line and destination boundaries")
    {
        char                   value[PIPEASIO_DEVICE_NAME_MAX];
        char                   config[PIPEASIO_CONFIG_LINE_MAX + 128];
        struct pipeasio_config c;
        memset(value, 'a', sizeof(value));
        value[sizeof(value) - 1] = '\0';
        snprintf(config, sizeof(config), "[pipeasio]\noutput_device = %s\n", value);
        write_cfg(config);
        pipeasio_config_load(&c);
        EXPECT_TRUE(strcmp(c.output_device, value) == 0);

        memset(config, 'x', sizeof(config));
        memcpy(config, "[pipeasio]\noutput_device = ", 27);
        config[sizeof(config) - 2] = '\n';
        config[sizeof(config) - 1] = '\0';
        write_cfg(config);
        pipeasio_config_load(&c);
        EXPECT_EQ(c.output_device[0], '\0');
    }

    remove_cfg();
    return test_report();
}
