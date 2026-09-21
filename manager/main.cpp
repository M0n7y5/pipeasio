/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Manager.hpp"
#include "pipeasio_config.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>

#include <csignal>
#include <cstdio>

namespace
{
void
cancelOperation(int)
{
    PipeASIOManager::requestCancellation();
}

bool
writeOutput(const QJsonObject &value, bool compact)
{
    QByteArray bytes = QJsonDocument(value).toJson(compact ? QJsonDocument::Compact
                                                           : QJsonDocument::Indented);
    if (compact)
        bytes.append('\n');
    const bool written
            = std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout)
              == static_cast<size_t>(bytes.size());
    if (!written || std::fflush(stdout) != 0)
    {
        PipeASIOManager::requestCancellation();
        return false;
    }
    return true;
}
}

int
main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("pipeasio-manage"));
    application.setApplicationVersion(QStringLiteral(PIPEASIO_VERSION));
    std::signal(SIGINT, cancelOperation);
    std::signal(SIGTERM, cancelOperation);
    std::signal(SIGPIPE, SIG_IGN);
    const auto arguments = application.arguments();
    if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("--version"))
    {
        std::puts(PIPEASIO_VERSION);
        return 0;
    }
    const bool         json = arguments.contains(QStringLiteral("--json"));
    QCommandLineParser parser;
    parser.setApplicationDescription(
            QStringLiteral("Manage PipeASIO in existing launcher-owned Wine prefixes."));
    parser.addHelpOption();
    parser.addPositionalArgument(
            QStringLiteral("command"),
            QStringLiteral("list, releases, add, preview, install, check, remove, or manifest"));
    parser.addOptions({
            { QStringLiteral("json"), QStringLiteral("Emit JSON progress and result records.") },
            { QStringLiteral("target"), QStringLiteral("Target ID from list."),
              QStringLiteral("id") },
            { QStringLiteral("prefix"), QStringLiteral("Existing custom Wine prefix."),
              QStringLiteral("path") },
            { QStringLiteral("wine"), QStringLiteral("Wine executable owning the custom prefix."),
              QStringLiteral("executable") },
            { QStringLiteral("name"), QStringLiteral("Display name for the custom prefix."),
              QStringLiteral("name") },
            { QStringLiteral("release"),
              QStringLiteral("Official release tag. Defaults to latest stable."),
              QStringLiteral("tag") },
            { QStringLiteral("include-32"),
              QStringLiteral("Include experimental new-WoW64 front end.") },
            { QStringLiteral("allow-permissions"),
              QStringLiteral("Approve Flatpak grants shown by preview.") },
            { QStringLiteral("version"), QStringLiteral("Release tag for manifest generation."),
              QStringLiteral("tag") },
            { QStringLiteral("asset"),
              QStringLiteral("Built driver archive for manifest generation."),
              QStringLiteral("path") },
            { QStringLiteral("root"), QStringLiteral("Staged driver tree for manifest generation."),
              QStringLiteral("path") },
            { QStringLiteral("architecture"),
              QStringLiteral("Architecture the staged tree was built for. Defaults to the host."),
              QStringLiteral("name") },
            { QStringLiteral("wine-sdk"),
              QStringLiteral("Wine build SDK recorded in the manifest."),
              QStringLiteral("version") },
            { QStringLiteral("output"), QStringLiteral("Manifest output path."),
              QStringLiteral("path") },
    });
    try
    {
        if (!parser.parse(arguments))
            throw PipeASIOManager::Error(parser.errorText());
        if (parser.isSet(QStringLiteral("help")))
            parser.showHelp();
        const QStringList positional = parser.positionalArguments();
        if (positional.size() != 1)
            throw PipeASIOManager::Error(
                    QStringLiteral("Choose one command. Use --help for usage."));
        const QString                    command = positional.first();
        const QMap<QString, QStringList> allowed = {
            { QStringLiteral("list"), {} },
            { QStringLiteral("releases"), {} },
            { QStringLiteral("add"),
              { QStringLiteral("prefix"), QStringLiteral("wine"), QStringLiteral("name") } },
            { QStringLiteral("preview"),
              { QStringLiteral("target"), QStringLiteral("release"),
                QStringLiteral("include-32") } },
            { QStringLiteral("install"),
              { QStringLiteral("target"), QStringLiteral("release"), QStringLiteral("include-32"),
                QStringLiteral("allow-permissions") } },
            { QStringLiteral("check"), { QStringLiteral("target") } },
            { QStringLiteral("remove"), { QStringLiteral("target") } },
            { QStringLiteral("manifest"),
              { QStringLiteral("version"), QStringLiteral("asset"), QStringLiteral("root"),
                QStringLiteral("architecture"), QStringLiteral("wine-sdk"),
                QStringLiteral("output") } },
        };
        if (!allowed.contains(command))
            throw PipeASIOManager::Error(QStringLiteral("Unknown command: ") + command);
        for (const QString &option : parser.optionNames())
            if (option != QStringLiteral("json") && !allowed.value(command).contains(option))
                throw PipeASIOManager::Error(QStringLiteral("Option --") + option
                                             + QStringLiteral(" does not apply to ") + command);
        const auto required = [&](const QString &name)
        {
            const QString value = parser.value(name);
            if (value.isEmpty())
                throw PipeASIOManager::Error(QStringLiteral("Missing --") + name
                                             + QStringLiteral(" for ") + command);
            return value;
        };
        const PipeASIOManager::Progress progress = [json](const QString &message)
        {
            if (json)
                writeOutput({ { QStringLiteral("event"), QStringLiteral("progress") },
                              { QStringLiteral("message"), message } },
                            true);
            else
            {
                const QByteArray text = message.toUtf8();
                std::fprintf(stderr, "%s\n", text.constData());
            }
        };
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        QNetworkAccessManager network;
        QJsonObject           result;
        if (command == QStringLiteral("list"))
            result.insert(QStringLiteral("targets"), PipeASIOManager::Installer::listTargets());
        else if (command == QStringLiteral("releases"))
            result.insert(QStringLiteral("releases"),
                          PipeASIOManager::Releases::list(network, progress));
        else if (command == QStringLiteral("add"))
            result = PipeASIOManager::Installer::addPrefix(required(QStringLiteral("prefix")),
                                                           required(QStringLiteral("wine")),
                                                           parser.value(QStringLiteral("name")));
        else if (command == QStringLiteral("preview"))
            result = PipeASIOManager::Installer::preview(
                    network, required(QStringLiteral("target")),
                    parser.value(QStringLiteral("release")),
                    parser.isSet(QStringLiteral("include-32")));
        else if (command == QStringLiteral("install"))
            result = PipeASIOManager::Installer::install(
                    network, required(QStringLiteral("target")),
                    parser.value(QStringLiteral("release")),
                    parser.isSet(QStringLiteral("include-32")),
                    parser.isSet(QStringLiteral("allow-permissions")), progress);
        else if (command == QStringLiteral("check"))
            result = PipeASIOManager::Installer::check(required(QStringLiteral("target")),
                                                       progress);
        else if (command == QStringLiteral("remove"))
            result = PipeASIOManager::Installer::remove(required(QStringLiteral("target")),
                                                        progress);
        else
        {
            result = PipeASIOManager::Releases::createManifest(
                    required(QStringLiteral("version")), required(QStringLiteral("asset")),
                    required(QStringLiteral("root")), required(QStringLiteral("wine-sdk")),
                    parser.value(QStringLiteral("architecture")));
            const QString output = parser.isSet(QStringLiteral("output"))
                                           ? parser.value(QStringLiteral("output"))
                                           : QStringLiteral("pipeasio-release.json");
            PipeASIOManager::writeJson(output, result);
        }
        PipeASIOManager::throwIfCancelled();
        return writeOutput(
                       json ? QJsonObject{ { QStringLiteral("event"), QStringLiteral("result") },
                                           { QStringLiteral("ok"), true },
                                           { QStringLiteral("data"), result } }
                            : result,
                       json)
                       ? 0
                       : 1;
    }
    catch (const std::exception &failure)
    {
        const QString message = QString::fromUtf8(failure.what());
        if (json)
            writeOutput({ { QStringLiteral("event"), QStringLiteral("result") },
                          { QStringLiteral("ok"), false },
                          { QStringLiteral("error"), message } },
                        true);
        else
            std::fprintf(stderr, "pipeasio-manage: %s\n", failure.what());
        return message == QStringLiteral("Operation cancelled.") ? 130 : 1;
    }
}
