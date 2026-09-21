/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Manager.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace PipeASIOManager;
namespace
{
#define CHECK(value) do { if (!(value)) throw std::runtime_error("CHECK failed: " #value); } while (false)
const QString Api = "https://api.github.com/repos/M0n7y5/pipeasio/releases";
const QString Download = "https://github.com/M0n7y5/pipeasio/releases/download/";
const QString Dll = "lib/wine/x86_64-windows/pipeasio64.dll";
const QString Unixlib = "lib/wine/x86_64-unix/pipeasio64.so";
const QString ArmDll = "lib/wine/aarch64-windows/pipeasio64.dll";
const QString ArmEcDll = "lib/wine/arm64ec-windows/pipeasio64.dll";
const QString ArmUnixlib = "lib/wine/aarch64-unix/pipeasio64.so";
const QString Probe = "share/pipeasio/manager/pipeasio-check.exe";
const QString Probe32 = "share/pipeasio/manager/pipeasio-check32.exe";

// Restores the x86_64 pin every other test in this suite relies on.
struct HostArchitecture
{
    explicit HostArchitecture(const QString &value) { Testing::setArchitecture(value); }
    ~HostArchitecture() { Testing::setArchitecture("x86_64"); }
};

QString hash(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QByteArray pe(quint16 machine = 0x8664, bool dll = true)
{
    QByteArray data(512, '\0');
    data.replace(0, 2, "MZ");
    qToLittleEndian<quint32>(128, data.data() + 60);
    data.replace(128, 4, QByteArray("PE\0\0", 4));
    qToLittleEndian<quint16>(machine, data.data() + 132);
    qToLittleEndian<quint16>(dll ? 0x2002 : 2, data.data() + 150);
    qToLittleEndian<quint16>(machine == 0x14c ? 0x10b : 0x20b, data.data() + 152);
    return data;
}

QByteArray elf(quint16 machine = 62)
{
    QByteArray data(1024, '\0');
    data.replace(0, 7, QByteArray("\x7f" "ELF\x02\x01\x01", 7));
    qToLittleEndian<quint16>(3, data.data() + 16);
    qToLittleEndian<quint16>(machine, data.data() + 18);
    qToLittleEndian<quint32>(1, data.data() + 20);
    qToLittleEndian<quint64>(352, data.data() + 32);
    qToLittleEndian<quint64>(512, data.data() + 40);
    qToLittleEndian<quint16>(64, data.data() + 52);
    qToLittleEndian<quint16>(56, data.data() + 54);
    qToLittleEndian<quint16>(2, data.data() + 56);
    qToLittleEndian<quint16>(64, data.data() + 58);
    qToLittleEndian<quint16>(5, data.data() + 60);
    qToLittleEndian<quint16>(4, data.data() + 62);
    const QByteArray strings("\0libc.so.6\0GLIBC_2.34\0", 22);
    data.replace(64, strings.size(), strings);
    qToLittleEndian<quint16>(1, data.data() + 128);
    qToLittleEndian<quint16>(1, data.data() + 130);
    qToLittleEndian<quint32>(1, data.data() + 132);
    qToLittleEndian<quint32>(16, data.data() + 136);
    qToLittleEndian<quint16>(2, data.data() + 150);
    qToLittleEndian<quint32>(11, data.data() + 152);
    qToLittleEndian<quint64>(1, data.data() + 192);
    qToLittleEndian<quint64>(1, data.data() + 200);
    qToLittleEndian<quint64>(5, data.data() + 208);
    qToLittleEndian<quint64>(64, data.data() + 216);
    qToLittleEndian<quint64>(10, data.data() + 224);
    qToLittleEndian<quint64>(strings.size(), data.data() + 232);
    qToLittleEndian<quint64>(0x6ffffffe, data.data() + 240);
    qToLittleEndian<quint64>(128, data.data() + 248);
    qToLittleEndian<quint64>(0x6fffffff, data.data() + 256);
    qToLittleEndian<quint64>(1, data.data() + 264);
    const QByteArray names("\0.dynstr\0.gnu.version_r\0.dynamic\0.shstrtab\0", 43);
    data.replace(288, names.size(), names);
    auto segment = [&](int index, quint32 type, quint64 offset, quint64 size, quint64 alignment) {
        char *at = data.data() + 352 + index * 56;
        qToLittleEndian(type, at);
        qToLittleEndian<quint32>(6, at + 4);
        qToLittleEndian(offset, at + 8);
        qToLittleEndian(offset, at + 16);
        qToLittleEndian(offset, at + 24);
        qToLittleEndian(size, at + 32);
        qToLittleEndian(size, at + 40);
        qToLittleEndian(alignment, at + 48);
    };
    segment(0, 1, 0, data.size(), 4096);
    segment(1, 2, 192, 96, 8);
    auto section = [&](int index, quint32 name, quint32 type, quint64 offset, quint64 size, quint32 link, quint32 info, quint64 entrySize) {
        char *at = data.data() + 512 + index * 64;
        qToLittleEndian(name, at);
        qToLittleEndian(type, at + 4);
        qToLittleEndian<quint64>(index == 4 ? 0 : (type == 6 ? 3 : 2), at + 8);
        qToLittleEndian<quint64>(index == 4 ? 0 : offset, at + 16);
        qToLittleEndian(offset, at + 24);
        qToLittleEndian(size, at + 32);
        qToLittleEndian(link, at + 40);
        qToLittleEndian(info, at + 44);
        qToLittleEndian<quint64>(type == 6 ? 8 : 1, at + 48);
        qToLittleEndian(entrySize, at + 56);
    };
    section(1, 1, 3, 64, strings.size(), 0, 0, 0);
    section(2, 9, 0x6ffffffe, 128, 32, 1, 1, 0);
    section(3, 24, 6, 192, 96, 1, 0, 16);
    section(4, 33, 3, 288, names.size(), 0, 0, 0);
    return data;
}

using Entries = QMap<QString, QByteArray>;
Entries payload(const QString &version = "v1.7.0", bool probes = false)
{
    Entries entries{{Dll, pe()}, {Unixlib, elf()}, {"BUILD-INFO.txt",
        ("PipeASIO " + version + " - prebuilt binaries (Arch Linux / CachyOS family, x86_64)\n"
         "Wine (build SDK): wine-10.15\nglibc: 2.42\nlibpipewire-0.3: 1.4.8 (built against; minimum 1.4.2)\n").toUtf8()},
        {"bin/pipeasio-register", "#!/bin/sh\nexit 99\n"}};
    if (probes)
    {
        entries.insert(Probe, pe(0x8664, false));
        entries.insert(Probe32, pe(0x14c, false));
    }
    return entries;
}

// What an ARM64 release tarball holds: the aarch64 front end, the arm64ec one
// for x86_64 hosts under FEX, and the single aarch64 unixlib both use.
Entries armPayload(const QString &version = "v1.8.0", bool emulated = true)
{
    Entries entries{{ArmDll, pe(0xaa64)}, {ArmUnixlib, elf(183)}, {"BUILD-INFO.txt",
        ("PipeASIO " + version + " - prebuilt binaries (Debian family, aarch64)\n"
         "Wine (build SDK): wine-10.15\nglibc: 2.42\nlibpipewire-0.3: 1.4.8 (built against; minimum 1.4.2)\n").toUtf8()},
        {Probe, pe(0xaa64, false)}};
    if (emulated)
        entries.insert(ArmEcDll, pe(0x8664));
    return entries;
}

QByteArray tar(const Entries &entries, const QString &extra = {}, int type = AE_IFREG, const QString &link = {}, qint64 size = 0, int repeats = 1)
{
    QByteArray storage(2 * 1024 * 1024, '\0');
    size_t used = 0;
    struct archive *writer = archive_write_new();
    CHECK(writer);
    CHECK(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
    CHECK(archive_write_add_filter_gzip(writer) == ARCHIVE_OK);
    CHECK(archive_write_set_bytes_per_block(writer, 0) == ARCHIVE_OK);
    CHECK(archive_write_open_memory(writer, storage.data(), storage.size(), &used) == ARCHIVE_OK);
    auto add = [&](const QString &name, const QByteArray &data, int entryType, const QString &target, qint64 declared) {
        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.toUtf8().constData());
        archive_entry_set_filetype(entry, entryType);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, declared);
        if (!target.isEmpty())
        {
            if (entryType == AE_IFLNK)
                archive_entry_set_symlink(entry, target.toUtf8().constData());
            else
                archive_entry_set_hardlink(entry, target.toUtf8().constData());
        }
        CHECK(archive_write_header(writer, entry) == ARCHIVE_OK);
        if (!data.isEmpty())
            CHECK(archive_write_data(writer, data.constData(), data.size()) == data.size());
        archive_entry_free(entry);
    };
    for (auto it = entries.begin(); it != entries.end(); ++it)
        add("./" + it.key(), it.value(), AE_IFREG, {}, it.value().size());
    if (!extra.isEmpty())
        for (int i = 0; i < repeats; ++i)
            add(extra + (repeats == 1 ? QString{} : QString::number(i)), {}, type, link, size);
    CHECK(archive_write_close(writer) == ARCHIVE_OK);
    CHECK(archive_write_free(writer) == ARCHIVE_OK);
    storage.resize(used);
    return storage;
}

struct Response
{
    QByteArray body;
    QMap<QByteArray, QByteArray> headers;
    int status = 200;
    QString redirect;
    bool omitLength = false;
    bool cancelOnReady = false;
};

class Reply final : public QNetworkReply
{
  public:
    Reply(const QNetworkRequest &request, const Response &response, QObject *parent)
        : QNetworkReply(parent), body(response.body)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        if (!response.redirect.isEmpty())
            setAttribute(QNetworkRequest::RedirectionTargetAttribute, QUrl(response.redirect));
        if (!response.omitLength)
            setRawHeader("Content-Length", QByteArray::number(body.size()));
        for (auto it = response.headers.begin(); it != response.headers.end(); ++it)
            setRawHeader(it.key(), it.value());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this, cancel = response.cancelOnReady] {
            if (cancel)
                requestCancellation();
            emit metaDataChanged();
            if (isFinished())
                return;
            emit readyRead();
            if (!isFinished())
            {
                setFinished(true);
                emit finished();
            }
        });
    }
    void abort() override
    {
        if (isFinished())
            return;
        setError(OperationCanceledError, "Aborted");
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override { return body.size() - position + QNetworkReply::bytesAvailable(); }
    bool isSequential() const override { return true; }
  protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const auto count = std::min(maxSize, qint64(body.size()) - position);
        if (!count)
            return -1;
        std::memcpy(data, body.constData() + position, size_t(count));
        position += count;
        return count;
    }
  private:
    QByteArray body;
    qint64 position = 0;
};

