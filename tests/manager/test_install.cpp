/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Manager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QNetworkReply>
#include <QTimer>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <functional>
#include <grp.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace PipeASIOManager;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

static QString dllPath(const QString &prefix, const QString &view)
{
    return prefix + "/drive_c/windows/" + (view == "64" ? "system32" : "syswow64") + "/pipeasio" + view + ".dll";
}

static QString windowsFile(QString path, const QString &prefix)
{
    if (!path.startsWith("C:\\") || path.contains(".."))
        throw Error("Unexpected fake-Wine path: " + path);
    path = path.mid(3);
    path.replace('\\', '/');
    return prefix + "/drive_c/" + path;
}

static int fakeWine(const QStringList &arguments)
{
    const QString prefix = qEnvironmentVariable("WINEPREFIX");
    if (arguments == QStringList{"--capture-wrapper"})
    {
        writeJson(prefix + "/wrapper-result.json", QJsonObject{{"prefix", prefix}, {"dllpath", qEnvironmentVariable("WINEDLLPATH")}});
        return 0;
    }
    if (prefix.isEmpty() || arguments.isEmpty() || qEnvironmentVariable("PIPEASIO_CHECK_ISOLATED") != "1" ||
        qEnvironmentVariable("PIPEASIO_CONNECT_TO_HARDWARE") != "off")
        throw Error("Fake Wine received a non-isolated operation");
    const QString config = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (!config.startsWith(prefix + "/drive_c/pipeasio-manager-") || !config.endsWith("/config") ||
        !QDir(config).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
        throw Error("Fake Wine received a nonempty or external config directory");
    const QString mode = QFileInfo::exists(prefix + "/fake-mode") ? QString::fromUtf8(readFile(prefix + "/fake-mode")) : QString{};
    const QString registryFile = prefix + "/fake-registry.json";
    auto registry = readJson(registryFile, QJsonObject{}).toObject();
    const QString executable = arguments[0];
    const QString view = executable.contains("syswow64") || executable.endsWith("pipeasio-check32.exe") ? "32" : "64";
    const QString keyPrefix = view + ':';
    if (executable.endsWith("pipeasio-check.exe") || executable.endsWith("pipeasio-check32.exe"))
    {
        if (!QFileInfo::exists(windowsFile(executable, prefix)))
            throw Error("Probe was not staged inside the prefix");
        if (arguments.size() == 5 && arguments[1] == "--registry-exists" &&
            (arguments[2] == "0" || arguments[2] == "1") && arguments[3] == "--result")
        {
            if (mode == "registry-invalid")
                writeJson(windowsFile(arguments[4], prefix), QJsonObject{{"registry_exists", "false"}});
            else
                writeJson(windowsFile(arguments[4], prefix), QJsonObject{{"registry_exists", registry.contains(keyPrefix + arguments[2])}});
            return 0;
        }
        if (arguments.size() == 3 && arguments[1] == "--result")
        {
            const QString output = windowsFile(arguments[2], prefix);
            if (mode == "malformed-check" || mode == "dead-on-check")
            {
                writeJson(output, QJsonArray{});
                if (mode == "dead-on-check" && !QFile::remove(QCoreApplication::applicationFilePath()))
                    throw Error("Cannot remove fixture runner");
                return 0;
            }
            const bool registered = registry.value(keyPrefix + "0") == "installed-" + view + "-0" &&
                                    registry.value(keyPrefix + "1") == "installed-" + view + "-1";
            const QString library = qEnvironmentVariable("WINEDLLPATH").section(':', 0, 0);
            // The runner is its own process, so a test's architecture pin does
            // not reach it: accept whichever unix directory the payload has.
            bool native = false;
            for (const auto &entry : QDir(library).entryList({ "*-unix" }, QDir::Dirs))
                if (QFileInfo(library + '/' + entry + "/pipeasio" + view + ".so").isFile())
                    native = true;
            writeJson(output, QJsonObject{{"registered", registered}, {"unixlib", native}, {"pipewire", mode != "pipewire-fail"}});
            return 0;
        }
    }
    const QString directory = view == "64" ? "system32" : "syswow64";
    if (arguments == QStringList{"C:\\windows\\" + directory + "\\regsvr32.exe", "/s",
                                 "C:\\windows\\" + directory + "\\pipeasio" + view + ".dll"})
    {
        if (!QFileInfo(dllPath(prefix, view)).isFile()) throw Error("Registration attempted before copying the DLL");
        registry[keyPrefix + "0"] = "installed-" + view + "-0";
        registry[keyPrefix + "1"] = "installed-" + view + "-1";
        writeJson(registryFile, registry);
        return 0;
    }
    if (executable == "C:\\windows\\" + directory + "\\reg.exe")
    {
        const QStringList keys{"HKLM\\Software\\Classes\\CLSID\\{2D3CA9E2-1193-4C5D-B5FD-38798F3DC074}", "HKLM\\Software\\ASIO\\PipeASIO"};
        if (arguments.size() == 5 && arguments[1] == "export" && keys.contains(arguments[2]) && arguments[4] == "/y")
        {
            if (mode == "export-fail") return 1;
            const QString key = keyPrefix + QString::number(keys.indexOf(arguments[2]));
            if (!registry.contains(key)) throw Error("Export attempted for a nonexistent key");
            writeFile(windowsFile(arguments[3], prefix), registry.value(key).toString().toUtf8());
            return 0;
        }
        if (arguments.size() == 4 && arguments[1] == "delete" && keys.contains(arguments[2]) && arguments[3] == "/f")
        {
            const QString key = keyPrefix + QString::number(keys.indexOf(arguments[2]));
            const bool existed = !registry.take(key).isUndefined();
            writeJson(registryFile, registry);
            return existed ? 0 : 1;
        }
        if (arguments.size() == 3 && arguments[1] == "import")
        {
            const QString filename = windowsFile(arguments[2], prefix);
            const QString name = QFileInfo(filename).fileName();
            const QString index = name == "restore-" + view + "-0.reg" ? "0" : name == "restore-" + view + "-1.reg" ? "1" : QString{};
            if (index.isEmpty()) throw Error("Unexpected registry import filename");
            registry[keyPrefix + index] = QString::fromUtf8(readFile(filename));
            writeJson(registryFile, registry);
            return 0;
        }
    }
    throw Error("Unexpected fake-Wine arguments: " + arguments.join(' '));
}

static int fakeFlatpak(const QStringList &arguments)
{
    if (arguments == QStringList{"info", "--show-location", "com.usebottles.bottles"})
    {
        std::printf("%s\n", qPrintable(dataDirectory() + "/flatpak/app/com.usebottles.bottles/current/active"));
        return 0;
    }
    if (arguments.size() == 4 && arguments[0] == "info" && arguments[1] == "--user" &&
        arguments[2].startsWith("--file-access=") && arguments[3] == "com.usebottles.bottles")
    {
        std::printf("read-write\n");
        return 0;
    }
    throw Error("Unexpected fake-Flatpak arguments: " + arguments.join(' '));
}

struct Fixture
{
    QTemporaryDir temporary;
    QString prefix;
    QString runner;
    QJsonObject target;
    QJsonObject payload;
    QNetworkAccessManager network;

    Fixture()
    {
        if (!temporary.isValid()) throw Error("Cannot create test fixture");
        qputenv("HOME", temporary.path().toUtf8());
        qputenv("XDG_DATA_HOME", (temporary.path() + "/data").toUtf8());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        qputenv("XDG_RUNTIME_DIR", (temporary.path() + "/runtime").toUtf8());
        qputenv("XDG_DATA_DIRS", (temporary.path() + "/system-data").toUtf8());
        qputenv("HOST_XDG_DATA_HOME", (temporary.path() + "/data").toUtf8());
        qunsetenv("APPIMAGE");
        qunsetenv("APPDIR");
        qunsetenv("FLATPAK_ID");
        qunsetenv("WINEDLLPATH");
        prefix = temporary.path() + "/prefix with 'quotes";
        QDir().mkpath(prefix + "/drive_c/windows/system32");
        QDir().mkpath(prefix + "/drive_c/windows/syswow64");
        writeFile(prefix + "/system.reg", "WINE REGISTRY Version 2\n");
        runner = temporary.path() + "/bin/wine";
        copyFile(QCoreApplication::applicationFilePath(), runner);
        QFile::setPermissions(runner, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("PATH", (temporary.path() + "/bin:/usr/bin:/bin").toUtf8());
        target = Installer::addPrefix(prefix, runner, "Studio");
        const QString root = managerDirectory() + "/versions/fixture";
        QJsonObject files;
        const QList<QPair<QString, QByteArray>> contents = {
            {"lib/wine/" + architecture() + "-windows/pipeasio64.dll", "fixture DLL64"},
            {"lib/wine/i386-windows/pipeasio32.dll", "fixture DLL32"},
            {"lib/wine/" + architecture() + "-unix/pipeasio64.so", "fixture native64"},
            {"lib/wine/" + architecture() + "-unix/pipeasio32.so", "fixture native32"},
            {"manager/pipeasio-check.exe", "fixture probe64"},
            {"manager/pipeasio-check32.exe", "fixture probe32"}};
        for (const auto &[name, bytes] : contents)
        {
            writeFile(root + '/' + name, bytes);
            files[name] = hashFile(root + '/' + name);
        }
        payload = {{"schema", 1}, {"version", "v1.7.0"}, {"architecture", architecture()}, {"root", root}, {"files", files}};
        writeJson(root + "/.pipeasio-payload.json", payload);
    }
    QString id() const { return target.value("id").toString(); }
    QString statePath() const { return managerDirectory() + "/prefixes/" + prefixKey(target) + "/state.json"; }
    QJsonObject install(bool include32 = false) { return Installer::install(network, id(), "v1.7.0", include32); }
    void mode(const QByteArray &value) { writeFile(prefix + "/fake-mode", value); }
    void original()
    {
        writeFile(dllPath(prefix, "64"), "previous DLL64");
        writeFile(dllPath(prefix, "32"), "previous DLL32");
        writeJson(prefix + "/fake-registry.json", QJsonObject{{"64:0", "original class64"}, {"64:1", "original ASIO64"},
                                                            {"32:0", "original class32"}, {"32:1", "original ASIO32"}});
        Launchers::setEnvironment(target, QJsonObject{{"WINEDLLPATH", "/prior"}, {"KEEP", "untouched"}});
    }
};

static QString failure(const std::function<void()> &operation)
{
    try { operation(); }
    catch (const std::exception &error) { return QString::fromUtf8(error.what()); }
    CHECK(false);
    return {};
}

class MetadataReply : public QNetworkReply
{
  public:
    MetadataReply(const QNetworkRequest &request, QByteArray data, QObject *parent)
        : QNetworkReply(parent), bytes(std::move(data))
    {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] { setFinished(true); emit readyRead(); emit finished(); });
    }
    void abort() override {}
    qint64 bytesAvailable() const override { return bytes.size() - offset + QNetworkReply::bytesAvailable(); }
  protected:
    qint64 readData(char *destination, qint64 maximum) override
    {
        const qint64 count = std::min(maximum, qint64(bytes.size()) - offset);
        if (count <= 0) return -1;
        std::memcpy(destination, bytes.constData() + offset, count);
        offset += count;
        return count;
    }
  private:
    QByteArray bytes;
    qint64 offset = 0;
};

