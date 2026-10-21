/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QScopeGuard>
#include <QSignalSpy>
#include <QtTest>

#include "accountmanager.h"
#include "accountstate.h"
#include "common/remotepermissions.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
#include "configfile.h"
#include "creds/credentials.h"
#include "creds/httpcredentials.h"
#include "folder.h"
#include "libsync/creds/credentialmanager.h"
#include "folderman.h"
#include "servermove.h"
#include "testutils/testutils.h"

#include <syncenginetestutils.h>

using namespace OCC;

namespace {

// Fähigkeiten eines ownCloud-10-Servers (owncloud.online): ohne Spaces
QVariantMap oc10Capabilities()
{
    auto caps = TestUtils::testCapabilities();
    caps.remove(QStringLiteral("spaces"));
    return caps;
}

const QByteArray statusOk = "{\"installed\":true,\"maintenance\":false,\"needsDbUpgrade\":false,\"version\":\"11.0.0.0\",\"versionstring\":\"11.0.0\",\"edition\":\"Community\",\"productname\":\"owncloud.online\",\"product\":\"owncloud.online\"}";

// Konto mit Benutzername und Kennwort: das Kennwort liegt im Speicher statt im Schlüsselbund, das Konto bleibt ohne Netz
class MemoryHttpCredentials : public HttpCredentials
{
public:
    using HttpCredentials::HttpCredentials;

    OCC::AccessManager *createAccessManager() const override { return new FakeAM({}, nullptr); }
    void askFromUser() override { ++asked; }

    QStringList stored;
    int asked = 0;

protected:
    void readPassword(const ReadCallback &done) override { done(KeychainResult::NotFound, {}, {}); }
    void storePassword(const QString &password, const StoreCallback &done) override
    {
        stored.append(password);
        done(true, {});
    }
    void removePassword() override { }
};

ServerStatusCheck::Result runStatusCheck(QNetworkAccessManager *accessManager, const QUrl &url)
{
    ServerStatusCheck check(accessManager, nullptr);
    QSignalSpy spy(&check, &ServerStatusCheck::finished);
    check.start(url);
    if (!spy.wait()) {
        return {};
    }
    return spy.first().first().value<ServerStatusCheck::Result>();
}

FolderDefinition oc10FolderDefinition(Account *account, const QString &localPath, const QString &remotePath)
{
    FolderDefinition definition(account->davUrl(), {}, {});
    definition.setLocalPath(localPath);
    definition.setTargetPath(remotePath);
    definition.setJournalPath(SyncJournalDb::makeDbName(definition.localPath()));
    return definition;
}

// ein synchronisierter Eintrag im Journal, wie ihn ein ownCloud-10-Server mit der Instanz @p instanceId hinterlässt
void addSyncedFile(Folder *folder, const QByteArray &path, const QByteArray &instanceId)
{
    SyncJournalFileRecord record;
    record._path = path;
    record._type = ItemTypeFile;
    record._etag = "etag";
    record._fileId = "00000042" + instanceId;
    record._modtime = 1700000000;
    record._fileSize = 5;
    record._inode = 4711;
    // setFileRecord() verlangt Rechte (OC_ASSERT, in Beta-Builds aktiv)
    record._remotePerm = RemotePermissions::fromDbValue("RW");
    QVERIFY(folder->journalDb()->setFileRecord(record));
}

// selbst signiert, nur für die Tests (CN=neu.example.org)
QSslCertificate selfSignedCertificate()
{
    static const QByteArray pem = "-----BEGIN CERTIFICATE-----\n"
                                  "MIIBijCCATGgAwIBAgIUYrW52114yWEhmgWAYPFQAY8YTYgwCgYIKoZIzj0EAwIw\n"
                                  "GjEYMBYGA1UEAwwPbmV1LmV4YW1wbGUub3JnMCAXDTI2MTAxOTE4NTc0MVoYDzIx\n"
                                  "MjYwOTI1MTg1NzQxWjAaMRgwFgYDVQQDDA9uZXUuZXhhbXBsZS5vcmcwWTATBgcq\n"
                                  "hkjOPQIBBggqhkjOPQMBBwNCAATW8SeLnHVWLUb7UK5oMQVgHYInOErX0gXkfNWh\n"
                                  "72JFrXCT8xmOH/CcHOs20l2wtk+HGVbbgOiQAq+ubc/S/s0Io1MwUTAdBgNVHQ4E\n"
                                  "FgQUiCKejgIlpAic1cgKOkfEN6lYjOswHwYDVR0jBBgwFoAUiCKejgIlpAic1cgK\n"
                                  "OkfEN6lYjOswDwYDVR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNHADBEAiB+OXim\n"
                                  "bL7QTMkKD+U1KIC0qsZ4v1In6909TD5N5w14OwIgGO9Nw7FNLc2uQ/MWKF0EIsVj\n"
                                  "OZBe8yMJ3vRWj9t5sxo=\n"
                                  "-----END CERTIFICATE-----\n";
    return QSslCertificate(pem, QSsl::Pem);
}

// zeigt, welche TLS-Fehler die Handler am Netzwerkzugang zulassen
class TlsProbeReply : public QNetworkReply
{
public:
    explicit TlsProbeReply(const QUrl &url) { setUrl(url); }
    void abort() override { }

