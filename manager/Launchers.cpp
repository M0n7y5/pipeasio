/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Manager.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <yaml-cpp/yaml.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace PipeASIOManager::Launchers
{
namespace
{
const QString            app = QStringLiteral("com.usebottles.bottles");
const QRegularExpression environmentKey(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));

QString
string(const YAML::Node &node, const QString &fallback = {})
{
    if (!node || node.IsNull())
        return fallback;
    if (!node.IsScalar())
        throw Error("Expected a scalar launcher setting");
    return QString::fromStdString(node.Scalar());
}

bool
truth(const YAML::Node &node)
{
    if (!node || node.IsNull())
        return false;
    if (!node.IsScalar())
        return node.size() != 0;
    const QString value = string(node);
    if (node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str")
        return !value.isEmpty();
    return !value.isEmpty() && value.compare("false", Qt::CaseInsensitive) != 0
           && value.compare("no", Qt::CaseInsensitive) != 0
           && value.compare("off", Qt::CaseInsensitive) != 0 && value != "0";
}

YAML::Node
mapping(const YAML::Node &node, const QString &description, int depth = 0)
{
    if (!node.IsMap() || depth > 32)
        throw Error("Expected a mapping in " + description);
    YAML::Node    result(YAML::NodeType::Map);
    QSet<QString> keys;
    for (const auto &entry : node)
    {
        const QString key = string(entry.first);
        if (keys.contains(key))
            throw Error("Duplicate YAML key in " + description + ": " + key);
        keys.insert(key);
        if (key != "<<")
            result.force_insert(YAML::Clone(entry.first), YAML::Clone(entry.second));
    }
    const auto inherited = node["<<"];
    if (inherited)
    {
        const auto add = [&](const YAML::Node &source)
        {
            const auto defaults = mapping(source, description, depth + 1);
            for (const auto &entry : defaults)
            {
                const QString key = string(entry.first);
                if (!keys.contains(key))
                {
                    keys.insert(key);
                    result.force_insert(YAML::Clone(entry.first), YAML::Clone(entry.second));
                }
            }
        };
        if (inherited.IsSequence())
            for (const auto &source : inherited)
                add(source);
        else
            add(inherited);
    }
    return result;
}

YAML::Node
parseYaml(const QByteArray &bytes, const QString &path)
{
    try
    {
        return mapping(YAML::Load(bytes.toStdString()), path);
    }
    catch (const YAML::Exception &error)
    {
        throw Error("Cannot read launcher configuration " + path + ": "
                    + QString::fromUtf8(error.what()));
    }
}

YAML::Node
loadYaml(const QString &path, bool optional = false)
{
    if (optional && !QFileInfo::exists(path))
        return YAML::Node(YAML::NodeType::Map);
    return parseYaml(readFile(path), path);
}

QJsonValue
parseJson(const QByteArray &bytes, const QString &path)
{
    QJsonParseError error;
    const auto      document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError)
        throw Error("Cannot read launcher configuration " + path + ": " + error.errorString());
    return document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
}

QJsonObject
object(const QJsonValue &value, const QString &description)
{
    if (!value.isObject())
        throw Error("Expected a mapping in " + description);
    return value.toObject();
}

void
merge(QJsonObject &destination, const QJsonObject &source)
{
    for (auto it = source.begin(); it != source.end(); ++it)
        destination.insert(it.key(), it.value());
}

void
merge(QProcessEnvironment &destination, const QJsonObject &source)
{
    for (auto it = source.begin(); it != source.end(); ++it)
    {
        if (!environmentKey.match(it.key()).hasMatch() || !it.value().isString()
            || it.value().toString().contains(QChar::Null))
            throw Error("Invalid configured launcher environment");
        destination.insert(it.key(), it.value().toString());
    }
}

QString
expand(QString text)
{
    if (text == "~" || text.startsWith("~/"))
        text.replace(0, 1, homeDirectory());
    static const QRegularExpression variables(
            QStringLiteral("\\$(?:\\{([A-Za-z_][A-Za-z0-9_]*)\\}|([A-Za-z_][A-Za-z0-9_]*))"));
    const auto                     environment = QProcessEnvironment::systemEnvironment();
    auto                           matches     = variables.globalMatch(text);
    QList<QRegularExpressionMatch> replacements;
    while (matches.hasNext())
        replacements.append(matches.next());
    for (auto it = replacements.crbegin(); it != replacements.crend(); ++it)
    {
        const QString key = it->captured(1).isEmpty() ? it->captured(2) : it->captured(1);
        if (environment.contains(key))
            text.replace(it->capturedStart(), it->capturedLength(), environment.value(key));
    }
    return text;
}

struct Token
{
    qsizetype start;
    qsizetype end;
    QString   value;
};

QList<Token>
tokens(const QString &text)
{
    QList<Token> result;
    qsizetype    i = 0;
    while (i < text.size())
    {
        while (i < text.size() && QStringLiteral(" \t\r\n").contains(text[i]))
            ++i;
        if (i == text.size())
            break;
        const qsizetype start = i;
        QString         value;
        QChar           quote;
        while (i < text.size())
        {
            const QChar c = text[i];
            if (quote.isNull() && QStringLiteral(" \t\r\n").contains(c))
                break;
            if (c == '\\' && quote != '\'')
            {
                if (++i == text.size())
                    throw Error("Invalid quoted launcher arguments: trailing escape");
                const QChar escaped = text[i++];
                if (quote == '"' && escaped != '"' && escaped != '\\')
                    value += '\\';
                value += escaped;
            }
            else if (quote.isNull() && (c == '\'' || c == '"'))
            {
                quote = c;
                ++i;
            }
            else if (!quote.isNull() && c == quote)
            {
                quote = {};
                ++i;
            }
            else
            {
                value += c;
                ++i;
            }
        }
        if (!quote.isNull())
            throw Error("Invalid quoted launcher arguments: unclosed quote");
        result.append({ start, i, value });
    }
    return result;
}

QJsonObject
assignments(const QString &text)
{
    QJsonObject result;
    for (const auto &token : tokens(expand(text)))
    {
        const auto    separator = token.value.indexOf('=');
        const QString key       = token.value.left(separator);
        if (separator > 0 && environmentKey.match(key).hasMatch())
            result.insert(key, token.value.mid(separator + 1));
    }
    return result;
}

QString
quoteAssignment(const QString &value)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    if (safe.match(value).hasMatch())
        return value;
    QString quoted = value;
    quoted.replace('\'', QStringLiteral("'\"'\"'"));
    return "'" + quoted + "'";
}

QString
patchAssignments(QString text, const QJsonObject &updates)
{
    struct Replacement
    {
        qsizetype start;
        qsizetype end;
        QString   text;
    };
    QList<Replacement> replacements;
    QSet<QString>      found;
    for (const auto &token : tokens(text))
    {
        const auto    separator = token.value.indexOf('=');
        const QString key       = token.value.left(separator);
        if (separator < 1 || !updates.contains(key))
            continue;
        const auto    value       = updates.value(key);
        const QString replacement = !value.isNull() && !found.contains(key)
                                            ? quoteAssignment(key + "=" + value.toString())
                                            : QString();
        qsizetype     start       = token.start;
        if (replacement.isEmpty() && start > 0 && text[start - 1] == ' ')
            --start;
        replacements.append({ start, token.end, replacement });
        found.insert(key);
    }
    for (auto it = replacements.crbegin(); it != replacements.crend(); ++it)
        text.replace(it->start, it->end - it->start, it->text);
    for (auto it = updates.begin(); it != updates.end(); ++it)
        if (!found.contains(it.key()) && !it.value().isNull())
            text += (text.isEmpty() ? "" : " ")
                    + quoteAssignment(it.key() + "=" + it.value().toString());
    return text;
}

QJsonObject
target(const QString &kind, const QString &identity, const QString &name, const QString &prefix,
       const QString &runner, const QJsonObject &metadata, const QString &error = {})
{
    return { { "id", stableId(kind, identity) },
             { "name", name },
             { "kind", kind },
             { "prefix", prefix },
             { "runner", runner },
             { "metadata", metadata },
             { "error", error },
             { "version", "" },
             { "status", error.isEmpty() ? "not-installed" : "unsupported" } };
}