class MetadataNetwork : public QNetworkAccessManager
{
  protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *) override
    {
        if (operation != GetOperation) throw Error("Unexpected metadata request method");
        const QString base = "https://github.com/M0n7y5/pipeasio/releases/download/";
        if (request.url().toString() == "https://api.github.com/repos/M0n7y5/pipeasio/releases?per_page=100&page=1")
        {
            QJsonArray releases;
            for (const QString &version : {QString("v1.9.0-rc1"), QString("v1.8.0")})
                releases.append(QJsonObject{{"tag_name", version}, {"name", version}, {"draft", false}, {"prerelease", version.contains("-rc")},
                    {"assets", QJsonArray{QJsonObject{{"name", "driver.tar.gz"}, {"browser_download_url", base + version + "/driver.tar.gz"}, {"size", 1}},
                                         QJsonObject{{"name", "pipeasio-release.json"}, {"browser_download_url", base + version + "/pipeasio-release.json"}, {"size", 1000}}}}});
            return new MetadataReply(request, QJsonDocument(releases).toJson(), this);
        }
        for (const QString &version : {QString("v1.9.0-rc1"), QString("v1.8.0")})
            if (request.url().toString() == base + version + "/pipeasio-release.json")
            {
                const QJsonObject artifact{{"architecture", architecture()}, {"asset", "driver.tar.gz"}, {"sha256", QString(64, 'a')},
                                           {"platform", "archlinux"}, {"minimum_glibc", "2.34"}, {"minimum_pipewire", "1.4.2"},
                                           {"wine_sdk", "wine-10.15"}, {"tested_runtimes", QJsonArray{}}};
                return new MetadataReply(request, QJsonDocument(QJsonObject{{"schema", 1}, {"version", version}, {"artifacts", QJsonArray{artifact}}}).toJson(), this);
            }
        throw Error("Unexpected metadata URL: " + request.url().toString());
    }
};