class Server final : public QNetworkAccessManager
{
  public:
    QString version, name, url, architecture;
    QJsonObject metadata;
    QMap<QString, Response> responses;
    QStringList requests;
    Server(const QByteArray &data, const QString &tag = "v1.7.0", const QJsonObject &manifest = {},
           const QString &arch = "x86_64") : version(tag), architecture(arch)
    {
        name = "pipeasio-" + version + "-archlinux-" + architecture + ".tar.gz";
        url = Download + version + '/' + name;
        responses.insert(url, {data});
        responses.insert(url + ".sha256", {(hash(data) + "  " + name + '\n').toLatin1()});
        QJsonArray assets{asset(name), asset(name + ".sha256")};
        if (!manifest.isEmpty())
        {
            responses.insert(Download + version + "/pipeasio-release.json", {QJsonDocument(manifest).toJson()});
            assets.append(asset("pipeasio-release.json"));
        }
        metadata = {{"tag_name", version}, {"name", "PipeASIO " + version}, {"draft", false}, {"prerelease", false}, {"assets", assets}};
    }
    QJsonObject asset(const QString &assetName) const
    {
        const QString address = Download + version + '/' + assetName;
        return {{"name", assetName}, {"browser_download_url", address}, {"size", responses.value(address).body.size()}};
    }
    QJsonObject selection() const
    {
        return {{"version", version}, {"asset", name}, {"url", url}, {"architecture", architecture},
            {"sha256", QString(64, '0')}, {"compatibility", QJsonObject{{"tested_runtimes", QJsonArray{"invented runtime"}}}}};
    }
  protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *outgoing) override
    {
        CHECK(operation == GetOperation);
        CHECK(!outgoing);
        const QString address = request.url().toString(QUrl::FullyEncoded);
        requests.append(address);
        if (responses.contains(address))
            return new Reply(request, responses.value(address), this);
        if (address == Api + "?per_page=100&page=1")
            return new Reply(request, {QJsonDocument(QJsonArray{metadata}).toJson()}, this);
        if (address == Api + "/tags/" + version)
            return new Reply(request, {QJsonDocument(metadata).toJson()}, this);
        throw std::runtime_error(("Unexpected network request: " + address).toStdString());
    }
};

