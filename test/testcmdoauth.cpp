/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// OAuth2-Anmeldung von owncloudcmd: Bearer-Header, Erneuern nach 401 und bei Ablauf,
// Rückschreiben der Token-Datei und Exit-Code, wenn das Erneuern scheitert.

#include "account.h"
#include "cmdsession.h"
#include "config/appconfig.h"
#include "httpoauthcredentials.h"
#include "networkjobs/jsonjob.h"
#include "oauthtokenfile.h"
#include "testutils/syncenginetestutils.h"
#include "testutils/testutils.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QUrlQuery>

#include <memory>

using namespace OCC;

namespace {

const QUrl serverUrlC(QStringLiteral("http://localhost/owncloud"));

QByteArray toJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject tokenFileContent(const QString &accessToken, const QString &refreshToken, qint64 expiresAt)
{
    return {{QStringLiteral("server_url"), serverUrlC.toString()}, {QStringLiteral("access_token"), accessToken},
        {QStringLiteral("refresh_token"), refreshToken}, {QStringLiteral("expires_at"), expiresAt}, {QStringLiteral("user_id"), QStringLiteral("admin")},
        {QStringLiteral("note"), QStringLiteral("kept")}};
}

QString writeTokenFile(const QTemporaryDir &dir, const QJsonObject &content)
{
    const QString path = dir.filePath(QStringLiteral("tokens.json"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    file.write(toJson(content));
    return path;
}

QJsonObject readTokenFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

// Antworten für einen Access-Manager, der sie wie bei Qt besitzt: Was beim Abbau des Kontos noch offen ist
// (etwa die Drives-Anfrage des SpacesManagers), geht mit ihm, statt später finished() an den gelöschten Manager zu melden
using OverrideFactory = std::function<FakeAM::Override(QObject *replyParent)>;

// Server mit oauth2-App: nimmt nur ein Access-Token an und beantwortet den Token-Endpunkt
class FakeOAuthServer
{
public:
    QByteArray acceptedAuthorization = QByteArrayLiteral("Bearer access-2");
    int tokenStatus = 200;
    QJsonObject tokenAnswer{{QStringLiteral("access_token"), QStringLiteral("access-2")}, {QStringLiteral("refresh_token"), QStringLiteral("refresh-2")},
        {QStringLiteral("token_type"), QStringLiteral("Bearer")}, {QStringLiteral("expires_in"), 3600}, {QStringLiteral("user_id"), QStringLiteral("admin")}};
    // die nächsten Antworten des Token-Endpunkts davor: HTTP-Status und Inhalt; 0 = Verbindung abgebrochen, -1 = Zeitüberschreitung
    QList<std::pair<int, QByteArray>> tokenFailures;

    int tokenRequests = 0;
    int apiRequests = 0;
    int unauthorizedAnswers = 0;
    QByteArray tokenAuthorization;
    QUrlQuery tokenForm;
    QList<QNetworkRequest> tokenRequestList;

    FakeAM::Override override(QObject *replyParent)
    {
        return [this, replyParent](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *device) -> QNetworkReply * {
            const QString path = request.url().path();
            if (path.endsWith(QLatin1String("/status.php"))) {
                const QJsonObject status{{QStringLiteral("installed"), true}, {QStringLiteral("maintenance"), false},
                    {QStringLiteral("needsDbUpgrade"), false}, {QStringLiteral("version"), QStringLiteral("10.5.0.10")},
                    {QStringLiteral("versionstring"), QStringLiteral("10.5.0")}, {QStringLiteral("edition"), QStringLiteral("Community")},
                    {QStringLiteral("productname"), QStringLiteral("owncloud.online")}};
                return new FakePayloadReply(op, request, toJson(status), replyParent);
            }
            if (path.endsWith(QLatin1String("/index.php/apps/oauth2/api/v1/token"))) {
                ++tokenRequests;
                tokenRequestList.append(request);
                tokenAuthorization = request.rawHeader(QByteArrayLiteral("Authorization"));
                // nur ansehen: der HTTP-Logger liest den Körper beim Abschluss noch ab Position 0
                tokenForm = QUrlQuery(QString::fromUtf8(device ? device->peek(device->size()) : QByteArray()));
                if (!tokenFailures.isEmpty()) {
                    const auto [status, body] = tokenFailures.takeFirst();
                    if (status <= 0) {
                        auto *reply = new FakeErrorReply(op, request, replyParent, 500);
                        if (status == 0) {
                            reply->setError(QNetworkReply::RemoteHostClosedError, QStringLiteral("Connection closed"));
                        } else {
                            reply->setError(QNetworkReply::TimeoutError, QStringLiteral("Operation timed out"));
                        }
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
                if (tokenStatus == 200) {
                    return new FakePayloadReply(op, request, toJson(tokenAnswer), replyParent);
                }
                return new FakeErrorReply(op, request, replyParent, tokenStatus, toJson(tokenAnswer));
            }
            if (path.contains(QLatin1String("/ocs/v2.php/cloud/")) || path.contains(QLatin1String("/remote.php/"))) {
                ++apiRequests;
                if (request.rawHeader(QByteArrayLiteral("Authorization")) != acceptedAuthorization) {
                    ++unauthorizedAnswers;
                    return new FakeErrorReply(op, request, replyParent, 401);
                }
                if (path.contains(QLatin1String("/remote.php/"))) {
                    // WebDAV beantwortet der FakeAM (leerer Ordner)
                    return nullptr;
                }
                const QJsonObject capabilities{{QStringLiteral("core"),
                                                   QJsonObject{{QStringLiteral("status"),
                                                       QJsonObject{{QStringLiteral("version"), QStringLiteral("11.0.0.0")},
                                                           {QStringLiteral("versionstring"), QStringLiteral("11.0.0")}}}}},
                    {QStringLiteral("dav"), QJsonObject{{QStringLiteral("chunking"), QStringLiteral("1.0")}}}};
                const QJsonObject data = path.endsWith(QLatin1String("/user")) ? QJsonObject{{QStringLiteral("id"), QStringLiteral("admin")}}
                                                                                : QJsonObject{{QStringLiteral("capabilities"), capabilities}};
                const QJsonObject meta{{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("statuscode"), 200}};
                const QJsonObject ocs{{QStringLiteral("ocs"), QJsonObject{{QStringLiteral("meta"), meta}, {QStringLiteral("data"), data}}}};
                return new FakePayloadReply(op, request, toJson(ocs), replyParent);
            }
            return new FakeErrorReply(op, request, replyParent, 404);
        };
    }

    OverrideFactory overrides()
    {
        return [this](QObject *replyParent) { return override(replyParent); };
    }
};

// Credentials, deren Access-Manager der Testserver beantwortet
class TestOAuthCredentials : public HttpOAuthCredentials
{
public:
    TestOAuthCredentials(Account *account, const std::shared_ptr<OAuthTokenFile> &tokens, const OverrideFactory &overrides)
        : HttpOAuthCredentials(account, tokens)
        , _overrides(overrides)
    {
    }

    OCC::AccessManager *createAccessManager() const override
    {
        auto *accessManager = new BearerAccessManager<FakeAM>(this, const_cast<TestOAuthCredentials *>(this));
        accessManager->setOverride(_overrides(accessManager));
        return accessManager;
    }

protected:
    std::chrono::milliseconds refreshRetryDelay(int) const override { return std::chrono::milliseconds(20); }

private:
    OverrideFactory _overrides;
};

// ein Lauf von owncloudcmd mit einer Token-Datei gegen den FakeOAuthServer, in einen leeren Ordner
struct CmdRun
{
    int exitCode = -1;
    QString errorString;
    bool signInRequired = false;
};

CmdRun runSession(const std::shared_ptr<OAuthTokenFile> &tokens, FakeOAuthServer &server)
{
    CmdRun result;
    const QTemporaryDir local = TestUtils::createTempDir();
    const QString localDir = local.path() + QLatin1Char('/');
    HttpOAuthCredentials *credentials = nullptr;
    CmdSession session(serverUrlC, QStringLiteral("admin"), localDir, QStringLiteral("/"), [&credentials, &server, &tokens](Account *account) -> AbstractCredentials * {
        credentials = new TestOAuthCredentials(account, tokens, server.overrides());
        return credentials;
    });
    QSignalSpy finishedSpy(&session, &CmdSession::finished);
    session.start();
    if (finishedSpy.wait(30000)) {
        result.exitCode = finishedSpy.first().first().toInt();
    }
    if (credentials) {
        result.errorString = credentials->errorString();
        result.signInRequired = credentials->signInRequired();
    }
    return result;
}

} // namespace

class TestCmdOAuth : public QObject
{
    Q_OBJECT

    std::shared_ptr<OAuthTokenFile> loadTokens(const QString &path)
    {
        QString error;
        auto tokens = OAuthTokenFile::load(path, &error);
        if (!tokens) {
            qWarning() << "cannot load token file:" << error;
        }
        return tokens;
    }

private Q_SLOTS:
    void testTokenFileChecksServer()
    {
        const QTemporaryDir dir = TestUtils::createTempDir();
        const auto tokens = loadTokens(writeTokenFile(dir, tokenFileContent(QStringLiteral("a"), QStringLiteral("r"), 0)));
        QVERIFY(tokens);
        QVERIFY(tokens->belongsTo(QUrl(QStringLiteral("http://localhost/owncloud/"))));
        QVERIFY(tokens->belongsTo(QUrl(QStringLiteral("http://LOCALHOST:80/owncloud"))));
        QVERIFY(!tokens->belongsTo(QUrl(QStringLiteral("https://localhost/owncloud"))));
        QVERIFY(!tokens->belongsTo(QUrl(QStringLiteral("http://localhost:8080/owncloud"))));
        QVERIFY(!tokens->belongsTo(QUrl(QStringLiteral("http://elsewhere.example/owncloud"))));
        QVERIFY(!tokens->belongsTo(QUrl(QStringLiteral("http://localhost/other"))));
        QCOMPARE(tokens->tokenEndpoint(), QUrl(QStringLiteral("http://localhost/owncloud/index.php/apps/oauth2/api/v1/token")));

        // ohne server_url keine Tokens: sie könnten sonst an jeden Server gehen
        QJsonObject withoutServer = tokenFileContent(QStringLiteral("a"), QStringLiteral("r"), 0);
        withoutServer.remove(QStringLiteral("server_url"));
        QString error;
        QVERIFY(!OAuthTokenFile::load(writeTokenFile(dir, withoutServer), &error));
        QVERIFY(error.contains(QLatin1String("server_url")));
    }

    void testBearerOnlyForOwnServer()
    {
        const QTemporaryDir dir = TestUtils::createTempDir();
        const auto tokens = loadTokens(writeTokenFile(dir, tokenFileContent(QStringLiteral("access-1"), QStringLiteral("refresh-1"), 0)));
        QVERIFY(tokens);

        QList<QNetworkRequest> seen;
        const OverrideFactory record = [&seen](QObject *replyParent) -> FakeAM::Override {
            return [&seen, replyParent](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
                seen.append(request);
                return new FakePayloadReply(op, request, QByteArrayLiteral("{}"), replyParent);
            };
        };
        auto account = std::make_unique<Account>(QUuid::createUuid(), QStringLiteral("admin"), serverUrlC);
        account->setCredentials(new TestOAuthCredentials(account.get(), tokens, record));

        QNetworkRequest own(QUrl(QStringLiteral("http://localhost/owncloud/remote.php/dav/files/admin/")));
        QNetworkRequest otherHost(QUrl(QStringLiteral("http://elsewhere.example/owncloud/")));
        QNetworkRequest otherPort(QUrl(QStringLiteral("http://localhost:8080/owncloud/")));
        QNetworkRequest withoutCredentials(own);
        withoutCredentials.setAttribute(DontAddCredentialsAttribute, true);
        for (const auto &request : {own, otherHost, otherPort, withoutCredentials}) {
            account->accessManager()->get(request);
        }

        QCOMPARE(seen.size(), 4);
        QCOMPARE(seen.at(0).rawHeader(QByteArrayLiteral("Authorization")), QByteArrayLiteral("Bearer access-1"));
        QVERIFY(!seen.at(1).hasRawHeader(QByteArrayLiteral("Authorization")));
        QVERIFY(!seen.at(2).hasRawHeader(QByteArrayLiteral("Authorization")));
        QVERIFY(!seen.at(3).hasRawHeader(QByteArrayLiteral("Authorization")));
    }

    void testRefreshAfterUnauthorized()
    {
        const QTemporaryDir dir = TestUtils::createTempDir();
        const QString path = writeTokenFile(dir, tokenFileContent(QStringLiteral("access-1"), QStringLiteral("refresh-1"), QDateTime::currentSecsSinceEpoch() + 3600));
        const auto tokens = loadTokens(path);
        QVERIFY(tokens);

        FakeOAuthServer server;
        auto account = std::make_unique<Account>(QUuid::createUuid(), QStringLiteral("admin"), serverUrlC);
        auto *credentials = new TestOAuthCredentials(account.get(), tokens, server.overrides());
        account->setCredentials(credentials);
        QSignalSpy failedSpy(credentials, &AbstractCredentials::authenticationFailed);

        auto *job = new JsonApiJob(account.get(), QStringLiteral("ocs/v2.php/cloud/capabilities"), {}, {}, this);
        bool done = false;
        QNetworkReply::NetworkError result = QNetworkReply::UnknownNetworkError;
        connect(job, &JsonApiJob::finishedSignal, this, [&done, &result, job] {
            result = job->reply()->error();
            done = true;
        });
        job->start();
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);

        // abgelehnt, erneuert, mit dem neuen Token wiederholt
        QCOMPARE(result, QNetworkReply::NoError);
        QCOMPARE(server.apiRequests, 2);
        QCOMPARE(server.unauthorizedAnswers, 1);
        QCOMPARE(server.tokenRequests, 1);
        QCOMPARE(failedSpy.count(), 0);

        // Anfrage am Token-Endpunkt wie beim Desktop-Client: client_secret_basic, kein Bearer
        const OpenIdConfig desktopClient = AppConfig().openIdConfig();
        QCOMPARE(server.tokenAuthorization,
            QByteArray(QByteArrayLiteral("Basic ") + QStringLiteral("%1:%2").arg(desktopClient.clientId(), desktopClient.clientSecret()).toUtf8().toBase64()));
        QCOMPARE(server.tokenForm.queryItemValue(QStringLiteral("grant_type")), QStringLiteral("refresh_token"));
        QCOMPARE(server.tokenForm.queryItemValue(QStringLiteral("refresh_token")), QStringLiteral("refresh-1"));

        // neue Tokens sofort in der Datei, alles andere bleibt
        const QJsonObject stored = readTokenFile(path);
        QCOMPARE(stored.value(QStringLiteral("access_token")).toString(), QStringLiteral("access-2"));
        QCOMPARE(stored.value(QStringLiteral("refresh_token")).toString(), QStringLiteral("refresh-2"));
        QVERIFY(stored.value(QStringLiteral("expires_at")).toInteger() >= QDateTime::currentSecsSinceEpoch() + 3500);
        QCOMPARE(stored.value(QStringLiteral("server_url")).toString(), serverUrlC.toString());
        QCOMPARE(stored.value(QStringLiteral("note")).toString(), QStringLiteral("kept"));
        QVERIFY(!QFile::exists(path + QStringLiteral(".lock")));
    }

    void testExpiredTokenIsRefreshedFirst()
    {
        const QTemporaryDir dir = TestUtils::createTempDir();
        const QString path = writeTokenFile(dir, tokenFileContent(QStringLiteral("expired"), QStringLiteral("refresh-1"), QDateTime::currentSecsSinceEpoch() - 10));
        const auto tokens = loadTokens(path);
        QVERIFY(tokens);
        QVERIFY(tokens->needsRefresh(std::chrono::seconds(60)));

        FakeOAuthServer server;
        auto account = std::make_unique<Account>(QUuid::createUuid(), QStringLiteral("admin"), serverUrlC);
        auto *credentials = new TestOAuthCredentials(account.get(), tokens, server.overrides());
        account->setCredentials(credentials);
        QSignalSpy fetchedSpy(credentials, &AbstractCredentials::fetched);
        QSignalSpy failedSpy(credentials, &AbstractCredentials::authenticationFailed);

        credentials->fetchFromKeychain();
        QVERIFY(fetchedSpy.wait(10000));
        QCOMPARE(server.tokenRequests, 1);
        QCOMPARE(server.apiRequests, 0);
        QCOMPARE(failedSpy.count(), 0);
        QVERIFY(credentials->ready());
        QCOMPARE(readTokenFile(path).value(QStringLiteral("access_token")).toString(), QStringLiteral("access-2"));
        QVERIFY(!tokens->needsRefresh(std::chrono::seconds(60)));
    }

    void testRevokedRefreshTokenEndsRun()
    {
        const QTemporaryDir dir = TestUtils::createTempDir();
        const QString path = writeTokenFile(dir, tokenFileContent(QStringLiteral("access-1"), QStringLiteral("refresh-1"), QDateTime::currentSecsSinceEpoch() + 3600));
        const auto tokens = loadTokens(path);
        QVERIFY(tokens);

        // Antwort der oauth2-App auf ein widerrufenes Refresh-Token
        FakeOAuthServer server;
        server.tokenStatus = 400;
        server.tokenAnswer = {{QStringLiteral("error"), QStringLiteral("invalid_grant")}, {QStringLiteral("error_description"), QStringLiteral("refresh token does not exist")}};

        HttpOAuthCredentials *credentials = nullptr;
        CmdSession session(serverUrlC, QStringLiteral("admin"), dir.path() + QStringLiteral("/local/"), QStringLiteral("/"),
            [&credentials, &server, &tokens](Account *account) -> AbstractCredentials * {
                credentials = new TestOAuthCredentials(account, tokens, server.overrides());
                return credentials;
            });
        QSignalSpy finishedSpy(&session, &CmdSession::finished);
        session.start();
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().first().toInt(), static_cast<int>(CmdSession::ExitSignInRequired));
        QVERIFY(credentials);
        QVERIFY(credentials->signInRequired());
        QVERIFY2(credentials->errorString().contains(QLatin1String("invalid_grant")), qPrintable(credentials->errorString()));
        QVERIFY2(credentials->errorString().contains(QLatin1String("sign in again")), qPrintable(credentials->errorString()));
        QCOMPARE(server.tokenRequests, 1);
        // die Datei bleibt, wie sie war
        QCOMPARE(readTokenFile(path).value(QStringLiteral("refresh_token")).toString(), QStringLiteral("refresh-1"));
    }

    void testRejectedNewTokenDoesNotLoop()
    {
        const QTemporaryDir dir = TestUtils::createTempDir();
        const QString path = writeTokenFile(dir, tokenFileContent(QStringLiteral("access-1"), QStringLiteral("refresh-1"), QDateTime::currentSecsSinceEpoch() + 3600));
        const auto tokens = loadTokens(path);
        QVERIFY(tokens);

        // z. B. ein Webserver, der den Authorization-Header nicht an PHP weitergibt
        FakeOAuthServer server;
        server.acceptedAuthorization = QByteArrayLiteral("Bearer never");

        HttpOAuthCredentials *credentials = nullptr;
        CmdSession session(serverUrlC, QStringLiteral("admin"), dir.path() + QStringLiteral("/local/"), QStringLiteral("/"),
            [&credentials, &server, &tokens](Account *account) -> AbstractCredentials * {
                credentials = new TestOAuthCredentials(account, tokens, server.overrides());
                return credentials;
            });
        QSignalSpy finishedSpy(&session, &CmdSession::finished);
        session.start();
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().first().toInt(), static_cast<int>(CmdSession::ExitFailure));
        QVERIFY(credentials);
        QVERIFY(!credentials->signInRequired());
        QVERIFY2(credentials->errorString().contains(QLatin1String("Authorization header")), qPrintable(credentials->errorString()));
        QCOMPARE(server.tokenRequests, 1);
    }

    // Netz- und Serverfehler beim Erneuern: nach Pausen erneut versuchen, das Refresh-Token bleibt, der Lauf geht durch
    void testTemporaryRefreshFailuresAreRetried_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");

        const QByteArray html = QByteArrayLiteral("<!DOCTYPE html><html><body>Wartung</body></html>");
        QTest::newRow("connection closed") << 0 << QByteArray();
        QTest::newRow("timeout") << -1 << QByteArray();
        QTest::newRow("503 html") << 503 << html;
        QTest::newRow("502 proxy") << 502 << QByteArray();
        QTest::newRow("500 server_error") << 500 << toJson({{QStringLiteral("error"), QStringLiteral("server_error")}});
        QTest::newRow("302 to the login page") << 302 << html;
        QTest::newRow("200 html") << 200 << html;
    }

