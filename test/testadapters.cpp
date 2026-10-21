/*
 * Copyright (C) Erik Verbruggen <erik.verbruggenkiteworks.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <QtTest>

#include "../src/gui/networkadapters/determineauthtypeadapter.h"
#include "../src/gui/networkadapters/discoverwebfingerserviceadapter.h"
#include "../src/gui/networkadapters/fetchcapabilitiesadapter.h"
// #include "../src/gui/networkadapters/resolveurladapter.h"
#include "../src/gui/networkadapters/serverdiagnostics.h"
#include "../src/gui/networkadapters/webfingerlookupadapter.h"
#include "testutils/syncenginetestutils.h"

using namespace OCC;

/**
 * @brief A reply object carrying json payload, with the headers set accordingly, and a 200 Ok
 *        response code.
 */
class AdapterJSonReply : public FakeReply
{
    Q_OBJECT

public:
    AdapterJSonReply(QNetworkAccessManager::Operation op, const QNetworkRequest &request, const QByteArray &payload, QObject *parent)
        : FakeReply(parent)
        , payload(payload)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        open(QIODevice::ReadOnly);

        QMetaObject::invokeMethod(this, &AdapterJSonReply::respond, Qt::QueuedConnection);
    }

    Q_INVOKABLE void respond()
    {
        setHeader(QNetworkRequest::ContentLengthHeader, payload.size());
        setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        Q_EMIT metaDataChanged();
        if (bytesAvailable())
            Q_EMIT readyRead();
        checkedFinished();
    }

    qint64 bytesAvailable() const override
    {
        return payload.size();
    }

    qint64 readData(char *data, qint64 maxlen) override
    {
        qint64 len = std::min(qint64 { payload.size() }, maxlen);
        std::copy(payload.cbegin(), payload.cbegin() + len, data);
        payload.remove(0, static_cast<int>(len));
        return len;
    }

private:
    QByteArray payload;
};

/*
 * Note: in this class we cannot use QCOMPARE/QVERIFY/etc, so we use asserts to validate the requests.
 */
class TestAdaptersAM : public FakeAM
{
    Q_OBJECT

public:
    TestAdaptersAM(const QUrl &urlWithoutPath, const QString &authToken = {})
        : FakeAM(nullptr)
        , _urlWithoutPath(urlWithoutPath)
        , _authToken(authToken)
    {
        setOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *device) -> QNetworkReply * {
            return this->handleRequest(op, req, device);
        });
    }

protected:
    virtual QNetworkReply *handleRequest(QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *device)
    {
        Q_UNUSED(device)

        if (op == QNetworkAccessManager::GetOperation) {
            QString reqPath = req.url().path();
            if (reqPath == QStringLiteral("/.well-known/webfinger")) {
                const QString resource = QUrlQuery(req.url()).queryItemValue(QStringLiteral("resource"));
                if (resource == _urlWithoutPath.toString()) {
                    return handleWebFingerDiscoveryRequest(op, req);
                } else if (resource == QStringLiteral("acct:me@%1").arg(_urlWithoutPath.host())) {
                    return handleWebFingerLookupRequest(op, req);
                }
            } else if (reqPath == QStringLiteral("/ocs/v2.php/cloud/capabilities")) {
                return handleCapabilitiesRequest(op, req);
            }
        } else if (op == QNetworkAccessManager::CustomOperation && req.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == "PROPFIND") {
            auto reply = new FakeErrorReply(op, req, this, 401, {});
            reply->setRawHeader(QByteArrayLiteral("www-authenticate"), QByteArrayLiteral("Bearer realm=\"ownCloudTest\""));
            return reply;
        }

        Q_UNREACHABLE(); // Bomb out, the adapter send a strange request operation or path
    }

    virtual QNetworkReply *handleWebFingerDiscoveryRequest(QNetworkAccessManager::Operation op, const QNetworkRequest &req)
    {
        return new AdapterJSonReply(op, req, {ValidWebFingerResponse}, this);
    }

    virtual QNetworkReply *handleWebFingerLookupRequest(QNetworkAccessManager::Operation op, const QNetworkRequest &req)
    {
        verifyAuthentication(req);
        const QString expectedResource = QStringLiteral("acct:me@%1").arg(_urlWithoutPath.host());
        Q_ASSERT(QUrlQuery(req.url()).queryItemValue(QStringLiteral("resource")) == expectedResource);

        return new AdapterJSonReply(op, req, {ValidWebFingerLookupResponse}, this);
    }

    virtual QNetworkReply *handleCapabilitiesRequest(QNetworkAccessManager::Operation op, const QNetworkRequest &req)
    {
        verifyAuthentication(req);
        Q_ASSERT(QUrlQuery(req.url()).queryItemValue(QStringLiteral("format")) == QStringLiteral("json"));

        return new AdapterJSonReply(op, req, ValidFetchCapabilitiesResponse, this);
    }

    QNetworkReply *errorResponse(QNetworkAccessManager::Operation op, const QNetworkRequest &req, int httpErrorCode = 404)
    {
        return new FakeErrorReply(op, req, this, httpErrorCode);
    }

    void verifyAuthentication(const QNetworkRequest &req)
    {
        Q_ASSERT(req.rawHeader("Authorization") == QByteArrayLiteral("Bearer ").append(_authToken.toUtf8()));
    }

    static QByteArray ValidWebFingerResponse;
    static QByteArray ValidWebFingerLookupResponse;
    static QByteArray ValidFetchCapabilitiesResponse;