QJsonObject manifest(const QByteArray &data, const QString &architecture = "x86_64")
{
    return {{"schema", 1}, {"version", "v1.8.0"}, {"artifacts", QJsonArray{QJsonObject{
        {"architecture", architecture},
        {"asset", "pipeasio-v1.8.0-archlinux-" + architecture + ".tar.gz"}, {"sha256", hash(data)},
        {"platform", "archlinux"}, {"minimum_glibc", "2.34"}, {"minimum_pipewire", "1.4.2"},
        {"wine_sdk", "wine-10.15"}, {"tested_runtimes", QJsonArray{}}}}}};
}

struct Fixture
{
    QTemporaryDir temporary;
    QString destination;
    Fixture()
    {
        CHECK(temporary.isValid());
        destination = temporary.path() + "/new";
        CHECK(QDir().mkdir(temporary.path() + "/installed"));
        writeFile(temporary.path() + "/installed/driver", "previous verified driver");
    }
    QJsonObject fetch(Server &server) { return Releases::fetch(server, server.selection(), destination); }
    void reject(Server &server)
    {
        bool failed = false;
        try { fetch(server); } catch (const Error &) { failed = true; }
        CHECK(failed);
        CHECK(!QFileInfo::exists(destination));
        CHECK(QDir(temporary.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden) == QStringList{"installed"});
        CHECK(readFile(temporary.path() + "/installed/driver") == "previous verified driver");
    }
};

