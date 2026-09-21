/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Manager.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

namespace PipeASIOManager
{
namespace
{
std::atomic_bool cancelled{ false };

// Set only by Testing::setArchitecture(); empty in every shipped code path.
QString architectureOverride;

QString
systemError(const QString &operation)
{
    return operation + QStringLiteral(": ") + QString::fromLocal8Bit(std::strerror(errno));
}

void
publishFile(QTemporaryFile &temporary, const QString &destination)
{
    if (!temporary.flush() || ::fsync(temporary.handle()) != 0)
        throw Error(systemError(QStringLiteral("Cannot flush ") + destination));
    const QByteArray source = QFile::encodeName(temporary.fileName());
    temporary.close();
    const QByteArray target = QFile::encodeName(destination);
    if (::rename(source.constData(), target.constData()) != 0)
        throw Error(systemError(QStringLiteral("Cannot replace ") + destination));
    temporary.setAutoRemove(false);
    const QByteArray directory = QFile::encodeName(QFileInfo(destination).absolutePath());
    const int        fd        = ::open(directory.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0)
    {
        const int result = ::fsync(fd);
        const int saved  = errno;
        ::close(fd);
        if (result != 0 && saved != EINVAL && saved != EROFS)
        {
            errno = saved;
            throw Error(
                    systemError(QStringLiteral("Cannot sync the directory for ") + destination));
        }
    }
}

QString
temporaryPattern(const QString &destination)
{
    const QString parent = QFileInfo(destination).absolutePath();
    if (!QDir().mkpath(parent))
        throw Error(QStringLiteral("Cannot create directory: ") + parent);
    return parent + QStringLiteral("/.pipeasio-XXXXXX");
}
}

Error::Error(const QString &message) : std::runtime_error(message.toUtf8().constData())
{
}

void
requestCancellation() noexcept
{
    cancelled.store(true, std::memory_order_relaxed);
}

bool
cancellationRequested() noexcept
{
    return cancelled.load(std::memory_order_relaxed);
}

void
throwIfCancelled()
{
    if (cancelled.exchange(false, std::memory_order_relaxed))
        throw Error(QStringLiteral("Operation cancelled."));
}

QString
homeDirectory()
{
    const QString home = qEnvironmentVariable("HOME");
    return home.isEmpty() ? QDir::homePath() : QDir::cleanPath(home);
}

QString
dataDirectory()
{
    const QString path = qEnvironmentVariable("XDG_DATA_HOME");
    return path.isEmpty() ? homeDirectory() + QStringLiteral("/.local/share")
                          : QDir::cleanPath(path);
}

QString
configDirectory()
{
    const QString path = qEnvironmentVariable("XDG_CONFIG_HOME");
    return path.isEmpty() ? homeDirectory() + QStringLiteral("/.config") : QDir::cleanPath(path);
}

QString
managerDirectory()
{
    return dataDirectory() + QStringLiteral("/pipeasio");
}

QString
architecture()
{
    if (!architectureOverride.isEmpty())
        return architectureOverride;
#if defined(__x86_64__)
    return QStringLiteral("x86_64");
#elif defined(__aarch64__)
    return QStringLiteral("aarch64");
#else
    return QStringLiteral("unsupported");
#endif
}

void
Testing::setArchitecture(const QString &architecture)
{
    architectureOverride = architecture;
}

const Layout *
layoutFor(const QString &architecture)
{
    // ARM64 releases carry two 64-bit front ends over the one aarch64 unixlib:
    // the aarch64 PE for native ARM64 Windows hosts, and, when the build host
    // had Wine's arm64ec import libraries, the arm64ec PE for x86_64 hosts
    // under FEX. An ARM64EC image reports machine 0x8664 with CHPE metadata,
    // which is why its expected machine is the x86_64 one.
    static const Layout layouts[]
            = { { QStringLiteral("x86_64"), QStringLiteral("x86_64-windows"), 0x8664, QString(), 0,
                  QStringLiteral("x86_64-unix"), 62 },
                { QStringLiteral("aarch64"), QStringLiteral("aarch64-windows"), 0xaa64,
                  QStringLiteral("arm64ec-windows"), 0x8664, QStringLiteral("aarch64-unix"),
                  183 } };
    for (const Layout &candidate : layouts)
        if (candidate.architecture == architecture)
            return &candidate;
    return nullptr;
}

const Layout &
layout()
{
    const Layout *host = layoutFor(architecture());
    if (!host)
        throw Error(QStringLiteral("PipeASIO has no driver layout for this CPU architecture."));
    return *host;
}

QString
absolutePath(const QString &path, const QString &base)
{
    if (path.isEmpty() || path.contains(QChar(0)))
        throw Error(QStringLiteral("A nonempty filesystem path is required."));
    QString expanded = path;
    if (expanded == QStringLiteral("~"))
        expanded = homeDirectory();
    else if (expanded.startsWith(QStringLiteral("~/")))
        expanded = homeDirectory() + expanded.mid(1);
    static const QRegularExpression variable(
            QStringLiteral("\\$\\{([^}]+)\\}|\\$([A-Za-z_][A-Za-z0-9_]*)"));
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    auto                      matches     = variable.globalMatch(expanded);
    QString                   resolved;
    qsizetype                 previous = 0;
    while (matches.hasNext())
    {
        const auto match = matches.next();
        resolved += expanded.mid(previous, match.capturedStart() - previous);
        const QString key = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
        resolved += environment.contains(key) ? environment.value(key) : match.captured();
        previous = match.capturedEnd();
    }
    resolved += expanded.mid(previous);
    if (!QDir::isAbsolutePath(resolved))
    {
        if (base.isEmpty() || !QDir::isAbsolutePath(base))
            throw Error(QStringLiteral("Use an absolute path: ") + path);
        resolved = QDir(base).absoluteFilePath(resolved);
    }
    resolved                = QDir::cleanPath(resolved);
    const QString canonical = QFileInfo(resolved).canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;
    QString     ancestor = resolved;
    QStringList suffix;
    while (!QFileInfo::exists(ancestor) && ancestor != QStringLiteral("/"))
    {
        const QFileInfo info(ancestor);
        suffix.prepend(info.fileName());
        ancestor = info.absolutePath();
    }
    const QString parent = QFileInfo(ancestor).canonicalFilePath();
    return parent.isEmpty()
                   ? resolved
                   : QDir::cleanPath(parent + QLatin1Char('/') + suffix.join(QLatin1Char('/')));
}

QString
stableId(const QString &kind, const QString &identity)
{
    return kind + QLatin1Char(':')
           + QString::fromLatin1(
                   QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256)
                           .toHex()
                           .left(24));
}