static void test_default_release_excludes_prereleases()
{
    Fixture f;
    MetadataNetwork network;
    CHECK(Installer::preview(network, f.id()).value("version") == "v1.8.0");
}

static void test_explicit_prerelease_is_selectable()
{
    Fixture f;
    MetadataNetwork network;
    CHECK(Installer::preview(network, f.id(), "v1.9.0-rc1").value("version") == "v1.9.0-rc1");
}

static void test_roundtrip_restores_preexisting_installation()
{
    Fixture f;
    f.original();
    const auto registry = readFile(f.prefix + "/fake-registry.json");
    const auto environment = Launchers::environment(f.target);
    const auto installed = f.install(true);
    CHECK(installed.value("status") == "ready");
    CHECK(installed.value("checks").toObject().contains("32"));
    CHECK(readFile(dllPath(f.prefix, "64")) == "fixture DLL64");
    const QString wrapper = installed.value("launcher").toString();
    const auto result = execute(wrapper, {"--capture-wrapper"}, cleanEnvironment());
    CHECK(result.exitCode == 0);
    const auto captured = readJson(f.prefix + "/wrapper-result.json").toObject();
    CHECK(captured.value("prefix") == f.prefix);
    CHECK(captured.value("dllpath") == f.payload.value("root").toString() + "/lib/wine:/prior");
    CHECK(Installer::check(f.id()).value("status") == "ready");
    CHECK(Installer::remove(f.id()).value("status") == "removed");
    CHECK(readFile(dllPath(f.prefix, "64")) == "previous DLL64");
    CHECK(readFile(dllPath(f.prefix, "32")) == "previous DLL32");
    CHECK(readFile(f.prefix + "/fake-registry.json") == registry);
    CHECK(Launchers::environment(f.target) == environment);
    CHECK(!QFileInfo::exists(wrapper));
    CHECK(!QFileInfo::exists(f.statePath()));
}

