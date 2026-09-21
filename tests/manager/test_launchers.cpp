/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Manager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QSet>
#include <QTemporaryDir>
#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <functional>
#include <unistd.h>

using namespace PipeASIOManager;

static int total = 0;
static int failures = 0;
#define CHECK(condition) do { ++total; if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL %s:%d CHECK(%s)\n", __FILE__, __LINE__, #condition); } } while (0)

static bool rejects(const std::function<void()> &operation, const QString &message = {})
{
    try
    {
        operation();
    }
    catch (const Error &error)
    {
        return message.isEmpty() || QString::fromUtf8(error.what()).contains(message);
    }
    return false;
}

static int fixture(const QStringList &arguments)
{
    const QString root = qEnvironmentVariable("PIPEASIO_FIXTURE_ROOT");
    const QString app = "com.usebottles.bottles";
    writeFile(root + "/executed", arguments.join('\n').toUtf8());
    QStringList command = arguments.mid(1);
    QJsonObject supplied;
    QString runner = QCoreApplication::applicationFilePath();
    if (QFileInfo(arguments[0]).fileName() == "flatpak")
    {
        if (command == QStringList{"info", "--show-location", app})
        {
            const QString mutationPath = root + "/concurrent-write.json";
            if (QFileInfo::exists(mutationPath))
            {
                auto mutation = readJson(mutationPath).toObject();
                const int remaining = mutation.value("after").toInt() - 1;
                mutation.insert("after", remaining);
                writeJson(mutationPath, mutation);
                if (remaining == 0)
                    writeFile(mutation.value("path").toString(), mutation.value("content").toString().toUtf8());
            }
            const QString location = readJson(root + "/flatpak-state.json", QJsonObject()).toObject().value("deployment").toString(root + "/data/flatpak/app/deployment");
            std::printf("%s\n", qPrintable(location));
            return 0;
        }
        if (command.size() == 4 && command[0] == "info" && command[1] == "--user"
            && command[2].startsWith("--file-access=") && command[3] == app)
        {
            const QString path = command[2].mid(14);
            const auto state = readJson(root + "/flatpak-state.json", QJsonObject()).toObject();
            QString mode = state.value("access").toObject().value(path).toString("hidden");
            const QString overrides = root + "/data/flatpak/overrides/" + app;
            if (QFileInfo::exists(overrides))
            {
                const QString text = QString::fromUtf8(readFile(overrides));
                for (const auto &line : text.split('\n'))
                {
                    if (!line.startsWith("filesystems="))
                        continue;
                    for (const auto &entry : line.mid(12).split(';'))
                    {
                        QString grant = entry;
                        if (grant.startsWith("xdg-run/"))
                            grant = root + "/runtime/" + grant.mid(8);
                        if (grant == path)
                            mode = "read-write";
                        if (grant == path + ":ro")
                            mode = "read-only";
                        if (grant == '!' + path)
                            mode = "hidden";
                    }
                }
            }
            std::printf("%s\n", qPrintable(mode));
            return 0;
        }
        if (command.mid(0, 4) != QStringList{"run", "--user", "--command=/usr/bin/env", app})
            return 91;
        command = command.mid(4);
        while (!command.isEmpty() && !command[0].startsWith('/'))
        {
            const QString assignment = command.takeFirst();
            const auto separator = assignment.indexOf('=');
            if (separator <= 0)
                return 92;
            supplied.insert(assignment.left(separator), assignment.mid(separator + 1));
        }
        if (command.isEmpty())
            return 93;
        runner = command.takeFirst();
    }
    else
    {
        const auto process = QProcessEnvironment::systemEnvironment();
        for (const auto &key : process.keys())
            supplied.insert(key, process.value(key));
    }
    const auto expected = readJson(root + "/expect.json").toObject();
    if (runner != expected.value("runner").toString()
        || QDir::currentPath() != expected.value("cwd").toString())
        return 94;
    QJsonArray actualArguments;
    for (const auto &argument : command)
        actualArguments.append(argument);
    if (actualArguments != expected.value("arguments").toArray())
        return 95;
    const auto required = expected.value("environment").toObject();
    for (auto it = required.begin(); it != required.end(); ++it)
        if (it.value().isNull() ? supplied.contains(it.key()) : supplied.value(it.key()) != it.value())
            return 96;
    std::printf("fixture accepted\n");
    return 37;
}

struct Fixture
{
    QTemporaryDir directory;
    QProcessEnvironment original = QProcessEnvironment::systemEnvironment();
    QString home = directory.path();
    QString data = home + "/data";
    QString config = home + "/config";

