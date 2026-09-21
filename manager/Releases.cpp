/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Manager.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtEndian>
#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/syscall.h>
#include <unistd.h>

namespace PipeASIOManager::Releases
{
namespace
{
const QString    Api      = QStringLiteral("https://api.github.com/repos/M0n7y5/pipeasio/releases");
const QString    Download = QStringLiteral("https://github.com/M0n7y5/pipeasio/releases/download/");
constexpr qint64 MetadataLimit = 2 * 1024 * 1024;
constexpr qint64 ArchiveLimit  = 64 * 1024 * 1024;
constexpr qint64 ExpandedLimit = 256 * 1024 * 1024;
constexpr qint64 FileLimit     = 32 * 1024 * 1024;
constexpr int    Deadline      = 120000;
const QString    Probe         = QStringLiteral("manager/pipeasio-check.exe");
const QString    Probe32       = QStringLiteral("manager/pipeasio-check32.exe");
const QString    BuildInfo     = QStringLiteral("BUILD-INFO.txt");

QString
moduleFile(const QString &directory, const QString &name)
{
    return QStringLiteral("lib/wine/") + directory + '/' + name;
}

QString
dll64(const Layout &host)
{
    return moduleFile(host.peDirectory, QStringLiteral("pipeasio64.dll"));
}

// The second 64-bit front end an ARM64 release may carry; empty elsewhere.
QString
dll64Emulated(const Layout &host)
{
    return host.peEmulated.isEmpty()
                   ? QString()
                   : moduleFile(host.peEmulated, QStringLiteral("pipeasio64.dll"));
}

QString
unixlib64(const Layout &host)
{
    return moduleFile(host.unixDirectory, QStringLiteral("pipeasio64.so"));
}

QString
dll32()
{
    return moduleFile(QStringLiteral("i386-windows"), QStringLiteral("pipeasio32.dll"));
}

QString
unixlib32(const Layout &host)
{
    return moduleFile(host.unixDirectory, QStringLiteral("pipeasio32.so"));
}

struct PayloadBinary
{
    QString name;
    bool    elf;
    quint16 machine;
};

// Every binary a payload for this host may carry, with the machine its header
// must report. The probe is built for the host's own PE architecture.
QList<PayloadBinary>
payloadBinaries(const Layout &host)
{
    QList<PayloadBinary> result{ { dll64(host), false, host.peMachine },
                                 { unixlib64(host), true, host.elfMachine },
                                 { dll32(), false, 0x14c },
                                 { unixlib32(host), true, host.elfMachine },
                                 { Probe, false, host.peMachine },
                                 { Probe32, false, 0x14c } };
    if (!host.peEmulated.isEmpty())
        result.append({ dll64Emulated(host), false, host.peEmulatedMachine });
    return result;
}

void
require(bool condition, const QString &message)
{
    if (!condition)
        throw Error(message);
}

bool
matches(const QString &value, const QString &pattern)
{
    return QRegularExpression(QStringLiteral("\\A(?:") + pattern + QStringLiteral(")\\z"))
            .match(value)
            .hasMatch();
}

bool
validHash(const QJsonValue &value)
{
    return value.isString() && matches(value.toString(), QStringLiteral("[0-9a-fA-F]{64}"));
}

bool
integer(const QJsonValue &value)
{
    return value.isDouble() && std::isfinite(value.toDouble())
           && std::floor(value.toDouble()) == value.toDouble();
}

QString
assetUrl(const QString &version, const QString &name)
{
    require(!version.isEmpty() && version.size() <= 128
                    && matches(version, QStringLiteral("[A-Za-z0-9][A-Za-z0-9._+-]*")),
            QStringLiteral("Invalid release version"));
    require(!name.isEmpty() && name.size() <= 240
                    && matches(name, QStringLiteral("[A-Za-z0-9][A-Za-z0-9._+-]*")),
            QStringLiteral("Invalid release asset name"));
    return Download + QString::fromLatin1(QUrl::toPercentEncoding(version)) + '/'
           + QString::fromLatin1(QUrl::toPercentEncoding(name));
}

QUrl
safeHttps(const QString &text)
{
    for (QChar c : text)
        require(c.unicode() > 32, QStringLiteral("Invalid release URL"));
    const QUrl url(text, QUrl::StrictMode);
    require(url.isValid() && url.scheme() == "https" && url.userInfo().isEmpty()
                    && (url.port() == -1 || url.port() == 443) && !url.hasFragment(),
            QStringLiteral("Release downloads require HTTPS without credentials"));
    return url;
}

bool
storageHost(const QString &host)
{
    return host == "release-assets.githubusercontent.com"
           || host == "objects.githubusercontent.com";
}

QByteArray
download(QNetworkAccessManager &network, const QString &text, qint64 limit, QFile *output = nullptr)
{
    QUrl url = safeHttps(text);
    require((url.host() == "github.com" && text.startsWith(Download))
                    || (url.host() == "api.github.com"
                        && (url.path() == QUrl(Api).path()
                            || url.path().startsWith(QUrl(Api).path() + "/tags/"))),
            QStringLiteral("Only official PipeASIO release URLs are allowed"));
    QElapsedTimer elapsed;
    elapsed.start();
    for (int redirects = 0; redirects <= 5; ++redirects)
    {
        throwIfCancelled();
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setRawHeader("User-Agent", "PipeASIO-Manager");
        request.setRawHeader("Accept", url.host() == "api.github.com"
                                               ? "application/vnd.github+json"
                                               : "application/octet-stream");
        request.setRawHeader("Accept-Encoding", "identity");
        request.setTransferTimeout(30000);
        std::unique_ptr<QNetworkReply> reply(network.get(request));
        reply->setReadBufferSize(65536);
        QEventLoop loop;
        QTimer     deadline;
        deadline.setSingleShot(true);
        QTimer idle;
        idle.setSingleShot(true);
        QTimer             cancellation;
        QString            failure;
        QByteArray         contents;
        QCryptographicHash digest(QCryptographicHash::Sha256);
        qint64             total = 0;
        auto               fail  = [&](const QString &message)
        {
            if (failure.isEmpty())
                failure = message;
            reply->abort();
            loop.quit();
        };
        auto consume = [&]
        {
            if (!failure.isEmpty())
                return;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 300 && status < 400)
                return;
            const QByteArray length      = reply->rawHeader("Content-Length");
            bool             validLength = false;
            const quint64    announced   = length.toULongLong(&validLength);
            if (reply->hasRawHeader("Content-Length")
                && (!validLength || !matches(QString::fromLatin1(length), QStringLiteral("[0-9]+"))
                    || announced > quint64(limit)))
            {
                fail(QStringLiteral("Release download exceeds size limit"));
                return;
            }
            if (reply->hasRawHeader("Content-Encoding")
                && reply->rawHeader("Content-Encoding") != "identity")
            {
                fail(QStringLiteral("Unexpected release content encoding"));
                return;
            }
            while (reply->bytesAvailable() > 0)
            {
                const QByteArray chunk = reply->read(qMin<qint64>(65536, limit - total + 1));
                if (chunk.isEmpty())
                    break;
                total += chunk.size();
                if (total > limit)
                {
                    fail(QStringLiteral("Release download exceeds size limit"));
                    return;
                }
                if (output)
                {
                    if (output->write(chunk) != chunk.size())
                    {
                        fail(QStringLiteral("Cannot write downloaded archive"));
                        return;
                    }
                    digest.addData(chunk);
                }
                else
                    contents.append(chunk);
                idle.start(30000);
            }
        };
        QObject::connect(reply.get(), &QNetworkReply::readyRead, &loop, consume);
        QObject::connect(reply.get(), &QNetworkReply::metaDataChanged, &loop, consume);
        QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&deadline, &QTimer::timeout, &loop,
                         [&] { fail(QStringLiteral("Release download exceeded time limit")); });
        QObject::connect(&idle, &QTimer::timeout, &loop,
                         [&] { fail(QStringLiteral("Release download stalled")); });
        QObject::connect(&cancellation, &QTimer::timeout, &loop,
                         [&]
                         {
                             if (cancellationRequested())
                                 fail(QStringLiteral("Release download cancelled"));
                         });
        cancellation.start(50);
        require(elapsed.elapsed() < Deadline,
                QStringLiteral("Release download exceeded time limit"));
        deadline.start(Deadline - int(elapsed.elapsed()));
        idle.start(30000);
        if (!reply->isFinished())
            loop.exec();
        throwIfCancelled();
        consume();
        require(failure.isEmpty(), failure);
        require(reply->error() == QNetworkReply::NoError,
                QStringLiteral("Cannot download official release: ") + reply->errorString());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400)
        {
            require(status == 301 || status == 302 || status == 303 || status == 307
                            || status == 308,
                    QStringLiteral("Unexpected release download response"));
            const QUrl next = safeHttps(
                    url.resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute)
                                         .toUrl())
                            .toString(QUrl::FullyEncoded));
            require((url.host() == "github.com" || storageHost(url.host()))
                            && storageHost(next.host())
                            && (next.path().startsWith("/github-production-release-asset/")
                                || next.path().startsWith("/github-production-release-asset-")),
                    QStringLiteral("Release redirected outside GitHub asset storage"));
            url = next;
            continue;
        }
        require(status == 200, QStringLiteral("Unexpected release download response"));
        if (reply->hasRawHeader("Content-Length"))
            require(total == reply->rawHeader("Content-Length").toLongLong(),
                    QStringLiteral("Incomplete release download"));
        return output ? digest.result().toHex() : contents;
    }
    throw Error(QStringLiteral("Too many release redirects"));
}