void legacyPayload()
{
    Fixture fixture;
    Server server(tar(payload()));
    const auto result = fixture.fetch(server);
    CHECK(readFile(fixture.destination + '/' + Dll) == pe());
    CHECK(!QFileInfo::exists(fixture.destination + "/bin"));
    CHECK(result.value("files").toObject().keys() == QStringList({"BUILD-INFO.txt", Unixlib, Dll}));
    const auto compatibility = result.value("compatibility").toObject();
    CHECK(compatibility.value("build_glibc") == "2.42");
    CHECK(!compatibility.contains("minimum_glibc"));
    CHECK(compatibility.value("tested_runtimes").toArray().isEmpty());
}

void manifestProbes()
{
    Fixture fixture;
    const auto data = tar(payload("v1.8.0", true));
    Server server(data, "v1.8.0", manifest(data));
    const auto result = fixture.fetch(server);
    CHECK(readFile(fixture.destination + "/manager/pipeasio-check.exe") == pe(0x8664, false));
    CHECK(readFile(fixture.destination + "/manager/pipeasio-check32.exe") == pe(0x14c, false));
    CHECK(result.value("compatibility").toObject().value("minimum_glibc") == "2.34");
}

void corruptArchives()
{
    const auto original = tar(payload());
    auto trailer = original;
    trailer[trailer.size() - 8] ^= 1;
    for (const auto &data : {original.first(original.size() - 5), trailer, original + "trailing"})
    {
        Fixture fixture;
        Server server(data);
        fixture.reject(server);
    }
}

void unsafeMembers()
{
    const QList<QByteArray> archives{
        tar(payload(), "../outside"), tar(payload(), "/outside"), tar(payload(), "bin/link", AE_IFLNK, "../../outside"),
        tar(payload(), "lib/alias", AE_IFREG, Dll), tar(payload(), "bin/device", AE_IFCHR), tar(payload(), Dll),
        tar(payload(), "bin\\outside"), tar(payload(), "bin/oversize", AE_IFREG, {}, 32 * 1024 * 1024 + 1)};
    for (const auto &data : archives)
    {
        Fixture fixture;
        Server server(data);
        fixture.reject(server);
    }
}