    Fixture()
    {
        if (!directory.isValid())
            throw Error("Cannot create launcher test fixture");
        for (const auto &key : original.keys())
            qunsetenv(key.toLocal8Bit().constData());
        qputenv("HOME", home.toUtf8());
        qputenv("XDG_DATA_HOME", data.toUtf8());
        qputenv("XDG_CONFIG_HOME", config.toUtf8());
        qputenv("HOST_XDG_DATA_HOME", data.toUtf8());
        qputenv("XDG_RUNTIME_DIR", (home + "/runtime").toUtf8());
        qputenv("PATH", (home + "/bin:/usr/bin:/bin").toUtf8());
        qputenv("PIPEASIO_FIXTURE_ROOT", home.toUtf8());
    }

    ~Fixture()
    {
        const auto current = QProcessEnvironment::systemEnvironment();
        for (const auto &key : current.keys())
            qunsetenv(key.toLocal8Bit().constData());
        for (const auto &key : original.keys())
            qputenv(key.toLocal8Bit().constData(), original.value(key).toLocal8Bit());
    }

    QString prefix(const QString &path)
    {
        if (!QDir().mkpath(path + "/drive_c"))
            throw Error("Cannot create fixture prefix");
        writeFile(path + "/system.reg", "WINE REGISTRY Version 2\n");
        return path;
    }

    QString executable(const QString &path)
    {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())
            || !QFile::copy(QCoreApplication::applicationFilePath(), path)
            || !QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
            throw Error("Cannot create strict native launcher fixture " + path);
        return path;
    }

    QString faugus(const QString &arguments = {}, const QString &runner = "Chosen Proton")
    {
        const QString selected = prefix(home + "/Faugus/DAW with spaces");
        executable(data + "/Steam/compatibilitytools.d/" + runner + "/proton");
        executable(data + "/faugus-launcher/umu-run");
        writeJson(data + "/faugus-launcher/games.json", QJsonArray{QJsonObject{
            {"gameid", "daw"}, {"title", "DAW"}, {"prefix", selected}, {"runner", runner},
            {"path", selected + "/drive_c/program.exe"}, {"launch_arguments", arguments},
            {"game_arguments", "--document 'song file.wav'"}, {"unknown", QJsonObject{{"preserve", true}}}}});
        return data + "/faugus-launcher/games.json";
    }

    QString bottle(const QString &base = {}, const QString &root = {})
    {
        const QString directory = base.isEmpty() ? data + "/bottles" : base;
        const QString bottles = root.isEmpty() ? directory + "/bottles" : root;
        const QString selected = prefix(bottles + "/Studio Bottle");
        if (!QFileInfo::exists(directory + "/runners/soda-pinned/bin/wine"))
            executable(directory + "/runners/soda-pinned/bin/wine");
        writeFile(selected + "/bottle.yml", "Name: Studio\nPath: Studio Bottle\nCustom_Path: false\nRunner: soda-pinned\nArch: win64\nParameters: {}\nEnvironment_Variables:\n  KEEP: 'value=with spaces'\n  EMPTY: ''\nUnknown_Setting:\n  Enabled: true\n  Count: 3\n");
        return selected + "/bottle.yml";
    }

    QJsonObject selected(const QString &kind)
    {
        for (const auto &value : Launchers::discover())
            if (value.toObject().value("kind") == kind && value.toObject().value("error").toString().isEmpty())
                return value.toObject();
        throw Error("No usable fixture target for " + kind);
    }

    void expect(const QString &runner, const QString &cwd, const QJsonObject &environment)
    {
        writeJson(home + "/expect.json", QJsonObject{{"runner", runner}, {"cwd", cwd},
            {"arguments", QJsonArray{"tool with spaces.exe", "argument with spaces", "quote'\\\""}}, {"environment", environment}});
    }

    CommandResult run(const QJsonObject &target, const QJsonObject &environment = {})
    {
        return Launchers::run(target, {"tool with spaces.exe", "argument with spaces", "quote'\\\""}, environment);
    }
};

static void faugusIdentityAndPrecedence()
{
    Fixture f;
    const QString path = f.faugus();
    writeJson(f.config + "/faugus-launcher/config.json", QJsonObject{{"default-runner", "Wrong Proton"}});
    auto target = f.selected("faugus");
    CHECK(target.value("runner") == f.data + "/Steam/compatibilitytools.d/Chosen Proton");
    CHECK(target.value("prefix") == f.home + "/Faugus/DAW with spaces");
    auto games = readJson(path).toArray();
    auto game = games[0].toObject();
    game.insert("prefix", "..");
    games[0] = game;
    writeJson(path, games);
    CHECK(f.selected("faugus").value("prefix") == target.value("prefix"));
    const QString redirected = f.prefix(f.home + "/Redirected Prefix");
    writeJson(f.config + "/faugus-launcher/envar.json", QJsonArray{"WINEPREFIX=" + redirected});
    CHECK(f.selected("faugus").value("prefix") == redirected);
    const auto before = readFile(path);
    CHECK(rejects([&] { Launchers::setEnvironment(target, {{"WINEDLLPATH", "/payload"}}); }, "prefix or runner changed"));
    CHECK(readFile(path) == before);
}