private:
    const QUrl _urlWithoutPath;
    const QString _authToken;
};

QByteArray TestAdaptersAM::ValidWebFingerResponse = QByteArrayLiteral(
    "{\"subject\":\"https://demo.test\",\"links\":[{\"rel\":\"http://openid.net/specs/connect/1.0/issuer\",\"href\":\"https://demo.test/realms/test\"}]}");
QByteArray TestAdaptersAM::ValidWebFingerLookupResponse =
    QByteArrayLiteral("{\"links\":[{\"rel\":\"http://webfinger.owncloud/rel/server-instance\",\"href\":\"one.demo.test\"},{\"rel\":\"http://webfinger.owncloud/"
                      "rel/server-instance\",\"href\":\"two.demo.test\"}]}");
QByteArray TestAdaptersAM::ValidFetchCapabilitiesResponse = QByteArrayLiteral(
    "{\"ocs\":{\"data\":{\"capabilities\":{\"checksums\":{\"preferredUploadType\":\"SHA3-256\",\"supportedTypes\":[\"SHA3-256\"]},\"core\":{\"pollinterval\":"
    "30000,\"status\":{\"edition\":\"Enterprise\",\"installed\":true,\"maintenance\":false,\"needsDbUpgrade\":false,\"product\":\"test\",\"productname\":"
    "\"test\",\"productversion\":\"8.4.1\",\"version\":\"10.0.11.5\",\"versionstring\":\"10.0.11\"},\"support-url-signing\":false},\"dav\":{\"chunking\":\"\","
    "\"chunkingParallelUploadDisabled\":false,\"reports\":null,\"trashbin\":\"\"},\"files\":{\"app_providers\":null,\"archivers\":null,\"bigfilechunking\":"
    "false,\"blacklisted_files\":null,\"favorites\":false,\"permanent_deletion\":false,\"privateLinks\":true,\"tus_support\":null,\"undelete\":false,"
    "\"versioning\":false},\"files_sharing\":{\"allow_custom\":false,\"api_enabled\":false,\"auto_accept_share\":false,\"can_rename\":false,\"default_"
    "permissions\":0,\"deny_access\":false,\"federation\":null,\"group_sharing\":false,\"public\":null,\"resharing\":false,\"resharing_default\":false,"
    "\"search_min_length\":0,\"share_with_group_members_only\":false,\"share_with_membership_groups_only\":false,\"user\":{\"profile_picture\":true,\"send_"
    "mail\":false,\"settings\":null},\"user_enumeration\":null},\"notifications\":{},\"spaces\":{\"enabled\":true,\"has_multiple_personal_spaces\":true,"
    "\"projects\":true,\"version\":\"1.0.0\"}},\"version\":{\"edition\":\"Enterprise\",\"major\":10,\"micro\":11,\"minor\":0,\"product\":\"test\",\"string\":"
    "\"10.0.11\"}},\"meta\":{\"message\":\"OK\",\"status\":\"ok\",\"statuscode\":100}}}");


