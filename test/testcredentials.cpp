/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// Erneuern der OAuth2-Anmeldung im Desktop-Client (Credentials, AccountState): zuerst mit dem Refresh-Token statt einer
// Anmeldeaufforderung, genau eine Erneuerung für viele gleichzeitige Ablehnungen, vorbeugend vor dem Ablauf und nach dem
// Ruhezustand, Pausen bei Netz- und Serverfehlern, eine neue Anmeldung nur bei invalid_grant, das neue Refresh-Token
// zuerst im Schlüsselbund.

#include "accountstate.h"
#include "common/syncjournaldb.h"
#include "common/vfs.h"
#include "creds/credentials.h"
#include "networkjobs/jsonjob.h"
#include "syncengine.h"
#include "syncoptions.h"
#include "testutils/syncenginetestutils.h"
#include "testutils/testutils.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QUrlQuery>

#include <functional>
#include <memory>

using namespace std::chrono_literals;
using namespace OCC;

namespace {

const QUrl serverUrlC(QStringLiteral("http://localhost/owncloud"));
const QString tokenPathC = QStringLiteral("/owncloud/index.php/apps/oauth2/api/v1/token");

QByteArray toJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray ocsPayload(const QJsonObject &data)
{
    const QJsonObject meta{{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("statuscode"), 200}};
    return toJson({{QStringLiteral("ocs"), QJsonObject{{QStringLiteral("meta"), meta}, {QStringLiteral("data"), data}}}});
}

// Capabilities eines owncloud.online-Servers ohne Spaces (sonst fragte der SpacesManager nach Drives)
QVariantMap serverCapabilities()
{
    return {{QStringLiteral("core"),
                QVariantMap{{QStringLiteral("status"),
                    QVariantMap{{QStringLiteral("installed"), QStringLiteral("1")}, {QStringLiteral("maintenance"), QStringLiteral("0")},
                        {QStringLiteral("version"), QStringLiteral("11.0.0.0")}, {QStringLiteral("versionstring"), QStringLiteral("11.0.0")},
                        {QStringLiteral("productname"), QStringLiteral("owncloud.online")}}}}},
        {QStringLiteral("dav"), QVariantMap{{QStringLiteral("chunking"), QStringLiteral("1.0")}}},
        {QStringLiteral("checksums"),
            QVariantMap{{QStringLiteral("preferredUploadType"), QStringLiteral("SHA1")}, {QStringLiteral("supportedTypes"), QVariantList{QStringLiteral("SHA1")}}}}};
}

/**
 * Server mit oauth2-App wie 0.9.0: Beim Erneuern verwirft er das alte Refresh-Token und das alte Access-Token (mit
 * keepPreviousAccessToken gilt das alte Access-Token weiter, wie mit der Gnadenfrist ab oauth2 0.9.3). WebDAV und OCS nimmt
 * er nur mit einem gültigen, nicht abgelaufenen Access-Token an; Dateien liefert der FakeAM.
 */
class TokenServer
{
public:
    QString accessToken = QStringLiteral("access-1");
    QString refreshToken = QStringLiteral("refresh-1");
    QString previousAccessToken;
    int generation = 1;
    // Lebensdauer neuer Access-Tokens (expires_in); nach der Uhr des Servers durchgesetzt
    qint64 expiresIn = 3600;
    QElapsedTimer accessTokenAge;
    bool accessTokenExpired = false;
    bool keepPreviousAccessToken = false;
    // kein Access-Token kommt an (etwa ein Webserver, der den Authorization-Kopf nicht weitergibt)
    bool rejectAllTokens = false;
    bool maintenance = false;
    // so verfällt das Access-Token mitten in einem Sync: nach so vielen angenommenen Anfragen (0: nie)
    int acceptedRequestsPerToken = 0;
    int acceptedWithCurrentToken = 0;
    // die nächsten Antworten des Token-Endpunkts statt eines Tauschs: HTTP-Status und Inhalt; 0 = Verbindung abgebrochen
    QList<std::pair<int, QByteArray>> tokenFailures;
    // Ablehnungen (401) auf diese Pfade kommen verzögert
    QStringList slowUnauthorizedPaths;

    int statusRequests = 0;
    int tokenRequests = 0;
    int unauthorized = 0;
    int expirations = 0;
    QList<QNetworkRequest> tokenRequestList;
    QList<QNetworkRequest> wellKnownRequestList;
    QStringList usedRefreshTokens;
    // bei jeder angenommenen Anfrage (Authorization-Kopf)
    std::function<void(const QByteArray &authorization)> onAccepted;

    TokenServer() { accessTokenAge.start(); }

    static QByteArray bearer(const QString &token) { return QByteArrayLiteral("Bearer ") + token.toUtf8(); }

    /** Das Access-Token gilt nicht mehr, das Refresh-Token schon (abgelaufen, Ruhezustand). */
    void expireAccessToken()
    {
        if (!accessTokenExpired) {
            accessTokenExpired = true;
            ++expirations;
        }
        previousAccessToken.clear();
    }