static void faugusEmptyRunner()
{
    Fixture f;
    const QString path = f.faugus({}, "UMU-Latest");
    auto games = readJson(path).toArray();
    auto game = games[0].toObject();
    game.insert("runner", "");
    games[0] = game;
    writeJson(path, games);
    writeJson(f.config + "/faugus-launcher/config.json", QJsonObject{{"default-runner", "Wrong Proton"}});
    CHECK(f.selected("faugus").value("runner") == f.data + "/Steam/compatibilitytools.d/UMU-Latest");
}

static void discoveryDoesNotMutate()
{
    Fixture f;
    const QString game = f.faugus();
    const QString bottle = f.bottle();
    const auto beforeGame = readFile(game);
    const auto beforeBottle = readFile(bottle);
    const auto targets = Launchers::discover();
    CHECK(targets.size() == 2);
    CHECK(readFile(game) == beforeGame);
    CHECK(readFile(bottle) == beforeBottle);
    CHECK(!QFileInfo::exists(f.home + "/executed"));
}

static void invalidLauncherIsolation()
{
    Fixture f;
    f.faugus();
    const QString path = f.bottle();
    writeFile(path, "Name: [broken\n");
    f.prefix(f.data + "/bottles/bottles/Other");
    writeFile(f.data + "/bottles/bottles/Other/bottle.yml", "Name: Other\nPath: Other\nRunner: soda-pinned\n");
    QDir().mkpath(f.home + "/.var/app/com.usebottles.bottles/data/bottles");
    f.executable(f.home + "/bin/flatpak");
    writeJson(f.home + "/flatpak-state.json", QJsonObject{{"deployment", f.home + "/named-installation"}});
    const auto targets = Launchers::discover();
    QSet<QString> usable;
    int invalid = 0;
    for (const auto &value : targets)
    {
        const auto target = value.toObject();
        if (target.value("error").toString().isEmpty())
            usable.insert(target.value("name").toString());
        else
            ++invalid;
    }
    CHECK(usable == QSet<QString>({"DAW", "Other"}));
    CHECK(invalid == 2);
}

static void faugusArgumentRoundtrip()
{
    Fixture f;
    const QString original = "KEEP='value with spaces' --profile 'my profile'  ";
    const QString path = f.faugus(original);
    const auto target = f.selected("faugus");
    Launchers::setEnvironment(target, {{"WINEDLLPATH", "/driver path"}, {"PROTON_USE_WOW64", "1"}});
    CHECK(Launchers::environment(target).value("WINEDLLPATH") == "/driver path");
    Launchers::setEnvironment(target, {{"WINEDLLPATH", QJsonValue::Null}, {"PROTON_USE_WOW64", QJsonValue::Null}});
    const auto game = readJson(path).toArray()[0].toObject();
    CHECK(game.value("launch_arguments") == original);
    CHECK(game.value("game_arguments") == "--document 'song file.wav'");
    CHECK(game.value("unknown").toObject() == QJsonObject({{"preserve", true}}));
}

static void faugusQuotedTokens()
{
    Fixture f;
    const QString path = f.faugus("KEEP='a b'  --flag=thing\tWINEDLLOVERRIDES='old=n,b' --name 'my project' ESC=one\\ two DQ=\"a\\qb\"");
    const auto target = f.selected("faugus");
    const auto configured = Launchers::environment(target);
    CHECK(configured.value("KEEP") == "a b");
    CHECK(configured.value("ESC") == "one two");
    CHECK(configured.value("DQ") == "a\\qb");
    Launchers::setEnvironment(target, {{"WINEDLLOVERRIDES", "old=n,b;wineasio=n,b"}, {"SPECIAL", "a'b\\c\"d"}});
    const auto game = readJson(path).toArray()[0].toObject();
    CHECK(game.value("launch_arguments").toString().startsWith("KEEP='a b'  --flag=thing\t"));
    CHECK(game.value("launch_arguments").toString().contains(" --name 'my project' ESC=one\\ two DQ=\"a\\qb\""));
    CHECK(Launchers::environment(target).value("SPECIAL") == "a'b\\c\"d");
}

static void faugusExpandsBeforeTokenization()
{
    Fixture f;
    qputenv("PIPEASIO_ARGUMENT", "two words");
    const QString path = f.faugus("SINGLE='$PIPEASIO_ARGUMENT' DOUBLE=\"${PIPEASIO_ARGUMENT}\" RAW=$PIPEASIO_ARGUMENT UNSET='$PIPEASIO_UNSET' TILDE=~/literal");
    auto games = readJson(path).toArray();
    auto game = games[0].toObject();
    game.insert("game_arguments", "GAME='$PIPEASIO_ARGUMENT'");
    games[0] = game;
    writeJson(path, games);
    CHECK(Launchers::environment(f.selected("faugus")) == QJsonObject({
        {"SINGLE", "two words"}, {"DOUBLE", "two words"}, {"RAW", "two"},
        {"UNSET", "$PIPEASIO_UNSET"}, {"TILDE", "~/literal"}, {"GAME", "two words"}}));
}