QJsonValue
json(QNetworkAccessManager &network, const QString &url)
{
    QJsonParseError     error;
    const QJsonDocument document
            = QJsonDocument::fromJson(download(network, url, MetadataLimit), &error);
    require(error.error == QJsonParseError::NoError && !document.isNull(),
            QStringLiteral("Invalid release JSON metadata"));
    return document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
}

QJsonObject
compatibility(const QJsonObject &artifact)
{
    QJsonObject result;
    for (const QString &key : { QStringLiteral("platform"), QStringLiteral("minimum_glibc"),
                                QStringLiteral("minimum_pipewire"), QStringLiteral("wine_sdk") })
    {
        const auto value = artifact.value(key);
        require(value.isString() && !value.toString().isEmpty() && value.toString().size() <= 256,
                QStringLiteral("Incomplete release compatibility metadata"));
        result.insert(key, value);
    }
    require(result.value("platform") == "archlinux",
            QStringLiteral("Unsupported release platform"));
    for (const QString &key :
         { QStringLiteral("minimum_glibc"), QStringLiteral("minimum_pipewire") })
        require(matches(result.value(key).toString(), QStringLiteral("[0-9]+(?:\\.[0-9]+)+")),
                QStringLiteral("Invalid release minimum library version"));
    require(artifact.value("tested_runtimes").isArray()
                    && artifact.value("tested_runtimes").toArray().size() <= 100,
            QStringLiteral("Invalid tested runtime metadata"));
    for (const auto value : artifact.value("tested_runtimes").toArray())
        require(value.isString() && !value.toString().isEmpty() && value.toString().size() <= 256,
                QStringLiteral("Invalid tested runtime metadata"));
    result.insert("tested_runtimes", artifact.value("tested_runtimes"));
    result.insert("legacy", false);
    return result;
}

