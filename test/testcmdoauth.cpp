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
#include <QSignalSpy>
#include <QTest>
#include <QUrlQuery>

#include <algorithm>
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

// Server mit oauth2-App: nimmt nur ein Access-Token an und beantwortet den Token-Endpunkt
class FakeOAuthServer
{
public:
    QByteArray acceptedAuthorization = QByteArrayLiteral("Bearer access-2");
    int tokenStatus = 200;
    QJsonObject tokenAnswer{{QStringLiteral("access_token"), QStringLiteral("access-2")}, {QStringLiteral("refresh_token"), QStringLiteral("refresh-2")},
        {QStringLiteral("token_type"), QStringLiteral("Bearer")}, {QStringLiteral("expires_in"), 3600}, {QStringLiteral("user_id"), QStringLiteral("admin")}};

    int tokenRequests = 0;
    int apiRequests = 0;
    int unauthorizedAnswers = 0;
    QByteArray tokenAuthorization;
    QUrlQuery tokenForm;

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
                tokenAuthorization = request.rawHeader(QByteArrayLiteral("Authorization"));
                // nur ansehen: der HTTP-Logger liest den Körper beim Abschluss noch ab Position 0
                tokenForm = QUrlQuery(QString::fromUtf8(device ? device->peek(device->size()) : QByteArray()));
                if (tokenStatus == 200) {
                    return new FakePayloadReply(op, request, toJson(tokenAnswer), replyParent);
                }
                return new FakeErrorReply(op, request, replyParent, tokenStatus, toJson(tokenAnswer));
            }
            if (path.contains(QLatin1String("/ocs/v2.php/cloud/"))) {
                ++apiRequests;
                if (request.rawHeader(QByteArrayLiteral("Authorization")) != acceptedAuthorization) {
                    ++unauthorizedAnswers;
                    return new FakeErrorReply(op, request, replyParent, 401);
                }
                const QJsonObject data = path.endsWith(QLatin1String("/user")) ? QJsonObject{{QStringLiteral("id"), QStringLiteral("admin")}}
                                                                                : QJsonObject{{QStringLiteral("capabilities"), QJsonObject{}}};
                const QJsonObject meta{{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("statuscode"), 200}};
                const QJsonObject ocs{{QStringLiteral("ocs"), QJsonObject{{QStringLiteral("meta"), meta}, {QStringLiteral("data"), data}}}};
                return new FakePayloadReply(op, request, toJson(ocs), replyParent);
            }
            return new FakeErrorReply(op, request, replyParent, 404);
        };
    }
};

// Credentials, deren Access-Manager der Testserver beantwortet
class TestOAuthCredentials : public HttpOAuthCredentials
{
public:
    TestOAuthCredentials(Account *account, const std::shared_ptr<OAuthTokenFile> &tokens, const FakeAM::Override &override)
        : HttpOAuthCredentials(account, tokens)
        , _override(override)
    {
    }

    OCC::AccessManager *createAccessManager() const override
    {
        auto *accessManager = new BearerAccessManager<FakeAM>(this, const_cast<TestOAuthCredentials *>(this));
        accessManager->setOverride(_override);
        return accessManager;
    }

private:
    FakeAM::Override _override;
};

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
        FakeAM::Override record = [&seen, this](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            seen.append(request);
            return new FakePayloadReply(op, request, QByteArrayLiteral("{}"), this);
        };
        auto account = std::make_unique<Account>(QUuid::createUuid(), QStringLiteral("admin"), serverUrlC);
        account->setCredentials(new TestOAuthCredentials(account.get(), tokens, record));

        QNetworkRequest own(QUrl(QStringLiteral("http://localhost/owncloud/remote.php/dav/files/admin/")));
        QNetworkRequest otherHost(QUrl(QStringLiteral("http://elsewhere.example/owncloud/")));
        QNetworkRequest otherPort(QUrl(QStringLiteral("http://localhost:8080/owncloud/")));
        QNetworkRequest withoutCredentials(own);
        withoutCredentials.setAttribute(DontAddCredentialsAttribute, true);
        QList<QNetworkReply *> replies;
        for (const auto &request : {own, otherHost, otherPort, withoutCredentials}) {
            replies.append(account->accessManager()->get(request));
        }
        // Die Antworten gehören dem Test, der Access-Manager aber dem Konto: erst nach ihrem Abschluss
        // aufräumen, sonst ruft Qt beim späten finished() den schon gelöschten Access-Manager auf
        QTRY_VERIFY(std::all_of(replies.cbegin(), replies.cend(), [](const QNetworkReply *reply) { return reply->isFinished(); }));

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
        auto *credentials = new TestOAuthCredentials(account.get(), tokens, server.override(this));
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
        auto *credentials = new TestOAuthCredentials(account.get(), tokens, server.override(this));
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
            [&credentials, &server, &tokens, this](Account *account) -> AbstractCredentials * {
                credentials = new TestOAuthCredentials(account, tokens, server.override(this));
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
            [&credentials, &server, &tokens, this](Account *account) -> AbstractCredentials * {
                credentials = new TestOAuthCredentials(account, tokens, server.override(this));
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
};

QTEST_GUILESS_MAIN(TestCmdOAuth)
#include "testcmdoauth.moc"
