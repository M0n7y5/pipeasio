/*
 * Config.hpp — INI read/write for the PipeASIO settings panel.
 *
 * Reuses `struct pipeasio_config` from the shared driver/panel contract.
 * parseIni() and serializeIni() are PURE (no file I/O) so they are unit
 * testable with fixture strings.
 *
 * Copyright (C) 2026 PipeASIO contributors
 */
#pragma once

#include <QString>

extern "C" {
#include "pipeasio_config.h"
}

namespace Config {

/* Defaults straight from the PIPEASIO_DEFAULT_* macros; device/node strings
 * are left empty. */
pipeasio_config defaults();

/* Parse a flat INI document. Pure: operates on the passed text only.
 * Lenient — optional [pipeasio] header, key = value lines, #/; comments,
 * whitespace trimmed. Unknown keys ignored, missing keys keep defaults,
 * out-of-range numerics fall back to defaults. */
pipeasio_config parseIni(const QString &text);

/* Serialize to the exact flat format the driver parses. Pure. */
QString serializeIni(const pipeasio_config &c);

/* Absolute path to the config file (QStandardPaths ConfigLocation). */
QString configPath();

/* Read configPath() and parse it; defaults() when the file is missing. */
pipeasio_config load();

/* mkpath the directory and write serializeIni() to configPath(). */
bool save(const pipeasio_config &c);

} // namespace Config