QJsonArray
records(QNetworkAccessManager &network, const QJsonValue &value)
{
    require(value.isObject(), QStringLiteral("Invalid GitHub release metadata"));
    const auto metadata = value.toObject();
    require(metadata.value("draft").isBool() && metadata.value("prerelease").isBool()
                    && (metadata.value("name").isUndefined() || metadata.value("name").isNull()
                        || metadata.value("name").isString()),
            QStringLiteral("Invalid GitHub release flags or name"));
    if (metadata.value("draft").toBool())
        return {};
    const QString version = metadata.value("tag_name").toString();
    assetUrl(version, "pipeasio-release.json");
    require(metadata.value("assets").isArray() && metadata.value("assets").toArray().size() <= 100,
            QStringLiteral("Invalid GitHub release assets"));
    QJsonObject indexed;
    for (const auto assetValue : metadata.value("assets").toArray())
    {
        require(assetValue.isObject(), QStringLiteral("Invalid GitHub release asset"));
        const auto    asset = assetValue.toObject();
        const QString name  = asset.value("name").toString();
        require(asset.value("browser_download_url") == assetUrl(version, name)
                        && !indexed.contains(name),
                QStringLiteral("Invalid or duplicate official release asset URL"));
        require(integer(asset.value("size")) && asset.value("size").toDouble() >= 0,
                QStringLiteral("Invalid release asset size"));
        indexed.insert(name, asset);
    }
    QJsonArray artifacts;
    if (indexed.contains("pipeasio-release.json"))
    {
        const auto asset = indexed.value("pipeasio-release.json").toObject();
        require(asset.value("size").toDouble() <= MetadataLimit,
                QStringLiteral("Release manifest exceeds size limit"));
        const auto document = json(network, asset.value("browser_download_url").toString());
        const auto manifest = document.toObject();
        require(document.isObject() && integer(manifest.value("schema"))
                        && manifest.value("schema").toInt() == 1
                        && manifest.value("version") == version
                        && manifest.value("artifacts").isArray()
                        && !manifest.value("artifacts").toArray().isEmpty()
                        && manifest.value("artifacts").toArray().size() <= 32,
                QStringLiteral("Unsupported or inconsistent release manifest"));
        QSet<QString> seen;
        for (const auto item : manifest.value("artifacts").toArray())
        {
            require(item.isObject(), QStringLiteral("Invalid release artifact metadata"));
            auto          artifact = item.toObject();
            const QString arch     = artifact.value("architecture").toString();
            require(!arch.isEmpty(), QStringLiteral("Invalid release architecture"));
            const QString name = artifact.value("asset").toString();
            require(!seen.contains(name) && indexed.contains(name),
                    QStringLiteral("Missing or duplicate manifest artifact"));
            seen.insert(name);
            require(validHash(artifact.value("sha256")), QStringLiteral("Invalid manifest SHA256"));
            artifact.insert("compatibility", compatibility(artifact));
            if (artifact.contains("files"))
            {
                require(artifact.value("files").isObject()
                                && artifact.value("files").toObject().size() <= 4096,
                        QStringLiteral("Invalid manifest file hashes"));
                const auto hashes = artifact.value("files").toObject();
                for (auto it = hashes.begin(); it != hashes.end(); ++it)
                    require(validHash(it.value()), QStringLiteral("Invalid manifest file SHA256"));
            }
            if (arch == architecture())
                artifacts.append(artifact);
        }
    }
    else if (architecture() == "x86_64" && (version == "v1.7.0" || version == "1.7.0"))
    {
        const QString name = "pipeasio-" + version + "-archlinux-x86_64.tar.gz";
        if (indexed.contains(name) && indexed.contains(name + ".sha256"))
            artifacts.append(QJsonObject{
                    { "architecture", "x86_64" },
                    { "asset", name },
                    { "sha256", "" },
                    { "compatibility", QJsonObject{ { "platform", "archlinux" },
                                                    { "tested_runtimes", QJsonArray{} },
                                                    { "legacy", true } } } });
    }
    QJsonArray result;
    for (const auto item : artifacts)
    {
        const auto    artifact = item.toObject();
        const QString name     = artifact.value("asset").toString();
        const auto    asset    = indexed.value(name).toObject();
        require(name.endsWith(".tar.gz") && asset.value("size").toDouble() <= ArchiveLimit,
                QStringLiteral("Unsupported or oversized driver archive"));
        const auto checksumAsset = indexed.value(name + ".sha256").toObject();
        require(checksumAsset.isEmpty() || checksumAsset.value("size").toDouble() <= MetadataLimit,
                QStringLiteral("Release checksum exceeds size limit"));
        QString    checksum = artifact.value("sha256").toString().toLower();
        const auto digest   = asset.value("digest");
        if (!digest.isUndefined() && !digest.isNull())
        {
            require(digest.isString() && digest.toString().startsWith("sha256:")
                            && validHash(digest.toString().mid(7)),
                    QStringLiteral("Invalid GitHub asset digest"));
            require(checksum.isEmpty() || checksum == digest.toString().mid(7).toLower(),
                    QStringLiteral("Manifest and GitHub checksums disagree"));
            checksum = digest.toString().mid(7).toLower();
        }
        QJsonObject record{ { "version", version },
                            { "name", metadata.value("name").toString().isEmpty()
                                              ? version
                                              : metadata.value("name").toString() },
                            { "architecture", artifact.value("architecture") },
                            { "asset", name },
                            { "url", asset.value("browser_download_url") },
                            { "sha256", checksum },
                            { "checksum_url",
                              checksumAsset.value("browser_download_url").toString() },
                            { "compatibility", artifact.value("compatibility") },
                            { "prerelease", metadata.value("prerelease") } };
        if (artifact.contains("files"))
            record.insert("file_hashes", artifact.value("files"));
        result.append(record);
    }
    return result;
}