    QList<QSslError> ignored;

protected:
    qint64 readData(char *, qint64) override { return 0; }
    void ignoreSslErrorsImplementation(const QList<QSslError> &errors) override { ignored = errors; }
};

// Netzwerkzugang einer neuen Adresse mit dem Zertifikatsfehler @p tlsErrors: Jede Anfrage scheitert wie ein echter
// Verbindungsaufbau, solange niemand die Fehler zulässt. status.php antwortet, PROPFIND beantwortet FakeAM mit der
// Datei-ID von @p server, die Anmeldung im Browser bleibt hängen.
FakeAM *serverWithTlsErrors(const FileInfo &server, const QList<QSslError> &tlsErrors, int *statusRequests)
{
    auto *accessManager = new FakeAM(server, nullptr);
    accessManager->setOverride([accessManager, tlsErrors, statusRequests](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
        const bool status = request.url().path().endsWith(QLatin1String("/status.php"));
        if (status) {
            ++*statusRequests;
        }
        TlsProbeReply probe(request.url());
        Q_EMIT accessManager->sslErrors(&probe, tlsErrors);
        if (probe.ignored != tlsErrors) {
            return new FakeErrorReply(op, request, accessManager, 0);
        }
        if (status) {
            return new FakePayloadReply(op, request, statusOk, accessManager);
        }
        if (request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == "PROPFIND") {
            return nullptr;
        }
        return new FakeHangingReply(op, request, accessManager);
    });
    return accessManager;
}

// OAuth2-Zugangsdaten mit Schlüsselbund im Speicher und gefälschtem Netzwerkzugang: zeigt, mit welchem Ablauf die Token
// nach dem Umzug gelten
class InMemoryCredentials : public Credentials
{
public:
    explicit InMemoryCredentials(Account *account)
        : Credentials(account)
    {
    }

    using Credentials::_accessTokenStaleAt;

    OCC::AccessManager *createAccessManager() const override { return new FakeAM(FileInfo(), const_cast<InMemoryCredentials *>(this)); }

protected:
    void readRefreshToken(const ReadCallback &done) override { done(KeychainResult::NotFound, {}, QStringLiteral("no entry")); }
    void storeRefreshToken(const QString &, const StoreCallback &done) override
    {
        QTimer::singleShot(0, this, [done] { done(true, {}); });
    }
    void removeRefreshToken() override { }
};
}

