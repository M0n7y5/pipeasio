/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Manager.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace PipeASIOManager::Installer
{
namespace
{
const QStringList registryKeys
        = { "HKLM\\Software\\Classes\\CLSID\\{2D3CA9E2-1193-4C5D-B5FD-38798F3DC074}",
            "HKLM\\Software\\ASIO\\PipeASIO" };

QString
dll(const QString &view)
{
    return "drive_c/windows/" + QString(view == "64" ? "system32" : "syswow64") + "/pipeasio" + view
           + ".dll";
}

QString
stateDirectory(const QJsonObject &target)
{
    return managerDirectory() + "/prefixes/" + prefixKey(target);
}

QJsonObject
object(const QJsonValue &value, const QString &description)
{
    if (!value.isObject())
        throw Error("Invalid object in " + description);
    return value.toObject();
}

QJsonObject
stateFor(const QJsonObject &target)
{
    const QString path  = stateDirectory(target) + "/state.json";
    const auto    value = readJson(path);
    return value.isNull() || value.isUndefined() ? QJsonObject{} : object(value, path);
}

QJsonValue
valueOrNull(const QJsonObject &values, const QString &key)
{
    return values.contains(key) ? values.value(key) : QJsonValue(QJsonValue::Null);
}

void
notify(const Progress &progress, const QString &message)
{
    if (progress)
        progress(message);
}

void
makeDirectory(const QString &path)
{
    if (!QDir().mkpath(path))
        throw Error("Cannot create directory: " + path);
}

void
unlinkFile(const QString &path)
{
    const QFileInfo info(path);
    if ((info.exists() || info.isSymLink()) && !QFile::remove(path))
        throw Error("Cannot remove file: " + path);
}

class Lock
{
  public:
    Lock()
    {
        makeDirectory(managerDirectory());
        descriptor = ::open(QFile::encodeName(managerDirectory() + "/manager.lock").constData(),
                            O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0)
            throw Error("Cannot open manager lock: "
                        + QString::fromLocal8Bit(std::strerror(errno)));
        if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0)
        {
            ::close(descriptor);
            throw Error("Another PipeASIO manager operation is running.");
        }
    }
    ~Lock()
    {
        ::close(descriptor);
    }
    Lock(const Lock &)            = delete;
    Lock &operator=(const Lock &) = delete;

  private:
    int descriptor;
};

QJsonObject
targetFor(const QString &id)
{
    for (const auto &value : listTargets())
        if (value.toObject().value("id").toString() == id)
            return value.toObject();
    throw Error("The selected prefix is no longer available. Refresh the list.");
}

void
validate(const QJsonObject &target)
{
    if (::geteuid() == 0)
        throw Error("Run the manager as your normal user, not with sudo.");
    if (!target.value("error").toString().isEmpty())
        throw Error(target.value("error").toString());
    const QString prefix = target.value("prefix").toString();
    const QString error  = prefixError(prefix);
    if (!error.isEmpty())
        throw Error(error);
    struct stat info{};
    if (::stat(QFile::encodeName(prefix).constData(), &info) != 0 || info.st_uid != ::geteuid())
        throw Error("The selected prefix must belong to your user.");
    if (::access(QFile::encodeName(prefix + "/drive_c/windows/system32").constData(), W_OK) != 0)
        throw Error("The selected prefix is not writable by your user.");
    const auto state = stateFor(target);
    if (!state.isEmpty() && state.value("target").toObject().value("id") != target.value("id")
        && !target.value("metadata").toObject().value("adoptable").toBool())
        throw Error("This prefix is already managed through "
                    + state.value("target").toObject().value("name").toString() + ".");
}

void
assertIdle(const QJsonObject &target)
{
    const QString       prefix = absolutePath(target.value("prefix").toString());
    const QSet<QString> services
            = { "wineserver",   "wineserver64", "services.exe", "winedevice.exe",
                "plugplay.exe", "rpcss.exe",    "svchost.exe",  "explorer.exe" };
    const QSet<QString> loaders = { "wine", "wine64", "wine-preloader", "wine64-preloader" };
    for (const auto &process : QDir("/proc").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        bool       numeric = false;
        const auto pid     = process.fileName().toLongLong(&numeric);
        if (!numeric || pid == ::getpid() || process.ownerId() != ::getuid())
            continue;
        QFile cmdline(process.filePath() + "/cmdline");
        if (!cmdline.open(QIODevice::ReadOnly))
            continue;
        QString executable = QString::fromLocal8Bit(cmdline.readAll().split('\0').value(0));
        executable.replace('\\', '/');
        executable = executable.section('/', -1).toLower();
        if (services.contains(executable)
            || (!executable.endsWith(".exe") && !loaders.contains(executable)))
            continue;
        QFile environment(process.filePath() + "/environ");
        if (!environment.open(QIODevice::ReadOnly))
            continue;
        for (const auto &entry : environment.readAll().split('\0'))
            if (entry.startsWith("WINEPREFIX=")
                && absolutePath(QString::fromLocal8Bit(entry.mid(11))) == prefix)
                throw Error("Close applications using this prefix before changing PipeASIO.");
    }
}

QString
versionsDirectory(const QJsonObject &target)
{
    return target.value("kind") == "bottles-flatpak"
                   ? homeDirectory() + "/.var/app/com.usebottles.bottles/data/pipeasio/versions"
                   : managerDirectory() + "/versions";
}

const Layout &
payloadLayout(const QJsonObject &payload)
{
    const Layout *host = layoutFor(payload.value("architecture").toString());
    if (!host)
        throw Error("This release was built for an architecture the manager cannot install.");
    return *host;
}

// The payload-relative PE that belongs in the prefix's system32 for a view.
//
// On ARM64 a release carries two 64-bit front ends and only one file can be
// installed, so this picks the host's own: aarch64-windows. Wine resolves a
// builtin through WINEDLLPATH by appending the directory its loader derives
// from the requesting machine, and get_pe_dir() in dlls/ntdll/unix/loader.c
// maps ARM64 to aarch64-windows and AMD64 to x86_64-windows. It knows no
// arm64ec-windows at all, so the arm64ec front end is never reachable that
// way, and the file the loader maps out of system32 has to report a machine
// the host process can run. Whether an x86_64 host under FEX can be served
// from the same prefix stays open on #23: Wine reaches for the x64 side of an
// ARM64X hybrid image in aarch64-windows, which a separate arm64ec PE is not,
// and nobody has run this on ARM64 hardware yet.
QString
peRelative(const QJsonObject &payload, const QString &view)
{
    const QString directory
            = view == "64" ? payloadLayout(payload).peDirectory : QStringLiteral("i386-windows");
    return directory + "/pipeasio" + view + ".dll";
}

QString
unixlibRelative(const QJsonObject &payload, const QString &view)
{
    return payloadLayout(payload).unixDirectory + "/pipeasio" + view + ".so";
}

QString
payloadFile(const QJsonObject &payload, const QString &relative)
{
    return payload.value("root").toString() + "/lib/wine/" + relative;
}

bool
verifiedPayload(const QJsonObject &payload, const QString &root)
{
    const auto files = payload.value("files").toObject();
    if (files.isEmpty() || payload.value("architecture") != architecture())
        return false;
    for (auto it = files.begin(); it != files.end(); ++it)
    {
        const QString relative = it.key();
        if (relative.isEmpty() || QDir::isAbsolutePath(relative)
            || relative.split('/').contains("..") || relative.contains('\\')
            || !it.value().isString())
            return false;
        const QFileInfo info(root + '/' + relative);
        if (!info.isFile()
            || !info.canonicalFilePath().startsWith(QFileInfo(root).canonicalFilePath() + '/')
            || hashFile(info.filePath()) != it.value().toString())
            return false;
    }
    return true;
}

QJsonObject
cachedPayload(const QJsonObject &target, const QString &version)
{
    for (const auto &entry : QDir(versionsDirectory(target))
                                     .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
    {
        const auto value = readJson(entry.filePath() + "/.pipeasio-payload.json");
        if (value.isNull() || value.isUndefined())
            continue;
        auto metadata = object(value, entry.filePath());
        if (metadata.value("version") == version && verifiedPayload(metadata, entry.filePath()))
        {
            metadata["root"] = entry.filePath();
            return metadata;
        }
    }
    return {};
}

QJsonObject
selectRelease(QNetworkAccessManager &network, const QString &version, const Progress &progress)
{
    for (const auto &value : Releases::list(network, progress))
    {
        const auto release = value.toObject();
        if (release.value("architecture") == architecture()
            && (version.isEmpty() ? !release.value("prerelease").toBool()
                                  : release.value("version") == version))
            return release;
    }
    throw Error("No compatible prebuilt is available for this architecture/release.");
}

QJsonObject
payloadFor(QNetworkAccessManager &network, const QJsonObject &target, const QString &version,
           const Progress &progress)
{
    if (!version.isEmpty())
    {
        auto cached = cachedPayload(target, version);
        if (!cached.isEmpty())
        {
            notify(progress, "Using verified cached PipeASIO " + version);
            return cached;
        }
    }
    const auto release = selectRelease(network, version, progress);
    auto       cached  = cachedPayload(target, release.value("version").toString());
    if (!cached.isEmpty())
        return cached;
    const QString root = versionsDirectory(target);
    makeDirectory(root);
    QTemporaryDir temporary(root + "/.download-XXXXXX");
    if (!temporary.isValid())
        throw Error("Cannot create release staging directory.");
    auto payload = Releases::fetch(network, release, temporary.path() + "/payload", progress);
    const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(
                    QJsonDocument(payload.value("files").toObject()).toJson(QJsonDocument::Compact),
                    QCryptographicHash::Sha256)
                    .toHex()
                    .left(16));
    QString tag = payload.value("version").toString();
    tag.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_");
    const QString destination
            = root + '/' + tag + '-' + payload.value("architecture").toString() + '-' + digest;
    if (QFileInfo::exists(destination))
        throw Error(
                "The cached release directory was modified. Remove that cache entry and retry.");
    if (!QDir().rename(payload.value("root").toString(), destination))
        throw Error("Cannot publish verified release cache: " + destination);
    payload["root"] = destination;
    writeJson(destination + "/.pipeasio-payload.json", payload);
    return payload;
}

QString
workspaceTemplate(const QJsonObject &target)
{
    return target.value("prefix").toString() + "/drive_c/pipeasio-manager-XXXXXX";
}

void
prepareWorkspace(const QTemporaryDir &workspace)
{
    if (!workspace.isValid())
        throw Error("Cannot create an isolated workspace in the selected prefix.");
    makeDirectory(workspace.path() + "/config");
}

QString
windowsPath(const QString &path, const QString &workspace)
{
    QString relative = QDir(workspace).relativeFilePath(path);
    relative.replace('/', '\\');
    return "C:\\" + QFileInfo(workspace).fileName() + '\\' + relative;
}

QString
registryProgram(const QString &view)
{
    return "C:\\windows\\" + QString(view == "64" ? "system32" : "syswow64") + "\\reg.exe";
}

CommandResult
run(const QJsonObject &target, const QStringList &arguments, const QJsonObject &environment,
    bool allowMissing = false)
{
    const auto result = Launchers::run(target, arguments, environment);
    if (result.exitCode != 0 && !(allowMissing && result.exitCode == 1))
    {
        QString detail = result.error.isEmpty() ? result.output : result.error;
        if (detail.isEmpty())
            detail = "No runner output";
        throw Error("Runner command failed (" + QString::number(result.exitCode)
                    + "): " + detail.trimmed().right(3000));
    }
    return result;
}

QString
probeSource(const QJsonObject &payload, const QString &view)
{
    const QString name = "pipeasio-check" + QString(view == "32" ? "32" : "") + ".exe";
    QStringList   paths;
    if (!payload.value("root").toString().isEmpty())
        paths << payload.value("root").toString() + "/manager/" + name;
    paths << QCoreApplication::applicationDirPath() + "/../share/pipeasio/manager/" + name;
#ifdef PIPEASIO_MANAGER_BUILD_PROBE_DIR
    paths << QString::fromUtf8(PIPEASIO_MANAGER_BUILD_PROBE_DIR) + '/' + name;
#endif
    for (const auto &path : paths)
        if (QFileInfo(path).isFile())
            return path;
    throw Error("The manager installation is missing " + name
                + ". Install the complete manager package before changing a prefix.");
}

QJsonObject
probeResult(const QString &path)
{
    QJsonParseError error;
    const auto      result = QJsonDocument::fromJson(readFile(path, 65536), &error);
    if (error.error != QJsonParseError::NoError || !result.isObject())
        throw Error("The installation check returned an invalid result object.");
    return result.object();
}

bool
registryExists(const QJsonObject &target, const QString &view, int index, const QString &workspace,
               const QJsonObject &environment, const QJsonObject &payload)
{
    const QString source = probeSource(payload, view);
    const QString probe  = workspace + '/' + QFileInfo(source).fileName();
    if (!QFileInfo::exists(probe))
        copyFile(source, probe);
    const QString resultFile
            = workspace + "/registry-" + view + '-' + QString::number(index) + ".json";
    unlinkFile(resultFile);
    const auto result = Launchers::run(target,
                                       { windowsPath(probe, workspace), "--registry-exists",
                                         QString::number(index), "--result",
                                         windowsPath(resultFile, workspace) },
                                       environment);
    const auto status = QFileInfo::exists(resultFile) ? probeResult(resultFile) : QJsonObject{};
    if (result.exitCode || !status.value("registry_exists").isBool())
        throw Error(status.value("error").toString("The runner could not inspect existing PipeASIO "
                                                   "registration. No backup was assumed empty."));
    return status.value("registry_exists").toBool();
}

QJsonObject
snapshot(const QJsonObject &target, const QStringList &views, const QString &destination,
         const QString &workspace, const QJsonObject &environment, const QJsonObject &payload)
{
    makeDirectory(destination);
    QJsonObject files, registry;
    for (const auto &view : views)
    {
        const QString   relative = dll(view);
        const QString   source   = target.value("prefix").toString() + '/' + relative;
        const QFileInfo info(source);
        QJsonObject     record{ { "exists", info.exists() || info.isSymLink() } };
        if (info.isSymLink())
        {
            QByteArray link(65536, '\0');
            const auto length
                    = ::readlink(QFile::encodeName(source).constData(), link.data(), link.size());
            if (length < 0 || length == link.size())
                throw Error("Cannot back up symbolic link: " + source);
            link.resize(length);
            record["link"] = QFile::decodeName(link);
        }
        else if (info.exists())
        {
            const QString backup = destination + "/pipeasio" + view + ".dll";
            copyFile(source, backup);
            record["backup"] = backup;
        }
        files[relative] = record;
        QJsonArray exports;
        for (int index = 0; index < registryKeys.size(); ++index)
        {
            const QString exported
                    = workspace + "/registry-" + view + '-' + QString::number(index) + ".reg";
            unlinkFile(exported);
            if (registryExists(target, view, index, workspace, environment, payload))
            {
                run(target,
                    { registryProgram(view), "export", registryKeys[index],
                      windowsPath(exported, workspace), "/y" },
                    environment);
                if (!QFileInfo(exported).isFile())
                    throw Error("The runner did not create the registry backup. No installation "
                                "was attempted.");
                const QString backup = destination + '/' + QFileInfo(exported).fileName();
                copyFile(exported, backup);
                exports.append(backup);
            }
            else
                exports.append(QJsonValue(QJsonValue::Null));
        }
        registry[view] = exports;
    }
    return { { "files", files }, { "registry", registry } };
}

void
restore(const QJsonObject &target, const QJsonObject &saved, const QString &workspace,
        const QJsonObject &environment, const QJsonObject &payload)
{
    QStringList errors;
    const auto  files = saved.value("files").toObject();
    for (auto it = files.begin(); it != files.end(); ++it)
    {
        try
        {
            const auto    record = it.value().toObject();
            const QString path   = target.value("prefix").toString() + '/' + it.key();
            unlinkFile(path);
            if (record.value("exists").toBool())
            {
                if (record.contains("link"))
                {
                    if (::symlink(QFile::encodeName(record.value("link").toString()).constData(),
                                  QFile::encodeName(path).constData())
                        != 0)
                        throw Error("Cannot restore symbolic link: " + path);
                }
                else
                    copyFile(record.value("backup").toString(), path);
            }
        }
        catch (const std::exception &error)
        {
            errors << QString::fromUtf8(error.what());
        }
    }
    const auto registry = saved.value("registry").toObject();
    for (auto it = registry.begin(); it != registry.end(); ++it)
    {
        const auto exports = it.value().toArray();
        for (int index = 0; index < registryKeys.size(); ++index)
        {
            try
            {
                const auto deleted = run(
                        target, { registryProgram(it.key()), "delete", registryKeys[index], "/f" },
                        environment, true);
                if (deleted.exitCode
                    && registryExists(target, it.key(), index, workspace, environment, payload))
                    throw Error("Cannot remove " + it.key() + "-bit registry key "
                                + registryKeys[index]);
                if (exports.at(index).isString())
                {
                    const QString copy = workspace + "/restore-" + it.key() + '-'
                                         + QString::number(index) + ".reg";
                    copyFile(exports.at(index).toString(), copy);
                    run(target,
                        { registryProgram(it.key()), "import", windowsPath(copy, workspace) },
                        environment);
                }
            }
            catch (const std::exception &error)
            {
                errors << QString::fromUtf8(error.what());
            }
        }
    }
    if (!errors.isEmpty())
        throw Error("Recovery could not restore every registry key/file: " + errors.join("; "));
}

QJsonObject
subset(const QJsonObject &saved, const QStringList &views)
{
    QJsonObject files, registry;
    for (const auto &view : views)
    {
        files[dll(view)] = saved.value("files").toObject().value(dll(view));
        registry[view]   = saved.value("registry").toObject().value(view);
    }
    return { { "files", files }, { "registry", registry } };
}

QJsonObject
persistOriginals(const QJsonObject &saved, const QString &destination)
{
    makeDirectory(destination);
    auto files = saved.value("files").toObject();
    for (auto it = files.begin(); it != files.end(); ++it)
    {
        auto record = it.value().toObject();
        if (record.contains("backup"))
        {
            const QString source = record.value("backup").toString();
            const QString path   = destination + '/' + QFileInfo(source).fileName();
            copyFile(source, path);
            record["backup"] = path;
            it.value()       = record;
        }
    }
    auto registry = saved.value("registry").toObject();
    for (auto it = registry.begin(); it != registry.end(); ++it)
    {
        auto exports = it.value().toArray();
        for (int index = 0; index < exports.size(); ++index)
            if (exports[index].isString())
            {
                const QString path = destination + "/original-" + it.key() + '-'
                                     + QString::number(index) + ".reg";
                copyFile(exports[index].toString(), path);
                exports[index] = path;
            }
        it.value() = exports;
    }
    return { { "files", files }, { "registry", registry } };
}

QJsonObject
runtimeEnvironment(const QJsonObject &target, const QString &workspace, const QString &dllpath = {})
{
    auto environment = Launchers::environment(target);
    if (!dllpath.isEmpty())
    {
        auto paths = environment.value("WINEDLLPATH").toString().split(':', Qt::SkipEmptyParts);
        paths.removeAll(dllpath);
        paths.prepend(dllpath);
        environment["WINEDLLPATH"] = paths.join(':');
    }
    environment["XDG_CONFIG_HOME"]              = workspace + "/config";
    environment["PIPEASIO_CONNECT_TO_HARDWARE"] = "off";
    environment["PIPEASIO_CHECK_ISOLATED"]      = "1";
    environment["WINEDEBUG"]                    = "-all";
    return environment;
}

QJsonObject
probe(const QJsonObject &target, const QJsonObject &payload, const QStringList &views,
      const QString &workspace, const QJsonObject &environment, const Progress &progress)
{
    QJsonObject results;
    for (const auto &view : views)
    {
        const QString source     = probeSource(payload, view);
        const QString executable = workspace + '/' + QFileInfo(source).fileName();
        copyFile(source, executable);
        const QString resultFile = workspace + "/check-" + view + ".json";
        unlinkFile(resultFile);
        notify(progress,
               "Checking " + view + "-bit registration, native library and PipeWire access");
        const auto result = Launchers::run(target,
                                           { windowsPath(executable, workspace), "--result",
                                             windowsPath(resultFile, workspace) },
                                           environment);
        if (!QFileInfo(resultFile).isFile())
        {
            QString detail = result.error.isEmpty() ? result.output : result.error;
            if (detail.isEmpty())
                detail = "No result from the runner";
            throw Error("The " + view + "-bit installation check did not complete: "
                        + detail.trimmed().right(2000));
        }
        const auto status = probeResult(resultFile);
        if (result.exitCode || status.value("registered") != QJsonValue(true)
            || status.value("unixlib") != QJsonValue(true)
            || status.value("pipewire") != QJsonValue(true))
            throw Error(status.value("error").toString("PipeASIO is not usable in this runner."));
        results[view] = status;
    }
    return results;
}

void
checkRunnerShadow(const QJsonObject &target, const QJsonObject &payload, const QStringList &views)
{
    const QFileInfo runner(absolutePath(target.value("runner").toString()));
    QStringList     roots{ runner.filePath() };
    if (runner.isFile())
        roots << QDir(runner.absolutePath()).absoluteFilePath("..");
    const QStringList suffixes = { "lib/wine",
                                   "lib64/wine",
                                   "files/lib/wine",
                                   "files/lib64/wine",
                                   "dist/lib/wine",
                                   "lib/x86_64-linux-gnu/wine",
                                   "lib/aarch64-linux-gnu/wine" };
    for (const auto &root : roots)
        for (const auto &suffix : suffixes)
            for (const auto &view : views)
            {
                const QString pe       = peRelative(payload, view);
                const QString native   = unixlibRelative(payload, view);
                const QString library  = root + '/' + suffix;
                const QString expected = payload.value("root").toString() + "/lib/wine/";
                if ((QFileInfo::exists(library + '/' + pe)
                     || QFileInfo::exists(library + '/' + native))
                    && (!QFileInfo(library + '/' + pe).isFile()
                        || !QFileInfo(library + '/' + native).isFile()
                        || hashFile(library + '/' + pe) != hashFile(expected + pe)
                        || hashFile(library + '/' + native) != hashFile(expected + native)))
                    throw Error("A different PipeASIO copy in " + library
                                + " would shadow this release. Remove it with its original "
                                  "installer first.");
            }
}

QString
overridePath()
{
    return dataDirectory() + "/flatpak/overrides/com.usebottles.bottles";
}

struct Permissions
{
    QByteArray  original;
    QStringList lines;
    QStringList entries;
    QList<int>  filesystemLines;
    int         contextEnd = -1;
};

Permissions
permissions()
{
    Permissions   result;
    const QString path = overridePath();
    if (QFileInfo::exists(path))
        result.original = readFile(path);
    result.lines = QString::fromUtf8(result.original).split('\n');
    QString section;
    for (int i = 0; i < result.lines.size(); ++i)
    {
        const QString line = result.lines[i].trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;
        if (line.startsWith('[') && line.endsWith(']'))
        {
            if (section == "Context")
                result.contextEnd = i;
            section = line.mid(1, line.size() - 2);
            continue;
        }
        const auto equal = line.indexOf('=');
        if (section.isEmpty() || equal < 1)
            throw Error("Invalid Flatpak permissions configuration: " + path);
        if (section == "Context" && line.left(equal).trimmed() == "filesystems")
        {
            result.entries = line.mid(equal + 1).trimmed().split(';', Qt::SkipEmptyParts);
            result.filesystemLines << i;
        }
    }
    if (section == "Context")
        result.contextEnd = result.lines.size();
    return result;
}

void
restorePermissions(const QStringList &before, const QStringList &after)
{
    auto current = permissions();
    auto entries = current.entries;
    for (const auto &entry : after)
        if (!before.contains(entry))
            entries.removeAll(entry);
    for (const auto &entry : before)
        if (!after.contains(entry) && !entries.contains(entry))
            entries << entry;
    const QString replacement
            = entries.isEmpty() ? QString{} : "filesystems=" + entries.join(';') + ';';
    if (!current.filesystemLines.isEmpty())
    {
        current.lines[current.filesystemLines.last()] = replacement;
        for (int i = 0; i + 1 < current.filesystemLines.size(); ++i)
            current.lines[current.filesystemLines[i]].clear();
    }
    else if (!replacement.isEmpty())
    {
        if (current.contextEnd >= 0)
            current.lines.insert(current.contextEnd, replacement);
        else
            current.lines << "[Context]" << replacement;
    }
    const QString path = overridePath();
    if ((QFileInfo::exists(path) ? readFile(path) : QByteArray{}) != current.original)
        throw Error("Flatpak permissions changed concurrently. Their backup was retained.");
    writeFile(path, current.lines.join('\n').toUtf8());
}

void
restoreEnvironment(const QJsonObject &target, const QJsonObject &before,
                   const QJsonObject &installed)
{
    const auto  current = Launchers::environment(target);
    QJsonObject updates;
    for (auto it = installed.begin(); it != installed.end(); ++it)
    {
        if (valueOrNull(current, it.key()) != it.value())
            throw Error("Launcher setting " + it.key()
                        + " was changed outside the manager. It was not overwritten.");
        updates[it.key()] = valueOrNull(before, it.key());
    }
    Launchers::setEnvironment(target, updates);
}

QString
shellQuote(QString value)
{
    value.replace('\'', "'\\''");
    return '\'' + value + '\'';
}

QString
manualLauncher(const QJsonObject &target)
{
    if (target.value("kind") != "wine")
        return {};
    const QString destination = stateDirectory(target) + "/launch";
    const auto    environment = Launchers::environment(target);
    const QString script
            = "#!/bin/sh\nexec env " + shellQuote("WINEPREFIX=" + target.value("prefix").toString())
              + ' ' + shellQuote("WINEDLLPATH=" + environment.value("WINEDLLPATH").toString()) + ' '
              + shellQuote(target.value("runner").toString()) + " \"$@\"\n";
    writeFile(destination, script.toUtf8(),
              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return destination;
}

QJsonObject
registryHashes(const QJsonObject &registry)
{
    QJsonObject hashes;
    for (auto it = registry.begin(); it != registry.end(); ++it)
    {
        QJsonArray values;
        for (const auto &filename : it.value().toArray())
            values.append(filename.isString() ? QJsonValue(hashFile(filename.toString()))
                                              : QJsonValue(QJsonValue::Null));
        hashes[it.key()] = values;
    }
    return hashes;
}

QJsonObject
ready(const QJsonObject &state, const QJsonObject &checks)
{
    const QString launcher = state.value("launcher").toString();
    return { { "status", "ready" },
             { "version", state.value("version") },
             { "checks", checks },
             { "launcher", launcher },
             { "message", launcher.isEmpty() ? "PipeASIO is ready in this launcher."
                                             : "Ready via the generated launch wrapper." } };
}

QJsonObject
finishRemoval(const QJsonObject &target, const QJsonObject &state)
{
    if (target.value("kind") == "bottles-flatpak")
    {
        const QString path  = managerDirectory() + "/flatpak-permissions.json";
        const auto    value = readJson(path);
        if (!value.isNull() && !value.isUndefined())
        {
            auto       journal = object(value, path);
            QJsonArray users;
            for (const auto &user : journal.value("users").toArray())
                if (user.toString() != prefixKey(target))
                    users.append(user);
            if (!users.isEmpty())
            {
                journal["users"] = users;
                writeJson(path, journal);
            }
            else
            {
                restorePermissions(journal.value("before").toVariant().toStringList(),
                                   journal.value("after").toVariant().toStringList());
                unlinkFile(path);
            }
        }
    }
    if (!state.value("launcher").toString().isEmpty())
        unlinkFile(state.value("launcher").toString());
    unlinkFile(stateDirectory(target) + "/state.json");
    return {
        { "status", "removed" },
        { "message",
          "Manager-owned changes removed. Any previous PipeASIO installation was restored." }
    };
}
}

QJsonArray
listTargets()
{
    auto          targets = Launchers::discover();
    QSet<QString> known;
    for (const auto &value : targets)
        known.insert(value.toObject().value("id").toString());
    const auto manual = readJson(managerDirectory() + "/manual.json", QJsonArray{});
    if (!manual.isArray())
        throw Error("Invalid manual prefix configuration.");
    for (const auto &value : manual.toArray())
    {
        const auto  saved = object(value, "manual prefix configuration");
        QJsonObject target;
        try
        {
            target = Launchers::manualTarget(saved.value("prefix").toString(),
                                             saved.value("runner").toString(),
                                             saved.value("name").toString());
        }
        catch (const std::exception &error)
        {
            target           = saved;
            target["error"]  = QString::fromUtf8(error.what());
            target["status"] = "unsupported";
        }
        const QString id = target.value("id").toString();
        if (!known.contains(id))
        {
            targets.append(target);
            known.insert(id);
        }
    }
    for (int i = 0; i < targets.size(); ++i)
    {
        auto target   = targets[i].toObject();
        auto metadata = target.value("metadata").toObject();
        if (!target.contains("error"))
            target["error"] = "";
        if (!target.contains("version"))
            target["version"] = "";
        if (!target.contains("status"))
            target["status"] = "not-installed";
        const auto state
                = target.value("prefix").toString().isEmpty() ? QJsonObject{} : stateFor(target);
        if (!state.isEmpty())
        {
            target["version"] = state.value("version");
            target["status"]  = "installed";
            const auto owner  = state.value("target").toObject();
            if (owner.value("id") != target.value("id"))
            {
                if (!known.contains(owner.value("id").toString())
                    && owner.value("kind") == target.value("kind")
                    && owner.value("runner") == target.value("runner"))
                {
                    metadata["adoptable"] = true;
                    metadata["note"]      = "The launcher entry changed. Repair to reconnect this "
                                            "managed prefix.";
                    target["status"]      = "needs-repair";
                }
                else
                    target["error"] = "This prefix is managed through "
                                      + owner.value("name").toString() + ".";
            }
            else if (owner.value("runner") != target.value("runner"))
            {
                target["status"] = "needs-repair";
                metadata["note"] = "Runner changed. Check or repair this installation.";
            }
            const auto files = state.value("files").toObject();
            for (auto it = files.begin(); it != files.end(); ++it)
            {
                const QString path = target.value("prefix").toString() + '/' + it.key();
                if (!QFileInfo(path).isFile() || hashFile(path) != it.value().toString())
                    target["status"] = "needs-repair";
            }
            metadata["include_32"] = state.value("include_32").toBool();
            if (!state.value("launcher").toString().isEmpty())
                metadata["launch_command"] = state.value("launcher");
            if (state.value("phase") == "permissions-pending")
            {
                target["status"]            = "needs-repair";
                metadata["removal_pending"] = true;
                metadata["note"] = "Driver removed. Use Remove again to finish restoring Flatpak "
                                   "permissions.";
            }
        }
        else if (QFileInfo::exists(target.value("prefix").toString() + '/' + dll("64")))
        {
            target["status"] = "needs-repair";
            metadata["note"] = "Existing PipeASIO installation, not managed here.";
        }
        if (!target.value("error").toString().isEmpty())
            target["status"] = "unsupported";
        target["metadata"] = metadata;
        targets[i]         = target;
    }
    return targets;
}

QJsonObject
addPrefix(const QString &prefix, const QString &wine, const QString &name)
{
    Lock       lock;
    const auto target = Launchers::manualTarget(prefix, wine, name);
    for (const auto &value : listTargets())
    {
        const auto existing = value.toObject();
        if (!existing.value("prefix").toString().isEmpty()
            && absolutePath(existing.value("prefix").toString())
                       == absolutePath(target.value("prefix").toString())
            && existing.value("kind") != "wine")
            throw Error("This prefix belongs to " + existing.value("name").toString()
                        + ". Select its detected launcher entry instead.");
    }
    const auto values = readJson(managerDirectory() + "/manual.json", QJsonArray{});
    if (!values.isArray())
        throw Error("Invalid manual prefix configuration.");
    QJsonArray saved;
    for (const auto &entry : values.toArray())
        if (entry.toObject().value("id") != target.value("id"))
            saved.append(entry);
    saved.append(target);
    writeJson(managerDirectory() + "/manual.json", saved);
    return target;
}

QJsonObject
preview(QNetworkAccessManager &network, const QString &targetId, const QString &version,
        bool include32)
{
    const auto target = targetFor(targetId);
    validate(target);
    auto selected = version.isEmpty() ? QJsonObject{} : cachedPayload(target, version);
    if (selected.isEmpty())
        selected = selectRelease(network, version, {});
    QJsonArray    warnings{ "Close applications using this prefix before installation." };
    const QString kind = target.value("kind").toString();
    if (kind == "faugus")
        warnings.append(
                "Close Faugus, including its tray process, before changing launch settings.");
    if (kind.startsWith("bottles"))
        warnings.append("Close Bottles before changing its configuration.");
    if (kind == "wine")
        warnings.append("Plain Wine needs a launch wrapper to retain WINEDLLPATH. The manager will "
                        "create one.");
    if (include32)
        warnings.append("Experimental 32-bit support requires new WoW64 and a successful 32-bit "
                        "runtime check.");
    const QString platform
            = selected.value("compatibility").toObject().value("platform").toString();
    if (!platform.isEmpty())
        warnings.append("Prebuilt target: " + platform
                        + ". Installation is accepted only after the selected runner passes its "
                          "runtime check.");
    const QString summary
            = "Application: " + target.value("name").toString() + "\nPrefix: "
              + target.value("prefix").toString() + "\nRunner: " + target.value("runner").toString()
              + "\nRelease: " + selected.value("version").toString()
              + "\nDriver files: " + versionsDirectory(target)
              + "\nExisting PipeASIO files and changed launcher settings are backed up.";
    return { { "summary", summary },
             { "warnings", warnings },
             { "permissions", QJsonArray::fromStringList(Launchers::permissionPlan(
                                      target, versionsDirectory(target))) },
             { "version", selected.value("version") } };
}

QJsonObject
install(QNetworkAccessManager &network, const QString &targetId, const QString &version,
        bool include32, bool allowPermissions, const Progress &progress)
{
    Lock       lock;
    const auto target = targetFor(targetId);
    validate(target);
    assertIdle(target);
    const auto previous = stateFor(target);
    if (previous.value("phase") == "permissions-pending")
        throw Error("Finish the pending removal before installing this prefix again.");
    const QStringList views    = include32 ? QStringList{ "64", "32" } : QStringList{ "64" };
    const QStringList oldViews = previous.value("include_32").toBool() ? QStringList{ "64", "32" }
                                                                       : QStringList{ "64" };
    const auto        payload  = payloadFor(network, target, version, progress);
    const QString     dllpath  = payload.value("root").toString() + "/lib/wine";
    for (const auto &view : views)
    {
        if (!QFileInfo(payloadFile(payload, peRelative(payload, view))).isFile()
            || !QFileInfo(payloadFile(payload, unixlibRelative(payload, view))).isFile())
            throw Error("The release does not contain the requested " + view + "-bit driver.");
        probeSource(payload, view);
    }
    checkRunnerShadow(target, payload, views);
    const auto plan = Launchers::permissionPlan(target, payload.value("root").toString());
    if (!plan.isEmpty() && !allowPermissions)
        throw Error("Installation requires approval for Flatpak access: " + plan.join("; "));
    const QString stateRoot = stateDirectory(target);
    makeDirectory(stateRoot);
    const auto environmentBefore = Launchers::environment(target);
    auto paths = environmentBefore.value("WINEDLLPATH").toString().split(':', Qt::SkipEmptyParts);
    if (!previous.isEmpty())
        paths.removeAll(previous.value("dllpath").toString());
    paths.removeAll(dllpath);
    paths.prepend(dllpath);
    QJsonObject updates{ { "WINEDLLPATH", paths.join(':') } };
    if (target.value("kind") == "faugus" && include32)
        updates["PROTON_USE_WOW64"] = "1";
    else if (target.value("kind") == "faugus"
             && previous.value("environment_installed").toObject().contains("PROTON_USE_WOW64"))
        updates["PROTON_USE_WOW64"]
                = valueOrNull(previous.value("environment_before").toObject(), "PROTON_USE_WOW64");
    const bool        flatpak            = target.value("kind") == "bottles-flatpak";
    const QStringList permissionBefore   = flatpak ? permissions().entries : QStringList{};
    QStringList       permissionAfter    = permissionBefore;
    const QString     journalPath        = managerDirectory() + "/flatpak-permissions.json";
    const auto        permissionJournal  = flatpak ? readJson(journalPath) : QJsonValue{};
    bool              environmentChanged = false;
    QJsonObject       saved, state, checks;
    QTemporaryDir     workspace(workspaceTemplate(target));
    prepareWorkspace(workspace);
    QTemporaryDir transaction(stateRoot + "/transaction-XXXXXX");
    if (!transaction.isValid())
        throw Error("Cannot create installation backup directory.");
    auto environment = runtimeEnvironment(target, workspace.path());
    try
    {
        if (!plan.isEmpty())
        {
            notify(progress, "Applying approved Flatpak permissions");
            try
            {
                Launchers::grantPermissions(target, payload.value("root").toString());
            }
            catch (...)
            {
                permissionAfter = permissions().entries;
                throw;
            }
            permissionAfter = permissions().entries;
        }
        notify(progress, "Backing up existing PipeASIO files and registration");
        QStringList allViews = oldViews + views;
        allViews.removeDuplicates();
        saved = snapshot(target, allViews, transaction.path(), workspace.path(), environment,
                         payload);
        auto        baseline = previous.isEmpty() ? QJsonObject{ { "files", QJsonObject{} },
                                                                 { "registry", QJsonObject{} } }
                                                  : previous.value("originals").toObject();
        QStringList newViews;
        for (const auto &view : views)
            if (!baseline.value("registry").toObject().contains(view))
                newViews << view;
        if (!newViews.isEmpty())
        {
            const auto originals
                    = persistOriginals(subset(saved, newViews), stateRoot + "/originals");
            for (const QString &category : { QString("files"), QString("registry") })
            {
                auto       merged = baseline.value(category).toObject();
                const auto added  = originals.value(category).toObject();
                for (auto it = added.begin(); it != added.end(); ++it)
                    merged[it.key()] = it.value();
                baseline[category] = merged;
            }
        }
        for (const auto &view : oldViews)
            if (!views.contains(view))
            {
                restore(target, subset(baseline, { view }), workspace.path(), environment, payload);
                auto files    = baseline.value("files").toObject();
                auto registry = baseline.value("registry").toObject();
                files.remove(dll(view));
                registry.remove(view);
                baseline["files"]    = files;
                baseline["registry"] = registry;
            }
        environmentChanged = true;
        Launchers::setEnvironment(target, updates);
        environment = runtimeEnvironment(target, workspace.path(), dllpath);
        notify(progress,
               "Copying and registering the driver through " + target.value("kind").toString());
        QJsonObject hashes;
        for (const auto &view : views)
        {
            const QString destination = target.value("prefix").toString() + '/' + dll(view);
            const QString unfinished  = destination + ".pipeasio-new";
            if (QFileInfo::exists(unfinished) || QFileInfo(unfinished).isSymLink())
                throw Error("An unfinished installation file exists at " + unfinished);
            copyFile(payloadFile(payload, peRelative(payload, view)), destination);
            hashes[dll(view)]       = hashFile(destination);
            const QString directory = view == "64" ? "system32" : "syswow64";
            run(target,
                { "C:\\windows\\" + directory + "\\regsvr32.exe", "/s",
                  "C:\\windows\\" + directory + "\\pipeasio" + view + ".dll" },
                environment);
        }
        checks = probe(target, payload, views, workspace.path(), environment, progress);
        const auto registry             = snapshot(target, views, transaction.path() + "/installed",
                                                   workspace.path(), environment, payload)
                                                  .value("registry")
                                                  .toObject();
        auto       originalsEnvironment = previous.value("environment_before").toObject();
        for (auto it = updates.begin(); it != updates.end(); ++it)
            if (!originalsEnvironment.contains(it.key())
                || (!previous.isEmpty()
                    && previous.value("target").toObject().value("id") != target.value("id")
                    && valueOrNull(environmentBefore, it.key())
                               != valueOrNull(previous.value("environment_installed").toObject(),
                                              it.key())))
                originalsEnvironment[it.key()] = valueOrNull(environmentBefore, it.key());
        state             = { { "schema", 1 },
                              { "target", target },
                              { "version", payload.value("version") },
                              { "payload", payload },
                              { "dllpath", dllpath },
                              { "include_32", include32 },
                              { "files", hashes },
                              { "registry_hashes", registryHashes(registry) },
                              { "originals", baseline },
                              { "environment_before", originalsEnvironment },
                              { "environment_installed", updates },
                              { "checks", checks } };
        state["launcher"] = manualLauncher(target);
        if (flatpak)
        {
            auto journal = permissionJournal.isObject()
                                   ? permissionJournal.toObject()
                                   : QJsonObject{
                                         { "before", QJsonArray::fromStringList(permissionBefore) },
                                         { "after", QJsonArray::fromStringList(permissionAfter) },
                                         { "users", QJsonArray{} }
                                     };
            auto before  = journal.value("before").toVariant().toStringList();
            const auto oldAfter = journal.value("after").toVariant().toStringList();
            for (const auto &entry : permissionBefore)
                if (!oldAfter.contains(entry) && !before.contains(entry))
                    before << entry;
            journal["before"] = QJsonArray::fromStringList(before);
            journal["after"]  = QJsonArray::fromStringList(permissionAfter);
            auto users        = journal.value("users").toVariant().toStringList();
            users << prefixKey(target);
            users.removeDuplicates();
            std::sort(users.begin(), users.end());
            journal["users"] = QJsonArray::fromStringList(users);
            writeJson(journalPath, journal);
        }
        writeJson(stateRoot + "/state.json", state);
    }
    catch (const std::exception &failure)
    {
        QStringList errors;
        if (!saved.isEmpty())
        {
            try
            {
                notify(progress, "Restoring the previous prefix installation");
                restore(target, saved, workspace.path(), environment, payload);
            }
            catch (const std::exception &error)
            {
                const QString recovery = stateRoot + "/recovery";
                try
                {
                    writeJson(recovery + "/snapshot.json", persistOriginals(saved, recovery));
                }
                catch (const std::exception &retentionError)
                {
                    transaction.setAutoRemove(false);
                    errors << "Transaction backup retained at " + transaction.path() + ": "
                                      + QString::fromUtf8(retentionError.what());
                }
                errors << "Prefix backup retained at " + recovery + ": "
                                  + QString::fromUtf8(error.what());
            }
        }
        if (environmentChanged)
        {
            try
            {
                const auto current = Launchers::environment(target);
                bool       changed = false;
                for (auto it = updates.begin(); it != updates.end(); ++it)
                    changed |= valueOrNull(current, it.key())
                               != valueOrNull(environmentBefore, it.key());
                if (changed)
                    restoreEnvironment(target, environmentBefore, updates);
            }
            catch (const std::exception &error)
            {
                errors << QString::fromUtf8(error.what());
                try
                {
                    writeJson(stateRoot + "/environment-recovery.json",
                              QJsonObject{ { "before", environmentBefore },
                                           { "installed", updates } });
                }
                catch (const std::exception &saveError)
                {
                    errors << QString::fromUtf8(saveError.what());
                }
            }
        }
        if (permissionAfter != permissionBefore)
            try
            {
                restorePermissions(permissionBefore, permissionAfter);
            }
            catch (const std::exception &error)
            {
                errors << QString::fromUtf8(error.what());
            }
        try
        {
            if (!previous.isEmpty())
            {
                writeJson(stateRoot + "/state.json", previous);
                if (!previous.value("launcher").toString().isEmpty())
                    manualLauncher(previous.value("target").toObject());
            }
            else
            {
                unlinkFile(stateRoot + "/state.json");
                unlinkFile(stateRoot + "/launch");
            }
            if (flatpak)
            {
                if (permissionJournal.isNull() || permissionJournal.isUndefined())
                    unlinkFile(journalPath);
                else
                    writeJson(journalPath, permissionJournal);
            }
        }
        catch (const std::exception &error)
        {
            errors << "Manager state recovery failed: " + QString::fromUtf8(error.what());
        }
        if (!errors.isEmpty())
            throw Error(QString::fromUtf8(failure.what())
                        + "\nRecovery needs attention: " + errors.join("; "));
        throw;
    }
    return ready(state, checks);
}

QJsonObject
check(const QString &targetId, const Progress &progress)
{
    Lock       lock;
    const auto target = targetFor(targetId);
    validate(target);
    const auto state = stateFor(target);
    if (state.isEmpty())
        throw Error("Install or repair this prefix with the manager before checking it.");
    if (target.value("status") == "needs-repair")
        throw Error("The runner or installed files changed. Repair this prefix first.");
    const auto payload = state.value("payload").toObject();
    if (!verifiedPayload(payload, payload.value("root").toString()))
        throw Error("The cached driver files changed. Repair this prefix first.");
    QTemporaryDir workspace(workspaceTemplate(target));
    prepareWorkspace(workspace);
    const auto        environment = runtimeEnvironment(target, workspace.path());
    const QStringList views
            = state.value("include_32").toBool() ? QStringList{ "64", "32" } : QStringList{ "64" };
    return ready(state, probe(target, payload, views, workspace.path(), environment, progress));
}

QJsonObject
remove(const QString &targetId, const Progress &progress)
{
    Lock       lock;
    const auto target = targetFor(targetId);
    validate(target);
    assertIdle(target);
    auto state = stateFor(target);
    if (state.isEmpty())
        throw Error("This prefix has no manager-owned installation to remove.");
    if (state.value("phase") == "permissions-pending")
        return finishRemoval(target, state);
    const auto files = state.value("files").toObject();
    for (auto it = files.begin(); it != files.end(); ++it)
    {
        const QString path = target.value("prefix").toString() + '/' + it.key();
        if (!QFileInfo(path).isFile() || hashFile(path) != it.value().toString())
            throw Error("A manager-owned file was changed outside the manager: " + path
                        + ". It was not removed.");
    }
    const auto currentEnvironment   = Launchers::environment(target);
    const auto installedEnvironment = state.value("environment_installed").toObject();
    for (auto it = installedEnvironment.begin(); it != installedEnvironment.end(); ++it)
        if (valueOrNull(currentEnvironment, it.key()) != it.value())
            throw Error("Launcher setting " + it.key()
                        + " changed outside the manager. It was not overwritten.");
    const auto    payload = state.value("payload").toObject();
    QTemporaryDir workspace(workspaceTemplate(target));
    prepareWorkspace(workspace);
    QTemporaryDir transaction(stateDirectory(target) + "/remove-XXXXXX");
    if (!transaction.isValid())
        throw Error("Cannot create removal backup directory.");
    const auto environment = runtimeEnvironment(target, workspace.path());
    const auto current     = snapshot(
            target, state.value("originals").toObject().value("registry").toObject().keys(),
            transaction.path(), workspace.path(), environment, payload);
    if (registryHashes(current.value("registry").toObject())
        != state.value("registry_hashes").toObject())
        throw Error("PipeASIO registration changed outside the manager. It was not removed.");
    try
    {
        notify(progress, "Restoring the prefix's previous PipeASIO files and registration");
        restore(target, state.value("originals").toObject(), workspace.path(), environment,
                payload);
        restoreEnvironment(target, state.value("environment_before").toObject(),
                           installedEnvironment);
        auto pending     = state;
        pending["phase"] = "permissions-pending";
        pending["files"] = QJsonObject{};
        writeJson(stateDirectory(target) + "/state.json", pending);
    }
    catch (const std::exception &failure)
    {
        QStringList errors;
        try
        {
            restore(target, current, workspace.path(), environment, payload);
        }
        catch (const std::exception &error)
        {
            transaction.setAutoRemove(false);
            try
            {
                writeJson(transaction.path() + "/snapshot.json", current);
            }
            catch (const std::exception &saveError)
            {
                errors << QString::fromUtf8(saveError.what());
            }
            errors << "Removal backup retained at " + transaction.path() + ": "
                              + QString::fromUtf8(error.what());
        }
        try
        {
            Launchers::setEnvironment(target, installedEnvironment);
        }
        catch (const std::exception &error)
        {
            errors << QString::fromUtf8(error.what());
        }
        if (!errors.isEmpty())
            throw Error(QString::fromUtf8(failure.what())
                        + "\nRecovery needs attention: " + errors.join("; "));
        throw;
    }
    state["phase"] = "permissions-pending";
    state["files"] = QJsonObject{};
    try
    {
        return finishRemoval(target, state);
    }
    catch (const std::exception &error)
    {
        throw Error("The driver was removed, but permission cleanup needs attention. Use Remove "
                    "again to retry: "
                    + QString::fromUtf8(error.what()));
    }
}
}