static void faugusGlobalBlocksMutation()
{
    Fixture f;
    const QString path = f.faugus();
    writeJson(f.config + "/faugus-launcher/envar.json", QJsonArray{"WINEDLLPATH=/global"});
    const auto target = f.selected("faugus");
    const auto before = readFile(path);
    CHECK(rejects([&] { Launchers::setEnvironment(target, {{"WINEDLLPATH", "/managed"}}); }, "global environment"));
    CHECK(readFile(path) == before);
}

static void faugusChildContext()
{
    Fixture f;
    f.faugus("CONFIGURED=from-game");
    writeJson(f.config + "/faugus-launcher/config.json", QJsonObject{{"wow64-enabled", "True"}, {"wayland-driver", "True"}});
    const auto target = f.selected("faugus");
    qputenv("WINEPREFIX", "/wrong-prefix");
    qputenv("PROTONPATH", "/wrong-runner");
    qputenv("FAUGUSID", "wrong-game");
    f.expect(f.data + "/faugus-launcher/umu-run", target.value("prefix").toString() + "/drive_c",
        {{"WINEPREFIX", target.value("prefix")}, {"PROTONPATH", target.value("runner")},
         {"CONFIGURED", "per-launch"}, {"PROTON_VERB", "waitforexitandrun"}, {"UMU_RUNTIME_UPDATE", "0"},
         {"PROTON_USE_WOW64", "1"}, {"PROTON_ENABLE_WAYLAND", "1"}, {"FAUGUSID", QJsonValue::Null}});
    const auto result = f.run(target, {{"CONFIGURED", "per-launch"}, {"WINEPREFIX", "/injected"}, {"PROTONPATH", "/injected"}});
    CHECK(result.exitCode == 37);
    CHECK(result.output == "fixture accepted\n");
}

static void customBottleRoot()
{
    Fixture f;
    const QString root = f.home + "/Custom Bottles With Spaces";
    const QString path = f.bottle({}, root);
    writeFile(f.data + "/bottles/data.yml", ("custom_bottles_path: '" + root + "'\n").toUtf8());
    const auto target = f.selected("bottles");
    CHECK(target.value("prefix") == QFileInfo(path).absolutePath());
    CHECK(target.value("runner") == f.data + "/bottles/runners/soda-pinned/bin/wine");
}

static void bottlePlaceholder()
{
    Fixture f;
    const QString path = f.bottle({}, f.home + "/External");
    auto config = YAML::Load(readFile(path).toStdString());
    config["Path"] = QFileInfo(path).absolutePath().toStdString();
    config["Custom_Path"] = true;
    writeFile(path, QByteArray::fromStdString(YAML::Dump(config)));
    const QString link = f.data + "/bottles/bottles/link/placeholder.yml";
    writeFile(link, ("Path: '" + QFileInfo(path).absolutePath() + "'\n").toUtf8());
    const auto target = f.selected("bottles");
    CHECK(target.value("prefix") == QFileInfo(path).absolutePath());
    writeFile(link, ("Path: '" + f.home + "/Missing'\n").toUtf8());
    CHECK(rejects([&] { f.run(target); }, "removed"));
    const auto unavailable = Launchers::discover()[0].toObject();
    CHECK(unavailable.value("error") == "The custom bottle location is unavailable");
}

static void runnerChangeRejected()
{
    Fixture f;
    const QString path = f.bottle();
    const auto target = f.selected("bottles");
    f.executable(f.data + "/bottles/runners/other/bin/wine");
    auto config = YAML::Load(readFile(path).toStdString());
    config["Runner"] = "other";
    writeFile(path, QByteArray::fromStdString(YAML::Dump(config)));
    const auto before = readFile(path);
    CHECK(rejects([&] { Launchers::setEnvironment(target, {{"WINEDLLPATH", "/payload"}}); }, "prefix or runner changed"));
    CHECK(rejects([&] { f.run(target); }, "prefix or runner changed"));
    CHECK(readFile(path) == before);
}

static void missingRunnerHasNoFallback()
{
    Fixture f;
    f.bottle();
    QFile::remove(f.data + "/bottles/runners/soda-pinned/bin/wine");
    f.executable(f.home + "/bin/wine");
    const auto target = Launchers::discover()[0].toObject();
    CHECK(target.value("status") == "unsupported");
    CHECK(target.value("runner").toString().isEmpty());
    CHECK(target.value("error").toString().contains("not executable"));
}