QJsonObject
globals(const QString &path)
{
    const auto value = readJson(path, QJsonArray());
    if (!value.isArray())
        throw Error("Expected an environment assignment array in " + path);
    QJsonObject result;
    for (const auto &row : value.toArray())
    {
        if (!row.isString())
            throw Error("Expected an environment assignment array in " + path);
        const QString text      = row.toString().trimmed();
        const auto    separator = text.indexOf('=');
        if (separator >= 0)
            result.insert(text.left(separator).trimmed(), text.mid(separator + 1).trimmed());
    }
    return result;
}

bool
executable(const QString &path)
{
    return QFileInfo(path).isFile() && ::access(QFile::encodeName(path).constData(), X_OK) == 0;
}

bool
beneath(const QString &path, const QString &root)
{
    return path == root || path.startsWith(root + '/');
}

QString
faugusRunner(QString name)
{
    QStringList roots;
    if (name == "Proton-CachyOS (System)")
    {
        for (const auto &path :
             qEnvironmentVariable("XDG_DATA_DIRS", "/usr/local/share:/usr/share").split(':'))
            roots.append(path + "/steam/compatibilitytools.d");
        name = "proton-cachyos-slr";
    }
    else
    {
        roots = { qEnvironmentVariable("HOST_XDG_DATA_HOME", homeDirectory() + "/.local/share")
                          + "/Steam/compatibilitytools.d",
                  homeDirectory()
                          + "/.var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d" };
    }
    if (name.isEmpty())
        name = "UMU-Latest";
    for (const auto &root : roots)
    {
        const QString candidate = QDir::isAbsolutePath(name) ? name : root + '/' + name;
        if (QFileInfo(candidate).isDir() && QFileInfo(candidate + "/proton").isFile())
            return absolutePath(candidate);
    }
    throw Error("Configured Faugus runner is not installed: " + name);
}

QJsonObject
faugusTarget(const QJsonObject &game, const QString &path, const QString &settingsPath,
             const QString &globalPath)
{
    const QString id = game.value("gameid").toString();
    QJsonObject   metadata{ { "config", path },
                            { "settings", settingsPath },
                            { "global_environment", globalPath },
                            { "gameid", id },
                            { "umu", dataDirectory() + "/faugus-launcher/umu-run" } };
    QString       prefix, runner, error;
    try
    {
        const auto      settings = object(readJson(settingsPath, QJsonObject()), settingsPath);
        const QFileInfo program(expand(game.value("path").toString()));
        const QString cwd = program.isAbsolute() && program.dir().exists() ? program.absolutePath()
                                                                           : QString();
        metadata.insert("cwd", cwd);
        QJsonObject configured{ { "WINEPREFIX", expand(game.value("prefix").toString()) } };
        if (!game.value("runner").toString().isEmpty())
            configured.insert("PROTONPATH", game.value("runner"));
        merge(configured, assignments(game.value("launch_arguments").toString()));
        merge(configured, assignments(game.value("game_arguments").toString()));
        if (configured.value("WINEPREFIX").toString().isEmpty())
        {
            configured.insert(
                    "WINEPREFIX",
                    expand(settings.value("default-prefix").toString(homeDirectory() + "/Faugus"))
                            + "/default");
            configured.insert("PROTONPATH",
                              settings.value("default-runner").toString("Proton-CachyOS Latest"));
        }
        merge(configured, globals(globalPath));
        prefix = absolutePath(configured.value("WINEPREFIX").toString(), cwd);
        runner = faugusRunner(configured.value("PROTONPATH").toString());
        if (!executable(metadata.value("umu").toString()))
            throw Error("Faugus's bundled umu-run is not installed or executable");
        metadata.insert("umu", absolutePath(metadata.value("umu").toString()));
        error = prefixError(prefix);
    }
    catch (const Error &exception)
    {
        error = QString::fromUtf8(exception.what());
    }
    return target("faugus", path + ':' + id, game.value("title").toString(id), prefix, runner,
                  metadata, error);
}

QJsonArray
faugusTargets()
{
    const QString config = configDirectory() + "/faugus-launcher";
    QString       path   = dataDirectory() + "/faugus-launcher/games.json";
    if (!QFileInfo::exists(path))
        path = config + "/games.json";
    if (!QFileInfo::exists(path))
        return {};
    const auto games = readJson(path);
    if (!games.isArray())
        throw Error("Expected an application array in " + path);
    QJsonArray    result;
    QSet<QString> seen;
    for (const auto &value : games.toArray())
    {
        if (!value.isObject())
        {
            result.append(target("faugus", path + ":invalid:" + QString::number(result.size()),
                                 "Invalid Faugus entry", "", "", {},
                                 "Expected an application mapping in " + path));
            continue;
        }
        const auto game = value.toObject();
        if (game.value("runner") == "Linux-Native" || game.value("runner") == "Steam")
            continue;
        auto current = faugusTarget(game, path, config + "/config.json", config + "/envar.json");
        const QString id = game.value("gameid").toString();
        if (id.isEmpty() || seen.contains(id))
        {
            current.insert("status", "unsupported");
            current.insert("error", "Missing or duplicate Faugus game identity");
            for (qsizetype i = 0; i < result.size(); ++i)
                if (result[i].toObject().value("id") == current.value("id"))
                {
                    auto previous = result[i].toObject();
                    previous.insert("status", "unsupported");
                    previous.insert("error", current.value("error"));
                    result[i] = previous;
                }
        }
        seen.insert(id);
        result.append(current);
    }
    return result;
}

CommandResult
flatpak(const QStringList &arguments)
{
    return execute("flatpak", arguments, cleanEnvironment(), {}, 20000);
}

QStringList
options(const QJsonObject &metadata)
{
    QStringList result;
    for (const auto &value : metadata.value("flatpak_options").toArray())
        result.append(value.toString());
    return result;
}

QJsonObject
flatpakContext()
{
    const auto result = flatpak({ "info", "--show-location", app });
    if (result.exitCode != 0)
        throw Error("Bottles Flatpak is not installed: " + result.error.trimmed());
    const QString deployment = QDir::cleanPath(result.output.trimmed());
    QString       option;
    if (beneath(deployment, dataDirectory() + "/flatpak"))
        option = "--user";
    else if (beneath(deployment, "/var/lib/flatpak"))
        option = "--system";
    else
        throw Error("Bottles is in a named Flatpak installation. Select a standard user or system "
                    "installation.");
    return { { "flatpak_options", QJsonArray{ option } },
             { "deployment", deployment },
             { "app_id", app } };
}

QString
access(const QJsonObject &metadata, const QString &path)
{
    QStringList arguments{ "info" };
    arguments.append(options(metadata));
    arguments.append({ "--file-access=" + path, app });
    const auto result = flatpak(arguments);
    if (result.exitCode != 0)
        throw Error("Cannot determine Bottles sandbox filesystem access: "
                    + result.error.trimmed());
    const QString mode = result.output.trimmed();
    if (mode != "hidden" && mode != "read-only" && mode != "read-write")
        throw Error("Unexpected Bottles sandbox filesystem access: " + mode);
    return mode;
}

QString
hostPath(const QString &path, const QJsonObject &metadata)
{
    const QString deployment = metadata.value("deployment").toString();
    if (!deployment.isEmpty() && beneath(path, "/app"))
        return deployment + "/files" + path.mid(4);
    if (!deployment.isEmpty() && beneath(path, "/var/data"))
        return homeDirectory() + "/.var/app/" + app + "/data" + path.mid(9);
    return path;
}