    bool accepts(const QByteArray &authorization)
    {
        if (rejectAllTokens) {
            return false;
        }
        if (keepPreviousAccessToken && !previousAccessToken.isEmpty() && authorization == bearer(previousAccessToken)) {
            return true;
        }
        if (authorization != bearer(accessToken)) {
            return false;
        }
        if (!accessTokenExpired && expiresIn > 0 && accessTokenAge.elapsed() >= expiresIn * 1000) {
            expireAccessToken();
        }
        if (accessTokenExpired) {
            return false;
        }
        if (acceptedRequestsPerToken > 0 && ++acceptedWithCurrentToken > acceptedRequestsPerToken) {
            expireAccessToken();
            return false;
        }
        return true;
    }

    FakeAM::Override override(QObject *replyParent)
    {
        return [this, replyParent](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *device) -> QNetworkReply * {
            const QString path = request.url().path();
            if (path.endsWith(QLatin1String("/status.php"))) {
                ++statusRequests;
                const QJsonObject status{{QStringLiteral("installed"), true}, {QStringLiteral("maintenance"), maintenance},
                    {QStringLiteral("needsDbUpgrade"), false}, {QStringLiteral("version"), QStringLiteral("11.0.0.0")},
                    {QStringLiteral("versionstring"), QStringLiteral("11.0.0")}, {QStringLiteral("edition"), QStringLiteral("Community")},
                    {QStringLiteral("productname"), QStringLiteral("owncloud.online")}};
                return new FakePayloadReply(op, request, toJson(status), replyParent);
            }
            if (path.endsWith(QLatin1String(".well-known/openid-configuration"))) {
                wellKnownRequestList.append(request);
                // wie bei owncloud.online: kein OpenID Connect, der Client nimmt die Wege der oauth2-App
                return new FakeErrorReply(op, request, replyParent, 404);
            }
            if (path == tokenPathC) {
                return tokenReply(op, request, device, replyParent);
            }

            const QByteArray authorization = request.rawHeader(QByteArrayLiteral("Authorization"));
            if (!accepts(authorization)) {
                ++unauthorized;
                if (slowUnauthorizedPaths.contains(path)) {
                    return new DelayedReply<FakeErrorReply>(300ms, op, request, replyParent, 401);
                }
                return new FakeErrorReply(op, request, replyParent, 401);
            }
            if (onAccepted) {
                onAccepted(authorization);
            }
            if (path.contains(QLatin1String("/ocs/v2.php/cloud/"))) {
                if (path.endsWith(QLatin1String("/user"))) {
                    return new FakePayloadReply(op, request,
                        ocsPayload({{QStringLiteral("id"), QStringLiteral("admin")}, {QStringLiteral("display-name"), QStringLiteral("Admin")}}), replyParent);
                }
                return new FakePayloadReply(
                    op, request, ocsPayload({{QStringLiteral("capabilities"), QJsonObject::fromVariantMap(serverCapabilities())}}), replyParent);
            }
            // Dateien und WebDAV-Abfragen beantwortet der FakeAM
            return nullptr;
        };
    }

private:
    QNetworkReply *tokenReply(QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *device, QObject *replyParent)
    {
        ++tokenRequests;
        tokenRequestList.append(request);
        // nur ansehen: der HTTP-Logger liest den Körper beim Abschluss noch ab Position 0
        const QUrlQuery form(QString::fromUtf8(device ? device->peek(device->size()) : QByteArray()));
        usedRefreshTokens.append(form.queryItemValue(QStringLiteral("refresh_token")));

        if (!tokenFailures.isEmpty()) {
            const auto [status, body] = tokenFailures.takeFirst();
            if (status == 0) {
                auto *reply = new FakeErrorReply(op, request, replyParent, 500);
                reply->setError(QNetworkReply::RemoteHostClosedError, QStringLiteral("Connection closed"));
                reply->setAttribute(QNetworkRequest::HttpStatusCodeAttribute, QVariant());
                return reply;
            }
            if (status < 400) {
                // HTML statt JSON oder eine Weiterleitung auf die Anmeldeseite (oauth2-App aus)
                auto *reply = new FakePayloadReply(op, request, body, replyParent);
                reply->setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
                return reply;
            }
            return new FakeErrorReply(op, request, replyParent, status, body);
        }
        if (form.queryItemValue(QStringLiteral("grant_type")) != QLatin1String("refresh_token")
            || form.queryItemValue(QStringLiteral("refresh_token")) != refreshToken) {
            return new FakeErrorReply(op, request, replyParent, 400,
                toJson({{QStringLiteral("error"), QStringLiteral("invalid_grant")}, {QStringLiteral("error_description"), QStringLiteral("refresh token does not exist")}}));
        }
        previousAccessToken = accessTokenExpired ? QString() : accessToken;
        ++generation;
        accessToken = QStringLiteral("access-%1").arg(generation);
        refreshToken = QStringLiteral("refresh-%1").arg(generation);
        accessTokenExpired = false;
        acceptedWithCurrentToken = 0;
        accessTokenAge.start();
        return new FakePayloadReply(op, request,
            toJson({{QStringLiteral("access_token"), accessToken}, {QStringLiteral("refresh_token"), refreshToken},
                {QStringLiteral("token_type"), QStringLiteral("Bearer")}, {QStringLiteral("expires_in"), expiresIn}, {QStringLiteral("user_id"), QStringLiteral("admin")}}),
            replyParent);
    }
};

class TestCredentials;

// wie CredentialsAccessManager: das Access-Token als Bearer, außer bei DontAddCredentialsAttribute
class BearerFakeAM : public FakeAM
{
public:
    BearerFakeAM(const TestCredentials *credentials, FileInfo root, QObject *parent);

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData) override;

private:
    QPointer<const TestCredentials> _credentials;
};