static void bottleEnvironmentRestoration()
{
    Fixture f;
    const QString path = f.bottle();
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup);
    const auto permissions = QFileInfo(path).permissions();
    const auto target = f.selected("bottles");
    Launchers::setEnvironment(target, {{"EMPTY", "installed"}, {"WINEDLLPATH", "/payload"}, {"NUMBER", "123"}});
    CHECK(Launchers::environment(target).value("NUMBER") == "123");
    Launchers::setEnvironment(target, {{"EMPTY", ""}, {"WINEDLLPATH", QJsonValue::Null}, {"NUMBER", QJsonValue::Null}});
    const auto config = YAML::Load(readFile(path).toStdString());
    CHECK(config["Environment_Variables"].size() == 2);
    CHECK(config["Environment_Variables"]["EMPTY"].as<std::string>().empty());
    CHECK(config["Environment_Variables"]["KEEP"].as<std::string>() == "value=with spaces");
    CHECK(config["Unknown_Setting"]["Enabled"].as<bool>());
    CHECK(config["Unknown_Setting"]["Count"].as<int>() == 3);
    CHECK(QFileInfo(path).permissions() == permissions);
}

static void yamlAnchorsAndMultiline()
{
    Fixture f;
    const QString path = f.bottle();
    writeFile(path, "Name: Studio\nPath: Studio Bottle\nRunner: soda-pinned\nDefaults: &env\n  KEEP: 'true'\n  MULTILINE: |\n    first line\n    second line\nEnvironment_Variables:\n  <<: *env\n  EMPTY: ''\nUnknown: [true, 3, null, '3']\n");
    const auto target = f.selected("bottles");
    Launchers::setEnvironment(target, {{"WINEDLLPATH", "/payload"}});
    const auto environment = Launchers::environment(target);
    CHECK(environment.value("KEEP") == "true");
    CHECK(environment.value("MULTILINE") == "first line\nsecond line\n");
    const auto config = YAML::Load(readFile(path).toStdString());
    CHECK(config["Unknown"][0].as<bool>());
    CHECK(config["Unknown"][1].as<int>() == 3);
    CHECK(config["Unknown"][2].IsNull());
    CHECK(config["Unknown"][3].as<std::string>() == "3");
    CHECK(!config["Defaults"]["WINEDLLPATH"]);
}

static void yamlScalarTypesSurviveEnvironmentEdits()
{
    Fixture f;
    const QString path = f.bottle();
    writeFile(path, "Name: Studio\nPath: Studio Bottle\nCustom_Path: false\nRunner: soda-pinned\n"
        "Environment_Variables:\n  BOOL_TRUE: 'true'\n  BOOL_FALSE: \"false\"\n  NUMBER: '3'\n  DATE: '2026-09-10'\n  EMPTY: ''\n"
        "Unknown: [true, false, 3, 2026-09-10, null, 'true', 'false', '3', '2026-09-10', '']\n"
        "'false': 'true'\nKeys: {'true': 'false'}\n");
    const auto target = f.selected("bottles");
    const QJsonObject expected{{"BOOL_TRUE", "true"}, {"BOOL_FALSE", "false"}, {"NUMBER", "3"},
        {"DATE", "2026-09-10"}, {"EMPTY", ""}};
    for (const auto &update : {QJsonValue("/payload"), QJsonValue(QJsonValue::Null)})
    {
        Launchers::setEnvironment(target, {{"WINEDLLPATH", update}});
        auto environment = Launchers::environment(target);
        environment.remove("WINEDLLPATH");
        CHECK(environment == expected);
        const auto config = YAML::Load(readFile(path).toStdString());
        CHECK(!config["Custom_Path"].as<bool>());
        CHECK(config["Unknown"][0].as<bool>());
        CHECK(!config["Unknown"][1].as<bool>());
        CHECK(config["false"].Scalar() == "true");
        CHECK(config["Keys"]["true"].Scalar() == "false");
        CHECK(config["Unknown"][2].as<int>() == 3);
        CHECK(config["Unknown"][3].Scalar() == "2026-09-10");
        CHECK(config["Unknown"][4].IsNull());
        for (int i = 0; i < 4; ++i)
        {
            CHECK(config["Unknown"][i].Tag() != "!");
            CHECK(config["Unknown"][i].Tag() != "tag:yaml.org,2002:str");
        }
        const QStringList strings{"true", "false", "3", "2026-09-10", ""};
        for (qsizetype i = 0; i < strings.size(); ++i)
        {
            const auto value = config["Unknown"][i + 5];
            CHECK(QString::fromStdString(value.Scalar()) == strings[i]);
            CHECK(value.Tag() == "!" || value.Tag() == "tag:yaml.org,2002:str");
        }
        for (const auto &mapping : {config, config["Keys"]})
            for (const auto &entry : mapping)
                if (entry.first.Scalar() == "true" || entry.first.Scalar() == "false")
                {
                    CHECK(entry.first.Tag() == "!" || entry.first.Tag() == "tag:yaml.org,2002:str");
                    CHECK(entry.second.Tag() == "!" || entry.second.Tag() == "tag:yaml.org,2002:str");
                }
    }
}