QString
bottleRunner(const YAML::Node &config, const QString &base, QJsonObject &metadata)
{
    const QString name = string(config["Runner"]);
    if (name.isEmpty())
        throw Error("The bottle has no configured runner");
    QString candidate;
    if (name.startsWith("sys-"))
    {
        if (metadata.contains("deployment"))
        {
            candidate = "/app/bin/wine";
            if (!QFileInfo(hostPath(candidate, metadata)).isFile())
                throw Error("The configured Flatpak system Wine cannot be resolved without "
                            "launching the sandbox");
        }
        else
        {
            candidate = QStandardPaths::findExecutable("wine",
                                                       cleanEnvironment().value("PATH").split(':'));
            if (candidate.isEmpty())
                throw Error("The bottle's configured system Wine is unavailable");
        }
    }
    else
    {
        QString root = base + "/runners/" + name;
        if (!QFileInfo(hostPath(root, metadata)).isDir())
        {
            root.clear();
            QStringList roots;
            for (const auto &part :
                 { ".var/app/com.valvesoftware.Steam/data/Steam", ".local/share/Steam",
                   ".steam/debian-installation", ".steam/root", ".steam/steam", ".steam" })
                roots.append(homeDirectory() + '/' + part + "/compatibilitytools.d");
            roots.append({ "/app/share/steam/compatibilitytools.d",
                           "/usr/share/steam/compatibilitytools.d" });
            for (const auto &directory : roots)
                if (QFileInfo(hostPath(directory + '/' + name + "/toolmanifest.vdf", metadata))
                            .isFile())
                {
                    root = directory + '/' + name;
                    break;
                }
        }
        if (root.isEmpty())
            throw Error("Configured Bottles runner is not installed: " + name);
        const QString                   manifest = hostPath(root + "/toolmanifest.vdf", metadata);
        static const QRegularExpression proton(
                QStringLiteral(
                        "\"(?:compatmanager_layer_name|commandline)\"\\s*\"[^\"]*proton[^\"]*\""),
                QRegularExpression::CaseInsensitiveOption);
        if (QFileInfo(manifest).isFile()
            && proton.match(QString::fromUtf8(readFile(manifest))).hasMatch())
            for (const auto &part : { "dist", "files" })
                if (QFileInfo(hostPath(root + '/' + part, metadata)).isDir())
                {
                    root += '/' + QString::fromLatin1(part);
                    break;
                }
        metadata.insert("runner_root", root);
        candidate = root + "/bin/wine";
    }
    if (string(config["Arch"], "win64") == "win64"
        && QFileInfo(hostPath(candidate + "64", metadata)).isFile())
        candidate += "64";
    if (!executable(hostPath(candidate, metadata)))
        throw Error("Configured Bottles Wine is not executable: " + candidate);
    metadata.insert("runner_identity", absolutePath(hostPath(candidate, metadata)));
    return candidate;
}

QJsonObject
bottleTarget(const QString &path, const QString &root, const QString &base, const QString &kind,
             const QJsonObject &context)
{
    auto metadata = context;
    metadata.insert("config", path);
    metadata.insert("base", base);
    metadata.insert("bottles_root", root);
    QString prefix, runner, error, name = QFileInfo(path).dir().dirName();
    try
    {
        const auto config = loadYaml(path);
        name              = string(config["Name"], name);
        if (string(config["Environment"]) == "Steam")
            throw Error("Bottles Steam compatibility prefixes must be managed by their Steam "
                        "runner, not as ordinary bottles");
        const QString value = string(config["Path"]);
        if (value.isEmpty())
            throw Error("The bottle has no configured prefix path");
        prefix            = absolutePath(hostPath(
                absolutePath(value, truth(config["Custom_Path"]) ? QString() : root), metadata));
        runner            = bottleRunner(config, base, metadata);
        const auto params = config["Parameters"]
                                    ? mapping(config["Parameters"], "Bottles Parameters")
                                    : YAML::Node(YAML::NodeType::Map);
        if (truth(params["sandbox"]))
            throw Error(
                    "Bottles' additional per-bottle sandbox does not expose the direct PipeWire "
                    "socket. Disable that sandbox in Bottles before managing this prefix.");
        if (truth(params["use_steam_runtime"]))
            throw Error("Bottles Steam Linux Runtime execution is not supported for installation. "
                        "Disable use_steam_runtime in Bottles or use a native Bottles runner.");
        if (prefix != absolutePath(QFileInfo(path).absolutePath()))
            throw Error("Bottle configuration Path disagrees with its configuration directory. "
                        "Repair the location in Bottles first.");
        error = prefixError(prefix);
    }
    catch (const Error &exception)
    {
        error = QString::fromUtf8(exception.what());
    }
    return target(kind, absolutePath(path), name, prefix, runner, metadata, error);
}

QJsonArray
bottleTargets(const QString &base, const QString &kind)
{
    if (!QFileInfo(base).isDir())
        return {};
    const auto    context      = kind == "bottles-flatpak" ? flatpakContext() : QJsonObject();
    const QString settingsPath = base + "/data.yml";
    const auto    settings     = loadYaml(settingsPath, true);
    QString       root         = base + "/bottles";
    const QString custom       = string(settings["custom_bottles_path"]);
    if (!custom.isEmpty() && !(custom.contains("/run/user/") && custom.contains("/doc/")))
    {
        const QString candidate = hostPath(absolutePath(custom), context);
        if (QFileInfo(candidate).isDir()
            && ::access(QFile::encodeName(candidate).constData(), W_OK) == 0
            && (kind != "bottles-flatpak" || access(context, candidate) == "read-write"))
            root = candidate;
    }
    QJsonArray    result;
    QSet<QString> seen;
    for (const auto &directory :
         QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
    {
        QString       path        = directory.absoluteFilePath() + "/bottle.yml";
        const QString placeholder = directory.absoluteFilePath() + "/placeholder.yml";
        try
        {
            if (!QFileInfo(path).isFile() && QFileInfo(placeholder).isFile())
            {
                const auto config = loadYaml(placeholder);
                path = hostPath(absolutePath(string(config["Path"])), context) + "/bottle.yml";
                if (!QFileInfo(path).isFile())
                {
                    auto metadata = context;
                    metadata.insert("config", path);
                    result.append(target(kind, directory.absoluteFilePath(), directory.fileName(),
                                         QFileInfo(path).absolutePath(), "", metadata,
                                         "The custom bottle location is unavailable"));
                    continue;
                }
            }
            if (QFileInfo(path).isFile() && !seen.contains(absolutePath(path)))
            {
                seen.insert(absolutePath(path));
                auto current  = bottleTarget(path, root, base, kind, context);
                auto metadata = current.value("metadata").toObject();
                metadata.insert("settings", settingsPath);
                if (QFileInfo(placeholder).isFile())
                    metadata.insert("placeholder", placeholder);
                current.insert("metadata", metadata);
                result.append(current);
            }
        }
        catch (const Error &exception)
        {
            result.append(target(kind, directory.absoluteFilePath(), directory.fileName(),
                                 directory.absoluteFilePath(), "", {},
                                 QString::fromUtf8(exception.what())));
        }
    }
    return result;
}

// Steam's text KeyValues files (.vdf, .acf): quoted keys and values, nested
// blocks and // comments. Steam compares keys case-insensitively.
QJsonObject
parseVdf(const QByteArray &bytes, const QString &path)
{
    enum class Kind
    {
        End,
        Text,
        Open,
        Close
    };
    const QString text = QString::fromUtf8(bytes);
    qsizetype     i    = 0;
    const auto    fail = [&](const QString &reason)
    { throw Error("Cannot read Steam file " + path + ": " + reason); };
    const auto next = [&](QString &value)
    {
        while (i < text.size())
        {
            if (text[i].isSpace())
                ++i;
            else if (text.mid(i, 2) == QLatin1String("//"))
                while (i < text.size() && text[i] != '\n')
                    ++i;
            else
                break;
        }
        if (i == text.size())
            return Kind::End;
        if (text[i] == '{' || text[i] == '}')
            return text[i++] == '{' ? Kind::Open : Kind::Close;
        value.clear();
        if (text[i] != '"')
        {
            while (i < text.size() && !text[i].isSpace()
                   && !QStringLiteral("\"{}").contains(text[i]))
                value += text[i++];
            return Kind::Text;
        }
        for (++i; i < text.size() && text[i] != '"'; ++i)
        {
            if (text[i] != '\\' || i + 1 == text.size())
            {
                value += text[i];
                continue;
            }
            const QChar escaped = text[++i];
            if (escaped == 'n')
                value += '\n';
            else if (escaped == 't')
                value += '\t';
            else if (escaped == '\\' || escaped == '"')
                value += escaped;
            else
                value += QStringLiteral("\\") + escaped;
        }
        if (i == text.size())
            fail("unterminated string");
        ++i;
        return Kind::Text;
    };
    std::function<QJsonObject(int)> block = [&](int depth)
    {
        QJsonObject result;
        QString     key, value;
        for (;;)
        {
            const Kind kind = next(key);
            if (kind == Kind::End)
            {
                if (depth > 0)
                    fail("unterminated block");
                return result;
            }
            if (kind == Kind::Close)
            {
                if (depth == 0)
                    fail("unbalanced brace");
                return result;
            }
            if (kind == Kind::Open)
                fail("block without a key");
            const Kind content = next(value);
            if (content == Kind::Open)
            {
                if (depth == 32)
                    fail("nested too deeply");
                result.insert(key, block(depth + 1));
            }
            else if (content == Kind::Text)
                result.insert(key, value);
            else
                fail("key without a value");
        }
    };
    return block(0);
}

QJsonObject
loadVdf(const QString &path, qint64 limit = 2 * 1024 * 1024)
{
    return parseVdf(readFile(path, limit), path);
}

QJsonValue
vdfValue(QJsonValue value, const QStringList &keys)
{
    for (const auto &key : keys)
    {
        const auto object = value.toObject();
        value             = QJsonValue(QJsonValue::Undefined);
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.key().compare(key, Qt::CaseInsensitive) == 0)
            {
                value = it.value();
                break;
            }
    }
    return value;
}