QString
prefixKey(const QJsonObject &target)
{
    return QString::fromLatin1(
            QCryptographicHash::hash(
                    absolutePath(target.value(QStringLiteral("prefix")).toString()).toUtf8(),
                    QCryptographicHash::Sha256)
                    .toHex()
                    .left(24));
}

QString
prefixError(const QString &prefix)
{
    if (prefix.isEmpty() || !QDir::isAbsolutePath(prefix) || prefix.contains(QChar(0)))
        return QStringLiteral("Select a Wine prefix by absolute path.");
    if (!QFileInfo(prefix).isDir() || !QFileInfo(prefix + QStringLiteral("/drive_c")).isDir()
        || !QFileInfo(prefix + QStringLiteral("/system.reg")).isFile())
        return QStringLiteral("Select an existing initialized Wine prefix. No prefix was created.");
    QFile registry(prefix + QStringLiteral("/system.reg"));
    if (!registry.open(QIODevice::ReadOnly))
        return QStringLiteral("Cannot read the Wine prefix registry.");
    if (registry.read(1024).contains("#arch=win32"))
        return QStringLiteral("PipeASIO needs a 64-bit prefix. Use new WoW64 in that prefix for "
                              "32-bit applications.");
    return {};
}

QByteArray
readFile(const QString &path, qint64 limit)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw Error(QStringLiteral("Cannot read ") + path + QStringLiteral(": ")
                    + file.errorString());
    if (file.size() > limit)
        throw Error(QStringLiteral("File exceeds the size limit: ") + path);
    const QByteArray contents = file.read(limit + 1);
    if (contents.size() > limit || file.error() != QFileDevice::NoError)
        throw Error(QStringLiteral("Cannot read a bounded file: ") + path);
    return contents;
}