static void corruptConfigCannotBeOverwritten()
{
    Fixture f;
    const QString path = f.bottle();
    const auto target = f.selected("bottles");
    const QByteArray corrupt = "Name: Studio\nPath: Studio Bottle\nRunner: soda-pinned\nEnvironment_Variables: []\n";
    writeFile(path, corrupt);
    CHECK(rejects([&] { Launchers::setEnvironment(target, {{"WINEDLLPATH", "/payload"}}); }, "mapping"));
    CHECK(readFile(path) == corrupt);
    const QByteArray duplicate = "Name: Studio\nPath: Studio Bottle\nRunner: soda-pinned\nRunner: other\n";
    writeFile(path, duplicate);
    CHECK(rejects([&] { Launchers::setEnvironment(target, {{"WINEDLLPATH", "/payload"}}); }));
    CHECK(readFile(path) == duplicate);
}

static void concurrentConfigChange()
{
    Fixture f;
    f.executable(f.home + "/bin/flatpak");
    const QString base = f.home + "/.var/app/com.usebottles.bottles/data/bottles";
    const QString path = f.bottle(base);
    const auto target = f.selected("bottles-flatpak");
    QByteArray changed = readFile(path);
    changed.replace("Name: Studio", "Name: User edit");
    writeJson(f.home + "/concurrent-write.json", QJsonObject{
        {"after", 2}, {"path", path}, {"content", QString::fromUtf8(changed)}});
    CHECK(rejects([&] { Launchers::setEnvironment(target, {{"WINEDLLPATH", "/payload"}}); }, "changed concurrently"));
    CHECK(readFile(path) == changed);
}

static void symlinkConfigRefused()
{
    Fixture f;
    const QString path = f.bottle();
    const QByteArray before = readFile(path);
    const QString backing = f.home + "/config-source.yml";
    QFile::rename(path, backing);
    QFile::link(backing, path);
    const auto target = f.selected("bottles");
    CHECK(rejects([&] { Launchers::setEnvironment(target, {{"WINEDLLPATH", "/payload"}}); }, "symlinked"));
    CHECK(QFileInfo(path).isSymLink());
    CHECK(readFile(backing) == before);
}

static void bottleRuntimeContext()
{
    Fixture f;
    const QString path = f.bottle();
    auto config = YAML::Load(readFile(path).toStdString());
    config["Parameters"]["sync"] = "fsync";
    config["Parameters"]["use_runtime"] = true;
    config["DLL_Overrides"]["d3d11"] = "n,b";
    config["Environment_Variables"]["LD_LIBRARY_PATH"] = "/configured";
    config["Environment_Variables"]["WINEDLLOVERRIDES"] = "other=n";
    writeFile(path, QByteArray::fromStdString(YAML::Dump(config)));
    QDir().mkpath(f.data + "/bottles/runtimes/lib");
    QDir().mkpath(f.data + "/bottles/runtimes/lib32");
    QDir().mkpath(f.data + "/bottles/runners/soda-pinned/lib64");
    const auto target = f.selected("bottles");
    f.expect(target.value("runner").toString(), target.value("prefix").toString(),
        {{"WINEPREFIX", target.value("prefix")}, {"WINEARCH", "win64"}, {"WINEFSYNC", "1"},
         {"KEEP", "value=with spaces"}, {"WINEDLLOVERRIDES", "other=n;d3d11=n,b;winemenubuilder.exe=d"},
         {"LD_LIBRARY_PATH", "/configured:" + f.data + "/bottles/runtimes/lib:" + f.data + "/bottles/runtimes/lib32:" + f.data + "/bottles/runners/soda-pinned/lib64"}});
    CHECK(f.run(target, {{"WINEPREFIX", "/wrong"}, {"WINEARCH", "win32"}}).exitCode == 37);
    QDir(f.data + "/bottles/runtimes/lib32").removeRecursively();
    CHECK(rejects([&] { f.run(target); }, "runtime is missing"));
}

static void unsupportedBottleContexts()
{
    for (const auto &parameter : {"sandbox", "use_steam_runtime"})
    {
        Fixture f;
        const QString path = f.bottle();
        auto config = YAML::Load(readFile(path).toStdString());
        config["Parameters"][parameter] = true;
        writeFile(path, QByteArray::fromStdString(YAML::Dump(config)));
        CHECK(Launchers::discover()[0].toObject().value("status") == "unsupported");
    }
}