QString
checksumFile(const QByteArray &data, const QString &name)
{
    for (unsigned char c : data)
        require(c < 128, QStringLiteral("Invalid release checksum file"));
    QStringList              found;
    const QRegularExpression pattern(QStringLiteral("\\A([0-9a-fA-F]{64}) [ *](.+)\\z"));
    for (QString line : QString::fromLatin1(data).split('\n'))
    {
        if (line.endsWith('\r'))
            line.chop(1);
        const auto match = pattern.match(line);
        if (match.hasMatch() && (match.captured(2) == name || match.captured(2) == "./" + name))
            found.append(match.captured(1).toLower());
    }
    require(found.size() == 1, QStringLiteral("Missing or ambiguous archive SHA256"));
    return found.first();
}

void
binary(const QString &path, bool elf, quint16 machine)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot read payload binary: ") + path);
    const QByteArray header = file.read(64);
    bool             valid  = false;
    if (elf)
    {
        valid = header.size() == 64
                && header.first(7)
                           == QByteArray("\x7f"
                                         "ELF\x02\x01\x01",
                                         7)
                && qFromLittleEndian<quint16>(header.constData() + 16) == 3
                && qFromLittleEndian<quint16>(header.constData() + 18) == machine
                && qFromLittleEndian<quint32>(header.constData() + 20) == 1;
    }
    else if (header.size() == 64 && header.first(2) == "MZ")
    {
        const auto offset = qFromLittleEndian<quint32>(header.constData() + 60);
        if (offset >= 64 && offset <= 1024 * 1024 && qint64(offset) <= file.size() - 26
            && file.seek(offset))
        {
            const QByteArray pe = file.read(26);
            require(pe.size() == 26, QStringLiteral("Truncated PE binary"));
            const quint16 flags = qFromLittleEndian<quint16>(pe.constData() + 22);
            valid               = pe.first(4) == QByteArray("PE\0\0", 4)
                                  && qFromLittleEndian<quint16>(pe.constData() + 4) == machine
                                  && qFromLittleEndian<quint16>(pe.constData() + 24)
                                             == (machine == 0x14c ? 0x10b : 0x20b)
                                  && (flags & 2) && (!path.endsWith(".dll") || (flags & 0x2000));
        }
    }
    require(valid,
            QStringLiteral("Invalid binary architecture or format: ") + QFileInfo(path).fileName());
}