class TestServerMove : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { qRegisterMetaType<ServerStatusCheck::Result>(); }

    void testNormalizeServerUrl_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");

        QTest::newRow("host only") << QStringLiteral("cloud.example.org") << QStringLiteral("https://cloud.example.org");
        QTest::newRow("spaces around") << QStringLiteral("  https://cloud.example.org/  ") << QStringLiteral("https://cloud.example.org");
        QTest::newRow("subfolder") << QStringLiteral("https://example.org/owncloud/") << QStringLiteral("https://example.org/owncloud");
        QTest::newRow("status.php") << QStringLiteral("https://example.org/oc/status.php") << QStringLiteral("https://example.org/oc");
        QTest::newRow("index.php") << QStringLiteral("https://example.org/index.php") << QStringLiteral("https://example.org");
        QTest::newRow("double slashes") << QStringLiteral("https://example.org//oc//") << QStringLiteral("https://example.org/oc");
        QTest::newRow("port") << QStringLiteral("example.org:8443") << QStringLiteral("https://example.org:8443");
        QTest::newRow("upper case scheme and host") << QStringLiteral("HTTPS://Cloud.Example.ORG") << QStringLiteral("https://cloud.example.org");
        // derselbe Server wie ohne Port: sonst folgte ein sinnloser Umzug mit Neuanmeldung
        QTest::newRow("explicit default port") << QStringLiteral("https://example.org:443/oc/") << QStringLiteral("https://example.org/oc");
        // die Anmeldung (OAuth2) verlangt https: http gleich ablehnen, nicht erst im Browser scheitern
        QTest::newRow("http") << QStringLiteral("http://intranet.local") << QString();
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("credentials") << QStringLiteral("https://user:secret@example.org") << QString();
        QTest::newRow("query") << QStringLiteral("https://example.org/?a=b") << QString();
        QTest::newRow("fragment") << QStringLiteral("https://example.org/#x") << QString();
        QTest::newRow("other scheme") << QStringLiteral("ftp://example.org") << QString();
        QTest::newRow("no host") << QStringLiteral("https://") << QString();
    }

    void testNormalizeServerUrl()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);

        QString error;
        const QUrl url = ServerMove::normalizeServerUrl(input, &error);
        QCOMPARE(url.toString(), expected);
        // ohne Ergebnis immer mit Begründung
        QCOMPARE(error.isEmpty(), !expected.isEmpty());
    }

    void testIsSafeRedirect()
    {
        const QUrl https(QStringLiteral("https://cloud.example.org/status.php"));
        const QUrl http(QStringLiteral("http://cloud.example.org/status.php"));
        QVERIFY(ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://cloud.example.org/owncloud/status.php"))));
        QVERIFY(ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://CLOUD.example.org/status.php"))));
        QVERIFY(ServerMove::isSafeRedirect(http, QUrl(QStringLiteral("https://cloud.example.org/status.php"))));
        // nie zu HTTP, nie auf einen anderen Host oder Port, keine Zugangsdaten
        QVERIFY(!ServerMove::isSafeRedirect(https, http));
        QVERIFY(!ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://evil.example.com/status.php"))));
        QVERIFY(!ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://cloud.example.org.evil.com/status.php"))));
        QVERIFY(!ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://cloud.example.org:8443/status.php"))));
        QVERIFY(!ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://user:pw@cloud.example.org/status.php"))));
        QVERIFY(!ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("ftp://cloud.example.org/status.php"))));
    }

    void testServerUrlFromStatusUrl()
    {
        QCOMPARE(ServerMove::serverUrlFromStatusUrl(QUrl(QStringLiteral("https://neu.example.org/oc/status.php"))), QUrl(QStringLiteral("https://neu.example.org/oc")));
        QCOMPARE(ServerMove::serverUrlFromStatusUrl(QUrl(QStringLiteral("https://neu.example.org/status.php?x=1"))), QUrl(QStringLiteral("https://neu.example.org")));
        // „//“ vom Server darf nicht bis zu concatUrlPath() durchschlagen
        QCOMPARE(ServerMove::serverUrlFromStatusUrl(QUrl(QStringLiteral("https://neu.example.org//oc//status.php"))), QUrl(QStringLiteral("https://neu.example.org/oc")));
        QCOMPARE(ServerMove::serverUrlFromStatusUrl(QUrl(QStringLiteral("https://neu.example.org/oc/index.php"))), QUrl());
        QCOMPARE(ServerMove::serverUrlFromStatusUrl(QUrl()), QUrl());
    }

    void testInstanceIdFromFileId()
    {
        QCOMPARE(ServerMove::instanceIdFromFileId("00000123oc1a2b3c4d5e"), QStringLiteral("oc1a2b3c4d5e"));
        QCOMPARE(ServerMove::instanceIdFromFileId("123456789ocabcdefghij"), QStringLiteral("ocabcdefghij"));
        // keine ownCloud-10-ID: zu kurz, ohne Instanz, oCIS-Format, leer
        QCOMPARE(ServerMove::instanceIdFromFileId("1234567ocabc"), QString());
        QCOMPARE(ServerMove::instanceIdFromFileId("00000123"), QString());
        QCOMPARE(ServerMove::instanceIdFromFileId("storage$space!opaque"), QString());
        QCOMPARE(ServerMove::instanceIdFromFileId("00000123oc$bad"), QString());
        QCOMPARE(ServerMove::instanceIdFromFileId(QByteArray()), QString());
    }

    void testFileIdFromPropfind()
    {
        const QByteArray xml = "<?xml version=\"1.0\"?>\n"
            "<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">\n"
            " <d:response><d:href>/remote.php/dav/files/admin/</d:href>\n"
            "  <d:propstat><d:prop><oc:id>00000005ocabcdefghij</oc:id></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>\n"
            " </d:response>\n"
            "</d:multistatus>";
        QCOMPARE(ServerMove::fileIdFromPropfind(xml), QByteArray("00000005ocabcdefghij"));
        QCOMPARE(ServerMove::fileIdFromPropfind("<d:multistatus xmlns:d=\"DAV:\"/>"), QByteArray());
        QCOMPARE(ServerMove::fileIdFromPropfind("kein xml"), QByteArray());
    }

    void testRebaseUrl()
    {
        const QUrl oldBase(QStringLiteral("https://alt.example.org/owncloud"));
        const QUrl newBase(QStringLiteral("https://neu.example.org"));
        QCOMPARE(ServerMove::rebaseUrl(QUrl(QStringLiteral("https://alt.example.org/owncloud/remote.php/dav/files/admin/")), oldBase, newBase),
            QUrl(QStringLiteral("https://neu.example.org/remote.php/dav/files/admin/")));
        QCOMPARE(ServerMove::rebaseUrl(QUrl(QStringLiteral("https://alt.example.org:443/owncloud/remote.php/dav/files/admin")), oldBase,
                     QUrl(QStringLiteral("https://neu.example.org/oc/"))),
            QUrl(QStringLiteral("https://neu.example.org/oc/remote.php/dav/files/admin")));
        // nicht unter der alten Adresse
        QVERIFY(!ServerMove::rebaseUrl(QUrl(QStringLiteral("https://alt.example.org/owncloud2/remote.php")), oldBase, newBase));
        QVERIFY(!ServerMove::rebaseUrl(QUrl(QStringLiteral("https://anders.example.org/owncloud/remote.php")), oldBase, newBase));
        QVERIFY(!ServerMove::rebaseUrl(QUrl(QStringLiteral("http://alt.example.org/owncloud/remote.php")), oldBase, newBase));
    }

    void testStatusCheck_data()
    {
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("ok");

        QTest::newRow("ok") << 200 << statusOk << true;
        QTest::newRow("installed as string") << 200 << QByteArray("{\"installed\":\"true\",\"maintenance\":\"false\",\"versionstring\":\"10.15.0\"}") << true;
        QTest::newRow("maintenance") << 200 << QByteArray("{\"installed\":true,\"maintenance\":true}") << false;
        QTest::newRow("upgrade pending") << 200 << QByteArray("{\"installed\":true,\"maintenance\":false,\"needsDbUpgrade\":true}") << false;
        QTest::newRow("not installed") << 200 << QByteArray("{\"installed\":false}") << false;
        QTest::newRow("no status") << 200 << QByteArray("<html>Hallo</html>") << false;
        QTest::newRow("not found") << 404 << QByteArray("not found") << false;
    }

    void testStatusCheck()
    {
        QFETCH(int, httpStatus);
        QFETCH(QByteArray, body);
        QFETCH(bool, ok);

        FakeAM accessManager({}, nullptr);
        QList<QNetworkRequest> requests;
        accessManager.setOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            requests.append(request);
            if (httpStatus == 200) {
                return new FakePayloadReply(op, request, body, &accessManager);
            }
            return new FakeErrorReply(op, request, &accessManager, httpStatus, body);
        });

        const auto result = runStatusCheck(&accessManager, QUrl(QStringLiteral("https://neu.example.org/owncloud")));
        QCOMPARE(result.ok, ok);
        QCOMPARE(result.error.isEmpty(), ok);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.first().url(), QUrl(QStringLiteral("https://neu.example.org/owncloud/status.php")));
        // ohne Zugangsdaten und ohne automatische Weiterleitungen
        QVERIFY(!requests.first().hasRawHeader("Authorization"));
        QCOMPARE(requests.first().attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), static_cast<int>(QNetworkRequest::ManualRedirectPolicy));
        if (ok) {
            QCOMPARE(result.serverUrl, QUrl(QStringLiteral("https://neu.example.org/owncloud")));
        }
    }

    // Weiterleitungen auf demselben Host (hier HTTP zu HTTPS und in einen Unterordner) gelten
    void testStatusCheckFollowsSameHostRedirect()
    {
        FakeAM accessManager({}, nullptr);
        QStringList requested;
        accessManager.setOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            requested.append(request.url().toString());
            if (request.url() == QUrl(QStringLiteral("http://neu.example.org/status.php"))) {
                return new FakeRedirectReply(op, request, &accessManager, 301, QUrl(QStringLiteral("https://neu.example.org/status.php")));
            }
            if (request.url() == QUrl(QStringLiteral("https://neu.example.org/status.php"))) {
                // relativ angegeben
                return new FakeRedirectReply(op, request, &accessManager, 302, QUrl(QStringLiteral("/owncloud/status.php")));
            }
            return new FakePayloadReply(op, request, statusOk, &accessManager);
        });

        const auto result = runStatusCheck(&accessManager, QUrl(QStringLiteral("http://neu.example.org")));
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.serverUrl, QUrl(QStringLiteral("https://neu.example.org/owncloud")));
        QCOMPARE(result.version, QStringLiteral("11.0.0"));
        QCOMPARE(requested.size(), 3);
    }

    // eine Weiterleitung auf einen anderen Host wird gemeldet, aber nicht befolgt
    void testStatusCheckRefusesForeignRedirect()
    {
        FakeAM accessManager({}, nullptr);
        QStringList hosts;
        accessManager.setOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            hosts.append(request.url().host());
            if (request.url().host() == QLatin1String("neu.example.org")) {
                return new FakeRedirectReply(op, request, &accessManager, 308, QUrl(QStringLiteral("https://evil.example.com/oc/status.php")));
            }
            return new FakePayloadReply(op, request, statusOk, &accessManager);
        });

        const auto result = runStatusCheck(&accessManager, QUrl(QStringLiteral("https://neu.example.org")));
        QVERIFY(!result.ok);
        QCOMPARE(result.foreignRedirect, QUrl(QStringLiteral("https://evil.example.com/oc")));
        QVERIFY(result.error.contains(QStringLiteral("evil.example.com")));
        QCOMPARE(hosts, QStringList{QStringLiteral("neu.example.org")});
    }

    void testStatusCheckRedirectLimit()
    {
        FakeAM accessManager({}, nullptr);
        int requests = 0;
        accessManager.setOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            ++requests;
            // im Kreis: /a → /b → /a …
            const bool atA = request.url().path().startsWith(QLatin1String("/a/"));
            return new FakeRedirectReply(op, request, &accessManager, 307, QUrl(atA ? QStringLiteral("/b/status.php") : QStringLiteral("/a/status.php")));
        });

        const auto result = runStatusCheck(&accessManager, QUrl(QStringLiteral("https://neu.example.org/a")));
        QVERIFY(!result.ok);
        QCOMPARE(requests, ServerStatusCheck::maxRedirects + 1);
    }

    // Umstellen: Konto, Ordnerpaare, Journale und Einstellungen ziehen mit, nichts entsteht neu
    void testApplyNewServerUrl()
    {
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QDir dir2(dir.path());
        QVERIFY(dir2.mkpath(QStringLiteral("docs")));
        QVERIFY(dir2.mkpath(QStringLiteral("photos")));
        const QString dirPath = dir2.canonicalPath();

        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        const QUuid uuid = account->uuid();
        FolderMan *folderMan = TestUtils::folderMan();

        Folder *docs = folderMan->addFolder(accountState.get(), oc10FolderDefinition(account, dirPath + QStringLiteral("/docs"), QStringLiteral("/A")));
        Folder *photos = folderMan->addFolder(accountState.get(), oc10FolderDefinition(account, dirPath + QStringLiteral("/photos"), QStringLiteral("/B")));
        QVERIFY(docs && photos);
        addSyncedFile(docs, "a1", "ocinstance01");
        docs->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, {QStringLiteral("sub/")});
        const QByteArray docsId = docs->id();

        const QUrl newUrl(QStringLiteral("https://neu.example.org/owncloud"));
        QCOMPARE(ServerMove::applyNewServerUrl(accountState.get(), newUrl, QStringLiteral("token"), QStringLiteral("refresh")), QString());

        QCOMPARE(account->url(), newUrl);
        QCOMPARE(account->davUrl(), QUrl(QStringLiteral("https://neu.example.org/owncloud/remote.php/dav/files/admin/")));

        const auto folders = folderMan->foldersForAccount(uuid);
        QCOMPARE(folders.size(), 2);
        Folder *newDocs = nullptr;
        for (Folder *folder : folders) {
            QVERIFY2(folder->webDavUrl().toString().startsWith(newUrl.toString() + QLatin1String("/remote.php/dav/files/admin")),
                qPrintable(folder->webDavUrl().toString()));
            if (folder->id() == docsId) {
                newDocs = folder;
            }
        }
        QVERIFY(newDocs);
        QCOMPARE(newDocs->remotePath(), QStringLiteral("/A"));
        QCOMPARE(newDocs->path(), dirPath + QStringLiteral("/docs/"));

        // das Journal bleibt: synchronisierte Einträge und Auswahl-Synchronisation sind noch da
        SyncJournalFileRecord record;
        QVERIFY(newDocs->journalDb()->getFileRecord(QByteArrayLiteral("a1"), &record));
        QVERIFY(record.isValid());
        QCOMPARE(record._fileId, QByteArray("00000042ocinstance01"));
        bool ok = false;
        QCOMPARE(newDocs->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, ok), QSet<QString>{QStringLiteral("sub/")});
        QVERIFY(ok);

        // die Einstellungen tragen die neue Adresse
        auto settings = ConfigFile::makeQSettings();
        settings.beginGroup(QStringLiteral("Accounts/%1").arg(account->groupIndex()));
        QCOMPARE(settings.value(QStringLiteral("url")).toUrl(), newUrl);
        settings.beginGroup(QStringLiteral("Folders/%1").arg(QString::fromUtf8(docsId)));
        QVERIFY(settings.value(QStringLiteral("davUrl")).toUrl().toString().startsWith(newUrl.toString()));
    }

    // derselbe Benutzer an einer anderen Installation: nichts wird umgestellt
    void testVerifyRejectsOtherInstance()
    {
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("docs")));

        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        const QUrl oldUrl = account->url();
        Folder *docs = TestUtils::folderMan()->addFolder(
            accountState.get(), oc10FolderDefinition(account, QDir(dir.path()).canonicalPath() + QStringLiteral("/docs"), QStringLiteral("/")));
        QVERIFY(docs);
        addSyncedFile(docs, "a1", "ocinstance01");
        QCOMPARE(ServerMove::knownInstanceId(account->uuid()), QStringLiteral("ocinstance01"));

        FileInfo otherServer = FileInfo::A12_B12_C12_S12();
        otherServer.fileId = "00000001ocother00001";
        auto *otherAm = new FakeAM(otherServer, nullptr);
        QList<QNetworkRequest> requests;
        otherAm->setOverride([&requests](QNetworkAccessManager::Operation, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            requests.append(request);
            return nullptr;
        });

        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([otherAm](QObject *parent) -> QNetworkAccessManager * {
            otherAm->setParent(parent);
            return otherAm;
        });
        QSignalSpy failed(&controller, &ServerMoveController::failed);
        QSignalSpy succeeded(&controller, &ServerMoveController::succeeded);
        controller.verifyAndApply(QUrl(QStringLiteral("https://neu.example.org/owncloud")), QStringLiteral("new-token"), QStringLiteral("new-refresh"));
        QVERIFY(failed.wait());
        QCOMPARE(succeeded.size(), 0);
        QCOMPARE(account->url(), oldUrl);

        // geprüft wurde mit dem neuen Token an der neuen Adresse, ohne Weiterleitungen
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.first().url().host(), QStringLiteral("neu.example.org"));
        QCOMPARE(requests.first().rawHeader("Authorization"), QByteArray("Bearer new-token"));
        QCOMPARE(requests.first().attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), static_cast<int>(QNetworkRequest::ManualRedirectPolicy));
    }

    void testVerifyAcceptsSameInstance()
    {
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("docs")));

        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        Folder *docs = TestUtils::folderMan()->addFolder(
            accountState.get(), oc10FolderDefinition(account, QDir(dir.path()).canonicalPath() + QStringLiteral("/docs"), QStringLiteral("/")));
        QVERIFY(docs);
        addSyncedFile(docs, "a1", "ocinstance01");

        FileInfo sameServer = FileInfo::A12_B12_C12_S12();
        sameServer.fileId = "00000001ocinstance01";
        auto *sameAm = new FakeAM(sameServer, nullptr);

        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([sameAm](QObject *parent) -> QNetworkAccessManager * {
            sameAm->setParent(parent);
            return sameAm;
        });
        QSignalSpy failed(&controller, &ServerMoveController::failed);
        QSignalSpy succeeded(&controller, &ServerMoveController::succeeded);
        const QUrl newUrl(QStringLiteral("https://neu.example.org/owncloud"));
        controller.verifyAndApply(newUrl, QStringLiteral("new-token"), QStringLiteral("new-refresh"));
        QVERIFY(succeeded.wait());
        QCOMPARE(failed.size(), 0);
        QCOMPARE(account->url(), newUrl);
        QCOMPARE(TestUtils::folderMan()->foldersForAccount(account->uuid()).size(), 1);
    }

    // Nach dem Umzug gilt der Ablauf des Access-Tokens aus der Anmeldung an der neuen Adresse (expires_in): Das Konto erneuert
    // vorbeugend wie nach jeder Anmeldung, nicht erst nach einer Ablehnung (401). Lief er bis zum Umstellen schon ab (Prüfung,
    // Warten auf Synchronisationen), gilt er gleich als abgelaufen.
    void testMoveKeepsTheTokenExpiry_data()
    {
        QTest::addColumn<qint64>("lifetime");
        QTest::newRow("valid") << qint64(3600);
        QTest::newRow("already expired") << qint64(-5);
    }

    void testMoveKeepsTheTokenExpiry()
    {
        // Arrange
        QFETCH(qint64, lifetime);
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("docs")));
        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        auto *credentials = new InMemoryCredentials(account);
        account->setCredentials(credentials);
        Folder *docs = TestUtils::folderMan()->addFolder(
            accountState.get(), oc10FolderDefinition(account, QDir(dir.path()).canonicalPath() + QStringLiteral("/docs"), QStringLiteral("/")));
        QVERIFY(docs);
        addSyncedFile(docs, "a1", "ocinstance01");
        FileInfo sameServer = FileInfo::A12_B12_C12_S12();
        sameServer.fileId = "00000001ocinstance01";
        auto *sameAm = new FakeAM(sameServer, nullptr);
        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([sameAm](QObject *parent) -> QNetworkAccessManager * {
            sameAm->setParent(parent);
            return sameAm;
        });
        QSignalSpy succeeded(&controller, &ServerMoveController::succeeded);
        QSignalSpy fetched(credentials, &AbstractCredentials::fetched);
        const qint64 expiresAt = QDateTime::currentSecsSinceEpoch() + lifetime;

        // Act
        controller.verifyAndApply(QUrl(QStringLiteral("https://neu.example.org/owncloud")), QStringLiteral("new-token"), QStringLiteral("new-refresh"), expiresAt);

        // Assert: gilt eine Minute vor dem Ablauf als abgelaufen, ein schon abgelaufener gleich
        QVERIFY(succeeded.wait());
        QTRY_COMPARE(fetched.size(), 1);
        const qint64 expectedStaleAt = lifetime > 0 ? expiresAt - 60 : QDateTime::currentSecsSinceEpoch() + 1;
        QVERIFY2(qAbs(credentials->_accessTokenStaleAt - expectedStaleAt) <= 2,
            qPrintable(QStringLiteral("stale at %1, expected %2").arg(credentials->_accessTokenStaleAt).arg(expectedStaleAt)));
    }

    // Konto mit Benutzername und Kennwort (Server ohne die App „OAuth2“): Die neue Adresse verlangt Basic, also fragt der
    // Ablauf nach dem Kennwort des Anmeldenamens. Ein falsches fragt erneut, das richtige prüft Benutzer und Instanz mit
    // Basic-Kopf und stellt um; das Kennwort liegt danach unter der neuen Adresse.
    void testMoveWithPassword()
    {
        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        auto *credentials = new MemoryHttpCredentials(account, QStringLiteral("anna"), QStringLiteral("old-password"));
        account->setCredentials(credentials);

        FileInfo sameServer = FileInfo::A12_B12_C12_S12();
        sameServer.fileId = "00000001ocinstance01";
        auto *accessManager = new FakeAM(sameServer, nullptr);
        QList<QByteArray> verifications;
        accessManager->setOverride(
            [accessManager, &verifications](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
                const QString path = request.url().path();
                if (path.endsWith(QLatin1String("/status.php"))) {
                    return new FakePayloadReply(op, request, statusOk, accessManager);
                }
                if (path.endsWith(QLatin1String("/.well-known/openid-configuration"))) {
                    return new FakeErrorReply(op, request, accessManager, 404);
                }
                if (path.endsWith(QLatin1String("/remote.php/webdav/"))) {
                    // ohne Zugangsdaten: der Server nennt nur Basic
                    auto *reply = new FakeErrorReply(op, request, accessManager, 401);
                    reply->setRawHeader(QByteArrayLiteral("WWW-Authenticate"), QByteArrayLiteral("Basic realm=\"owncloud.online\""));
                    return reply;
                }
                verifications.append(request.rawHeader("Authorization"));
                if (request.rawHeader("Authorization") != basicAuthorizationHeader(QStringLiteral("anna"), QStringLiteral("new-password"))) {
                    return new FakeErrorReply(op, request, accessManager, 401);
                }
                // PROPFIND mit der Datei-ID wie ein echter Server
                return nullptr;
            });
        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([accessManager](QObject *parent) -> QNetworkAccessManager * {
            accessManager->setParent(parent);
            return accessManager;
        });
        QSignalSpy passwordRequired(&controller, &ServerMoveController::passwordRequired);
        QSignalSpy failed(&controller, &ServerMoveController::failed);
        QSignalSpy succeeded(&controller, &ServerMoveController::succeeded);

        const QUrl newUrl(QStringLiteral("https://neu.example.org/owncloud"));
        controller.start(newUrl.toString());
        QVERIFY(passwordRequired.wait());
        QCOMPARE(passwordRequired.last().first().toString(), QStringLiteral("anna"));
        QVERIFY(controller.isBusy());

        // vertippt: noch einmal fragen, nichts umstellen
        controller.signInWithPassword(QStringLiteral("wrong"));
        QVERIFY(passwordRequired.wait());
        QCOMPARE(failed.size(), 0);
        QVERIFY(account->url() != newUrl);

        controller.signInWithPassword(QStringLiteral("new-password"));
        QVERIFY(succeeded.wait());
        QCOMPARE(failed.size(), 0);
        QCOMPARE(account->url(), newUrl);
        QCOMPARE(verifications,
            (QList<QByteArray>{basicAuthorizationHeader(QStringLiteral("anna"), QStringLiteral("wrong")),
                basicAuthorizationHeader(QStringLiteral("anna"), QStringLiteral("new-password"))}));
        QCOMPARE(credentials->stored, QStringList{QStringLiteral("new-password")});
        QVERIFY(credentials->ready());
        QVERIFY(!controller.isBusy());
    }

    // Ein Konto mit Kennwort zieht nicht an eine Adresse um, die im Browser anmeldet
    void testMoveWithPasswordRejectsBrowserSignIn()
    {
        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        account->setCredentials(new MemoryHttpCredentials(account, QStringLiteral("anna"), QStringLiteral("old-password")));
        const QUrl oldUrl = account->url();

        auto *accessManager = new FakeAM({}, nullptr);
        accessManager->setOverride([accessManager](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (request.url().path().endsWith(QLatin1String("/status.php"))) {
                return new FakePayloadReply(op, request, statusOk, accessManager);
            }
            auto *reply = new FakeErrorReply(op, request, accessManager, 401);
            reply->setRawHeader(QByteArrayLiteral("WWW-Authenticate"), QByteArrayLiteral("Bearer realm=\"owncloud.online\""));
            return reply;
        });
        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([accessManager](QObject *parent) -> QNetworkAccessManager * {
            accessManager->setParent(parent);
            return accessManager;
        });
        QSignalSpy passwordRequired(&controller, &ServerMoveController::passwordRequired);
        QSignalSpy failed(&controller, &ServerMoveController::failed);
        controller.start(QStringLiteral("https://neu.example.org/owncloud"));
        QVERIFY(failed.wait());
        QCOMPARE(passwordRequired.size(), 0);
        QCOMPARE(account->url(), oldUrl);
        QVERIFY(!controller.isBusy());
    }

    // Eingaben, die gar nicht erst an einen Server gehen
    void testStartRejectsInvalidTargets()
    {
        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});

        int requests = 0;
        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([&requests](QObject *parent) -> QNetworkAccessManager * {
            auto *accessManager = new FakeAM({}, parent);
            accessManager->setOverride([&requests](QNetworkAccessManager::Operation, const QNetworkRequest &, QIODevice *) -> QNetworkReply * {
                ++requests;
                return nullptr;
            });
            return accessManager;
        });
        QSignalSpy failed(&controller, &ServerMoveController::failed);

        // dieselbe Adresse wie bisher
        account->setUrl(QUrl(QStringLiteral("https://alt.example.org/owncloud")));
        controller.start(QStringLiteral("https://alt.example.org:443/owncloud/"));
        QCOMPARE(failed.size(), 1);
        // Unsinn
        controller.start(QStringLiteral("https://user:pw@neu.example.org"));
        QCOMPARE(failed.size(), 2);
        // von https zu http: nie eine unverschlüsselte Verbindung
        controller.start(QStringLiteral("http://neu.example.org/owncloud"));
        QCOMPARE(failed.size(), 3);
        QVERIFY(failed.last().first().toString().contains(QStringLiteral("https://")));
        QCOMPARE(requests, 0);
        QVERIFY(!controller.isBusy());
        QCOMPARE(account->url(), QUrl(QStringLiteral("https://alt.example.org/owncloud")));
    }

    // ein zweites Konto mit derselben Adresse und demselben Benutzer: kein Umzug dorthin
    void testStartRejectsDuplicateAccount()
    {
        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        auto otherState = createDummyAccount();
        const QUrl target(QStringLiteral("https://neu.example.org/owncloud"));
        otherState->account()->setUrl(target);
        QVERIFY(AccountManager::instance()->accountState(otherState->account()->uuid()));

        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([](QObject *parent) -> QNetworkAccessManager * {
            auto *accessManager = new FakeAM({}, parent);
            accessManager->setOverride([accessManager](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
                return new FakePayloadReply(op, request, statusOk, accessManager);
            });
            return accessManager;
        });
        QSignalSpy failed(&controller, &ServerMoveController::failed);
        controller.start(target.toString());
        QVERIFY(failed.wait());
        QVERIFY(!controller.isBusy());
        QVERIFY(account->url() != target);
    }

    // nach einem Umzug steht die Adresse ggf. ohne „/“ am Ende: Der Assistent erkennt das Konto trotzdem als eingerichtet
    void testAccountForLoginExistsAfterMove()
    {
        auto state = createDummyAccount();
        Account *account = state->account();
        account->setUrl(QUrl(QStringLiteral("https://cloud.example.org")));
        const QString user = account->davUser();
        auto *manager = AccountManager::instance();
        QVERIFY(manager->accountForLoginExists(QUrl(QStringLiteral("https://cloud.example.org/")), user));
        QVERIFY(manager->accountForLoginExists(QUrl(QStringLiteral("https://CLOUD.example.org")), user.toUpper()));
        QVERIFY(!manager->accountForLoginExists(QUrl(QStringLiteral("https://cloud.example.org/andere")), user));
        QVERIFY(!manager->accountForLoginExists(QUrl(QStringLiteral("https://cloud.example.org/")), user + QStringLiteral("x")));
    }

    // Antworten der neuen Adresse auf die Prüfung, die den Umzug stoppen
    void testVerifyRejectsServerAnswers_data()
    {
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<QByteArray>("rootFileId");

        QTest::newRow("not signed in") << 401 << QByteArray();
        QTest::newRow("forbidden") << 403 << QByteArray();
        QTest::newRow("user unknown") << 404 << QByteArray();
        QTest::newRow("redirect") << 301 << QByteArray();
        QTest::newRow("server error") << 500 << QByteArray();
        // ohne synchronisierte Einträge: trotzdem nur eine ownCloud-10-Installation, kein Infinite Scale
        QTest::newRow("no oc10 instance") << 207 << QByteArray("storage$space!opaque");
    }

    void testVerifyRejectsServerAnswers()
    {
        QFETCH(int, httpStatus);
        QFETCH(QByteArray, rootFileId);

        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        const QUrl oldUrl = account->url();
        QVERIFY(ServerMove::knownInstanceId(account->uuid()).isEmpty());

        FileInfo server = FileInfo::A12_B12_C12_S12();
        server.fileId = rootFileId;
        auto *accessManager = new FakeAM(server, nullptr);
        if (httpStatus != 207) {
            accessManager->setOverride([httpStatus, accessManager](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
                if (httpStatus == 301) {
                    return new FakeRedirectReply(op, request, accessManager, 301, QUrl(QStringLiteral("https://anders.example.org/remote.php/dav/files/admin/")));
                }
                return new FakeErrorReply(op, request, accessManager, httpStatus);
            });
        }
        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([accessManager](QObject *parent) -> QNetworkAccessManager * {
            accessManager->setParent(parent);
            return accessManager;
        });
        QSignalSpy failed(&controller, &ServerMoveController::failed);
        QSignalSpy succeeded(&controller, &ServerMoveController::succeeded);
        controller.verifyAndApply(QUrl(QStringLiteral("https://neu.example.org/owncloud")), QStringLiteral("new-token"), QStringLiteral("new-refresh"));
        QVERIFY(failed.wait());
        QCOMPARE(succeeded.size(), 0);
        QVERIFY(!failed.first().first().toString().isEmpty());
        QCOMPARE(account->url(), oldUrl);
        QVERIFY(!controller.isBusy());
    }

    // Nach dem Umzug liegt der neue Token unter dem Schlüssel des neuen Hosts (der Schlüsselbund ist nach Host getrennt)
    void testReplaceTokensStoresUnderNewHost()
    {
#ifndef Q_OS_WIN
        QSKIP("needs a credential store that also works in the CI runners");
#else
        auto accountState = createDummyAccount();
        Account *account = accountState->account();
        auto *credentials = new Credentials(QString(), QString(), account);
        QSignalSpy fetched(credentials, &AbstractCredentials::fetched);
        account->setUrl(QUrl(QStringLiteral("https://neu-%1.example.org/owncloud").arg(QUuid::createUuid().toString(QUuid::Id128))));
        const auto cleanup = qScopeGuard([account] { account->credentialManager()->clear(); });

        credentials->replaceTokens(QStringLiteral("new-access"), QStringLiteral("new-refresh"));

        // wie nach jeder Anmeldung: Erst bestätigt der Schlüsselbund das neue Refresh-Token, dann gilt das Access-Token
        QVERIFY(!credentials->ready());
        QTRY_COMPARE_WITH_TIMEOUT(fetched.size(), 1, 15000);
        QVERIFY(credentials->ready());
        QTRY_VERIFY(account->credentialManager()->contains(QStringLiteral("http/oauthtoken")));
        auto *job = account->credentialManager()->get(QStringLiteral("http/oauthtoken"));
        QVERIFY(job);
        QSignalSpy read(job, &CredentialJob::finished);
        QVERIFY(read.wait());
        QCOMPARE(job->data().toString(), QStringLiteral("new-refresh"));
#endif
    }

    // Ein unbekanntes Zertifikat der neuen Adresse lässt sich bestätigen wie bei der Kontoeinrichtung: Danach prüft der
    // Controller dieselbe Adresse noch einmal, fragt nicht erneut, und nach dem Umzug vertraut das Konto dem Zertifikat.
    void testUnknownCertificateCanBeConfirmed()
    {
        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        const QSslCertificate certificate = selfSignedCertificate();
        QVERIFY(!certificate.isNull());
        QVERIFY(!account->approvedCerts().contains(certificate));

        FileInfo sameServer = FileInfo::A12_B12_C12_S12();
        sameServer.fileId = "00000001ocinstance01";
        int statusRequests = 0;
        FakeAM *accessManager = serverWithTlsErrors(sameServer, {QSslError(QSslError::SelfSignedCertificate, certificate)}, &statusRequests);
        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([accessManager](QObject *parent) -> QNetworkAccessManager * {
            accessManager->setParent(parent);
            return accessManager;
        });
        QStringList prompts;
        controller.setTlsPrompt([&prompts](const QList<QSslError> &, const QString &host) {
            prompts.append(host);
            return true;
        });
        QSignalSpy failed(&controller, &ServerMoveController::failed);
        QSignalSpy succeeded(&controller, &ServerMoveController::succeeded);
        QSignalSpy progress(&controller, &ServerMoveController::progress);

        const QUrl newUrl(QStringLiteral("https://selbst.example.org/owncloud"));
        controller.start(newUrl.toString());
        // nach der Bestätigung noch einmal geprüft, dann geht es zur Anmeldung (die einzige Meldung mit dem Benutzer)
        QTRY_VERIFY(!progress.isEmpty() && progress.last().first().toString().contains(account->davUser()));
        QCOMPARE(statusRequests, 2);
        QCOMPARE(prompts, QStringList{QStringLiteral("selbst.example.org")});
        QCOMPARE(failed.size(), 0);
        QVERIFY(!account->approvedCerts().contains(certificate));

        // die Anmeldung im Browser überspringen: Prüfung mit demselben Netzwerkzugang, dann umstellen
        controller.cancel();
        controller.verifyAndApply(newUrl, QStringLiteral("new-token"), QStringLiteral("new-refresh"));
        QVERIFY(succeeded.wait());
        QCOMPARE(failed.size(), 0);
        QCOMPARE(prompts.size(), 1);
        QCOMPARE(account->url(), newUrl);
        QVERIFY(account->approvedCerts().contains(certificate));
    }

    // Abgelehnt oder nicht bestätigbar (Fehler ohne Zertifikat): Die Prüfung scheitert, und das Konto vertraut nichts Neuem
    void testUnconfirmedCertificateStopsTheMove_data()
    {
        QTest::addColumn<bool>("withCertificate");

        QTest::newRow("rejected") << true;
        QTest::newRow("error without certificate") << false;
    }

    void testUnconfirmedCertificateStopsTheMove()
    {
        QFETCH(bool, withCertificate);

        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        const QUrl oldUrl = account->url();
        const QSslCertificate certificate = selfSignedCertificate();
        QVERIFY(!certificate.isNull());
        const QSslError tlsError = withCertificate ? QSslError(QSslError::SelfSignedCertificate, certificate) : QSslError(QSslError::UnspecifiedError);

        int statusRequests = 0;
        FakeAM *accessManager = serverWithTlsErrors(FileInfo::A12_B12_C12_S12(), {tlsError}, &statusRequests);
        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([accessManager](QObject *parent) -> QNetworkAccessManager * {
            accessManager->setParent(parent);
            return accessManager;
        });
        int prompts = 0;
        controller.setTlsPrompt([&prompts](const QList<QSslError> &, const QString &) {
            ++prompts;
            return false;
        });
        QSignalSpy failed(&controller, &ServerMoveController::failed);
        controller.start(QStringLiteral("https://selbst.example.org/owncloud"));
        QVERIFY(failed.wait());
        QCOMPARE(prompts, withCertificate ? 1 : 0);
        QCOMPARE(statusRequests, 1);
        QVERIFY(!controller.isBusy());
        QCOMPARE(account->url(), oldUrl);
        QVERIFY(!account->approvedCerts().contains(certificate));
    }
};

QTEST_GUILESS_MAIN(TestServerMove)
#include "testservermove.moc"