QJsonValue
readJson(const QString &path, const QJsonValue &missing)
{
    if (!QFileInfo::exists(path))
        return missing;
    QJsonParseError error;
    const auto      document = QJsonDocument::fromJson(readFile(path), &error);
    if (error.error != QJsonParseError::NoError)
        throw Error(QStringLiteral("Invalid JSON in ") + path + QStringLiteral(": ")
                    + error.errorString());
    if (document.isObject())
        return document.object();
    if (document.isArray())
        return document.array();
    throw Error(QStringLiteral("Expected a JSON object or array in ") + path);
}

void
writeFile(const QString &path, const QByteArray &contents, QFileDevice::Permissions permissions)
{
    throwIfCancelled();
    QTemporaryFile temporary(temporaryPattern(path));
    if (!temporary.open() || !temporary.setPermissions(permissions))
        throw Error(QStringLiteral("Cannot create an atomic replacement for ") + path);
    if (temporary.write(contents) != contents.size())
        throw Error(QStringLiteral("Cannot write ") + path + QStringLiteral(": ")
                    + temporary.errorString());
    publishFile(temporary, path);
}

void
writeJson(const QString &path, const QJsonValue &value)
{
    if (!value.isObject() && !value.isArray())
        throw Error(QStringLiteral("Only JSON objects and arrays can be persisted."));
    const QJsonDocument document
            = value.isObject() ? QJsonDocument(value.toObject()) : QJsonDocument(value.toArray());
    writeFile(path, document.toJson(QJsonDocument::Indented));
}

QString
hashFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw Error(QStringLiteral("Cannot hash ") + path + QStringLiteral(": ")
                    + file.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        throw Error(QStringLiteral("Cannot hash ") + path + QStringLiteral(": ")
                    + file.errorString());
    return QString::fromLatin1(hash.result().toHex());
}

void
copyFile(const QString &source, const QString &destination)
{
    throwIfCancelled();
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
        throw Error(QStringLiteral("Cannot copy ") + source + QStringLiteral(": ")
                    + input.errorString());
    QTemporaryFile output(temporaryPattern(destination));
    if (!output.open())
        throw Error(QStringLiteral("Cannot stage ") + destination);
    std::array<char, 65536> buffer;
    for (;;)
    {
        throwIfCancelled();
        const qint64 count = input.read(buffer.data(), buffer.size());
        if (count < 0)
            throw Error(QStringLiteral("Cannot read ") + source + QStringLiteral(": ")
                        + input.errorString());
        if (count == 0)
            break;
        if (output.write(buffer.data(), count) != count)
            throw Error(QStringLiteral("Cannot write ") + destination + QStringLiteral(": ")
                        + output.errorString());
    }
    if (!output.setPermissions(QFileInfo(source).permissions()))
        throw Error(QStringLiteral("Cannot set file permissions: ") + destination);
    publishFile(output, destination);
}