class FailureAM : public TestAdaptersAM
{
public:
    FailureAM(const QUrl &urlWithoutPath)
        : TestAdaptersAM(urlWithoutPath)
    {
    }

protected:
    QNetworkReply *handleRequest(QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *device) override
    {
        Q_UNUSED(device);

        // Send the reply
        // no well-known endpoints defined
        return errorResponse(op, req);
    }
};

/*
 * Simuliert für den DetermineAuthTypeAdapter einen owncloud.online-Server, dessen PROPFIND-Antwort, OpenID-Konfiguration
 * und Token-Endpunkt der App „OAuth2“ je Test einstellbar sind. Voreingestellt ist ein Server ohne die App, so wie ihn
 * testoo zeigte: DAV verlangt nur Basic, keine OpenID-Konfiguration (404), der Token-Endpunkt leitet zur Anmeldeseite um.
 */
class AuthProbeAM : public FakeAM
{
    Q_OBJECT

public:
    using Responder = std::function<QNetworkReply *(QNetworkAccessManager::Operation, const QNetworkRequest &)>;

    AuthProbeAM()
        : FakeAM(nullptr)
    {
        setOverride([this](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *) -> QNetworkReply * {
            const QString path = req.url().path();
            if (op == QNetworkAccessManager::CustomOperation && req.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == "PROPFIND"
                && path == QLatin1String("/remote.php/webdav/")) {
                return propfind(op, req);
            }
            if (op == QNetworkAccessManager::GetOperation && path == QLatin1String("/.well-known/openid-configuration")) {
                ++openIdRequests;
                openIdTimeoutMs = req.transferTimeout();
                return openIdConfiguration(op, req);
            }
            if (op == QNetworkAccessManager::PostOperation && path == QLatin1String("/index.php/apps/oauth2/api/v1/token")) {
                ++tokenRequests;
                tokenTimeoutMs = req.transferTimeout();
                return token(op, req);
            }
            ++unexpectedRequests;
            return new FakeErrorReply(op, req, this, 404);
        });
    }

    QNetworkReply *challenge(QNetworkAccessManager::Operation op, const QNetworkRequest &req, const QByteArray &wwwAuthenticate)
    {
        auto reply = new FakeErrorReply(op, req, this, 401);
        reply->setRawHeader(QByteArrayLiteral("WWW-Authenticate"), wwwAuthenticate);
        return reply;
    }

    // so antwortet der Kern auf eine unbekannte Route, solange niemand angemeldet ist
    QNetworkReply *redirectToLogin(QNetworkAccessManager::Operation op, const QNetworkRequest &req)
    {
        auto reply = new FakePayloadReply(op, req, QByteArrayLiteral("<!DOCTYPE html><html><body></body></html>"), this);
        reply->setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 302);
        reply->setRawHeader(QByteArrayLiteral("Location"), QByteArrayLiteral("https://demo.test/index.php/login"));
        reply->setRawHeader(QByteArrayLiteral("Content-Type"), QByteArrayLiteral("text/html; charset=UTF-8"));
        return reply;
    }

    // so antwortet die App „OAuth2“ auf eine Anfrage ohne grant_type
    QNetworkReply *oauth2InvalidRequest(QNetworkAccessManager::Operation op, const QNetworkRequest &req)
    {
        auto reply = new FakeErrorReply(op, req, this, 400, QByteArrayLiteral("{\"error\":\"invalid_request\"}"));
        reply->setRawHeader(QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json; charset=utf-8"));
        return reply;
    }

    Responder propfind = [this](QNetworkAccessManager::Operation op, const QNetworkRequest &req) {
        return challenge(op, req, QByteArrayLiteral("Basic realm=\"owncloud.online\", charset=\"UTF-8\""));
    };
    Responder openIdConfiguration = [this](QNetworkAccessManager::Operation op, const QNetworkRequest &req) -> QNetworkReply * {
        return new FakeErrorReply(op, req, this, 404);
    };
    Responder token = [this](QNetworkAccessManager::Operation op, const QNetworkRequest &req) { return redirectToLogin(op, req); };

    int openIdRequests = 0;
    int tokenRequests = 0;
    int unexpectedRequests = 0;
    int openIdTimeoutMs = -1;
    int tokenTimeoutMs = -1;

    static QByteArray ValidOpenIdConfiguration;
};