static void test_disabling_32_restores_its_originals()
{
    Fixture f;
    f.original();
    f.install();
    f.install(true);
    f.install(false);
    CHECK(readFile(dllPath(f.prefix, "32")) == "previous DLL32");
    const auto registry = readJson(f.prefix + "/fake-registry.json").toObject();
    CHECK(registry.value("32:0") == "original class32");
    CHECK(registry.value("32:1") == "original ASIO32");
    CHECK(!Installer::check(f.id()).value("checks").toObject().contains("32"));
    Installer::remove(f.id());
    CHECK(readFile(dllPath(f.prefix, "64")) == "previous DLL64");
}

static void test_symlink_original_does_not_modify_referent()
{
    Fixture f;
    writeFile(f.prefix + "/drive_c/windows/original.dll", "original symlink target");
    CHECK(::symlink("../original.dll", QFile::encodeName(dllPath(f.prefix, "64")).constData()) == 0);
    f.install();
    CHECK(readFile(f.prefix + "/drive_c/windows/original.dll") == "original symlink target");
    CHECK(!QFileInfo(dllPath(f.prefix, "64")).isSymLink());
    Installer::remove(f.id());
    CHECK(QFileInfo(dllPath(f.prefix, "64")).isSymLink());
    char link[64]{};
    const auto length = ::readlink(QFile::encodeName(dllPath(f.prefix, "64")).constData(), link, sizeof(link));
    CHECK(QByteArray(link, length) == "../original.dll");
}