// Credentials gegen den TokenServer, Schlüsselbund im Speicher, kurze Pausen, die Anmeldeaufforderung wird nur gezählt
class TestCredentials : public Credentials
{
public:
    TestCredentials(Account *account, TokenServer *server, const QString &accessToken, const QString &refreshToken, const FileInfo &files)
        : Credentials(accessToken, refreshToken, account)
        , _server(server)
        , _files(files)
    {
    }

    using Credentials::_accessTokenStaleAt;
    using Credentials::_proactiveRefreshAt;
    using Credentials::_proactiveRefreshTimer;
    using Credentials::_refreshToken;
    using Credentials::_sinceAccessToken;
    using Credentials::waitForStoredRefreshToken;

    QString accessToken() const { return _accessToken; }

    OCC::AccessManager *createAccessManager() const override
    {
        // der erste gehört dem Konto (Account::setCredentials) und hält die Dateien; jede Prüfung des Servers bekommt einen
        // eigenen, den der CheckServerJob mit sich löscht
        auto *am = new BearerFakeAM(this, _handedOut ? FileInfo() : _files, nullptr);
        am->setOverride(_server->override(am));
        if (!_handedOut) {
            _handedOut = true;
            am->setParent(const_cast<TestCredentials *>(this));
        }
        return am;
    }

    void askFromUser() override { ++askedUser; }

    QString keychain;
    int keychainReadFailures = 0;
    int keychainStoreFailures = 0;
    int keychainStores = 0;
    int keychainRemovals = 0;
    std::chrono::milliseconds keychainDelay = 1ms;
    std::chrono::milliseconds retryDelay = 50ms;
    std::chrono::milliseconds keychainRetry = 10ms;
    int askedUser = 0;

protected:
    void readRefreshToken(const ReadCallback &done) override
    {
        QTimer::singleShot(keychainDelay, this, [this, done] {
            if (keychainReadFailures > 0) {
                --keychainReadFailures;
                done(KeychainResult::Error, {}, QStringLiteral("the keychain is locked"));
            } else if (keychain.isEmpty()) {
                done(KeychainResult::NotFound, {}, QStringLiteral("no entry"));
            } else {
                done(KeychainResult::Ok, keychain, {});
            }
        });
    }

    void storeRefreshToken(const QString &refreshToken, const StoreCallback &done) override
    {
        ++keychainStores;
        QTimer::singleShot(keychainDelay, this, [this, refreshToken, done] {
            if (keychainStoreFailures > 0) {
                --keychainStoreFailures;
                done(false, QStringLiteral("could not write"));
                return;
            }
            keychain = refreshToken;
            done(true, {});
        });
    }

    void removeRefreshToken() override
    {
        ++keychainRemovals;
        keychain.clear();
    }

    std::chrono::milliseconds refreshRetryDelay(int) const override { return retryDelay; }
    std::chrono::milliseconds keychainRetryDelay(int) const override { return keychainRetry; }

private:
    TokenServer *_server;
    FileInfo _files;
    mutable bool _handedOut = false;
};

BearerFakeAM::BearerFakeAM(const TestCredentials *credentials, FileInfo root, QObject *parent)
    : FakeAM(std::move(root), parent)
    , _credentials(credentials)
{
}

QNetworkReply *BearerFakeAM::createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData)
{
    QNetworkRequest authorized(request);
    if (_credentials && !authorized.attribute(DontAddCredentialsAttribute).toBool() && !_credentials->accessToken().isEmpty()) {
        authorized.setRawHeader(QByteArrayLiteral("Authorization"), TokenServer::bearer(_credentials->accessToken()));
    }
    return FakeAM::createRequest(op, authorized, outgoingData);
}

// eine OCS-Anfrage über die Auftragswarteschlange
struct Request
{
    bool finished = false;
    QNetworkReply::NetworkError error = QNetworkReply::UnknownNetworkError;
};

bool allFinishedOk(const QList<std::shared_ptr<Request>> &requests)
{
    return std::all_of(requests.cbegin(), requests.cend(), [](const auto &r) { return r->finished && r->error == QNetworkReply::NoError; });
}

// Konto mit den TestCredentials. Ohne Access-Token wie nach einem Neustart (das Refresh-Token liegt im Schlüsselbund),
// sonst mit einem Token, das schon länger gilt.
struct Fixture
{
    TokenServer server;
    std::unique_ptr<Account> account;
    TestCredentials *credentials = nullptr;