QByteArray AuthProbeAM::ValidOpenIdConfiguration = QByteArrayLiteral(
    "{\"issuer\":\"https://demo.test\",\"authorization_endpoint\":\"https://demo.test/authorize\",\"token_endpoint\":\"https://demo.test/token\","
    "\"token_endpoint_auth_methods_supported\":[\"client_secret_basic\"]}");

class TestAdapters : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testWebFingerDiscovery_Failure()
    {
        FailureAM fakeAM(testUrl());

        DiscoverWebFingerServiceAdapter adapter(&fakeAM, testUrl());
        DiscoverWebFingerServiceResult result = adapter.getResult();
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.href.isEmpty());
    }

    void testWebFingerDiscovery_Success()
    {
        TestAdaptersAM fakeAM(testUrl());

        DiscoverWebFingerServiceAdapter adapter(&fakeAM, testUrl());
        DiscoverWebFingerServiceResult result = adapter.getResult();
        QVERIFY(result.error.isEmpty());
        QCOMPARE(result.href, QStringLiteral("https://demo.test/realms/test"));
    }

    void testWebFingerDiscovery_BrokenJSON()
    {
        class BrokenAM : public TestAdaptersAM
        {
        public:
            BrokenAM()
                : TestAdaptersAM(testUrl())
            {
            }

        protected:
            QNetworkReply *handleWebFingerDiscoveryRequest(QNetworkAccessManager::Operation op, const QNetworkRequest &req) override
            {
                return new AdapterJSonReply(op, req, ValidWebFingerResponse.first(80), this);
            }
        } fakeAM;

        DiscoverWebFingerServiceAdapter adapter(&fakeAM, testUrl());
        DiscoverWebFingerServiceResult result = adapter.getResult();
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.href.isEmpty());
    }

    void testDetermineAuthType_OAUTH()
    {
        TestAdaptersAM fakeAM(testUrl());

        DetermineAuthTypeAdapter adapter(&fakeAM, testUrl());
        DetermineAuthTypeResult result = adapter.getResult();
        QVERIFY(result.error.isEmpty());
        QCOMPARE(result.type, AuthenticationType::OAuth);
    }

    void testFetchCapabilitiesAdapter_Success()
    {
        TestAdaptersAM fakeAM(testUrl(), testToken());

        FetchCapabilitiesAdapter adapter(&fakeAM, testToken(), testUrl(), nullptr);
        FetchCapabilitiesResult result = adapter.getResult();
        QVERIFY(result.error.isEmpty());
    }

    void testWebFingerLookupAdapter_Success()
    {
        TestAdaptersAM fakeAM(testUrl(), testToken());

        WebFingerLookupAdapter adapter(&fakeAM, testToken(), testUrl(), nullptr);
        WebFingerLookupResult result = adapter.getResult();
        QVERIFY(result.success());
        QCOMPARE(result.urls.size(), 2);
    }

    void testDetermineAuthType_OAuth2AppMissing_data()
    {
        QTest::addColumn<bool>("redirect");
        // der Kern leitet unbekannte Routen zur Anmeldeseite um, manche Reverse-Proxys antworten selbst mit 404
        QTest::newRow("redirect to login") << true;
        QTest::newRow("not found") << false;
    }

    void testDetermineAuthType_OAuth2AppMissing()
    {
        QFETCH(bool, redirect);
        AuthProbeAM fakeAM;
        // Verlangt DAV Benutzername und Kennwort, gilt das als Anmeldung mit Kennwort (testDetermineAuthType_Basic). Als
        // fehlende App zählt nur eine Ablehnung ohne bekanntes Verfahren (etwa hinter einem Proxy, der den Kopf entfernt).
        fakeAM.propfind = [&fakeAM](QNetworkAccessManager::Operation op, const QNetworkRequest &req) { return fakeAM.challenge(op, req, {}); };
        if (!redirect) {
            fakeAM.token = [&fakeAM](QNetworkAccessManager::Operation op, const QNetworkRequest &req) -> QNetworkReply * {
                return new FakeErrorReply(op, req, &fakeAM, 404, QByteArrayLiteral("<html><body>Not Found</body></html>"));
            };
        }

        DetermineAuthTypeAdapter adapter(&fakeAM, testUrl());
        const DetermineAuthTypeResult result = adapter.getResult();
        QVERIFY(!result.success());
        QCOMPARE(result.issue, ServerDiagnostics::Issue::OAuth2AppMissing);
        QCOMPARE(result.error, ServerDiagnostics::message(ServerDiagnostics::Issue::OAuth2AppMissing, testUrl()));
        QVERIFY(result.error.contains(QLatin1String("\"OAuth2\"")));
        QCOMPARE(fakeAM.openIdRequests, 1);
        QCOMPARE(fakeAM.tokenRequests, 1);
        QCOMPARE(fakeAM.unexpectedRequests, 0);
    }

    // Die Proben dienen nur der Fehlermeldung: höchstens 15 s je Anfrage statt httpTimeout (5 min), sonst hielte ein WAF
    // oder Tarpit die Adressseite bis zu 10 Minuten fest
    void testDetermineAuthType_ProbesAreShort()
    {
        AuthProbeAM fakeAM;
        fakeAM.propfind = [&fakeAM](QNetworkAccessManager::Operation op, const QNetworkRequest &req) {
            return fakeAM.challenge(op, req, QByteArrayLiteral("Negotiate"));
        };

        DetermineAuthTypeAdapter adapter(&fakeAM, testUrl());
        adapter.getResult();

        QCOMPARE(fakeAM.tokenRequests, 1);
        QVERIFY2(fakeAM.openIdTimeoutMs > 0 && fakeAM.openIdTimeoutMs <= 15000, qPrintable(QString::number(fakeAM.openIdTimeoutMs)));
        QVERIFY2(fakeAM.tokenTimeoutMs > 0 && fakeAM.tokenTimeoutMs <= 15000, qPrintable(QString::number(fakeAM.tokenTimeoutMs)));
    }

    void testDetermineAuthType_OAuth2AppPresent()
    {
        // mit der App verlangt DAV einen Bearer-Token; weitere Anfragen sind dann unnötig
        AuthProbeAM fakeAM;
        fakeAM.propfind = [&fakeAM](QNetworkAccessManager::Operation op, const QNetworkRequest &req) {
            return fakeAM.challenge(op, req, QByteArrayLiteral("Bearer realm=\"owncloud.online\""));
        };
        fakeAM.token = [&fakeAM](QNetworkAccessManager::Operation op, const QNetworkRequest &req) { return fakeAM.oauth2InvalidRequest(op, req); };

        DetermineAuthTypeAdapter adapter(&fakeAM, testUrl());
        const DetermineAuthTypeResult result = adapter.getResult();
        QVERIFY(result.success());
        QCOMPARE(result.type, AuthenticationType::OAuth);
        QCOMPARE(result.issue, ServerDiagnostics::Issue::None);
        QCOMPARE(fakeAM.openIdRequests + fakeAM.tokenRequests + fakeAM.unexpectedRequests, 0);
    }

    void testDetermineAuthType_OAuth2AppAnswersWithoutBearer()
    {
        // antwortet die App am Token-Endpunkt, fehlt sie nicht, auch wenn DAV keinen Bearer-Token verlangt
        AuthProbeAM fakeAM;
        fakeAM.propfind = [&fakeAM](QNetworkAccessManager::Operation op, const QNetworkRequest &req) { return fakeAM.challenge(op, req, {}); };
        fakeAM.token = [&fakeAM](QNetworkAccessManager::Operation op, const QNetworkRequest &req) { return fakeAM.oauth2InvalidRequest(op, req); };

        DetermineAuthTypeAdapter adapter(&fakeAM, testUrl());
        const DetermineAuthTypeResult result = adapter.getResult();
        QVERIFY(!result.success());
        QCOMPARE(result.issue, ServerDiagnostics::Issue::Other);
        QCOMPARE(fakeAM.tokenRequests, 1);
        QCOMPARE(fakeAM.unexpectedRequests, 0);
    }

    void testDetermineAuthType_OpenIdConnect_data()
    {
        QTest::addColumn<bool>("bearer");
        QTest::newRow("with bearer challenge") << true;
        QTest::newRow("without bearer challenge") << false;
    }

    void testDetermineAuthType_OpenIdConnect()
    {
        // ein Server mit OpenID Connect braucht die App „OAuth2“ nicht: nie als fehlende App melden
        QFETCH(bool, bearer);
        AuthProbeAM fakeAM;
        if (bearer) {
            fakeAM.propfind = [&fakeAM](QNetworkAccessManager::Operation op, const QNetworkRequest &req) {
                return fakeAM.challenge(op, req, QByteArrayLiteral("Basic realm=\"test\", Bearer realm=\"test\""));
            };
        }
        fakeAM.openIdConfiguration = [&fakeAM](QNetworkAccessManager::Operation op, const QNetworkRequest &req) -> QNetworkReply * {
            return new AdapterJSonReply(op, req, AuthProbeAM::ValidOpenIdConfiguration, &fakeAM);
        };

        DetermineAuthTypeAdapter adapter(&fakeAM, testUrl());
        const DetermineAuthTypeResult result = adapter.getResult();
        // ohne Bearer-Challenge entscheidet die OpenID-Konfiguration: Anmeldung im Browser, nicht mit Kennwort
        QVERIFY(result.success());
        QCOMPARE(result.type, AuthenticationType::OAuth);
        QCOMPARE(result.issue, ServerDiagnostics::Issue::None);
        QCOMPARE(fakeAM.openIdRequests, bearer ? 0 : 1);
        QCOMPARE(fakeAM.tokenRequests, 0);
        QCOMPARE(fakeAM.unexpectedRequests, 0);
    }

    // Server ohne die App „OAuth2“ und ohne OpenID Connect (testoo): Anmeldung mit Benutzername und Kennwort statt Fehler
    void testDetermineAuthType_Basic()
    {
        AuthProbeAM fakeAM;

        DetermineAuthTypeAdapter adapter(&fakeAM, testUrl());
        const DetermineAuthTypeResult result = adapter.getResult();
        QVERIFY(result.success());
        QCOMPARE(result.type, AuthenticationType::Basic);
        QCOMPARE(result.issue, ServerDiagnostics::Issue::None);
        QCOMPARE(fakeAM.openIdRequests, 1);
        QVERIFY2(fakeAM.openIdTimeoutMs > 0 && fakeAM.openIdTimeoutMs <= 15000, qPrintable(QString::number(fakeAM.openIdTimeoutMs)));
        QCOMPARE(fakeAM.tokenRequests, 0);
        QCOMPARE(fakeAM.unexpectedRequests, 0);
    }

    void testAuthenticationTypeFromChallenge_data()
    {
        QTest::addColumn<QByteArray>("challenge");
        QTest::addColumn<int>("type");

        QTest::newRow("basic") << QByteArray("Basic realm=\"owncloud.online\", charset=\"UTF-8\"") << int(AuthenticationType::Basic);
        QTest::newRow("basic upper case") << QByteArray("BASIC realm=\"x\"") << int(AuthenticationType::Basic);
        QTest::newRow("bearer") << QByteArray("Bearer realm=\"owncloud.online\"") << int(AuthenticationType::OAuth);
        QTest::newRow("basic and bearer") << QByteArray("Basic realm=\"x\", Bearer realm=\"x\"") << int(AuthenticationType::OAuth);
        QTest::newRow("bearer without parameters") << QByteArray("Basic realm=\"x\", Bearer") << int(AuthenticationType::OAuth);
        QTest::newRow("realm named basic") << QByteArray("Negotiate, Digest realm=\"basic\"") << int(AuthenticationType::Unknown);
        QTest::newRow("scheme with prefix") << QByteArray("XBasic realm=\"x\"") << int(AuthenticationType::Unknown);
        QTest::newRow("empty") << QByteArray() << int(AuthenticationType::Unknown);
    }

    void testAuthenticationTypeFromChallenge()
    {
        QFETCH(QByteArray, challenge);
        QFETCH(int, type);
        QCOMPARE(int(authenticationTypeFromChallenge(challenge)), type);
    }

    void testDetermineAuthType_ServerProblems_data()
    {
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<int>("networkError");
        QTest::addColumn<int>("issue");
        QTest::newRow("maintenance") << 503 << int(QNetworkReply::ServiceUnavailableError) << int(ServerDiagnostics::Issue::Maintenance);
        QTest::newRow("unreachable") << 0 << int(QNetworkReply::HostNotFoundError) << int(ServerDiagnostics::Issue::Unreachable);
        QTest::newRow("tls") << 0 << int(QNetworkReply::SslHandshakeFailedError) << int(ServerDiagnostics::Issue::Tls);
        QTest::newRow("no dav") << 404 << int(QNetworkReply::ContentNotFoundError) << int(ServerDiagnostics::Issue::NotOwncloudServer);
    }

    void testDetermineAuthType_ServerProblems()
    {
        QFETCH(int, httpStatus);
        QFETCH(int, networkError);
        QFETCH(int, issue);
        AuthProbeAM fakeAM;
        fakeAM.propfind = [&](QNetworkAccessManager::Operation op, const QNetworkRequest &req) -> QNetworkReply * {
            auto reply = new FakeErrorReply(op, req, &fakeAM, httpStatus);
            if (httpStatus == 0) {
                // keine HTTP-Antwort, nur der Transportfehler
                reply->setAttribute(QNetworkRequest::HttpStatusCodeAttribute, QVariant());
            }
            reply->setError(static_cast<QNetworkReply::NetworkError>(networkError), QStringLiteral("Fake error"));
            return reply;
        };

        DetermineAuthTypeAdapter adapter(&fakeAM, testUrl());
        const DetermineAuthTypeResult result = adapter.getResult();
        QVERIFY(!result.success());
        QCOMPARE(int(result.issue), issue);
        QCOMPARE(result.error, ServerDiagnostics::message(result.issue, testUrl()));
        QCOMPARE(fakeAM.openIdRequests + fakeAM.tokenRequests + fakeAM.unexpectedRequests, 0);
    }

    void testServerDiagnostics_ClassifyFailure_data()
    {
        QTest::addColumn<int>("networkError");
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<int>("issue");
        QTest::newRow("host not found") << int(QNetworkReply::HostNotFoundError) << 0 << int(ServerDiagnostics::Issue::Unreachable);
        QTest::newRow("connection refused") << int(QNetworkReply::ConnectionRefusedError) << 0 << int(ServerDiagnostics::Issue::Unreachable);
        QTest::newRow("transfer timeout") << int(QNetworkReply::OperationCanceledError) << 0 << int(ServerDiagnostics::Issue::Unreachable);
        QTest::newRow("proxy") << int(QNetworkReply::ProxyConnectionRefusedError) << 0 << int(ServerDiagnostics::Issue::Unreachable);
        QTest::newRow("tls handshake") << int(QNetworkReply::SslHandshakeFailedError) << 0 << int(ServerDiagnostics::Issue::Tls);
        QTest::newRow("503") << int(QNetworkReply::ServiceUnavailableError) << 503 << int(ServerDiagnostics::Issue::Maintenance);
        QTest::newRow("502") << int(QNetworkReply::UnknownServerError) << 502 << int(ServerDiagnostics::Issue::Maintenance);
        QTest::newRow("404") << int(QNetworkReply::ContentNotFoundError) << 404 << int(ServerDiagnostics::Issue::NotOwncloudServer);
        QTest::newRow("500") << int(QNetworkReply::InternalServerError) << 500 << int(ServerDiagnostics::Issue::Other);
        QTest::newRow("403") << int(QNetworkReply::ContentAccessDenied) << 403 << int(ServerDiagnostics::Issue::Other);
    }

    void testServerDiagnostics_ClassifyFailure()
    {
        QFETCH(int, networkError);
        QFETCH(int, httpStatus);
        QFETCH(int, issue);
        QCOMPARE(int(ServerDiagnostics::classifyFailure(static_cast<QNetworkReply::NetworkError>(networkError), httpStatus)), issue);
    }

    void testServerDiagnostics_ParseStatus_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<bool>("maintenance");
        QTest::newRow("ready") << QByteArray("{\"installed\":true,\"maintenance\":false,\"needsDbUpgrade\":false,\"version\":\"11.0.0.0\","
                                             "\"versionstring\":\"11.0.0\",\"productname\":\"owncloud.online\"}")
                               << true << false;
        QTest::newRow("maintenance") << QByteArray("{\"installed\":true,\"maintenance\":true,\"needsDbUpgrade\":false,\"version\":\"11.0.0.0\"}") << true
                                     << true;
        QTest::newRow("upgrade pending") << QByteArray("{\"installed\":true,\"maintenance\":false,\"needsDbUpgrade\":true,\"version\":\"11.0.0.0\"}") << true
                                         << true;
        QTest::newRow("html") << QByteArray("<!DOCTYPE html><html><body>Welcome</body></html>") << false << false;
        QTest::newRow("other json") << QByteArray("{\"status\":\"ok\"}") << false << false;
        QTest::newRow("empty") << QByteArray() << false << false;
    }

    void testServerDiagnostics_ParseStatus()
    {
        QFETCH(QByteArray, body);
        QFETCH(bool, valid);
        QFETCH(bool, maintenance);
        const ServerDiagnostics::ServerStatus status = ServerDiagnostics::parseStatus(body);
        QCOMPARE(status.valid, valid);
        QCOMPARE(status.maintenance, maintenance);
    }

    void testServerDiagnostics_ClassifyOAuth2TokenReply_data()
    {
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("location");
        QTest::addColumn<int>("issue");
        const QString none;
        QTest::newRow("app answers") << 400 << QByteArray("{\"error\":\"invalid_request\"}") << none << int(ServerDiagnostics::Issue::Other);
        QTest::newRow("app answers 401") << 401 << QByteArray("{\"error\":\"invalid_client\"}") << none << int(ServerDiagnostics::Issue::Other);
        // so antwortet der Kern ohne die App (lib/base.php), mit und ohne index.php in der Adresse
        QTest::newRow("redirect to login") << 302 << QByteArray() << QStringLiteral("https://demo.test/index.php/login") << int(ServerDiagnostics::Issue::OAuth2AppMissing);
        QTest::newRow("redirect to login, pretty url") << 302 << QByteArray() << QStringLiteral("/owncloud/login?redirect_url=x") << int(ServerDiagnostics::Issue::OAuth2AppMissing);
        QTest::newRow("not found") << 404 << QByteArray("<html></html>") << none << int(ServerDiagnostics::Issue::OAuth2AppMissing);
        QTest::newRow("gone") << 410 << QByteArray() << none << int(ServerDiagnostics::Issue::OAuth2AppMissing);
        // sagt nichts über die App: Weiterleitung anderswohin (etwa auf https), Proxy-Anmeldung, Sammelseite
        QTest::newRow("redirect elsewhere") << 301 << QByteArray() << QStringLiteral("https://demo.test/index.php/apps/oauth2/api/v1/token") << int(ServerDiagnostics::Issue::Other);
        QTest::newRow("redirect without location") << 302 << QByteArray() << none << int(ServerDiagnostics::Issue::Other);
        QTest::newRow("proxy basic auth") << 401 << QByteArray("<html>Authorization Required</html>") << none << int(ServerDiagnostics::Issue::Other);
        QTest::newRow("waf") << 403 << QByteArray("<html>Forbidden</html>") << none << int(ServerDiagnostics::Issue::Other);
        QTest::newRow("html page") << 200 << QByteArray("<!DOCTYPE html><html></html>") << none << int(ServerDiagnostics::Issue::Other);
        // ohne HTTP-Antwort oder bei einem Serverfehler ist die Ursache unklar
        QTest::newRow("no reply") << 0 << QByteArray() << none << int(ServerDiagnostics::Issue::Other);
        QTest::newRow("server error") << 500 << QByteArray("<html></html>") << none << int(ServerDiagnostics::Issue::Other);
    }

    void testServerDiagnostics_ClassifyOAuth2TokenReply()
    {
        QFETCH(int, httpStatus);
        QFETCH(QByteArray, body);
        QFETCH(QString, location);
        QFETCH(int, issue);
        QCOMPARE(int(ServerDiagnostics::classifyOAuth2TokenReply(httpStatus, body, QUrl(location))), issue);
    }

private:
    static QUrl testUrl() { return QUrl(QStringLiteral("https://demo.test")); }
    static QString testToken() { return QStringLiteral("theToken"); }
};

QTEST_GUILESS_MAIN(TestAdapters)
#include "testadapters.moc"