void payloadBoundaries()
{
    auto missing = payload();
    missing.remove(Unixlib);
    auto wrongPe = payload();
    wrongPe[Dll] = pe(0x14c);
    auto wrongElf = payload();
    wrongElf[Unixlib] = elf(183);
    auto unpaired = payload();
    unpaired["lib/wine/i386-windows/pipeasio32.dll"] = pe(0x14c);
    auto build = payload();
    build["BUILD-INFO.txt"] = "PipeASIO v1.7.0 - x86_64\n";
    auto duplicateBuild = payload();
    duplicateBuild["BUILD-INFO.txt"].append("glibc: 2.42\n");
    for (const auto &entries : {missing, wrongPe, wrongElf, unpaired, build, duplicateBuild, payload("v1.6.0")})
    {
        Fixture fixture;
        Server server(tar(entries));
        fixture.reject(server);
    }
    for (bool wrongProbe : {false, true})
    {
        auto entries = payload("v1.8.0", wrongProbe);
        if (wrongProbe)
            entries[Probe32] = pe(0x8664, false);
        const auto data = tar(entries);
        Fixture fixture;
        Server server(data, "v1.8.0", manifest(data));
        fixture.reject(server);
    }
}

void checksumBoundaries()
{
    for (int mode = 0; mode < 4; ++mode)
    {
        Fixture fixture;
        Server server(tar(payload()));
        auto &body = server.responses[server.url + ".sha256"].body;
        if (mode == 0)
            body = (QString(64, '0') + "  " + server.name + '\n').toLatin1();
        if (mode == 1)
            body = (QString(64, '0') + "  other.tar.gz\n").toLatin1();
        if (mode == 2)
            body += body;
        if (mode == 3)
            server.responses[server.url].body.append('x');
        fixture.reject(server);
    }
}

void networkBoundaries()
{
    const QList<QMap<QByteArray, QByteArray>> headers{
        {{"Content-Length", "99999999999999999999999999999"}}, {{"Content-Length", "-1"}},
        {{"Content-Length", "67108865"}}, {{"Content-Length", "1"}}, {{"Content-Encoding", "gzip"}}};
    for (const auto &value : headers)
    {
        Fixture fixture;
        Server server(tar(payload()));
        server.responses[server.url].headers = value;
        fixture.reject(server);
    }
    Fixture fixture;
    Server server(tar(payload()));
    server.responses[server.url].status = 206;
    fixture.reject(server);
}

void expandedLimit()
{
    Fixture fixture;
    Server server(tar(payload(), "bin/unused", AE_IFREG, {}, 32 * 1024 * 1024, 9));
    fixture.reject(server);
}

void unannouncedDownloadLimit()
{
    Fixture fixture;
    Server server(tar(payload()));
    server.responses[server.url].body = QByteArray(64 * 1024 * 1024 + 1, 'x');
    server.responses[server.url].omitLength = true;
    fixture.reject(server);
}

void cancellationCleanup()
{
    Fixture fixture;
    Server server(tar(payload()));
    server.responses[server.url].cancelOnReady = true;
    fixture.reject(server);
    server.responses[server.url].cancelOnReady = false;
    fixture.fetch(server);
    CHECK(readFile(fixture.destination + '/' + Dll) == pe());
}

void redirectBoundaries()
{
    for (const QString &path : {QStringLiteral("/github-production-release-asset/123/file"), QStringLiteral("/github-production-release-asset-123/file")})
    {
        Fixture fixture;
        const auto data = tar(payload());
        Server server(data);
        const QString address = "https://release-assets.githubusercontent.com" + path + "?signature=value";
        server.responses[server.url] = {{}, {}, 302, address};
        server.responses[address] = {data};
        fixture.fetch(server);
        CHECK(readFile(fixture.destination + '/' + Dll) == pe());
    }
    for (const QString &address : {QStringLiteral("https://example.com/archive"),
             QStringLiteral("http://release-assets.githubusercontent.com/github-production-release-asset/file"),
             QStringLiteral("https://release-assets.githubusercontent.com/other/file"),
             QStringLiteral("https://user@release-assets.githubusercontent.com/github-production-release-asset/file"),
             QStringLiteral("https://release-assets.githubusercontent.com:444/github-production-release-asset/file"),
             QStringLiteral("https://release-assets.githubusercontent.com/github-production-release-asset/file#fragment")})
    {
        Fixture fixture;
        Server server(tar(payload()));
        server.responses[server.url] = {{}, {}, 302, address};
        fixture.reject(server);
        CHECK(server.requests.last() == server.url);
    }
    Fixture fixture;
    Server server(tar(payload()));
    const QString storage = "https://objects.githubusercontent.com/github-production-release-asset/file";
    server.responses[server.url] = {{}, {}, 302, storage};
    server.responses[storage] = {{}, {}, 302, storage};
    fixture.reject(server);
    CHECK(server.requests.count(storage) == 5);
}