void
verifyBinaries(const QString &root, const QJsonObject &files, const Layout &host)
{
    for (const auto &expected : payloadBinaries(host))
        if (files.contains(expected.name))
            binary(root + '/' + expected.name, expected.elf, expected.machine);
}

QJsonObject
buildInfo(const QString &path, const QString &version, const Layout &host)
{
    const QByteArray data = readFile(path, 65536);
    const QString    text = QString::fromUtf8(data);
    require(text.toUtf8() == data, QStringLiteral("Invalid BUILD-INFO.txt"));
    const QStringList lines = text.split('\n');
    require(!lines.isEmpty() && lines.first().startsWith("PipeASIO " + version + " - ")
                    && lines.first().contains(host.architecture),
            QStringLiteral("BUILD-INFO.txt does not match release version or architecture"));
    QMap<QString, QString> fields;
    for (const QString &line : lines.mid(1))
    {
        const int     colon = line.indexOf(':');
        const QString key   = line.left(colon);
        if (colon >= 0 && (key == "Wine (build SDK)" || key == "glibc" || key == "libpipewire-0.3"))
        {
            require(!fields.contains(key),
                    QStringLiteral("Duplicate BUILD-INFO.txt compatibility field"));
            fields.insert(key, line.mid(colon + 1).trimmed());
        }
    }
    const auto pipewire = QRegularExpression(QStringLiteral("minimum ([0-9]+(?:\\.[0-9]+)+)\\)"))
                                  .match(fields.value("libpipewire-0.3"));
    require(!fields.value("Wine (build SDK)").isEmpty()
                    && matches(fields.value("glibc"), QStringLiteral("[0-9]+(?:\\.[0-9]+)+"))
                    && pipewire.hasMatch(),
            QStringLiteral("Incomplete BUILD-INFO.txt compatibility data"));
    return { { "platform", "archlinux" },
             { "build_glibc", fields.value("glibc") },
             { "minimum_pipewire", pipewire.captured(1) },
             { "wine_sdk", fields.value("Wine (build SDK)") },
             { "tested_runtimes", QJsonArray{} },
             { "legacy", true } };
}

void
decompress(const QString &source, const QString &destination, QElapsedTimer &elapsed)
{
    QFile input(source), output(destination);
    require(input.open(QIODevice::ReadOnly)
                    && output.open(QIODevice::WriteOnly | QIODevice::NewOnly),
            QStringLiteral("Cannot open release archive"));
    z_stream stream{};
    require(inflateInit2(&stream, 15 + 16) == Z_OK,
            QStringLiteral("Cannot initialize gzip decoder"));
    struct EndInflate
    {
        z_stream *stream;
        ~EndInflate()
        {
            inflateEnd(stream);
        }
    } cleanup{ &stream };
    std::array<char, 65536> in{}, out{};
    qint64                  expanded = 0;
    int                     status   = Z_OK;
    while (status != Z_STREAM_END)
    {
        throwIfCancelled();
        require(elapsed.elapsed() < Deadline,
                QStringLiteral("Archive extraction exceeded time limit"));
        if (stream.avail_in == 0)
        {
            const qint64 count = input.read(in.data(), in.size());
            require(count > 0, QStringLiteral("Truncated gzip archive"));
            stream.next_in  = reinterpret_cast<Bytef *>(in.data());
            stream.avail_in = uInt(count);
        }
        stream.next_out  = reinterpret_cast<Bytef *>(out.data());
        stream.avail_out = out.size();
        status           = inflate(&stream, Z_NO_FLUSH);
        require(status == Z_OK || status == Z_STREAM_END, QStringLiteral("Corrupt gzip archive"));
        const qint64 count = qint64(out.size()) - stream.avail_out;
        expanded += count;
        require(expanded <= ExpandedLimit, QStringLiteral("Expanded archive exceeds size limit"));
        require(output.write(out.data(), count) == count,
                QStringLiteral("Cannot write expanded archive"));
    }
    require(stream.avail_in == 0 && input.atEnd(),
            QStringLiteral("Trailing data after gzip archive"));
}

