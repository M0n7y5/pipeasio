/*
 * main.cpp - pipeasio-settings entry point.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <QApplication>
#include <QIcon>

#include "SettingsDialog.hpp"

int
main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pipeasio-settings"));
    // Matches the installed pipeasio-settings.desktop (Wayland app_id) and
    // the hicolor icon, so compositors/taskbars pick up the right icon.
    app.setDesktopFileName(QStringLiteral("pipeasio-settings"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("pipeasio")));

    SettingsDialog dlg;
    return dlg.exec();
}