void metadataBoundaries()
{
    const auto data = tar(payload("v1.8.0", true));
    for (int mode = 0; mode < 8; ++mode)
    {
        auto description = manifest(data);
        auto artifact = description.value("artifacts").toArray().first().toObject();
        if (mode == 0) description.insert("schema", true);
        if (mode == 1) description.insert("version", "v1.9.0");
        if (mode == 2) artifact.remove("tested_runtimes");
        if (mode == 3) artifact.insert("sha256", "abcd");
        if (mode == 4) artifact.insert("minimum_glibc", "latest");
        if (mode == 5) artifact.insert("sha256", QString(64, '0'));
        if (mode == 6) artifact.insert("files", QJsonObject{{Dll, QString(64, '0')}});
        if (mode == 7) artifact.insert("tested_runtimes", QJsonArray{true});
        description.insert("artifacts", QJsonArray{artifact});
        Fixture fixture;
        Server server(data, "v1.8.0", description);
        fixture.reject(server);
    }
    for (int mode = 0; mode < 5; ++mode)
    {
        Fixture fixture;
        Server server(tar(payload()));
        auto assets = server.metadata.value("assets").toArray();
        auto asset = assets.first().toObject();
        if (mode == 0) asset.insert("browser_download_url", "https://example.com/archive");
        if (mode == 1) asset.insert("size", true);
        if (mode == 2) asset.insert("digest", "md5:abcd");
        if (mode == 3) server.metadata.insert("prerelease", "false");
        assets.replace(0, asset);
        if (mode == 4) assets.append(asset);
        server.metadata.insert("assets", assets);
        fixture.reject(server);
    }
}

void existingDestination()
{
    Fixture fixture;
    CHECK(QDir().mkdir(fixture.destination));
    writeFile(fixture.destination + "/existing", "do not replace");
    Server server(tar(payload()));
    bool failed = false;
    try { fixture.fetch(server); } catch (const Error &) { failed = true; }
    CHECK(failed);
    CHECK(readFile(fixture.destination + "/existing") == "do not replace");
    CHECK(server.requests.isEmpty());
}

void destinationRace()
{
    Fixture fixture;
    Server server(tar(payload()));
    bool failed = false;
    try
    {
        Releases::fetch(server, server.selection(), fixture.destination, [&](const QString &message) {
            if (message.startsWith("Checking driver"))
            {
                CHECK(QDir().mkdir(fixture.destination));
                writeFile(fixture.destination + "/existing", "concurrent writer");
            }
        });
    }
    catch (const Error &) { failed = true; }
    CHECK(failed);
    CHECK(readFile(fixture.destination + "/existing") == "concurrent writer");
}

void listing()
{
    Server old(tar(payload("v1.6.0")), "v1.6.0");
    CHECK(Releases::list(old).isEmpty());
    const auto data = tar(payload("v1.8.0", true));
    auto description = manifest(data);
    auto artifact = description.value("artifacts").toArray().first().toObject();
    artifact.insert("tested_runtimes", QJsonArray{"Wine 10.15"});
    description.insert("artifacts", QJsonArray{artifact});
    Server server(data, "v1.8.0", description);
    server.metadata.insert("prerelease", true);
    const auto releases = Releases::list(server);
    CHECK(releases.size() == 1);
    CHECK(releases.first().toObject().value("prerelease").toBool());
    CHECK(releases.first().toObject().value("compatibility").toObject().value("tested_runtimes") == QJsonArray{"Wine 10.15"});
}