    void testTemporaryRefreshFailuresAreRetried()
    {
        // Arrange: das Access-Token ist abgelaufen, der Token-Endpunkt scheitert dreimal
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        const QTemporaryDir dir = TestUtils::createTempDir();
        const QString path = writeTokenFile(dir, tokenFileContent(QStringLiteral("expired"), QStringLiteral("refresh-1"), QDateTime::currentSecsSinceEpoch() - 10));
        const auto tokens = loadTokens(path);
        QVERIFY(tokens);
        FakeOAuthServer server;
        for (int i = 0; i < 3; ++i) {
            server.tokenFailures.append({status, body});
        }

        // Act
        const CmdRun run = runSession(tokens, server);

        // Assert
        QCOMPARE(run.exitCode, static_cast<int>(CmdSession::ExitSuccess));
        QVERIFY2(run.errorString.isEmpty(), qPrintable(run.errorString));
        QCOMPARE(server.tokenRequests, 4);
        QCOMPARE(server.tokenForm.queryItemValue(QStringLiteral("refresh_token")), QStringLiteral("refresh-1"));
        QCOMPARE(readTokenFile(path).value(QStringLiteral("refresh_token")).toString(), QStringLiteral("refresh-2"));
        QVERIFY(!QFile::exists(path + QStringLiteral(".lock")));
    }