static void test_export_failure_never_assumes_missing_registration()
{
    Fixture f;
    f.original();
    f.mode("export-fail");
    CHECK(failure([&] { f.install(); }).contains("Runner command failed"));
    CHECK(readFile(dllPath(f.prefix, "64")) == "previous DLL64");
    CHECK(readJson(f.prefix + "/fake-registry.json").toObject().value("64:0") == "original class64");
    CHECK(Launchers::environment(f.target).value("WINEDLLPATH") == "/prior");
    CHECK(!QFileInfo::exists(f.statePath()));
}

static void test_registry_probe_requires_boolean_existence()
{
    Fixture f;
    f.original();
    f.mode("registry-invalid");
    CHECK(failure([&] { f.install(); }).contains("No backup was assumed empty"));
    CHECK(readFile(dllPath(f.prefix, "64")) == "previous DLL64");
    CHECK(!QFileInfo::exists(f.statePath()));
}

static void test_malformed_check_rolls_back_registration()
{
    Fixture f;
    f.original();
    f.mode("malformed-check");
    CHECK(failure([&] { f.install(); }).contains("invalid result object"));
    CHECK(readFile(dllPath(f.prefix, "64")) == "previous DLL64");
    CHECK(readJson(f.prefix + "/fake-registry.json").toObject().value("64:1") == "original ASIO64");
    CHECK(Launchers::environment(f.target).value("WINEDLLPATH") == "/prior");
    CHECK(!QFileInfo::exists(f.statePath()));
}

static void test_dead_runner_does_not_prevent_dll_recovery()
{
    Fixture f;
    f.original();
    f.mode("dead-on-check");
    CHECK(failure([&] { f.install(); }).contains("Recovery needs attention"));
    CHECK(readFile(dllPath(f.prefix, "64")) == "previous DLL64");
    const QString recovery = QFileInfo(f.statePath()).absolutePath() + "/recovery/snapshot.json";
    CHECK(QFileInfo::exists(recovery));
    const auto backup = readJson(recovery).toObject().value("files").toObject().value("drive_c/windows/system32/pipeasio64.dll").toObject();
    CHECK(readFile(backup.value("backup").toString()) == "previous DLL64");
    CHECK(!QFileInfo::exists(f.statePath()));
}

static void test_failed_repair_retains_previous_state()
{
    Fixture f;
    f.original();
    f.install();
    const auto state = readFile(f.statePath());
    const auto environment = Launchers::environment(f.target);
    f.mode("pipewire-fail");
    CHECK(failure([&] { f.install(true); }).contains("not usable"));
    CHECK(readFile(f.statePath()) == state);
    CHECK(readFile(dllPath(f.prefix, "64")) == "fixture DLL64");
    CHECK(readFile(dllPath(f.prefix, "32")) == "previous DLL32");
    CHECK(Launchers::environment(f.target) == environment);
}

static void test_external_file_edit_blocks_removal()
{
    Fixture f;
    f.install();
    writeFile(dllPath(f.prefix, "64"), "external replacement");
    CHECK(failure([&] { Installer::remove(f.id()); }).contains("changed outside the manager"));
    CHECK(readFile(dllPath(f.prefix, "64")) == "external replacement");
    CHECK(QFileInfo::exists(f.statePath()));
    CHECK(failure([&] { Installer::check(f.id()); }).contains("Repair"));
}