QJsonObject
extract(const QString &archivePath, const QString &root, const QJsonObject &release,
        const Layout &host)
{
    QElapsedTimer elapsed;
    elapsed.start();
    const QString tarPath = QFileInfo(archivePath).dir().filePath("driver.tar");
    decompress(archivePath, tarPath, elapsed);
    std::unique_ptr<struct archive, decltype(&archive_read_free)> archive(archive_read_new(),
                                                                          archive_read_free);
    require(bool(archive), QStringLiteral("Cannot initialize archive reader"));
    archive_read_support_format_tar(archive.get());
    require(archive_read_open_filename(archive.get(), QFile::encodeName(tarPath).constData(), 65536)
                    == ARCHIVE_OK,
            QStringLiteral("Cannot open driver tar archive"));
    QJsonObject             files;
    QSet<QString>           seen;
    struct archive_entry   *entry = nullptr;
    int                     count = 0;
    int                     status;
    std::array<char, 65536> buffer{};
    const auto              expectedHashes = release.value("file_hashes").toObject();
    QStringList             accepted{ BuildInfo };
    for (const auto &expected : payloadBinaries(host))
        accepted.append(expected.name);
    while ((status = archive_read_next_header(archive.get(), &entry)) == ARCHIVE_OK)
    {
        throwIfCancelled();
        require(elapsed.elapsed() < Deadline,
                QStringLiteral("Archive extraction exceeded time limit"));
        require(++count <= 4096, QStringLiteral("Archive contains too many entries"));
        const char *pathname = archive_entry_pathname_utf8(entry);
        require(pathname != nullptr, QStringLiteral("Invalid archive path encoding"));
        const QString original = QString::fromUtf8(pathname);
        require(original.toUtf8() == QByteArray(pathname) && !original.startsWith('/')
                        && !original.split('/').contains("..") && !original.contains('\\')
                        && original.size() <= 1024 && !original.isEmpty(),
                QStringLiteral("Unsafe archive path"));
        for (const QChar c : original)
            require(c.unicode() >= 32, QStringLiteral("Unsafe archive path"));
        QString    name = QDir::cleanPath(original);
        const auto type = archive_entry_filetype(entry);
        const auto size = archive_entry_size(entry);
        require((type == AE_IFREG || type == AE_IFDIR) && !archive_entry_symlink(entry)
                        && !archive_entry_hardlink(entry) && archive_entry_sparse_count(entry) == 0
                        && size >= 0 && size <= FileLimit,
                QStringLiteral("Unsafe archive entry: ") + original);
        require(!seen.contains(name), QStringLiteral("Duplicate archive path: ") + original);
        seen.insert(name);
        if (type == AE_IFDIR)
            continue;
        if (name == "share/pipeasio/" + Probe || name == "share/pipeasio/" + Probe32)
            name.remove(0, 15);
        if (!accepted.contains(name))
            continue;
        require(!files.contains(name), QStringLiteral("Duplicate payload file: ") + name);
        require(name != BuildInfo || size <= 65536,
                QStringLiteral("BUILD-INFO.txt exceeds size limit"));
        const QString target = root + '/' + name;
        require(QDir().mkpath(QFileInfo(target).absolutePath()),
                QStringLiteral("Cannot create payload directory"));
        QFile output(target);
        require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                QStringLiteral("Cannot create payload file"));
        QCryptographicHash digest(QCryptographicHash::Sha256);
        qint64             written = 0;
        la_ssize_t         bytes;
        while ((bytes = archive_read_data(archive.get(), buffer.data(), buffer.size())) > 0)
        {
            throwIfCancelled();
            require(elapsed.elapsed() < Deadline,
                    QStringLiteral("Archive extraction exceeded time limit"));
            written += bytes;
            require(written <= size && output.write(buffer.data(), bytes) == bytes,
                    QStringLiteral("Cannot extract payload file"));
            digest.addData(QByteArrayView(buffer.data(), bytes));
        }
        require(bytes == 0 && written == size, QStringLiteral("Truncated driver archive"));
        const QString hash = QString::fromLatin1(digest.result().toHex());
        if (release.contains("file_hashes"))
        {
            const QString sourceName
                    = name == Probe || name == Probe32 ? "share/pipeasio/" + name : name;
            const auto expected = expectedHashes.contains(sourceName)
                                          ? expectedHashes.value(sourceName)
                                          : expectedHashes.value(name);
            require(validHash(expected) && expected.toString().toLower() == hash,
                    QStringLiteral("Manifest payload SHA256 mismatch: ") + name);
        }
        require(output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                      | QFileDevice::ReadGroup | QFileDevice::ReadOther
                                      | (name.endsWith(".exe")
                                                 ? QFileDevice::ExeOwner | QFileDevice::ExeGroup
                                                           | QFileDevice::ExeOther
                                                 : QFileDevice::Permissions{})),
                QStringLiteral("Cannot set payload permissions"));
        files.insert(name, hash);
    }
    require(status == ARCHIVE_EOF, QStringLiteral("Invalid driver tar archive"));
    for (const QString &name : { dll64(host), unixlib64(host), BuildInfo })
        require(files.contains(name), QStringLiteral("Missing driver payload: ") + name);
    if (!release.value("compatibility").toObject().value("legacy").toBool())
        require(files.contains(Probe), QStringLiteral("Missing driver payload: ") + Probe);
    require(files.contains(dll32()) == files.contains(unixlib32(host)),
            QStringLiteral("Incomplete 32-bit driver payload"));
    verifyBinaries(root, files, host);
    return files;
}
}

