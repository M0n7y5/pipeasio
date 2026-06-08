/*
 * main.cpp — pipeasio-settings entry point.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING.GUI for the full license text.
 */
#include <QApplication>

#include "SettingsDialog.hpp"

int
main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pipeasio-settings"));

    SettingsDialog dlg;
    return dlg.exec();
}