static void flatpakContextAndPermissions()
{
    Fixture f;
    f.executable(f.home + "/bin/flatpak");
    const QString base = f.home + "/.var/app/com.usebottles.bottles/data/bottles";
    const QString path = f.bottle(base);
    const QString payload = f.home + "/payload";
    QDir().mkpath(payload);
    f.executable(f.data + "/bottles/runners/soda-pinned/bin/wine");
    const auto target = f.selected("bottles-flatpak");
    CHECK(target.value("runner") == base + "/runners/soda-pinned/bin/wine");
    f.expect(target.value("runner").toString(), QFileInfo(path).absolutePath(),
        {{"WINEPREFIX", QFileInfo(path).absolutePath()}, {"WINEARCH", "win64"},
         {"WINEDLLOVERRIDES", "winemenubuilder.exe=d"}, {"LD_LIBRARY_PATH", "/app/lib:/app/lib/i386-linux-gnu"},
         {"KEEP", "value=with spaces"}, {"EMPTY", ""}});
    qputenv("LD_LIBRARY_PATH", "/wrong-host-libraries");
    CHECK(f.run(target).exitCode == 37);
    const QString overridePath = f.data + "/flatpak/overrides/com.usebottles.bottles";
    writeFile(overridePath, "[Context]\nfilesystems=!xdg-run/pipewire-0;/keep:ro;\n[Environment]\nKEEP=untouched\n");
    const QStringList expected{QFileInfo(path).absolutePath(), payload + ":ro", "xdg-run/pipewire-0"};
    CHECK(Launchers::permissionPlan(target, payload) == expected);
    CHECK(Launchers::grantPermissions(target, payload) == expected);
    CHECK(Launchers::permissionPlan(target, payload).isEmpty());
    CHECK(Launchers::grantPermissions(target, payload).isEmpty());
    const QString contents = QString::fromUtf8(readFile(overridePath));
    CHECK(contents.contains("/keep:ro;"));
    CHECK(contents.contains("[Environment]\nKEEP=untouched\n"));
    CHECK(!contents.contains("!xdg-run/pipewire-0"));
    CHECK(!contents.contains("host;"));
}

static void flatpakInaccessibleCustomRoot()
{
    Fixture f;
    f.executable(f.home + "/bin/flatpak");
    const QString base = f.home + "/.var/app/com.usebottles.bottles/data/bottles";
    const QString defaultPath = f.bottle(base);
    const QString custom = f.home + "/Host Only Bottles";
    f.bottle(base, custom);
    writeFile(base + "/data.yml", ("custom_bottles_path: '" + custom + "'\n").toUtf8());
    CHECK(f.selected("bottles-flatpak").value("prefix") == QFileInfo(defaultPath).absolutePath());
}

static void manualOwnershipGuards()
{
    Fixture f;
    const QString path = f.bottle();
    const QString wine = f.executable(f.home + "/wine");
    CHECK(rejects([&] { Launchers::manualTarget(QFileInfo(path).absolutePath(), wine); }, "belongs to bottles"));
    const QString prefix = f.prefix(f.home + "/Manual");
    const auto target = Launchers::manualTarget(prefix, wine);
    writeFile(prefix + "/tracked_files", "");
    CHECK(rejects([&] { Launchers::manualTarget(prefix, wine); }, "launcher-managed"));
    CHECK(rejects([&] { Launchers::setEnvironment(target, {{"WINEDLLPATH", "/payload"}}); }, "launcher-managed"));
    CHECK(rejects([&] { f.run(target); }, "launcher-managed"));
}

static void manualEnvironmentAndStatus()
{
    Fixture f;
    const QString prefix = f.prefix(f.home + "/Manual Prefix");
    const QString runner = f.executable(f.home + "/Manual Wine");
    const auto target = Launchers::manualTarget(prefix, runner);
    Launchers::setEnvironment(target, {{"KEEP", "persisted"}, {"EMPTY", ""}});
    f.expect(runner, prefix, {{"WINEPREFIX", prefix}, {"KEEP", "per-launch"}, {"EMPTY", ""}});
    const auto result = f.run(target, {{"KEEP", "per-launch"}, {"WINEPREFIX", "/wrong-prefix"}});
    CHECK(result.exitCode == 37);
    CHECK(result.output == "fixture accepted\n");
    CHECK(Launchers::environment(target).value("KEEP") == "persisted");
    Launchers::setEnvironment(target, {{"KEEP", QJsonValue::Null}, {"EMPTY", QJsonValue::Null}});
    CHECK(Launchers::environment(target).isEmpty());
    const QString environment = target.value("metadata").toObject().value("environment_file").toString();
    CHECK((QFileInfo(environment).permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)) == 0);
}