void manifestGeneration()
{
    Fixture fixture;
    const auto entries = payload("v1.8.0", true);
    const QString root = fixture.temporary.path() + "/build";
    for (auto it = entries.begin(); it != entries.end(); ++it)
    {
        CHECK(QDir().mkpath(QFileInfo(root + '/' + it.key()).absolutePath()));
        writeFile(root + '/' + it.key(), it.value());
    }
    const QString asset = fixture.temporary.path() + "/pipeasio-v1.8.0-archlinux-x86_64.tar.gz";
    writeFile(asset, tar(entries));
    const auto description = Releases::createManifest("v1.8.0", asset, root, "wine-10.15");
    const auto artifact = description.value("artifacts").toArray().first().toObject();
    CHECK(artifact.value("minimum_glibc") == "2.34");
    CHECK(artifact.value("minimum_pipewire") == "1.4.2");
    CHECK(artifact.value("tested_runtimes") == QJsonArray{});
    CHECK(artifact.value("libraries").toObject().value(Unixlib) == QJsonArray{"libc.so.6"});
    Server server(readFile(asset), "v1.8.0", description);
    const auto result = fixture.fetch(server);
    CHECK(readFile(fixture.destination + "/manager/pipeasio-check32.exe") == pe(0x14c, false));
    CHECK(result.value("compatibility").toObject().value("minimum_glibc") == "2.34");
}

void aarch64Payload()
{
    HostArchitecture host("aarch64");
    const auto data = tar(armPayload());
    Server server(data, "v1.8.0", manifest(data, "aarch64"), "aarch64");
    const auto releases = Releases::list(server);
    CHECK(releases.size() == 1);
    CHECK(releases.first().toObject().value("architecture") == "aarch64");
    Fixture fixture;
    const auto result = fixture.fetch(server);
    CHECK(result.value("architecture") == "aarch64");
    CHECK(readFile(fixture.destination + '/' + ArmDll) == pe(0xaa64));
    CHECK(readFile(fixture.destination + '/' + ArmEcDll) == pe(0x8664));
    CHECK(readFile(fixture.destination + '/' + ArmUnixlib) == elf(183));
    CHECK(result.value("files").toObject().contains(ArmEcDll));
    CHECK(readFile(fixture.destination + "/manager/pipeasio-check.exe") == pe(0xaa64, false));
}

void aarch64PayloadBoundaries()
{
    HostArchitecture host("aarch64");
    auto wrongPe = armPayload();
    wrongPe[ArmDll] = pe(0x8664);
    auto wrongEc = armPayload();
    wrongEc[ArmEcDll] = pe(0xaa64);
    auto wrongElf = armPayload();
    wrongElf[ArmUnixlib] = elf(62);
    auto x86Payload = armPayload();
    x86Payload.remove(ArmDll);
    x86Payload.remove(ArmUnixlib);
    x86Payload[Dll] = pe();
    x86Payload[Unixlib] = elf();
    for (const auto &entries : {wrongPe, wrongEc, wrongElf, x86Payload})
    {
        Fixture fixture;
        const auto data = tar(entries);
        Server server(data, "v1.8.0", manifest(data, "aarch64"), "aarch64");
        fixture.reject(server);
    }
    // An aarch64 release without the optional arm64ec front end still installs.
    const auto data = tar(armPayload("v1.8.0", false));
    Server server(data, "v1.8.0", manifest(data, "aarch64"), "aarch64");
    Fixture fixture;
    CHECK(!fixture.fetch(server).value("files").toObject().contains(ArmEcDll));
}

void foreignArchitectureIsFiltered()
{
    const auto data = tar(armPayload());
    Server server(data, "v1.8.0", manifest(data, "aarch64"), "aarch64");
    CHECK(Releases::list(server).isEmpty());
    Fixture fixture;
    fixture.reject(server);
}