QJsonArray
list(QNetworkAccessManager &network, const Progress &progress)
{
    if (progress)
        progress(QStringLiteral("Reading official PipeASIO releases"));
    QJsonArray result;
    for (int page = 1; page <= 5; ++page)
    {
        const auto metadata = json(network, Api + "?per_page=100&page=" + QString::number(page));
        require(metadata.isArray() && metadata.toArray().size() <= 100,
                QStringLiteral("Invalid GitHub release listing"));
        for (const auto release : metadata.toArray())
            for (const auto record : records(network, release))
                result.append(record);
        if (metadata.toArray().size() < 100)
            return result;
    }
    throw Error(QStringLiteral("Official release listing exceeds safety limit"));
}

QJsonObject
fetch(QNetworkAccessManager &network, const QJsonObject &release, const QString &destination,
      const Progress &progress)
{
    const Layout &host   = layout();
    const QString target = QFileInfo(destination).absoluteFilePath();
    require(!QFileInfo::exists(target) && !QFileInfo(target).isSymLink(),
            QStringLiteral("Release destination already exists"));
    const QString version = release.value("version").toString();
    const QString asset   = release.value("asset").toString();
    require(release.value("url") == assetUrl(version, asset)
                    && release.value("architecture") == host.architecture,
            QStringLiteral("Unsupported or unofficial release selection"));
    if (progress)
        progress(QStringLiteral("Verifying official release metadata"));
    QJsonObject selected;
    for (const auto item :
         records(network,
                 json(network,
                      Api + "/tags/" + QString::fromLatin1(QUrl::toPercentEncoding(version)))))
    {
        const auto record = item.toObject();
        if (record.value("version") == version && record.value("asset") == asset)
            selected = record;
    }
    require(!selected.isEmpty(),
            QStringLiteral("Selected official release artifact is no longer available"));
    QString checksum = selected.value("sha256").toString();
    if (!selected.value("checksum_url").toString().isEmpty())
    {
        const QString published = checksumFile(
                download(network, selected.value("checksum_url").toString(), MetadataLimit), asset);
        require(checksum.isEmpty() || checksum == published,
                QStringLiteral("Official release checksums disagree"));
        checksum = published;
    }
    require(!checksum.isEmpty(), QStringLiteral("Official release has no SHA256 checksum"));
    const QString parent = QFileInfo(target).absolutePath();
    require(QDir().mkpath(parent), QStringLiteral("Cannot create release destination parent"));
    QTemporaryDir temporary(parent + "/.pipeasio-download-XXXXXX");
    require(temporary.isValid(), QStringLiteral("Cannot create private release staging directory"));
    const QString root = temporary.path() + "/payload";
    require(QDir().mkdir(root), QStringLiteral("Cannot create payload staging directory"));
    const QString archivePath = temporary.path() + "/driver.tar.gz";
    QFile         archive(archivePath);
    require(archive.open(QIODevice::WriteOnly | QIODevice::NewOnly),
            QStringLiteral("Cannot create release archive"));
    if (progress)
        progress(QStringLiteral("Downloading ") + asset);
    const QString actual = QString::fromLatin1(
            download(network, selected.value("url").toString(), ArchiveLimit, &archive));
    archive.close();
    require(actual == checksum, QStringLiteral("Driver archive SHA256 mismatch"));
    if (progress)
        progress(QStringLiteral("Checking driver archive and binary architectures"));
    const auto files  = extract(archivePath, root, selected, host);
    const auto build  = buildInfo(root + '/' + BuildInfo, version, host);
    const auto compat = selected.value("compatibility").toObject();
    require(::syscall(SYS_renameat2, AT_FDCWD, QFile::encodeName(root).constData(), AT_FDCWD,
                      QFile::encodeName(target).constData(), RENAME_NOREPLACE)
                    == 0,
            QStringLiteral("Cannot stage verified release without replacing existing files: ")
                    + QString::fromLocal8Bit(std::strerror(errno)));
    return { { "root", target },
             { "version", version },
             { "architecture", host.architecture },
             { "compatibility", compat.value("legacy").toBool() ? build : compat },
             { "files", files } };
}