static void hostLoaderRestoration()
{
    Fixture f;
    const QString prefix = f.prefix(f.home + "/Manual");
    const QString runner = f.executable(f.home + "/wine");
    const auto target = Launchers::manualTarget(prefix, runner);
    qputenv("APPDIR", "/appimage");
    qputenv("PIPEASIO_ORIGINAL_SET_LD_LIBRARY_PATH", "1");
    qputenv("PIPEASIO_ORIGINAL_LD_LIBRARY_PATH", "/host/lib");
    qputenv("LD_LIBRARY_PATH", "/appimage/lib");
    qputenv("QT_PLUGIN_PATH", "/appimage/plugins");
    qunsetenv("PIPEASIO_ORIGINAL_SET_QT_QPA_PLATFORMTHEME");
    qunsetenv("PIPEASIO_ORIGINAL_QT_QPA_PLATFORMTHEME");
    qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    qunsetenv("PIPEASIO_ORIGINAL_SET_QT_STYLE_OVERRIDE");
    qunsetenv("PIPEASIO_ORIGINAL_QT_STYLE_OVERRIDE");
    qputenv("QT_STYLE_OVERRIDE", "Fusion");
    f.expect(runner, prefix, {{"LD_LIBRARY_PATH", "/host/lib"}, {"QT_PLUGIN_PATH", QJsonValue::Null},
        {"QT_QPA_PLATFORMTHEME", QJsonValue::Null}, {"QT_STYLE_OVERRIDE", QJsonValue::Null},
        {"APPDIR", QJsonValue::Null},
        {"PIPEASIO_ORIGINAL_LD_LIBRARY_PATH", QJsonValue::Null},
        {"PIPEASIO_ORIGINAL_SET_LD_LIBRARY_PATH", QJsonValue::Null}});
    CHECK(f.run(target).exitCode == 37);
}

static void hostPlatformThemeRestoration()
{
    Fixture f;
    const QString prefix = f.prefix(f.home + "/Manual");
    const QString runner = f.executable(f.home + "/wine");
    const auto target = Launchers::manualTarget(prefix, runner);
    qputenv("APPDIR", "/appimage");
    qputenv("PIPEASIO_ORIGINAL_SET_QT_QPA_PLATFORMTHEME", "1");
    qputenv("PIPEASIO_ORIGINAL_QT_QPA_PLATFORMTHEME", "qt6ct");
    qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    qputenv("PIPEASIO_ORIGINAL_SET_QT_STYLE_OVERRIDE", "1");
    qputenv("PIPEASIO_ORIGINAL_QT_STYLE_OVERRIDE", "Windows");
    qputenv("QT_STYLE_OVERRIDE", "Fusion");
    f.expect(runner, prefix, {{"QT_QPA_PLATFORMTHEME", "qt6ct"}, {"QT_STYLE_OVERRIDE", "Windows"},
        {"PIPEASIO_ORIGINAL_QT_QPA_PLATFORMTHEME", QJsonValue::Null},
        {"PIPEASIO_ORIGINAL_SET_QT_QPA_PLATFORMTHEME", QJsonValue::Null},
        {"PIPEASIO_ORIGINAL_QT_STYLE_OVERRIDE", QJsonValue::Null},
        {"PIPEASIO_ORIGINAL_SET_QT_STYLE_OVERRIDE", QJsonValue::Null}});
    CHECK(f.run(target).exitCode == 37);
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (qEnvironmentVariableIsSet("PIPEASIO_FIXTURE_ROOT"))
    {
        try
        {
            return fixture(application.arguments());
        }
        catch (const std::exception &)
        {
            return 97;
        }
    }
    const std::pair<const char *, void (*)()> tests[] = {
        {"Faugus prefix identity and precedence", faugusIdentityAndPrecedence},
        {"Faugus empty runner", faugusEmptyRunner},
        {"Read-only discovery", discoveryDoesNotMutate},
        {"Invalid launcher isolation", invalidLauncherIsolation},
        {"Owned argument whitespace restoration", faugusArgumentRoundtrip},
        {"POSIX quoted arguments", faugusQuotedTokens},
        {"Faugus expansion before tokenization", faugusExpandsBeforeTokenization},
        {"Global override protection", faugusGlobalBlocksMutation},
        {"Faugus child context", faugusChildContext},
        {"Custom bottle root", customBottleRoot},
        {"Bottle placeholder identity", bottlePlaceholder},
        {"Runner change protection", runnerChangeRejected},
        {"No system runner fallback", missingRunnerHasNoFallback},
        {"Bottle environment restoration", bottleEnvironmentRestoration},
        {"YAML anchors and multiline values", yamlAnchorsAndMultiline},
        {"YAML scalar types survive environment edits", yamlScalarTypesSurviveEnvironmentEdits},
        {"Config corruption protection", corruptConfigCannotBeOverwritten},
        {"Concurrent config change protection", concurrentConfigChange},
        {"Symlink config protection", symlinkConfigRefused},
        {"Bottles runtime context", bottleRuntimeContext},
        {"Unsupported Bottles contexts", unsupportedBottleContexts},
        {"Flatpak environment and minimal grants", flatpakContextAndPermissions},
        {"Flatpak custom root visibility", flatpakInaccessibleCustomRoot},
        {"Manual ownership guards", manualOwnershipGuards},
        {"Manual environment and child status", manualEnvironmentAndStatus},
        {"Host loader restoration", hostLoaderRestoration},
        {"Host platform theme restoration", hostPlatformThemeRestoration},
    };
    for (const auto &test : tests)
    {
        try
        {
            test.second();
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", test.first, error.what());
        }
    }
    if (failures)
        std::fprintf(stderr, "%d failed checks out of %d\n", failures, total);
    return failures ? 1 : 0;
}
