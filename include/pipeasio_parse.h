/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define PIPEASIO_CONFIG_LINE_MAX 1024

static inline bool
pipeasio_parse_int(const char *text, int minimum, int maximum, int *result)
{
    char *end;
    long  value;
    if (!text || !result || minimum > maximum)
        return false;
    while (isspace((unsigned char)*text))
        ++text;
    if (!*text)
        return false;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || value < minimum || value > maximum)
        return false;
    while (isspace((unsigned char)*end))
        ++end;
    if (*end)
        return false;
    *result = (int)value;
    return true;
}

static inline bool
pipeasio_ascii_equal_ci(const char *left, const char *right)
{
    while (*left && *right)
    {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (tolower(a) != tolower(b))
            return false;
    }
    return !*left && !*right;
}

static inline bool
pipeasio_parse_bool(const char *text, bool *result)
{
    const char *end;
    size_t      length;
    char        value[8];
    if (!text || !result)
        return false;
    while (isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        --end;
    length = (size_t)(end - text);
    if (!length || length >= sizeof(value))
        return false;
    memcpy(value, text, length);
    value[length] = '\0';
    if (!strcmp(value, "1") || pipeasio_ascii_equal_ci(value, "true")
        || pipeasio_ascii_equal_ci(value, "on") || pipeasio_ascii_equal_ci(value, "yes"))
    {
        *result = true;
        return true;
    }
    if (!strcmp(value, "0") || pipeasio_ascii_equal_ci(value, "false")
        || pipeasio_ascii_equal_ci(value, "off") || pipeasio_ascii_equal_ci(value, "no"))
    {
        *result = false;
        return true;
    }
    return false;
}