    // Scheitert auch der letzte Versuch, endet der Lauf mit Exit 1 statt 2: das Refresh-Token gilt weiter, die Datei bleibt
    void testGivesUpAfterRepeatedFailures()
    {
        const QTemporaryDir dir = TestUtils::createTempDir();
        const QString path = writeTokenFile(dir, tokenFileContent(QStringLiteral("expired"), QStringLiteral("refresh-1"), QDateTime::currentSecsSinceEpoch() - 10));
        const auto tokens = loadTokens(path);
        QVERIFY(tokens);
        FakeOAuthServer server;
        for (int i = 0; i < 10; ++i) {
            server.tokenFailures.append({503, QByteArrayLiteral("<html>Service Unavailable</html>")});
        }

        const CmdRun run = runSession(tokens, server);

        QCOMPARE(run.exitCode, static_cast<int>(CmdSession::ExitFailure));
        QVERIFY(!run.signInRequired);
        QCOMPARE(server.tokenRequests, HttpOAuthCredentials::refreshAttempts);
        QCOMPARE(readTokenFile(path).value(QStringLiteral("refresh_token")).toString(), QStringLiteral("refresh-1"));
        QVERIFY(!QFile::exists(path + QStringLiteral(".lock")));
    }

    // Nur invalid_grant verlangt eine neue Anmeldung (Exit 2). Andere Fehler des Token-Endpunkts (Einrichtung, gesperrtes
    // Konto) enden mit Exit 1 ohne weitere Versuche, das Refresh-Token bleibt.
    void testOtherTokenErrorsDoNotRequireSignIn_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("error");
        QTest::newRow("invalid_request") << 400 << QStringLiteral("invalid_request");
        QTest::newRow("unauthorized_client") << 400 << QStringLiteral("unauthorized_client");
        QTest::newRow("invalid_client") << 401 << QStringLiteral("invalid_client");
    }

    void testOtherTokenErrorsDoNotRequireSignIn()
    {
        QFETCH(int, status);
        QFETCH(QString, error);
        const QTemporaryDir dir = TestUtils::createTempDir();
        const QString path = writeTokenFile(dir, tokenFileContent(QStringLiteral("expired"), QStringLiteral("refresh-1"), QDateTime::currentSecsSinceEpoch() - 10));
        const auto tokens = loadTokens(path);
        QVERIFY(tokens);
        FakeOAuthServer server;
        server.tokenFailures.append({status, toJson({{QStringLiteral("error"), error}})});

        const CmdRun run = runSession(tokens, server);

        QCOMPARE(run.exitCode, static_cast<int>(CmdSession::ExitFailure));
        QVERIFY(!run.signInRequired);
        QVERIFY2(run.errorString.contains(error), qPrintable(run.errorString));
        QCOMPARE(server.tokenRequests, 1);
        QCOMPARE(readTokenFile(path).value(QStringLiteral("refresh_token")).toString(), QStringLiteral("refresh-1"));
    }

    // Läuft das Access-Token während des Laufs ab, wird es vor der nächsten Anfrage erneuert, ohne Ablehnung (401)
    void testTokenExpiringDuringTheRunIsRefreshedFirst()
    {
        // Arrange: noch 7 s gültig, Lebensdauer 30 s, erneuert wird 6 s vor dem Ablauf
        const QTemporaryDir dir = TestUtils::createTempDir();
        QJsonObject content = tokenFileContent(QStringLiteral("access-1"), QStringLiteral("refresh-1"), QDateTime::currentSecsSinceEpoch() + 7);
        content.insert(QStringLiteral("expires_in"), 30);
        const auto tokens = loadTokens(writeTokenFile(dir, content));
        QVERIFY(tokens);
        FakeOAuthServer server;
        auto account = std::make_unique<Account>(QUuid::createUuid(), QStringLiteral("admin"), serverUrlC);
        auto *credentials = new TestOAuthCredentials(account.get(), tokens, server.overrides());
        account->setCredentials(credentials);
        QSignalSpy fetchedSpy(credentials, &AbstractCredentials::fetched);
        credentials->fetchFromKeychain();
        QCOMPARE(fetchedSpy.count(), 1);
        QCOMPARE(server.tokenRequests, 0);

        // Act: nach 1,5 s liegt der Ablauf innerhalb des Vorlaufs
        QTest::qWait(1500);
        auto *job = new JsonApiJob(account.get(), QStringLiteral("ocs/v2.php/cloud/capabilities"), {}, {}, this);
        bool done = false;
        QNetworkReply::NetworkError result = QNetworkReply::UnknownNetworkError;
        connect(job, &JsonApiJob::finishedSignal, this, [&done, &result, job] {
            result = job->reply()->error();
            done = true;
        });
        job->start();

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QCOMPARE(result, QNetworkReply::NoError);
        QCOMPARE(server.tokenRequests, 1);
        QCOMPARE(server.unauthorizedAnswers, 0);
    }

    // Mehrere Anfragen werden gleichzeitig abgelehnt: eine Erneuerung, alle wiederholt; Token-Anfrage ohne Cookies, ohne
    // Weiterleitung, mit langer Frist
    void testParallelUnauthorizedRefreshOnce()
    {
        const QTemporaryDir dir = TestUtils::createTempDir();
        const auto tokens = loadTokens(writeTokenFile(dir, tokenFileContent(QStringLiteral("access-1"), QStringLiteral("refresh-1"), QDateTime::currentSecsSinceEpoch() + 3600)));
        QVERIFY(tokens);
        FakeOAuthServer server;
        auto account = std::make_unique<Account>(QUuid::createUuid(), QStringLiteral("admin"), serverUrlC);
        account->setCredentials(new TestOAuthCredentials(account.get(), tokens, server.overrides()));

        int done = 0;
        int ok = 0;
        for (int i = 0; i < 5; ++i) {
            auto *job = new JsonApiJob(account.get(), QStringLiteral("ocs/v2.php/cloud/user"), {}, {}, this);
            connect(job, &JsonApiJob::finishedSignal, this, [&done, &ok, job] {
                ++done;
                ok += job->reply()->error() == QNetworkReply::NoError ? 1 : 0;
            });
            job->start();
        }

        QTRY_COMPARE_WITH_TIMEOUT(done, 5, 10000);
        QCOMPARE(ok, 5);
        QCOMPARE(server.tokenRequests, 1);
        QCOMPARE(server.unauthorizedAnswers, 5);
        const QNetworkRequest token = server.tokenRequestList.first();
        QCOMPARE(token.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), static_cast<int>(QNetworkRequest::Manual));
        QCOMPARE(token.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(), static_cast<int>(QNetworkRequest::Manual));
        QCOMPARE(token.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), static_cast<int>(QNetworkRequest::ManualRedirectPolicy));
        QVERIFY(token.transferTimeout() >= 120000);
    }

    // Lässt sich die Token-Datei nicht schreiben, endet der Lauf nicht: das neue Refresh-Token bleibt im Speicher (das alte hat
    // der Server verworfen), die Datei wird später erneut geschrieben
    void testUnwritableTokenFileDoesNotEndTheRun()
    {
        // Arrange
        const QTemporaryDir dir = TestUtils::createTempDir();
        const QString path = writeTokenFile(dir, tokenFileContent(QStringLiteral("expired"), QStringLiteral("refresh-1"), QDateTime::currentSecsSinceEpoch() - 10));
        const auto tokens = loadTokens(path);
        QVERIFY(tokens);
        // schreibgeschützt: QSaveFile ersetzt die Datei dann nicht, die Sperrdatei daneben geht weiter
        const auto filePermissions = QFile::permissions(path);
        QFile::setPermissions(path, QFileDevice::ReadOwner);
        const auto restore = qScopeGuard([&] { QFile::setPermissions(path, filePermissions); });
        QString probeError;
        if (tokens->save(&probeError)) {
            QSKIP("the token file stays writable here (running as root?)");
        }
        FakeOAuthServer server;

        // Act
        const CmdRun run = runSession(tokens, server);

        // Assert
        QCOMPARE(run.exitCode, static_cast<int>(CmdSession::ExitSuccess));
        QCOMPARE(tokens->refreshToken(), QStringLiteral("refresh-2"));
        QCOMPARE(server.tokenRequests, 1);
    }
};

QTEST_GUILESS_MAIN(TestCmdOAuth)
#include "testcmdoauth.moc"