// The published manifest merges one artifact per architecture (#23). Each host
// must see its own artifact and only that one.
void mergedManifestArchitectures()
{
    const auto x86 = tar(payload("v1.8.0", true));
    const auto arm = tar(armPayload());
    const QString armAsset = "pipeasio-v1.8.0-debian-aarch64.tar.gz";
    auto armArtifact = manifest(arm, "aarch64").value("artifacts").toArray().first().toObject();
    armArtifact.insert("asset", armAsset);
    auto merged = manifest(x86);
    merged.insert("artifacts", QJsonArray{merged.value("artifacts").toArray().first(), armArtifact});
    Server server(x86, "v1.8.0", merged);
    server.responses.insert(Download + "v1.8.0/" + armAsset, {arm});
    server.responses.insert(Download + "v1.8.0/" + armAsset + ".sha256", {(hash(arm) + "  " + armAsset + '\n').toLatin1()});
    auto assets = server.metadata.value("assets").toArray();
    assets.append(server.asset(armAsset));
    assets.append(server.asset(armAsset + ".sha256"));
    server.metadata.insert("assets", assets);
    const QMap<QString, QPair<QString, QByteArray>> hosts{
        {"x86_64", {server.name, x86}}, {"aarch64", {armAsset, arm}}};
    for (auto it = hosts.begin(); it != hosts.end(); ++it)
    {
        HostArchitecture pinned(it.key());
        const auto releases = Releases::list(server);
        CHECK(releases.size() == 1);
        const auto record = releases.first().toObject();
        CHECK(record.value("architecture") == it.key());
        CHECK(record.value("asset") == it.value().first);
        CHECK(record.value("sha256") == hash(it.value().second));
    }
}

void aarch64ManifestGeneration()
{
    HostArchitecture host("aarch64");
    Fixture fixture;
    const auto entries = armPayload();
    const QString root = fixture.temporary.path() + "/build";
    for (auto it = entries.begin(); it != entries.end(); ++it)
    {
        CHECK(QDir().mkpath(QFileInfo(root + '/' + it.key()).absolutePath()));
        writeFile(root + '/' + it.key(), it.value());
    }
    const QString asset = fixture.temporary.path() + "/pipeasio-v1.8.0-archlinux-aarch64.tar.gz";
    writeFile(asset, tar(entries));
    const auto description = Releases::createManifest("v1.8.0", asset, root, "wine-10.15", "aarch64");
    const auto artifact = description.value("artifacts").toArray().first().toObject();
    CHECK(artifact.value("architecture") == "aarch64");
    CHECK(artifact.value("files").toObject().contains(ArmEcDll));
    CHECK(artifact.value("libraries").toObject().value(ArmUnixlib) == QJsonArray{"libc.so.6"});
    Server server(readFile(asset), "v1.8.0", description, "aarch64");
    const auto result = fixture.fetch(server);
    CHECK(result.value("files").toObject().value(ArmEcDll) == hash(pe(0x8664)));
    CHECK(result.value("compatibility").toObject().value("minimum_glibc") == "2.34");
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    Testing::setArchitecture("x86_64");
    const std::pair<const char *, void (*)()> tests[]{
        {"legacy payload", legacyPayload}, {"manifest probes", manifestProbes}, {"gzip corruption", corruptArchives},
        {"unsafe archive members", unsafeMembers}, {"payload boundaries", payloadBoundaries}, {"checksums", checksumBoundaries},
        {"network bounds", networkBoundaries}, {"redirect boundaries", redirectBoundaries}, {"metadata boundaries", metadataBoundaries},
        {"existing destination", existingDestination}, {"destination race", destinationRace}, {"release listing", listing},
        {"expanded archive limit", expandedLimit}, {"unannounced download limit", unannouncedDownloadLimit},
        {"cancellation cleanup", cancellationCleanup},
        {"manifest generation", manifestGeneration},
        {"aarch64 payload", aarch64Payload}, {"aarch64 payload boundaries", aarch64PayloadBoundaries},
        {"foreign architecture is filtered", foreignArchitectureIsFiltered},
        {"aarch64 manifest generation", aarch64ManifestGeneration},
        {"merged manifest architectures", mergedManifestArchitectures}};
    int failures = 0;
    for (const auto &test : tests)
    {
        try { test.second(); }
        catch (const std::exception &error)
        {
            std::cerr << test.first << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures ? 1 : 0;
}