QProcessEnvironment
cleanEnvironment()
{
    const auto        original    = QProcessEnvironment::systemEnvironment();
    auto              environment = original;
    const QString     appdir      = original.value(QStringLiteral("APPDIR"));
    const QStringList modified    = { QStringLiteral("LD_LIBRARY_PATH"),
                                      QStringLiteral("QT_PLUGIN_PATH"),
                                      QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
                                      QStringLiteral("QT_QPA_PLATFORMTHEME"),
                                      QStringLiteral("QT_STYLE_OVERRIDE"),
                                      QStringLiteral("QML2_IMPORT_PATH"),
                                      QStringLiteral("SPA_PLUGIN_DIR"),
                                      QStringLiteral("PIPEWIRE_MODULE_DIR"),
                                      QStringLiteral("PIPEWIRE_CONFIG_DIR"),
                                      QStringLiteral("SSL_CERT_FILE") };
    if (!appdir.isEmpty())
    {
        for (const QString &key : modified)
        {
            if (original.value(QStringLiteral("PIPEASIO_ORIGINAL_SET_") + key)
                == QStringLiteral("1"))
                environment.insert(key, original.value(QStringLiteral("PIPEASIO_ORIGINAL_") + key));
            else if (key != QStringLiteral("SSL_CERT_FILE"))
                environment.remove(key);
        }
        for (const QString &key : { QStringLiteral("PATH"), QStringLiteral("XDG_DATA_DIRS") })
        {
            QStringList paths;
            for (const QString &path :
                 environment.value(key).split(QLatin1Char(':'), Qt::SkipEmptyParts))
                if (path != appdir && !path.startsWith(appdir + QLatin1Char('/')))
                    paths.append(path);
            environment.insert(key, paths.join(QLatin1Char(':')));
        }
    }
    const QStringList removed = { QStringLiteral("APPIMAGE"),
                                  QStringLiteral("APPDIR"),
                                  QStringLiteral("ARGV0"),
                                  QStringLiteral("OWD"),
                                  QStringLiteral("WINEPREFIX"),
                                  QStringLiteral("WINEARCH"),
                                  QStringLiteral("WINEDLLOVERRIDES"),
                                  QStringLiteral("WINEDLLPATH"),
                                  QStringLiteral("WINELOADER"),
                                  QStringLiteral("WINESERVER"),
                                  QStringLiteral("PROTONPATH"),
                                  QStringLiteral("FAUGUSID") };
    for (const QString &key : original.keys())
        if (removed.contains(key) || key.startsWith(QStringLiteral("PIPEASIO_ORIGINAL_"))
            || key.startsWith(QStringLiteral("STEAM_COMPAT_"))
            || key.startsWith(QStringLiteral("UMU_"))
            || key.startsWith(QStringLiteral("APPIMAGE_")))
            environment.remove(key);
    return environment;
}

CommandResult
execute(const QString &program, const QStringList &arguments,
        const QProcessEnvironment &environment, const QString &workingDirectory, int timeoutMs)
{
    throwIfCancelled();
    if (program.isEmpty() || program.contains(QChar(0)))
        throw Error(QStringLiteral("An executable is required."));
    for (const QString &argument : arguments)
        if (argument.contains(QChar(0)))
            throw Error(QStringLiteral("Process arguments cannot contain NUL characters."));
    QString executable = program;
    if (!program.contains(QLatin1Char('/')))
    {
        const QString search = environment.value(QStringLiteral("PATH"),
                                                 QStringLiteral("/usr/local/bin:/usr/bin:/bin"));
        executable           = QStandardPaths::findExecutable(
                program, search.split(QLatin1Char(':'), Qt::KeepEmptyParts));
        if (executable.isEmpty())
            throw Error(QStringLiteral("Executable not found: ") + program);
    }
    QProcess process;
    process.setProcessEnvironment(environment);
    if (!workingDirectory.isEmpty())
        process.setWorkingDirectory(workingDirectory);
    process.setChildProcessModifier([] { ::setpgid(0, 0); });
    process.start(executable, arguments);
    if (!process.waitForStarted(qMin(timeoutMs, 30000)))
        throw Error(QStringLiteral("Cannot start ") + program + QStringLiteral(": ")
                    + process.errorString());
    const qint64 group = process.processId();
    const auto   stop  = [&]
    {
        if (process.state() != QProcess::NotRunning)
        {
            if (group > 0)
                ::kill(-static_cast<pid_t>(group), SIGKILL);
            process.kill();
            process.waitForFinished(1000);
        }
    };
    QElapsedTimer deadline;
    deadline.start();
    QByteArray output, errors;
    while (process.state() != QProcess::NotRunning)
    {
        process.waitForFinished(100);
        output += process.readAllStandardOutput();
        errors += process.readAllStandardError();
        if (output.size() + errors.size() > 4 * 1024 * 1024)
        {
            stop();
            throw Error(QStringLiteral("Process output exceeds the safety limit: ") + program);
        }
        if (cancellationRequested())
        {
            stop();
            throwIfCancelled();
        }
        if (deadline.elapsed() >= timeoutMs && process.state() != QProcess::NotRunning)
        {
            stop();
            throw Error(QStringLiteral("Process timed out: ") + program);
        }
    }
    output += process.readAllStandardOutput();
    errors += process.readAllStandardError();
    if (output.size() + errors.size() > 4 * 1024 * 1024)
        throw Error(QStringLiteral("Process output exceeds the safety limit: ") + program);
    return { process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1,
             QString::fromUtf8(output), QString::fromUtf8(errors) };
}
}