QStringList
steamRoots()
{
    QStringList result;
    for (const QString &candidate :
         { dataDirectory() + "/Steam", homeDirectory() + "/.local/share/Steam",
           homeDirectory() + "/.steam/steam", homeDirectory() + "/.steam/root",
           homeDirectory() + "/.steam/debian-installation" })
    {
        const QString root = QFileInfo(candidate).canonicalFilePath();
        if (!root.isEmpty() && QFileInfo(root + "/steamapps").isDir() && !result.contains(root))
            result.append(root);
    }
    return result;
}

QStringList
steamLibraries(const QString &root)
{
    QStringList result{ root };
    QJsonObject folders;
    try
    {
        folders = vdfValue(loadVdf(root + "/steamapps/libraryfolders.vdf"), { "libraryfolders" })
                          .toObject();
    }
    catch (const Error &)
    {
    }
    for (auto it = folders.begin(); it != folders.end(); ++it)
    {
        bool numeric = false;
        it.key().toUInt(&numeric);
        const QString path    = it.value().isObject() ? vdfValue(it.value(), { "path" }).toString()
                                                      : it.value().toString();
        const QString library = QFileInfo(path).canonicalFilePath();
        if (numeric && !library.isEmpty() && QFileInfo(library + "/steamapps").isDir()
            && !result.contains(library))
            result.append(library);
    }
    return result;
}