static void test_external_registry_edit_blocks_removal()
{
    Fixture f;
    f.install();
    auto registry = readJson(f.prefix + "/fake-registry.json").toObject();
    registry["64:1"] = "external ASIO registration";
    writeJson(f.prefix + "/fake-registry.json", registry);
    CHECK(failure([&] { Installer::remove(f.id()); }).contains("registration changed outside"));
    CHECK(readJson(f.prefix + "/fake-registry.json").toObject().value("64:1") == "external ASIO registration");
    CHECK(readFile(dllPath(f.prefix, "64")) == "fixture DLL64");
}

static void test_external_environment_edit_blocks_removal()
{
    Fixture f;
    f.install();
    Launchers::setEnvironment(f.target, QJsonObject{{"WINEDLLPATH", "/external"}});
    CHECK(failure([&] { Installer::remove(f.id()); }).contains("changed outside the manager"));
    CHECK(Launchers::environment(f.target).value("WINEDLLPATH") == "/external");
    CHECK(readFile(dllPath(f.prefix, "64")) == "fixture DLL64");
}

static void test_cache_tampering_blocks_readiness()
{
    Fixture f;
    f.install();
    writeFile(f.payload.value("root").toString() + "/lib/wine/" + architecture() + "-unix/pipeasio64.so", "tampered native library");
    CHECK(failure([&] { Installer::check(f.id()); }).contains("cached driver files changed"));
}

