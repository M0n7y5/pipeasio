/*
 * main.cpp — pipeasio-settings entry point.
 *
 * Copyright (C) 2026 PipeASIO contributors
 */
#include <QApplication>

#include "SettingsDialog.hpp"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pipeasio-settings"));

    SettingsDialog dlg;
    return dlg.exec();
}