// Custom compatibility tools declare their Steam names in
// compatibilitytool.vdf. The first directory Steam searches wins.
QHash<QString, QString>
customSteamTools(const QString &root)
{
    QStringList directories{ root + "/compatibilitytools.d" };
    directories += qEnvironmentVariable("STEAM_EXTRA_COMPAT_TOOLS_PATHS")
                           .split(':', Qt::SkipEmptyParts);
    for (const auto &path : qEnvironmentVariable("XDG_DATA_DIRS", "/usr/local/share:/usr/share")
                                    .split(':', Qt::SkipEmptyParts))
        directories << path + "/steam/compatibilitytools.d";
    directories << "/usr/local/share/steam/compatibilitytools.d"
                << "/usr/share/steam/compatibilitytools.d";
    QHash<QString, QString> result;
    for (const auto &directory : directories)
        for (const auto &tool :
             QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        {
            QJsonObject declared;
            try
            {
                declared = vdfValue(loadVdf(tool.absoluteFilePath() + "/compatibilitytool.vdf"),
                                    { "compatibilitytools", "compat_tools" })
                                   .toObject();
            }
            catch (const Error &)
            {
                continue;
            }
            for (auto it = declared.begin(); it != declared.end(); ++it)
            {
                const QString path = QDir::cleanPath(
                        QDir(tool.absoluteFilePath())
                                .absoluteFilePath(
                                        vdfValue(it.value(), { "install_path" }).toString(".")));
                if (!result.contains(it.key()) && QFileInfo(path + "/proton").isFile())
                    result.insert(it.key(), path);
            }
        }
    return result;
}

// Valve's Protons are Steam apps: proton_9 is the app named "Proton 9.0",
// proton_experimental the one named "Proton Experimental".
QString
protonAppName(QString name)
{
    name = name.trimmed().toLower();
    if (name.endsWith(" (beta)"))
        name.chop(7);
    return name;
}

QString
valveToolAppName(const QString &tool)
{
    static const QRegularExpression numbered(QStringLiteral("^proton_(\\d+)$"));
    const auto                      match = numbered.match(tool);
    if (match.hasMatch())
        return "proton " + match.captured(1) + ".0";
    return tool.startsWith("proton_") ? "proton " + tool.mid(7) : QString();
}

QString
steamRunner(const QString &tool, const QHash<QString, QString> &custom,
            const QHash<QString, QString> &valve, const QString &compatdata)
{
    if (!tool.isEmpty())
    {
        if (custom.contains(tool))
            return absolutePath(custom.value(tool));
        const QString name = valveToolAppName(tool);
        if (!name.isEmpty() && valve.contains(name))
            return absolutePath(valve.value(name));
        throw Error("The Proton selected for this game in Steam is not installed: " + tool
                    + ". Choose an installed one in the game's Properties > Compatibility, or "
                      "launch the game once so Steam downloads it, then refresh.");
    }
    // Without a per-game choice Steam picks the Proton itself, and the prefix
    // records the one it last ran: its fonts directory is config_info's second line.
    static const QRegularExpression fonts(QStringLiteral("^(/.+)/(?:files|dist)/share/fonts/?$"));
    QString                         line;
    try
    {
        line = QString::fromUtf8(readFile(compatdata + "/config_info", 65536))
                       .split('\n')
                       .value(1)
                       .trimmed();
    }
    catch (const Error &)
    {
    }
    const auto match = fonts.match(line);
    if (match.hasMatch() && QFileInfo(match.captured(1) + "/proton").isFile())
        return absolutePath(match.captured(1));
    throw Error("Cannot tell which Proton runs this game. Choose one in the game's Properties > "
                "Compatibility, launch the game once, then refresh.");
}

// The launch options live in the settings of the account that signed in last.
QString
steamLocalConfig(const QString &root)
{
    constexpr quint64 accountBase = 76561197960265728ULL;
    QString           account;
    try
    {
        const auto users
                = vdfValue(loadVdf(root + "/config/loginusers.vdf"), { "users" }).toObject();
        for (auto it = users.begin(); it != users.end(); ++it)
        {
            bool       numeric = false;
            const auto id      = it.key().toULongLong(&numeric);
            if (numeric && id > accountBase
                && vdfValue(it.value(), { "MostRecent" }).toString() == "1")
                account = QString::number(id - accountBase);
        }
    }
    catch (const Error &)
    {
    }
    if (account.isEmpty())
    {
        QStringList accounts;
        for (const auto &user :
             QDir(root + "/userdata").entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        {
            bool numeric = false;
            if (user.toULongLong(&numeric) > 0 && numeric)
                accounts << user;
        }
        if (accounts.size() == 1)
            account = accounts.first();
    }
    const QString path = root + "/userdata/" + account + "/config/localconfig.vdf";
    return !account.isEmpty() && QFileInfo(path).isFile() ? path : QString();
}

QString
steamUmu()
{
    const QString path = QStandardPaths::findExecutable(
            "umu-run", cleanEnvironment().value("PATH").split(':', Qt::SkipEmptyParts));
    if (!path.isEmpty())
        return absolutePath(path);
    const QString bundled = dataDirectory() + "/faugus-launcher/umu-run";
    return executable(bundled) ? absolutePath(bundled) : QString();
}

// umu-run downloads the Steam Runtime a Proton requires on first use.
bool
umuRuntimeReady(const QString &runner)
{
    QString appid;
    try
    {
        appid = vdfValue(loadVdf(runner + "/toolmanifest.vdf"),
                         { "manifest", "require_tool_appid" })
                        .toString();
    }
    catch (const Error &)
    {
        return true;
    }
    const QString name = appid == "4183110" ? "steamrt4" : appid == "1628350" ? "steamrt3" : "";
    const QString root = dataDirectory() + "/umu/" + name;
    return name.isEmpty() || QFileInfo::exists(root + "/.installed.ok")
           || QFileInfo::exists(root + "/VERSIONS.txt");
}

// Games that ran through Proton, plus those set to a Proton that have not run
// yet. Native Linux games and Steam's own tools have no prefix and no mapping.
QJsonArray
steamTargets()
{
    QJsonArray    result;
    QSet<QString> seen;
    const QString umu = steamUmu();
    for (const auto &root : steamRoots())
    {
        const QString config = root + "/config/config.vdf";
        const auto    mapping
                = QFileInfo::exists(config)
                          ? vdfValue(loadVdf(config), { "InstallConfigStore", "Software", "Valve",
                                                        "Steam", "CompatToolMapping" })
                                    .toObject()
                          : QJsonObject();
        const QString localConfig = steamLocalConfig(root);
        bool          known       = false;
        QJsonObject   launch;
        if (!localConfig.isEmpty())
            try
            {
                launch = vdfValue(loadVdf(localConfig, 64 * 1024 * 1024),
                                  { "UserLocalConfigStore", "Software", "Valve", "Steam", "apps" })
                                 .toObject();
                known  = true;
            }
            catch (const Error &)
            {
            }
        const auto                         custom = customSteamTools(root);
        QHash<QString, QString>            valve;
        QList<QPair<QString, QJsonObject>> games;
        for (const auto &library : steamLibraries(root))
        {
            if (seen.contains(library))
                continue;
            seen.insert(library);
            for (const auto &file :
                 QDir(library + "/steamapps")
                         .entryInfoList({ "appmanifest_*.acf" }, QDir::Files, QDir::Name))
            {
                QJsonObject state;
                try
                {
                    state = vdfValue(loadVdf(file.absoluteFilePath()), { "AppState" }).toObject();
                }
                catch (const Error &exception)
                {
                    result.append(target("steam", file.absoluteFilePath(), file.fileName(), "", "",
                                         {}, QString::fromUtf8(exception.what())));
                    continue;
                }
                const QString installed = vdfValue(state, { "installdir" }).toString();
                const QString directory = library + "/steamapps/common/" + installed;
                if (!installed.isEmpty() && QFileInfo(directory + "/toolmanifest.vdf").isFile())
                {
                    if (QFileInfo(directory + "/proton").isFile())
                        valve.insert(protonAppName(vdfValue(state, { "name" }).toString()),
                                     directory);
                    continue;
                }
                games.append({ library, state });
            }
        }
        for (const auto &[library, state] : games)
        {
            const QString appid   = vdfValue(state, { "appid" }).toString();
            bool          numeric = false;
            appid.toUInt(&numeric);
            if (!numeric)
                continue;
            QString compatdata = library + "/steamapps/compatdata/" + appid;
            if (!QFileInfo(compatdata + "/pfx").isDir()
                && QFileInfo(root + "/steamapps/compatdata/" + appid + "/pfx").isDir())
                compatdata = root + "/steamapps/compatdata/" + appid;
            const bool    launched = QFileInfo(compatdata + "/pfx").isDir();
            const QString tool     = vdfValue(mapping, { appid, "name" }).toString();
            if (!launched && tool.isEmpty())
                continue;
            QJsonObject metadata{ { "steam_root", root }, { "library", library },
                                  { "appid", appid },     { "compatdata", compatdata },
                                  { "tool", tool },       { "umu", umu } };
            if (known)
                metadata.insert("launch_options",
                                vdfValue(launch, { appid, "LaunchOptions" }).toString());
            QString prefix, runner, error;
            try
            {
                if (!launched)
                    throw Error("Launch this game once in Steam so Proton creates its prefix, "
                                "then refresh.");
                prefix = compatdata + "/pfx";
                runner = steamRunner(tool, custom, valve, compatdata);
                if (umu.isEmpty())
                    throw Error("Steam games are set up through umu-run. Install the "
                                "umu-launcher package, or Faugus, which includes it.");
                error = prefixError(prefix);
            }
            catch (const Error &exception)
            {
                error = QString::fromUtf8(exception.what());
            }
            metadata.insert("runtime_ready", runner.isEmpty() || umuRuntimeReady(runner));
            auto    current  = target("steam", root + ':' + appid,
                                      vdfValue(state, { "name" }).toString(appid), prefix, runner,
                                      metadata, error);
            QString filename = current.value("id").toString();
            filename.replace(':', '-');
            metadata.insert("environment_file",
                            managerDirectory() + "/steam-environments/" + filename + ".json");
            current.insert("metadata", metadata);
            result.append(current);
        }
    }
    const QString sandboxed = homeDirectory() + "/.var/app/com.valvesoftware.Steam/data/Steam";
    if (QFileInfo(sandboxed + "/steamapps").isDir())
        result.append(target("steam", sandboxed, "Steam (Flatpak)", "", "", {},
                             "Flatpak Steam is not supported: its game prefixes sit where the "
                             "Steam sandbox can write, like Flatpak Bottles (issue #36)."));
    return result;
}

bool
managedMarker(const QString &prefix)
{
    const QDir parent      = QFileInfo(prefix).dir();
    const QDir grandparent = QFileInfo(parent.absolutePath()).dir();
    return QFileInfo::exists(prefix + "/bottle.yml") || QFileInfo::exists(prefix + "/tracked_files")
           || QFileInfo::exists(parent.filePath("tracked_files"))
           || QFileInfo::exists(prefix + "/compatdata")
           || QFileInfo::exists(parent.filePath("config_info")) || parent.dirName() == "compatdata"
           || grandparent.dirName() == "compatdata";
}

QJsonObject
fresh(const QJsonObject &selected)
{
    if (selected.value("kind") == "wine")
    {
        const auto current = manualTarget(selected.value("prefix").toString(),
                                          selected.value("runner").toString(),
                                          selected.value("name").toString());
        if (current.value("prefix") != selected.value("prefix")
            || current.value("runner") != selected.value("runner")
            || current.value("id") != selected.value("id"))
            throw Error("Manual prefix or runner changed. Refresh the target before continuing.");
        return current;
    }
    for (const auto &value : discover())
    {
        const auto current = value.toObject();
        if (current.value("id") != selected.value("id"))
            continue;
        const auto metadata = current.value("metadata").toObject();
        const auto previous = selected.value("metadata").toObject();
        if (current.value("prefix") != selected.value("prefix")
            || current.value("runner") != selected.value("runner")
            || metadata.value("runner_identity") != previous.value("runner_identity")
            || metadata.value("umu") != previous.value("umu")
            || metadata.value("deployment") != previous.value("deployment"))
            throw Error("Launcher prefix or runner changed. Refresh the target before continuing.");
        if (!current.value("error").toString().isEmpty())
            throw Error(current.value("error").toString());
        return current;
    }
    throw Error("The launcher entry was removed. Refresh the target before continuing.");
}

void
quiescent(const QJsonObject &target)
{
    const QString kind = target.value("kind").toString();
    const QString name = kind == "faugus" ? "faugus" : kind.startsWith("bottles") ? "bottles" : "";
    if (name.isEmpty())
        return;
    for (const auto &process : QDir("/proc").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        bool       numeric = false;
        const auto pid     = process.fileName().toLongLong(&numeric);
        if (!numeric || pid == ::getpid() || process.ownerId() != ::getuid())
            continue;
        QFile file(process.absoluteFilePath() + "/cmdline");
        if (!file.open(QIODevice::ReadOnly))
        {
            if (!QFileInfo::exists(process.absoluteFilePath()))
                continue;
            throw Error("Cannot establish that launcher writers are stopped: process inspection "
                        "was denied");
        }
        const auto command = file.read(65536).split('\0');
        for (qsizetype i = 0; i < command.size() && i < 3; ++i)
            if (QFileInfo(QFile::decodeName(command[i]))
                        .fileName()
                        .contains(name, Qt::CaseInsensitive))
                throw Error("Close the launcher, its tray process and running applications before "
                            "modifying this prefix.");
    }
}

struct Snapshot
{
    bool        exists = false;
    QByteArray  bytes;
    struct stat status{};
};

Snapshot
snapshot(const QString &path)
{
    Snapshot result;
    if (::lstat(QFile::encodeName(path).constData(), &result.status) != 0)
    {
        if (errno == ENOENT)
            return result;
        throw Error("Cannot inspect launcher configuration " + path + ": "
                    + QString::fromLocal8Bit(std::strerror(errno)));
    }
    result.exists = true;
    result.bytes  = readFile(path);
    return result;
}

bool
sameSnapshot(const Snapshot &left, const Snapshot &right)
{
    return left.exists == right.exists
           && (!left.exists
               || (left.bytes == right.bytes && left.status.st_dev == right.status.st_dev
                   && left.status.st_ino == right.status.st_ino
                   && left.status.st_mode == right.status.st_mode));
}

void
atomicUpdate(const QString &path, const QJsonObject &target,
             const std::function<QByteArray(const QByteArray &)> &change)
{
    const QString locks = managerDirectory() + "/locks";
    if (!QDir().mkpath(locks) || !QDir().mkpath(QFileInfo(path).absolutePath()))
        throw Error("Cannot create launcher configuration directory");
    const QString lockPath
            = locks + '/'
              + QString::fromLatin1(
                      QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex())
              + ".lock";
    QFile lock(lockPath);
    if (!lock.open(QIODevice::ReadWrite) || ::flock(lock.handle(), LOCK_EX) != 0)
        throw Error("Cannot lock launcher configuration " + path);
    quiescent(target);
    QMap<QString, Snapshot> dependencies;
    const auto              metadata = target.value("metadata").toObject();
    for (const auto &key : { "config", "settings", "global_environment", "placeholder" })
        if (!metadata.value(key).toString().isEmpty())
            dependencies.insert(metadata.value(key).toString(),
                                snapshot(metadata.value(key).toString()));
    fresh(target);
    const auto before = snapshot(path);
    if (before.exists && S_ISLNK(before.status.st_mode))
        throw Error("Refusing to replace symlinked launcher configuration: " + path);
    const QByteArray after = change(before.bytes);
    if (before.exists && after == before.bytes)
        return;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        throw Error("Cannot stage launcher configuration " + path + ": " + file.errorString());
    if (::fchmod(file.handle(), before.exists ? before.status.st_mode & 0777 : 0600) != 0
        || file.write(after) != after.size() || !file.flush())
        throw Error("Cannot write launcher configuration " + path);
    quiescent(target);
    fresh(target);
    if (!sameSnapshot(before, snapshot(path)))
        throw Error("Launcher configuration changed concurrently: " + path);
    for (auto it = dependencies.cbegin(); it != dependencies.cend(); ++it)
        if (!sameSnapshot(it.value(), snapshot(it.key())))
            throw Error("Launcher configuration changed concurrently: " + it.key());
    if (!file.commit())
        throw Error("Cannot replace launcher configuration " + path + ": " + file.errorString());
    const int directory = ::open(QFile::encodeName(QFileInfo(path).absolutePath()).constData(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0)
    {
        const int result = ::fsync(directory);
        ::close(directory);
        if (result != 0)
            throw Error("Cannot sync launcher configuration directory "
                        + QFileInfo(path).absolutePath());
    }
}

QJsonObject
yamlEnvironment(const YAML::Node &config)
{
    QJsonObject result;
    if (!config["Environment_Variables"])
        return result;
    const auto values = mapping(config["Environment_Variables"], "Environment_Variables");
    static const QRegularExpression typed(
            QStringLiteral("^(?:true|false|yes|no|on|off|~|null|[-+]?(?:[0-9][0-9_]*(?:\\.[0-9_]*)?"
                           "(?:e[-+]?[0-9]+)?|\\.[0-9]+|0x[0-9a-f]+|\\.inf|\\.nan))$"),
            QRegularExpression::CaseInsensitiveOption);
    for (const auto &entry : values)
    {
        const auto &value = entry.second;
        if (!entry.first.IsScalar() || !value.IsScalar())
            continue;
        const QString text = string(value);
        if (value.Tag() == "!" || value.Tag() == "tag:yaml.org,2002:str"
            || !typed.match(text).hasMatch())
            result.insert(string(entry.first), text);
    }
    return result;
}

QJsonObject
bottleRunEnvironment(const QJsonObject &target, const YAML::Node &config)
{
    const auto  metadata = target.value("metadata").toObject();
    const auto  params = config["Parameters"] ? mapping(config["Parameters"], "Bottles Parameters")
                                              : YAML::Node(YAML::NodeType::Map);
    QJsonObject result{ { "WINEPREFIX", target.value("prefix") },
                        { "WINEARCH", string(config["Arch"], "win64") },
                        { "BOTTLE", string(config["Path"]) } };
    QStringList overrides;
    if (config["DLL_Overrides"])
        for (const auto &entry : mapping(config["DLL_Overrides"], "DLL_Overrides"))
            overrides.append(string(entry.first) + '=' + string(entry.second));
    overrides.append("winemenubuilder.exe=d");
    result.insert("WINEDLLOVERRIDES", overrides.join(';'));
    const QString sync = string(params["sync"], "wine");
    if (sync == "esync" || sync == "fsync" || sync == "ntsync")
        result.insert(sync == "esync"   ? "WINEESYNC"
                      : sync == "fsync" ? "WINEFSYNC"
                                        : "WINENTSYNC",
                      "1");
    const QString root
            = metadata.value("runner_root")
                      .toString(
                              QFileInfo(QFileInfo(target.value("runner").toString()).absolutePath())
                                      .absolutePath());
    QStringList libraries;
    if (truth(params["use_runtime"]))
    {
        QStringList runtimes;
        if (metadata.contains("deployment"))
            runtimes.append("/app/etc/runtime");
        runtimes.append(metadata.value("base").toString() + "/runtimes");
        QString runtime;
        for (const auto &candidate : runtimes)
            if (QFileInfo(hostPath(candidate + "/lib", metadata)).isDir()
                && QFileInfo(hostPath(candidate + "/lib32", metadata)).isDir())
            {
                runtime = candidate;
                break;
            }
        if (runtime.isEmpty())
            throw Error("The configured Bottles runtime is missing");
        libraries.append({ runtime + "/lib", runtime + "/lib32" });
    }
    QStringList parts{ "lib64", "lib" };
    for (const QString &wine : { layout().unixDirectory, QStringLiteral("i386-unix") })
        for (const QString &base :
             { QStringLiteral("lib"), QStringLiteral("lib32"), QStringLiteral("lib64") })
            parts.append(base + "/wine/" + wine);
    for (const auto &part : parts)
        if (QFileInfo(hostPath(root + '/' + part, metadata)).isDir())
            libraries.append(root + '/' + part);
    if (metadata.contains("deployment"))
        libraries.append({ "/app/lib", "/app/lib/i386-linux-gnu" });
    if (!libraries.isEmpty())
        result.insert("LD_LIBRARY_PATH", libraries.join(':'));
    return result;
}
}

QJsonArray
discover()
{
    QJsonArray                           result;
    const QList<QPair<QString, QString>> sources{
        { "faugus", dataDirectory() + "/faugus-launcher" },
        { "bottles", dataDirectory() + "/bottles" },
        { "bottles-flatpak", homeDirectory() + "/.var/app/" + app + "/data/bottles" },
        { "steam", dataDirectory() + "/Steam" }
    };
    for (const auto &source : sources)
    {
        try
        {
            const auto targets = source.first == "faugus" ? faugusTargets()
                                 : source.first == "steam"
                                         ? steamTargets()
                                         : bottleTargets(source.second, source.first);
            for (const auto &current : targets)
                result.append(current);
        }
        catch (const std::exception &exception)
        {
            result.append(target(source.first, source.second,
                                 source.first + " configuration unavailable", source.second, "", {},
                                 QString::fromUtf8(exception.what())));
        }
    }
    return result;
}

QJsonObject
manualTarget(const QString &prefix, const QString &wine, const QString &name)
{
    const QString path  = absolutePath(prefix);
    const QString error = prefixError(path);
    if (!error.isEmpty())
        throw Error(error);
    for (const auto &value : discover())
    {
        const auto current = value.toObject();
        if (!current.value("prefix").toString().isEmpty()
            && absolutePath(current.value("prefix").toString()) == path)
            throw Error("This prefix belongs to " + current.value("kind").toString()
                        + ". Select its discovered entry instead.");
    }
    if (managedMarker(path))
        throw Error("This appears to be a launcher-managed prefix. Its owning runner must be "
                    "discovered first.");
    const QString runner = absolutePath(wine);
    if (!executable(runner))
        throw Error("Select an existing, executable Wine binary by absolute path");
    auto    result = target("wine", path, name.isEmpty() ? QFileInfo(path).fileName() : name, path,
                            runner, {});
    QString filename = result.value("id").toString();
    filename.replace(':', '-');
    result.insert("metadata",
                  QJsonObject{ { "environment_file", managerDirectory() + "/manual-environments/"
                                                             + filename + ".json" } });
    return result;
}

QJsonObject
environment(const QJsonObject &selected)
{
    const auto target   = fresh(selected);
    const auto metadata = target.value("metadata").toObject();
    if (target.value("kind") == "wine" || target.value("kind") == "steam")
        return object(readJson(metadata.value("environment_file").toString(), QJsonObject()),
                      target.value("kind") == "wine" ? "manual environment" : "Steam environment");
    if (target.value("kind") != "faugus")
        return yamlEnvironment(loadYaml(metadata.value("config").toString()));
    for (const auto &value : readJson(metadata.value("config").toString()).toArray())
    {
        const auto game = value.toObject();
        if (game.value("gameid") != metadata.value("gameid"))
            continue;
        auto result = assignments(game.value("launch_arguments").toString());
        merge(result, assignments(game.value("game_arguments").toString()));
        merge(result, globals(metadata.value("global_environment").toString()));
        return result;
    }
    throw Error("Faugus application identity changed");
}

void
setEnvironment(const QJsonObject &selected, const QJsonObject &updates)
{
    for (auto it = updates.begin(); it != updates.end(); ++it)
        if (!environmentKey.match(it.key()).hasMatch()
            || (!it.value().isNull()
                && (!it.value().isString() || it.value().toString().contains(QChar::Null))))
            throw Error("Invalid environment update");
    const auto    target   = fresh(selected);
    const auto    metadata = target.value("metadata").toObject();
    const QString kind     = target.value("kind").toString();
    const QString path
            = metadata.value(kind == "wine" || kind == "steam" ? "environment_file" : "config")
                      .toString();
    atomicUpdate(
            path, target,
            [&](const QByteArray &before)
            {
                if (kind == "faugus")
                {
                    const auto parsed = parseJson(before, path);
                    if (!parsed.isArray())
                        throw Error("Expected an application array in " + path);
                    auto      games = parsed.toArray();
                    qsizetype index = -1;
                    for (qsizetype i = 0; i < games.size(); ++i)
                        if (games[i].toObject().value("gameid") == metadata.value("gameid"))
                        {
                            if (index >= 0)
                                throw Error("Faugus application identity changed");
                            index = i;
                        }
                    if (index < 0)
                        throw Error("Faugus application identity changed");
                    const auto global = globals(metadata.value("global_environment").toString());
                    for (auto it = updates.begin(); it != updates.end(); ++it)
                        if (global.contains(it.key()) && global.value(it.key()) != it.value())
                            throw Error("Faugus global environment overrides " + it.key()
                                        + ". Remove that global assignment in Faugus before "
                                          "managing this entry.");
                    auto game = games[index].toObject();
                    game.insert(
                            "launch_arguments",
                            patchAssignments(game.value("launch_arguments").toString(), updates));
                    QJsonObject   removals;
                    const QString arguments  = game.value("game_arguments").toString();
                    const auto    configured = assignments(arguments);
                    for (auto it = updates.begin(); it != updates.end(); ++it)
                        if (configured.contains(it.key()))
                            removals.insert(it.key(), QJsonValue::Null);
                    if (!removals.isEmpty())
                        game.insert("game_arguments", patchAssignments(arguments, removals));
                    games[index] = game;
                    return QJsonDocument(games).toJson(QJsonDocument::Indented);
                }
                if (kind == "wine" || kind == "steam")
                {
                    auto values = before.isEmpty() ? QJsonObject()
                                                   : object(parseJson(before, path), path);
                    for (auto it = updates.begin(); it != updates.end(); ++it)
                        if (it.value().isNull())
                            values.remove(it.key());
                        else
                            values.insert(it.key(), it.value());
                    return QJsonDocument(values).toJson(QJsonDocument::Indented);
                }
                auto config = parseYaml(before, path);
                auto values = config["Environment_Variables"]
                                      ? mapping(config["Environment_Variables"],
                                                "Environment_Variables")
                                      : YAML::Node(YAML::NodeType::Map);
                for (auto it = updates.begin(); it != updates.end(); ++it)
                    if (it.value().isNull())
                        values.remove(it.key().toStdString());
                    else
                    {
                        YAML::Node value(it.value().toString().toStdString());
                        value.SetTag("tag:yaml.org,2002:str");
                        values[it.key().toStdString()] = value;
                    }
                config["Environment_Variables"] = values;
                QList<YAML::Node> pending{ config };
                QList<YAML::Node> visited;
                while (!pending.isEmpty())
                {
                    auto node = pending.takeLast();
                    if (node.IsScalar())
                    {
                        // yaml-cpp drops the non-specific string tag when emitting plain scalars.
                        if (node.Tag() == "!")
                            node.SetTag("tag:yaml.org,2002:str");
                    }
                    else if ((node.IsMap() || node.IsSequence()) && !visited.contains(node))
                    {
                        visited.append(node);
                        if (node.IsMap())
                            for (const auto &entry : node)
                                pending.append({ entry.first, entry.second });
                        else
                            for (const auto &entry : node)
                                pending.append(entry);
                    }
                }
                YAML::Emitter emitter;
                emitter << config;
                if (!emitter.good())
                    throw Error("Cannot serialize launcher configuration " + path);
                return QByteArray(emitter.c_str(), static_cast<qsizetype>(emitter.size())) + '\n';
            });
}

CommandResult
run(const QJsonObject &selected, const QStringList &arguments, const QJsonObject &updates,
    int timeoutMs)
{
    if (arguments.isEmpty())
        throw Error("Runner arguments must be a nonempty list of strings");
    for (const auto &argument : arguments)
        if (argument.contains(QChar::Null))
            throw Error("Runner arguments cannot contain NUL");
    QProcessEnvironment checked;
    merge(checked, updates);
    const auto target = fresh(selected);
    quiescent(target);
    const auto    metadata   = target.value("metadata").toObject();
    const QString kind       = target.value("kind").toString();
    auto          child      = cleanEnvironment();
    const auto    configured = environment(target);
    QString       program    = target.value("runner").toString();
    QStringList   command    = arguments;
    QString       cwd        = target.value("prefix").toString();
    if (kind == "faugus")
    {
        const auto settings = object(readJson(metadata.value("settings").toString(), QJsonObject()),
                                     "Faugus settings");
        QJsonObject game;
        for (const auto &value : readJson(metadata.value("config").toString()).toArray())
            if (value.toObject().value("gameid") == metadata.value("gameid"))
                game = value.toObject();
        if (!game.value("protonfix").toString().isEmpty())
            child.insert("GAMEID", game.value("protonfix").toString());
        if (game.value("sdl_enabled").toBool())
            child.insert("PROTON_PREFER_SDL", "1");
        const QString components = dataDirectory() + "/faugus-launcher/components";
        child.insert("PROTON_EAC_RUNTIME", components + "/eac");
        child.insert("PROTON_BATTLEYE_RUNTIME", components + "/be");
        child.insert("UMU_USE_STEAM", "1");
        child.insert("UMU_CONTAINER_NSENTER", "1");
        for (const auto &pair :
             QList<QPair<QString, QString>>{ { "wayland-driver", "PROTON_ENABLE_WAYLAND" },
                                             { "wow64-enabled", "PROTON_USE_WOW64" },
                                             { "discrete-gpu", "DRI_PRIME" } })
            if (settings.value(pair.first) == "True")
                child.insert(pair.second, "1");
        merge(child, configured);
        merge(child, updates);
        child.insert("WINEPREFIX", target.value("prefix").toString());
        child.insert("PROTONPATH", target.value("runner").toString());
        child.insert("UMU_RUNTIME_UPDATE", "0");
        child.insert("PROTON_VERB", "waitforexitandrun");
        child.remove("FAUGUSID");
        program = metadata.value("umu").toString();
        if (!metadata.value("cwd").toString().isEmpty())
            cwd = metadata.value("cwd").toString();
    }
    else if (kind.startsWith("bottles"))
    {
        const auto config = loadYaml(metadata.value("config").toString());
        if (truth(config["Limit_System_Environment"]))
        {
            QSet<QString> allowed;
            const auto    inherited = config["Inherited_Environment_Variables"];
            if (inherited && !inherited.IsSequence())
                throw Error("Expected an Inherited_Environment_Variables array");
            if (inherited)
                for (const auto &key : inherited)
                    allowed.insert(string(key));
            for (const auto &key : child.keys())
                if (!allowed.contains(key))
                    child.remove(key);
        }
        auto          runner    = bottleRunEnvironment(target, config);
        const QString libraries = runner.take("LD_LIBRARY_PATH").toString();
        const QString overrides = runner.take("WINEDLLOVERRIDES").toString();
        merge(runner, configured);
        merge(runner, updates);
        QStringList paths{ runner.value("LD_LIBRARY_PATH")
                                   .toString(kind == "bottles" ? child.value("LD_LIBRARY_PATH")
                                                               : QString()),
                           libraries };
        paths.removeAll(QString());
        if (!paths.isEmpty())
            runner.insert("LD_LIBRARY_PATH", paths.join(':'));
        QStringList dlls{ runner.value("WINEDLLOVERRIDES").toString(), overrides };
        dlls.removeAll(QString());
        if (!dlls.isEmpty())
            runner.insert("WINEDLLOVERRIDES", dlls.join(';'));
        runner.insert("WINEPREFIX", target.value("prefix"));
        runner.insert("WINEARCH", string(config["Arch"], "win64"));
        if (kind == "bottles-flatpak")
        {
            QProcessEnvironment validated;
            merge(validated, runner);
            command = { "run" };
            command.append(options(metadata));
            command.append({ "--command=/usr/bin/env", app });
            for (auto it = runner.begin(); it != runner.end(); ++it)
                command.append(it.key() + '=' + it.value().toString());
            command.append(program);
            command.append(arguments);
            program = "flatpak";
        }
        else
            merge(child, runner);
        if (!string(config["WorkingDir"]).isEmpty())
            cwd = hostPath(absolutePath(string(config["WorkingDir"])), metadata);
    }
    else if (kind == "steam")
    {
        merge(child, configured);
        merge(child, updates);
        // As under Steam: the compatdata directory, whose pfx is the Wine prefix.
        child.insert("WINEPREFIX", metadata.value("compatdata").toString());
        child.insert("PROTONPATH", target.value("runner").toString());
        child.insert("GAMEID", "umu-0");
        child.insert("PROTONFIXES_DISABLE", "1");
        child.insert("UMU_RUNTIME_UPDATE", "0");
        child.insert("PROTON_VERB", "waitforexitandrun");
        program = metadata.value("umu").toString();
        // Its first run downloads the Steam Runtime, which outlasts one command.
        if (!umuRuntimeReady(target.value("runner").toString()))
            timeoutMs = std::max(timeoutMs, 30 * 60 * 1000);
    }
    else
    {
        merge(child, configured);
        merge(child, updates);
        child.insert("WINEPREFIX", target.value("prefix").toString());
    }
    fresh(target);
    return execute(program, command, child, cwd, timeoutMs);
}

QJsonObject
launchAssignments(const QString &options)
{
    QJsonObject result;
    for (const auto &token : tokens(expand(options)))
    {
        const auto    separator = token.value.indexOf('=');
        const QString key       = token.value.left(separator);
        if (separator < 1 || !environmentKey.match(key).hasMatch())
            break;
        result.insert(key, token.value.mid(separator + 1));
    }
    return result;
}

QString
launchOptions(const QString &options, const QJsonObject &updates)
{
    // Steam runs the options as a command line with %command% standing for the
    // game. Options without it are arguments Steam appends to the game's.
    QString text = options.trimmed();
    if (!text.contains("%command%"))
        text = text.isEmpty() ? QStringLiteral("%command%") : "%command% " + text;
    qsizetype split = 0;
    for (const auto &token : tokens(text))
    {
        const auto separator = token.value.indexOf('=');
        if (separator < 1 || !environmentKey.match(token.value.left(separator)).hasMatch())
        {
            split = token.start;
            break;
        }
    }
    const QString assignments = patchAssignments(text.left(split).trimmed(), updates).trimmed();
    return assignments.isEmpty() ? text.mid(split) : assignments + ' ' + text.mid(split);
}

QStringList
permissionPlan(const QJsonObject &selected, const QString &payloadRoot)
{
    if (selected.value("kind") != "bottles-flatpak")
        return {};
    const auto  target   = fresh(selected);
    const auto  metadata = target.value("metadata").toObject();
    QStringList grants;
    for (const auto &entry :
         QList<QPair<QString, QString>>{ { target.value("prefix").toString(), "read-write" },
                                         { absolutePath(payloadRoot), "read-only" } })
    {
        static const QRegularExpression invalid(QStringLiteral("[\\n\\r;\\\\:]"));
        if (invalid.match(entry.first).hasMatch())
            throw Error(
                    "The sandbox filesystem path contains unsupported Flatpak keyfile characters");
        const QString mode = access(metadata, entry.first);
        if (mode != "read-write" && (entry.second == "read-write" || mode != "read-only"))
            grants.append(entry.first + (entry.second == "read-only" ? ":ro" : ""));
    }
    const QString remote = environment(target).value("PIPEWIRE_REMOTE").toString("pipewire-0");
    static const QRegularExpression socket(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    if (!socket.match(remote).hasMatch())
        throw Error("Only a named local PipeWire socket is supported for Bottles Flatpak");
    const QString runtime
            = qEnvironmentVariable("XDG_RUNTIME_DIR", "/run/user/" + QString::number(::getuid()));
    const QString mode = access(metadata, runtime + '/' + remote);
    if (mode != "read-only" && mode != "read-write")
        grants.append("xdg-run/" + remote);
    grants.removeDuplicates();
    return grants;
}

QStringList
grantPermissions(const QJsonObject &selected, const QString &payloadRoot)
{
    const auto grants = permissionPlan(selected, payloadRoot);
    if (grants.isEmpty())
        return {};
    const auto    target = fresh(selected);
    const QString path   = dataDirectory() + "/flatpak/overrides/" + app;
    atomicUpdate(
            path, target,
            [&](const QByteArray &before)
            {
                QString     text = QString::fromUtf8(before);
                QStringList lines;
                qsizetype   position = 0;
                while (position < text.size())
                {
                    const auto newline = text.indexOf('\n', position);
                    const auto end     = newline < 0 ? text.size() : newline + 1;
                    lines.append(text.mid(position, end - position));
                    position = end;
                }
                qsizetype start = -1;
                for (qsizetype i = 0; i < lines.size(); ++i)
                    if (lines[i].trimmed() == "[Context]")
                    {
                        if (start >= 0)
                            throw Error(
                                    "Duplicate Context sections in the Bottles Flatpak override");
                        start = i;
                    }
                if (start < 0)
                    return (text + (!text.isEmpty() && !text.endsWith('\n') ? "\n" : "")
                            + "[Context]\nfilesystems=" + grants.join(';') + ";\n")
                            .toUtf8();
                qsizetype end = start + 1;
                while (end < lines.size() && !lines[end].trimmed().startsWith('['))
                    ++end;
                qsizetype entry = -1;
                for (qsizetype i = start + 1; i < end; ++i)
                    if (lines[i].section('=', 0, 0).trimmed() == "filesystems")
                    {
                        if (entry >= 0)
                            throw Error("Duplicate filesystems entries in the Bottles Flatpak "
                                        "override");
                        entry = i;
                    }
                QStringList values;
                if (entry >= 0)
                    values = lines[entry]
                                     .mid(lines[entry].indexOf('=') + 1)
                                     .trimmed()
                                     .split(';', Qt::SkipEmptyParts);
                for (const auto &grant : grants)
                {
                    const QString path = grant.endsWith(":ro") ? grant.chopped(3) : grant;
                    values.removeAll('!' + path);
                    values.removeAll(path + ":ro");
                    values.removeAll(path);
                    values.append(grant);
                }
                const QString replacement = "filesystems=" + values.join(';') + ";\n";
                if (entry >= 0)
                    lines[entry] = replacement;
                else
                {
                    if (end > 0 && !lines[end - 1].endsWith('\n'))
                        lines[end - 1] += '\n';
                    lines.insert(end, replacement);
                }
                return lines.join(QString()).toUtf8();
            });
    const auto remaining = permissionPlan(target, payloadRoot);
    if (!remaining.isEmpty())
        throw Error("Flatpak did not accept the requested filesystem permissions: "
                    + remaining.join(", "));
    return grants;
}
}