QJsonObject
createManifest(const QString &version, const QString &asset, const QString &root,
               const QString &wineSdk, const QString &arch)
{
    const QString target = arch.isEmpty() ? architecture() : arch;
    const Layout *host   = layoutFor(target);
    require(host != nullptr, QStringLiteral("Unsupported release architecture: ") + target);
    const QString name = QFileInfo(asset).fileName();
    assetUrl(version, name);
    require(name.endsWith(".tar.gz"), QStringLiteral("Unsupported driver archive name"));
    require(!wineSdk.isEmpty() && wineSdk.size() <= 256,
            QStringLiteral("Invalid Wine SDK version"));
    QStringList required{ dll64(*host), unixlib64(*host), BuildInfo, "share/pipeasio/" + Probe };
    // The second ARM64 front end only exists where the build host had Wine's
    // arm64ec import libraries, so it is recorded when the tree carries it.
    const QString emulated = dll64Emulated(*host);
    if (!emulated.isEmpty() && QFileInfo::exists(root + '/' + emulated))
        required.append(emulated);
    if (QFileInfo::exists(root + '/' + dll32()) || QFileInfo::exists(root + '/' + unixlib32(*host)))
        required.append({ dll32(), unixlib32(*host), "share/pipeasio/" + Probe32 });
    else if (QFileInfo::exists(root + "/share/pipeasio/" + Probe32))
        required.append("share/pipeasio/" + Probe32);
    QJsonObject    hashes, normalized, libraries;
    QList<quint64> maximum;
    for (const QString &file : required)
    {
        const QString path = root + '/' + file;
        require(QFileInfo(path).isFile() && !QFileInfo(path).isSymLink(),
                QStringLiteral("Missing regular payload file: ") + file);
        const QString hash = hashFile(path);
        hashes.insert(file, hash);
        normalized.insert(file.startsWith("share/pipeasio/") ? file.mid(15) : file, hash);
        if (!file.endsWith(".so"))
            continue;
        const auto versions = execute("readelf", { "--version-info", path }, cleanEnvironment());
        require(versions.exitCode == 0,
                QStringLiteral("Cannot read ELF version requirements: ") + versions.error);
        auto matches = QRegularExpression(QStringLiteral("\\bGLIBC_([0-9]+(?:\\.[0-9]+)+)\\b"))
                               .globalMatch(versions.output);
        while (matches.hasNext())
        {
            QList<quint64> components;
            for (const auto &component : matches.next().captured(1).split('.'))
            {
                bool       ok;
                const auto number = component.toULongLong(&ok);
                require(ok, QStringLiteral("Invalid ELF GLIBC requirement"));
                components.append(number);
            }
            if (std::lexicographical_compare(maximum.begin(), maximum.end(), components.begin(),
                                             components.end()))
                maximum = components;
        }
        const auto dynamic = execute("readelf", { "--dynamic", path }, cleanEnvironment());
        require(dynamic.exitCode == 0,
                QStringLiteral("Cannot read ELF dynamic requirements: ") + dynamic.error);
        auto          needed = QRegularExpression(QStringLiteral("\\(NEEDED\\)[^\\n]*\\[(.*?)\\]"))
                                       .globalMatch(dynamic.output);
        QSet<QString> unique;
        while (needed.hasNext())
            unique.insert(needed.next().captured(1));
        auto names = unique.values();
        names.sort();
        libraries.insert(file, QJsonArray::fromStringList(names));
    }
    for (const auto &expected : payloadBinaries(*host))
        if (normalized.contains(expected.name))
            binary(root + '/'
                           + (expected.name == Probe || expected.name == Probe32
                                      ? "share/pipeasio/" + expected.name
                                      : expected.name),
                   expected.elf, expected.machine);
    require(!maximum.isEmpty(),
            QStringLiteral("No GLIBC version requirements found in the built unixlibs"));
    QStringList components;
    for (quint64 component : maximum)
        components.append(QString::number(component));
    const auto        build = buildInfo(root + '/' + BuildInfo, version, *host);
    const QJsonObject artifact{ { "architecture", target },
                                { "asset", name },
                                { "sha256", hashFile(asset) },
                                { "platform", "archlinux" },
                                { "minimum_glibc", components.join('.') },
                                { "minimum_pipewire", build.value("minimum_pipewire") },
                                { "wine_sdk", wineSdk },
                                { "tested_runtimes", QJsonArray{} },
                                { "libraries", libraries },
                                { "files", hashes } };
    return { { "schema", 1 }, { "version", version }, { "artifacts", QJsonArray{ artifact } } };
}
}
