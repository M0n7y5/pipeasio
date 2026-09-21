/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QByteArray>
#include <QFileDevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <functional>
#include <stdexcept>

class QNetworkAccessManager;

namespace PipeASIOManager
{
using Progress = std::function<void(const QString &)>;

class Error : public std::runtime_error
{
  public:
    explicit Error(const QString &message);
};

void requestCancellation() noexcept;
bool cancellationRequested() noexcept;
void throwIfCancelled();

struct CommandResult
{
    int     exitCode = -1;
    QString output;
    QString error;
};

QString             homeDirectory();
QString             dataDirectory();
QString             configDirectory();
QString             managerDirectory();
QString             architecture();
QString             absolutePath(const QString &path, const QString &base = {});
QString             stableId(const QString &kind, const QString &identity);
QString             prefixKey(const QJsonObject &target);
QString             prefixError(const QString &prefix);
QByteArray          readFile(const QString &path, qint64 limit = 2 * 1024 * 1024);
QJsonValue          readJson(const QString &path, const QJsonValue &missing = {});
void                writeFile(const QString &path, const QByteArray &contents,
                              QFileDevice::Permissions permissions
                              = QFileDevice::ReadOwner | QFileDevice::WriteOwner);
void                writeJson(const QString &path, const QJsonValue &value);
QString             hashFile(const QString &path);
void                copyFile(const QString &source, const QString &destination);
QProcessEnvironment cleanEnvironment();
CommandResult execute(const QString &program, const QStringList &arguments,
                      const QProcessEnvironment &environment, const QString &workingDirectory = {},
                      int timeoutMs = 120000);

// Where a release keeps its binaries on one host architecture, and the machine
// each of them must report. Wine names its module directories after the PE and
// unix architecture it loads them for, so every such path the manager builds
// comes from this table rather than from a literal.
struct Layout
{
    QString architecture;      // as architecture() reports it
    QString peDirectory;       // 64-bit PE front end; its DLL is what system32 gets
    quint16 peMachine;         // PE machine of that front end
    QString peEmulated;        // extra 64-bit front end, empty when the host has none
    quint16 peEmulatedMachine; // PE machine of that front end, 0 when there is none
    QString unixDirectory;     // unixlib directory, shared by both bitnesses
    quint16 elfMachine;        // ELF machine of the unixlibs
};

const Layout &layout();
const Layout *layoutFor(const QString &architecture);

namespace Testing
{
// Pins architecture(), so one build machine can exercise every host layout.
void setArchitecture(const QString &architecture);
}

namespace Launchers
{
QJsonArray    discover();
QJsonObject   manualTarget(const QString &prefix, const QString &wine, const QString &name = {});
QJsonObject   environment(const QJsonObject &target);
void          setEnvironment(const QJsonObject &target, const QJsonObject &updates);
CommandResult run(const QJsonObject &target, const QStringList &arguments,
                  const QJsonObject &environment, int timeoutMs = 120000);
QStringList   permissionPlan(const QJsonObject &target, const QString &payloadRoot);
QStringList   grantPermissions(const QJsonObject &target, const QString &payloadRoot);
}

namespace Releases
{
QJsonArray  list(QNetworkAccessManager &network, const Progress &progress = {});
QJsonObject fetch(QNetworkAccessManager &network, const QJsonObject &release,
                  const QString &destination, const Progress &progress = {});
QJsonObject createManifest(const QString &version, const QString &asset, const QString &root,
                           const QString &wineSdk, const QString &architecture = {});
}

namespace Installer
{
QJsonArray  listTargets();
QJsonObject addPrefix(const QString &prefix, const QString &wine, const QString &name = {});
QJsonObject preview(QNetworkAccessManager &network, const QString &targetId,
                    const QString &version = {}, bool include32 = false);
QJsonObject install(QNetworkAccessManager &network, const QString &targetId,
                    const QString &version = {}, bool include32 = false,
                    bool allowPermissions = false, const Progress &progress = {});
QJsonObject check(const QString &targetId, const Progress &progress = {});
QJsonObject remove(const QString &targetId, const Progress &progress = {});
}
}