    explicit Fixture(const QString &accessToken = QStringLiteral("access-1"), const QString &keychain = QStringLiteral("refresh-1"), const FileInfo &files = {})
        : account(std::make_unique<Account>(QUuid::createUuid(), QStringLiteral("admin"), serverUrlC))
    {
        credentials = new TestCredentials(account.get(), &server, accessToken, accessToken.isEmpty() ? QString() : keychain, files);
        credentials->keychain = keychain;
        credentials->_sinceAccessToken.invalidate();
        account->setCredentials(credentials);
    }

    static std::unique_ptr<Fixture> restart(const QString &keychain = QStringLiteral("refresh-1")) { return std::make_unique<Fixture>(QString(), keychain); }

    std::shared_ptr<Request> get(QObject *scope, const QString &path = QStringLiteral("ocs/v2.php/cloud/user"))
    {
        auto result = std::make_shared<Request>();
        auto *job = new JsonApiJob(account.get(), path, {}, {}, scope);
        QObject::connect(job, &JsonApiJob::finishedSignal, scope, [job, result] {
            result->error = job->reply()->error();
            result->finished = true;
        });
        job->start();
        return result;
    }
};

} // namespace

class TestCredentialsRefresh : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Viele Anfragen scheitern gleichzeitig mit 401: genau eine Erneuerung, alle werden danach mit dem neuen Token wiederholt
    void testParallelUnauthorizedRefreshOnce()
    {
        // Arrange
        Fixture f;
        QObject scope;
        f.server.expireAccessToken();
        QSignalSpy failedSpy(f.credentials, &AbstractCredentials::authenticationFailed);
        QList<std::shared_ptr<Request>> requests;

        // Act
        for (int i = 0; i < 6; ++i) {
            requests.append(f.get(&scope));
        }

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(allFinishedOk(requests), 10000);
        QCOMPARE(f.server.unauthorized, 6);
        QCOMPARE(f.server.tokenRequests, 1);
        QCOMPARE(f.server.usedRefreshTokens, QStringList{QStringLiteral("refresh-1")});
        QCOMPARE(f.credentials->askedUser, 0);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(f.credentials->accessToken(), QStringLiteral("access-2"));
        QCOMPARE(f.credentials->keychain, QStringLiteral("refresh-2"));
    }

    // Erst landet das neue Refresh-Token im Schlüsselbund, dann geht die erste Anfrage mit dem neuen Access-Token hinaus
    void testNewRefreshTokenIsStoredBeforeUse()
    {
        // Arrange
        Fixture f;
        QObject scope;
        f.server.expireAccessToken();
        f.credentials->keychainDelay = 200ms;
        QStringList keychainAtFirstUse;
        f.server.onAccepted = [&](const QByteArray &authorization) {
            if (authorization == QByteArrayLiteral("Bearer access-2") && keychainAtFirstUse.isEmpty()) {
                keychainAtFirstUse.append(f.credentials->keychain);
            }
        };

        // Act
        const auto request = f.get(&scope);

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(request->finished, 10000);
        QCOMPARE(request->error, QNetworkReply::NoError);
        QCOMPARE(keychainAtFirstUse, QStringList{QStringLiteral("refresh-2")});
    }

    // Scheitert das Speichern, geht das neue Refresh-Token nicht verloren: Es wird nach wenigen Versuchen trotzdem benutzt und
    // weiter gespeichert, bis es klappt
    void testStoreFailureKeepsTheNewRefreshToken()
    {
        // Arrange
        Fixture f;
        QObject scope;
        f.server.expireAccessToken();
        f.credentials->keychainStoreFailures = 4;

        // Act
        const auto request = f.get(&scope);

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(request->finished, 10000);
        QCOMPARE(request->error, QNetworkReply::NoError);
        QCOMPARE(f.credentials->_refreshToken, QStringLiteral("refresh-2"));
        QTRY_COMPARE_WITH_TIMEOUT(f.credentials->keychain, QStringLiteral("refresh-2"), 5000);
        QCOMPARE(f.credentials->keychainStores, 5);
        QCOMPARE(f.credentials->askedUser, 0);
    }

    // Netz- und Serverfehler beim Erneuern: das Refresh-Token bleibt, nach einer Pause ein neuer Versuch, keine Anmeldeaufforderung;
    // auch nach mehr als drei Fehlschlägen in Folge (6.0.3 und beta2 meldeten dann ab)
    void testTemporaryFailuresKeepTheRefreshToken_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");

        const QByteArray html = QByteArrayLiteral("<!DOCTYPE html><html><body>Login</body></html>");
        QTest::newRow("connection closed") << 0 << QByteArray();
        QTest::newRow("500") << 500 << QByteArray();
        QTest::newRow("502 proxy") << 502 << QByteArrayLiteral("<html>Bad Gateway</html>");
        QTest::newRow("503 maintenance html") << 503 << html;
        QTest::newRow("504 proxy timeout") << 504 << QByteArray();
        QTest::newRow("302 to the login page") << 302 << html;
        QTest::newRow("200 html") << 200 << html;
        QTest::newRow("400 invalid_request") << 400 << toJson({{QStringLiteral("error"), QStringLiteral("invalid_request")}});
        QTest::newRow("400 unauthorized_client") << 400
                                                 << toJson({{QStringLiteral("error"), QStringLiteral("unauthorized_client")},
                                                        {QStringLiteral("error_description"), QStringLiteral("user not enabled")}});
        QTest::newRow("400 invalid_client") << 400 << toJson({{QStringLiteral("error"), QStringLiteral("invalid_client")}});
        QTest::newRow("200 without access token") << 200 << toJson({{QStringLiteral("token_type"), QStringLiteral("Bearer")}});
    }

    void testTemporaryFailuresKeepTheRefreshToken()
    {
        // Arrange
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        Fixture f;
        QObject scope;
        f.server.expireAccessToken();
        for (int i = 0; i < 4; ++i) {
            f.server.tokenFailures.append({status, body});
        }
        QSignalSpy delayedSpy(f.credentials, &AbstractCredentials::authenticationDelayed);
        QSignalSpy failedSpy(f.credentials, &AbstractCredentials::authenticationFailed);

        // Act
        const auto request = f.get(&scope);

        // Assert: viermal gescheitert, beim fünften Mal erneuert, die Anfrage wiederholt
        QTRY_VERIFY_WITH_TIMEOUT(request->finished, 10000);
        QCOMPARE(request->error, QNetworkReply::NoError);
        QCOMPARE(f.server.tokenRequests, 5);
        QCOMPARE(f.server.usedRefreshTokens, QStringList(5, QStringLiteral("refresh-1")));
        QCOMPARE(delayedSpy.count(), 4);
        QCOMPARE(delayedSpy.first().at(0).toBool(), false);
        QVERIFY(!delayedSpy.first().at(1).toString().isEmpty());
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(f.credentials->askedUser, 0);
        QCOMPARE(f.credentials->keychainRemovals, 0);
        QCOMPARE(f.credentials->keychain, QStringLiteral("refresh-2"));
    }

    // Die Pausen wachsen von 30 s auf höchstens 5 min
    void testRetryDelays()
    {
        QCOMPARE(Credentials::defaultRefreshRetryDelay(1), 30s);
        QCOMPARE(Credentials::defaultRefreshRetryDelay(2), 60s);
        QCOMPARE(Credentials::defaultRefreshRetryDelay(3), 120s);
        QCOMPARE(Credentials::defaultRefreshRetryDelay(4), 240s);
        QCOMPARE(Credentials::defaultRefreshRetryDelay(5), 5min);
        QCOMPARE(Credentials::defaultRefreshRetryDelay(1000), 5min);
    }

    // Nur invalid_grant heißt: neu anmelden. Das Refresh-Token verschwindet auch aus dem Schlüsselbund, angehaltene Aufträge
    // werden verworfen, das Konto bekommt fetched() ohne Token
    void testRejectedRefreshTokenEndsTheSignIn()
    {
        // Arrange
        Fixture f;
        QObject scope;
        f.server.expireAccessToken();
        f.server.refreshToken = QStringLiteral("refresh-other");
        QSignalSpy failedSpy(f.credentials, &AbstractCredentials::authenticationFailed);
        QSignalSpy fetchedSpy(f.credentials, &AbstractCredentials::fetched);

        // Act
        f.get(&scope);

        // Assert
        QTRY_COMPARE_WITH_TIMEOUT(fetchedSpy.count(), 1, 10000);
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(f.server.tokenRequests, 1);
        QVERIFY(!f.credentials->ready());
        QVERIFY(f.credentials->_refreshToken.isEmpty());
        QVERIFY(f.credentials->keychain.isEmpty());
        QVERIFY(!f.credentials->refreshAccessToken());
        QVERIFY(!f.account->jobQueue()->isBlocked());
    }

    // Wartung (status.php): nicht am Token-Endpunkt anklopfen, abwarten, das Refresh-Token gilt weiter
    void testMaintenanceIsWaitedOut()
    {
        // Arrange
        Fixture f;
        QObject scope;
        f.server.expireAccessToken();
        f.server.maintenance = true;
        QSignalSpy delayedSpy(f.credentials, &AbstractCredentials::authenticationDelayed);

        // Act
        const auto request = f.get(&scope);
        QTRY_VERIFY_WITH_TIMEOUT(delayedSpy.count() >= 2, 10000);
        QCOMPARE(f.server.tokenRequests, 0);
        f.server.maintenance = false;

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(request->finished, 10000);
        QCOMPARE(request->error, QNetworkReply::NoError);
        QCOMPARE(delayedSpy.first().at(0).toBool(), true);
        QCOMPARE(f.server.tokenRequests, 1);
        QCOMPARE(f.credentials->askedUser, 0);
    }

    // Vor dem Ablauf (80 % von expires_in) erneuert, ohne Ablehnung; die Anfragen laufen dabei weiter
    void testProactiveRefreshBeforeExpiry()
    {
        // Arrange: Start, das Token lebt 2 s; der Server lässt das vorige Access-Token kurz gelten (oauth2 ab 0.9.3)
        auto f = Fixture::restart();
        QObject scope;
        f->server.expiresIn = 2;
        f->server.keepPreviousAccessToken = true;
        QSignalSpy fetchedSpy(f->credentials, &AbstractCredentials::fetched);
        f->credentials->fetchFromKeychain();
        QTRY_COMPARE_WITH_TIMEOUT(fetchedSpy.count(), 1, 10000);

        // Act: 5 s lang alle 100 ms eine Anfrage
        QList<std::shared_ptr<Request>> requests;
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 5000) {
            requests.append(f->get(&scope));
            QTest::qWait(100);
        }

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(allFinishedOk(requests), 10000);
        QCOMPARE(f->server.unauthorized, 0);
        QCOMPARE(f->server.expirations, 0);
        QVERIFY2(f->server.tokenRequests >= 4, qPrintable(QString::number(f->server.tokenRequests)));
        // eine vorbeugende Erneuerung meldet dem Konto nichts, es wartete ja nicht
        QCOMPARE(fetchedSpy.count(), 1);
    }

    // Nach dem Ruhezustand ist das Token nach der Uhr abgelaufen, der Zeitgeber kam (Linux, macOS) nicht: erst erneuern, dann
    // senden, ohne Ablehnung
    void testExpiredTokenIsRefreshedBeforeSending()
    {
        // Arrange
        Fixture f;
        QObject scope;
        f.server.expireAccessToken();
        f.credentials->_proactiveRefreshTimer.stop();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        f.credentials->_accessTokenStaleAt = now - 10;
        f.credentials->_proactiveRefreshAt = now - 100;
        QVERIFY(!f.credentials->ready());

        // Act
        const auto request = f.get(&scope);

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(request->finished, 10000);
        QCOMPARE(request->error, QNetworkReply::NoError);
        QCOMPARE(f.server.unauthorized, 0);
        QCOMPARE(f.server.tokenRequests, 1);
    }

    // Eine Ablehnung für eine Anfrage mit dem alten Token kommt erst nach der Erneuerung an: keine zweite Erneuerung
    void testLateUnauthorizedForTheOldTokenDoesNotRefreshAgain()
    {
        // Arrange: die Abfrage der Capabilities wird langsam abgelehnt
        Fixture f;
        QObject scope;
        f.server.expireAccessToken();
        f.server.slowUnauthorizedPaths.append(QStringLiteral("/owncloud/ocs/v2.php/cloud/capabilities"));

        // Act
        const auto slow = f.get(&scope, QStringLiteral("ocs/v2.php/cloud/capabilities"));
        const auto fast = f.get(&scope);

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(fast->finished && slow->finished, 10000);
        QCOMPARE(fast->error, QNetworkReply::NoError);
        QCOMPARE(slow->error, QNetworkReply::NoError);
        QCOMPARE(f.server.tokenRequests, 1);
    }

    // Ein eben ausgestelltes Token, das der Server nie annimmt (Webserver gibt den Authorization-Kopf nicht weiter): nicht in
    // einer Schleife erneuern, sondern mit Pause
    void testFreshTokenRejectedWaits()
    {
        // Arrange
        Fixture f;
        QObject scope;
        f.server.rejectAllTokens = true;
        f.credentials->retryDelay = 1s;

        // Act
        f.get(&scope);
        QTRY_COMPARE_WITH_TIMEOUT(f.server.tokenRequests, 1, 10000);
        QTest::qWait(600);

        // Assert: die zweite Erneuerung frühestens nach der Pause
        QCOMPARE(f.server.tokenRequests, 1);
        QTRY_COMPARE_WITH_TIMEOUT(f.server.tokenRequests, 2, 5000);
        QCOMPARE(f.credentials->askedUser, 0);
    }

    // Anfragen an .well-known und den Token-Endpunkt ohne Cookies (sonst „Login failed“ und Bremse im Kern), ohne Bearer,
    // ohne Weiterleitung, mit langer Frist
    void testTokenRequestsWithoutCookies()
    {
        // Arrange
        Fixture f;
        QObject scope;
        f.server.expireAccessToken();

        // Act
        const auto request = f.get(&scope);

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(request->finished, 10000);
        QCOMPARE(f.server.tokenRequestList.size(), 1);
        QCOMPARE(f.server.wellKnownRequestList.size(), 1);
        for (const auto &r : {f.server.tokenRequestList.first(), f.server.wellKnownRequestList.first()}) {
            QCOMPARE(r.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), static_cast<int>(QNetworkRequest::Manual));
            QCOMPARE(r.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(), static_cast<int>(QNetworkRequest::Manual));
            QVERIFY(!r.rawHeader(QByteArrayLiteral("Authorization")).startsWith("Bearer"));
        }
        const QNetworkRequest token = f.server.tokenRequestList.first();
        QCOMPARE(token.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), static_cast<int>(QNetworkRequest::ManualRedirectPolicy));
        QVERIFY(token.rawHeader(QByteArrayLiteral("Authorization")).startsWith("Basic "));
        // nicht nach 30 s aufgeben: der Server tauscht die Tokens womöglich trotzdem
        QVERIFY(token.transferTimeout() >= 120000);
    }

    // Start: das Refresh-Token aus dem Schlüsselbund erst erneuern, dann senden; ein noch gesperrter Schlüsselbund wird erneut
    // gelesen, statt eine neue Anmeldung zu verlangen
    void testStartRefreshesBeforeRequests()
    {
        // Arrange
        auto f = Fixture::restart();
        QObject scope;
        f->credentials->keychainReadFailures = 2;
        QSignalSpy fetchedSpy(f->credentials, &AbstractCredentials::fetched);

        // Act: eine Anfrage, bevor das Token da ist
        f->credentials->fetchFromKeychain();
        const auto request = f->get(&scope);

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(request->finished, 10000);
        QCOMPARE(request->error, QNetworkReply::NoError);
        QCOMPARE(fetchedSpy.count(), 1);
        QVERIFY(f->credentials->ready());
        QCOMPARE(f->server.unauthorized, 0);
        QCOMPARE(f->server.tokenRequests, 1);
        QCOMPARE(f->server.usedRefreshTokens, QStringList{QStringLiteral("refresh-1")});
    }

    // Beim Beenden wartet das Programm, bis der Schlüsselbund ein noch nicht gespeichertes Refresh-Token hat
    void testShutdownWaitsForTheKeychain()
    {
        // Arrange: der Schlüsselbund scheitert zweimal, das neue Access-Token ist schon in Gebrauch (nach drei Fehlschlägen)
        Fixture f;
        QObject scope;
        f.server.expireAccessToken();
        f.credentials->keychainStoreFailures = 3;
        f.credentials->keychainDelay = 100ms;
        f.credentials->keychainRetry = 2s;
        const auto request = f.get(&scope);
        QTRY_VERIFY_WITH_TIMEOUT(request->finished, 10000);
        QCOMPARE(f.credentials->_refreshToken, QStringLiteral("refresh-2"));
        QVERIFY(f.credentials->keychain != QStringLiteral("refresh-2"));

        // Act
        f.credentials->waitForStoredRefreshToken();

        // Assert
        QCOMPARE(f.credentials->keychain, QStringLiteral("refresh-2"));
    }

    // Ohne Eintrag im Schlüsselbund: fetched() ohne Token, das Konto fragt dann den Nutzer
    void testStartWithoutKeychainEntry()
    {
        auto f = Fixture::restart(QString());
        QSignalSpy fetchedSpy(f->credentials, &AbstractCredentials::fetched);

        f->credentials->fetchFromKeychain();

        QTRY_COMPARE_WITH_TIMEOUT(fetchedSpy.count(), 1, 5000);
        QVERIFY(!f->credentials->ready());
        QCOMPARE(f->server.tokenRequests, 0);
        QVERIFY(!f->account->jobQueue()->isBlocked());
    }

    // Sync mit mehreren parallelen Übertragungen, das Access-Token verfällt mittendrin (mehrmals): je Ablauf genau eine
    // Erneuerung, keine Anmeldeaufforderung, alles übertragen
    void testTokenExpiresDuringSync_data()
    {
        QTest::addColumn<bool>("upload");
        QTest::newRow("download") << false;
        QTest::newRow("upload") << true;
    }

    void testTokenExpiresDuringSync()
    {
        // Arrange
        QFETCH(bool, upload);
        constexpr int fileCount = 30;
        FileInfo remote;
        const QTemporaryDir dir = TestUtils::createTempDir();
        const QString localPath = dir.path() + QLatin1Char('/');
        for (int i = 0; i < fileCount; ++i) {
            const QString name = QStringLiteral("file%1.dat").arg(i);
            if (upload) {
                QFile file(localPath + name);
                QVERIFY(file.open(QIODevice::WriteOnly));
                file.write(QByteArray(2048 + i, 'U'));
            } else {
                remote.insert(name, static_cast<quint64>(2048 + i), 'D');
            }
        }
        Fixture f(QStringLiteral("access-1"), QStringLiteral("refresh-1"), remote);
        f.account->setCapabilities({f.account->url(), serverCapabilities()});
        f.server.acceptedRequestsPerToken = 7;
        QSignalSpy failedSpy(f.credentials, &AbstractCredentials::authenticationFailed);

        QObject scope;
        auto *db = new SyncJournalDb(localPath + SyncJournalDb::makeDbName(localPath), &scope);
        Vfs *vfs = VfsPluginManager::instance().createVfsFromPlugin(Vfs::Off, db);
        QVERIFY(vfs);
        auto *engine = new SyncEngine(f.account.get(), f.account->davUrl(), localPath, QString(), db, &scope);
        engine->setSyncOptions(SyncOptions(vfs));
        engine->addManualExclude(QStringLiteral("]*.~*"));
        SyncEngine::minimumFileAgeForUpload = 0s;
        QSignalSpy finishedSpy(engine, &SyncEngine::finished);

        // Act
        engine->startSync();

        // Assert
        QVERIFY(finishedSpy.wait(60000));
        QVERIFY2(finishedSpy.first().first().toBool(), "sync failed");
        QVERIFY2(f.server.expirations >= 2, qPrintable(QString::number(f.server.expirations)));
        QCOMPARE(f.server.tokenRequests, f.server.expirations);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(f.credentials->askedUser, 0);
        auto *am = static_cast<FakeAM *>(f.account->accessManager());
        for (int i = 0; i < fileCount; ++i) {
            const QString name = QStringLiteral("file%1.dat").arg(i);
            if (upload) {
                const FileInfo *uploaded = am->currentRemoteState().find(name);
                QVERIFY2(uploaded, qPrintable(name));
                QCOMPARE(uploaded->contentSize, static_cast<decltype(uploaded->contentSize)>(2048 + i));
            } else {
                QCOMPARE(QFileInfo(localPath + name).size(), static_cast<qint64>(2048 + i));
            }
        }
    }

    // Konto: Eine vorübergehend gescheiterte Erneuerung zeigt „Server vorübergehend nicht erreichbar“ samt Grund und fragt nicht
    // nach einer Anmeldung; danach verbindet es sich wieder, und die Verbindungsprüfung läuft weiter (_waitingForNewCredentials
    // ist zurückgesetzt)
    void testAccountStateWaitsInsteadOfAsking()
    {
        // Arrange: verbunden
        Fixture f;
        f.account->setCapabilities({f.account->url(), serverCapabilities()});
        std::ignore = TestUtils::folderMan();
        auto state = std::make_unique<AccountState>(f.account.get());
        QList<AccountState::State> states;
        QStringList errorsAtNetworkError;
        connect(state.get(), &AccountState::stateChanged, this, [&](AccountState::State s) {
            states.append(s);
            if (s == AccountState::NetworkError) {
                errorsAtNetworkError = state->connectionErrors();
            }
        });
        state->checkConnectivity();
        QTRY_COMPARE_WITH_TIMEOUT(state->state(), AccountState::Connected, 10000);

        // Act: das Token läuft ab, der Token-Endpunkt antwortet zweimal mit 503
        f.server.expireAccessToken();
        const QByteArray unavailable = QByteArrayLiteral("<html>Service Unavailable</html>");
        f.server.tokenFailures = {{503, unavailable}, {503, unavailable}};
        f.credentials->retryDelay = 300ms;
        states.clear();
        state->freshConnectionAttempt();

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(states.contains(AccountState::NetworkError), 10000);
        QVERIFY(!errorsAtNetworkError.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(state->state(), AccountState::Connected, 15000);
        QVERIFY(!states.contains(AccountState::AskingCredentials));
        QCOMPARE(f.credentials->askedUser, 0);
        QCOMPARE(f.server.tokenRequests, 3);

        // die Verbindungsprüfung wartet nicht mehr auf neue Anmeldedaten
        const int statusRequests = f.server.statusRequests;
        state->freshConnectionAttempt();
        QTRY_VERIFY_WITH_TIMEOUT(f.server.statusRequests > statusRequests, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(state->state(), AccountState::Connected, 10000);
    }

    // Konto: Erst wenn der Server das Refresh-Token ablehnt, fragt es den Nutzer
    void testAccountStateAsksAfterInvalidGrant()
    {
        // Arrange
        Fixture f;
        f.account->setCapabilities({f.account->url(), serverCapabilities()});
        std::ignore = TestUtils::folderMan();
        auto state = std::make_unique<AccountState>(f.account.get());
        state->checkConnectivity();
        QTRY_COMPARE_WITH_TIMEOUT(state->state(), AccountState::Connected, 10000);

        // Act
        f.server.expireAccessToken();
        f.server.refreshToken = QStringLiteral("refresh-other");
        state->freshConnectionAttempt();

        // Assert
        QTRY_COMPARE_WITH_TIMEOUT(state->state(), AccountState::AskingCredentials, 10000);
        QCOMPARE(f.credentials->askedUser, 1);
        QCOMPARE(f.server.tokenRequests, 1);
    }

    // Konto beim Start während der Wartung: Wartung anzeigen, keine Anmeldeaufforderung, danach erneuern
    void testAccountStartDuringMaintenance()
    {
        // Arrange
        auto f = Fixture::restart();
        f->server.maintenance = true;
        f->account->setCapabilities({f->account->url(), serverCapabilities()});
        std::ignore = TestUtils::folderMan();
        auto state = std::make_unique<AccountState>(f->account.get());

        // Act
        state->checkConnectivity();

        // Assert
        QTRY_COMPARE_WITH_TIMEOUT(state->state(), AccountState::MaintenanceMode, 10000);
        QCOMPARE(f->server.tokenRequests, 0);
        f->server.maintenance = false;
        QTRY_VERIFY_WITH_TIMEOUT(f->server.tokenRequests == 1 && f->credentials->ready(), 10000);
        QCOMPARE(f->credentials->askedUser, 0);
        QVERIFY(state->state() != AccountState::AskingCredentials);
    }
};

QTEST_MAIN(TestCredentialsRefresh)
#include "testcredentials.moc"