static void test_faugus_readoption_does_not_steal_live_owner()
{
    Fixture f;
    writeJson(managerDirectory() + "/manual.json", QJsonArray{});
    const QString base = dataDirectory() + "/faugus-launcher";
    copyFile(f.runner, base + "/umu-run");
    QFile::setPermissions(base + "/umu-run", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    writeFile(dataDirectory() + "/Steam/compatibilitytools.d/fixture/proton", "fixture proton");
    QJsonObject game{{"gameid", "old"}, {"title", "Studio"}, {"prefix", f.prefix}, {"runner", "fixture"},
                     {"path", f.prefix + "/drive_c/studio.exe"}, {"launch_arguments", ""}, {"game_arguments", ""}};
    writeJson(base + "/games.json", QJsonArray{game});
    f.target = Installer::listTargets().at(0).toObject();
    CHECK(f.target.value("error").toString().isEmpty());
    f.install(true);
    CHECK(Launchers::environment(f.target).value("PROTON_USE_WOW64") == "1");
    game["gameid"] = "new";
    writeJson(base + "/games.json", QJsonArray{game});
    f.target = Installer::listTargets().at(0).toObject();
    CHECK(f.target.value("metadata").toObject().value("adoptable").toBool());
    f.install(false);
    CHECK(!Launchers::environment(f.target).contains("PROTON_USE_WOW64"));
    CHECK(readJson(f.statePath()).toObject().value("target").toObject().value("id") == f.target.value("id"));
    auto other = game;
    other["gameid"] = "other";
    writeJson(base + "/games.json", QJsonArray{other, game});
    const auto targets = Installer::listTargets();
    CHECK(targets[0].toObject().value("status") == "unsupported");
    CHECK(targets[1].toObject().value("status") == "installed");
    CHECK(failure([&] { Installer::install(f.network, targets[0].toObject().value("id").toString(), "v1.7.0"); }).contains("managed through"));
}

static QJsonObject pendingFlatpak(Fixture &f, const QString &name)
{
    copyFile(f.runner, f.temporary.path() + "/bin/flatpak");
    QFile::setPermissions(f.temporary.path() + "/bin/flatpak", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString base = homeDirectory() + "/.var/app/com.usebottles.bottles/data/bottles";
    const QString prefix = base + "/bottles/" + name;
    QDir().mkpath(prefix + "/drive_c/windows/system32");
    writeFile(prefix + "/system.reg", "WINE REGISTRY Version 2\n");
    copyFile(f.runner, base + "/runners/fixture/bin/wine");
    QFile::setPermissions(base + "/runners/fixture/bin/wine", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    writeFile(prefix + "/bottle.yml", ("Name: " + name + "\nPath: " + name + "\nRunner: fixture\nArch: win64\nParameters: {}\n").toUtf8());
    QJsonObject target;
    for (const auto &value : Installer::listTargets())
        if (value.toObject().value("kind") == "bottles-flatpak" && value.toObject().value("prefix") == prefix) target = value.toObject();
    CHECK(!target.isEmpty());
    CHECK(target.value("error").toString().isEmpty());
    const QString statePath = managerDirectory() + "/prefixes/" + prefixKey(target) + "/state.json";
    writeJson(statePath, QJsonObject{{"schema", 1}, {"target", target}, {"version", "v1.7.0"}, {"phase", "permissions-pending"}, {"files", QJsonObject{}}});
    return target;
}

static void test_pending_permissions_cleanup_is_resumable()
{
    Fixture f;
    const auto target = pendingFlatpak(f, "Studio");
    const QString statePath = managerDirectory() + "/prefixes/" + prefixKey(target) + "/state.json";
    const QString journalPath = managerDirectory() + "/flatpak-permissions.json";
    writeJson(journalPath, QJsonObject{{"before", QJsonArray{"/keep:ro"}}, {"after", QJsonArray{"/keep:ro", "xdg-run/pipewire-0"}},
                                    {"users", QJsonArray{prefixKey(target)}}});
    const QString override = dataDirectory() + "/flatpak/overrides/com.usebottles.bottles";
    writeFile(override, "not a keyfile\n");
    CHECK(failure([&] { Installer::remove(target.value("id").toString()); }).contains("permissions configuration"));
    CHECK(QFileInfo::exists(statePath));
    CHECK(QFileInfo::exists(journalPath));
    writeFile(override, "[Context]\nfilesystems=/keep:ro;xdg-run/pipewire-0;/external:ro;\n[Environment]\nKEEP=yes\n");
    CHECK(Installer::remove(target.value("id").toString()).value("status") == "removed");
    const auto permissions = readFile(override);
    CHECK(permissions.contains("filesystems=/keep:ro;/external:ro;"));
    CHECK(permissions.contains("KEEP=yes"));
    CHECK(!permissions.contains("pipewire-0"));
    CHECK(!QFileInfo::exists(statePath));
    CHECK(!QFileInfo::exists(journalPath));
}

static void test_update_keeps_the_pre_manager_originals()
{
    Fixture f;
    f.original();
    f.install();
    auto payload = f.payload;
    const QString root = managerDirectory() + "/versions/updated";
    auto files = payload.value("files").toObject();
    for (auto it = files.begin(); it != files.end(); ++it)
        copyFile(f.payload.value("root").toString() + '/' + it.key(), root + '/' + it.key());
    const QString driver = "lib/wine/" + architecture() + "-windows/pipeasio64.dll";
    writeFile(root + '/' + driver, "updated DLL64");
    files[driver] = hashFile(root + '/' + driver);
    payload["version"] = "v1.8.0";
    payload["root"] = root;
    payload["files"] = files;
    writeJson(root + "/.pipeasio-payload.json", payload);
    const auto result = Installer::install(f.network, f.id(), "v1.8.0");
    CHECK(result.value("version") == "v1.8.0");
    CHECK(readFile(dllPath(f.prefix, "64")) == "updated DLL64");
    CHECK(!Launchers::environment(f.target).value("WINEDLLPATH").toString().contains("/versions/fixture/"));
    Installer::remove(f.id());
    CHECK(readFile(dllPath(f.prefix, "64")) == "previous DLL64");
    CHECK(readJson(f.prefix + "/fake-registry.json").toObject().value("64:0") == "original class64");
}

static void test_shared_permissions_survive_until_last_removal()
{
    Fixture f;
    const auto first = pendingFlatpak(f, "First");
    const auto second = pendingFlatpak(f, "Second");
    const QString journalPath = managerDirectory() + "/flatpak-permissions.json";
    writeJson(journalPath, QJsonObject{{"before", QJsonArray{"/keep:ro"}}, {"after", QJsonArray{"/keep:ro", "xdg-run/pipewire-0"}},
                                    {"users", QJsonArray{prefixKey(first), prefixKey(second)}}});
    const QString override = dataDirectory() + "/flatpak/overrides/com.usebottles.bottles";
    const QByteArray original = "[Context]\nfilesystems=/keep:ro;xdg-run/pipewire-0;\n";
    writeFile(override, original);
    Installer::remove(first.value("id").toString());
    CHECK(readFile(override) == original);
    CHECK(readJson(journalPath).toObject().value("users").toArray() == QJsonArray{prefixKey(second)});
    Installer::remove(second.value("id").toString());
    CHECK(!readFile(override).contains("pipewire-0"));
    CHECK(readFile(override).contains("/keep:ro"));
    CHECK(!QFileInfo::exists(journalPath));
}

static void test_aarch64_payload_installs_the_aarch64_front_end()
{
    Testing::setArchitecture("aarch64");
    struct Restore
    {
        ~Restore() { Testing::setArchitecture(QString()); }
    } restore;
    Fixture f;
    // An ARM64 release also carries the arm64ec front end. Only one file can
    // live in system32 and the manager installs the host's own aarch64 PE.
    writeFile(f.payload.value("root").toString() + "/lib/wine/arm64ec-windows/pipeasio64.dll",
              "fixture EC DLL64");
    CHECK(f.install().value("status") == "ready");
    CHECK(readFile(dllPath(f.prefix, "64")) == "fixture DLL64");
    CHECK(Installer::check(f.id()).value("status") == "ready");
    CHECK(Installer::remove(f.id()).value("status") == "removed");
}

static int tests()
{
    const std::pair<const char *, void (*)()> cases[] = {
        {"latest stable", test_default_release_excludes_prereleases},
        {"explicit prerelease", test_explicit_prerelease_is_selectable},
        {"roundtrip originals", test_roundtrip_restores_preexisting_installation},
        {"disable 32", test_disabling_32_restores_its_originals},
        {"symlink originals", test_symlink_original_does_not_modify_referent},
        {"export failure", test_export_failure_never_assumes_missing_registration},
        {"registry boolean", test_registry_probe_requires_boolean_existence},
        {"malformed check", test_malformed_check_rolls_back_registration},
        {"dead runner", test_dead_runner_does_not_prevent_dll_recovery},
        {"failed repair", test_failed_repair_retains_previous_state},
        {"external DLL edit", test_external_file_edit_blocks_removal},
        {"external registry edit", test_external_registry_edit_blocks_removal},
        {"external environment edit", test_external_environment_edit_blocks_removal},
        {"cache tampering", test_cache_tampering_blocks_readiness},
        {"version update", test_update_keeps_the_pre_manager_originals},
        {"Faugus readoption", test_faugus_readoption_does_not_steal_live_owner},
        {"shared permissions", test_shared_permissions_survive_until_last_removal},
        {"pending permissions", test_pending_permissions_cleanup_is_resumable},
        {"aarch64 payload", test_aarch64_payload_installs_the_aarch64_front_end}};
    for (const auto &[name, test] : cases)
        try { test(); }
        catch (const std::exception &error)
        {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
        }
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments().mid(1);
    try
    {
        if (QFileInfo(application.applicationFilePath()).fileName() == "flatpak")
            return fakeFlatpak(arguments);
        if (!arguments.isEmpty()) return fakeWine(arguments);
        if (::geteuid() == 0)
        {
            const auto child = ::fork();
            if (child < 0) throw Error("Cannot fork unprivileged installer tests");
            if (child == 0)
            {
                if (::setgroups(0, nullptr) != 0 || ::setgid(65534) != 0 || ::setuid(65534) != 0)
                    ::_exit(2);
                ::_exit(tests());
            }
            int status = 0;
            if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) return 2;
            return WEXITSTATUS(status);
        }
        return tests();
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
