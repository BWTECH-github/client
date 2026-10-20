/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QtTest/QtTest>
#include <QDesktopServices>

#include "accountsgui/accountmodalwidget.h"
#include "common/asserts.h"
#include "creds/oauth.h"
#include "creds/requestauthenticationcontroller.h"
#include "creds/requestauthenticationwidget.h"
#include "newaccountwizard/oauthpagecontroller.h"
#include "testutils/syncenginetestutils.h"
#include "theme.h"

#include <config/appconfig.h>

#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWizard>
#include <QWizardPage>

using namespace std::chrono_literals;
using namespace OCC;

namespace {

// IPv6-Loopback fehlt z. B. in Containern mit abgeschaltetem IPv6
bool ipv6LoopbackAvailable()
{
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHostIPv6, 0);
}

bool acceptsConnections(const QHostAddress &address, quint16 port)
{
    QTcpSocket socket;
    socket.connectToHost(address, port);
    return socket.waitForConnected(5000);
}

quint16 redirectPort(const QUrl &authorisationLink)
{
    const QUrl redirectUri(QUrlQuery(authorisationLink).queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
    return static_cast<quint16>(redirectUri.port());
}

// simulierter Browser nach der Anmeldung: ruft http://localhost:<port>/?code=…&state=… zum Anmeldelink über 127.0.0.1 auf
void browserCallback(const QUrl &authorisationLink, QObject *parent)
{
    const QByteArray state = QUrlQuery(authorisationLink).queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded).toUtf8();
    const QByteArray get = "GET /?code=probe-code&state=" + QUrl::toPercentEncoding(QString::fromUtf8(state)) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto *socket = new QTcpSocket(parent);
    QObject::connect(socket, &QTcpSocket::connected, socket, [socket, get] { socket->write(get); });
    socket->connectToHost(QHostAddress::LocalHost, redirectPort(authorisationLink));
}

QByteArray openIdConfigurationPayload()
{
    return QJsonDocument(QJsonObject{{QStringLiteral("authorization_endpoint"), QStringLiteral("oauthtest://auth.example.com/realms/test/protocol/openid-connect/auth")},
                             {QStringLiteral("token_endpoint"), QStringLiteral("oauthtest://openidserver/token_endpoint")},
                             {QStringLiteral("token_endpoint_auth_methods_supported"), QJsonArray{QStringLiteral("client_secret_post")}}})
        .toJson();
}

QByteArray userInfoJson()
{
    return QByteArrayLiteral("{\"ocs\":{\"data\":{\"id\":\"admin\",\"display-name\":\"Admin\",\"email\":\"admin@admin.admin\"}}}");
}

QByteArray tokenJson()
{
    return QByteArrayLiteral("{\"access_token\":\"123\",\"refresh_token\":\"456\",\"token_type\":\"Bearer\",\"expires_in\":3600,\"user_id\":\"admin\"}");
}

}

class DesktopServiceHook : public QObject
{
    Q_OBJECT
Q_SIGNALS:
    void hooked(const QUrl &);

public:
    DesktopServiceHook() { QDesktopServices::setUrlHandler(QStringLiteral("oauthtest"), this, "hooked"); }
    ~DesktopServiceHook() { QDesktopServices::unsetUrlHandler(QStringLiteral("oauthtest")); }
};

static const QUrl sOpenIdBaseURL(QStringLiteral("oauthtest://auth.example.com"));
static const QUrl sOpenIdAuthURL(QStringLiteral("oauthtest://auth.example.com/realms/test/protocol/openid-connect/auth"));


class FakePostReply : public QNetworkReply
{
    Q_OBJECT
public:
    std::unique_ptr<QIODevice> payload;
    bool aborted = false;

    FakePostReply(QNetworkAccessManager::Operation op, const QNetworkRequest &request,
                  std::unique_ptr<QIODevice> payload_, QObject *parent)
        : QNetworkReply{parent}, payload{std::move(payload_)}
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        open(QIODevice::ReadOnly);
        payload->open(QIODevice::ReadOnly);
        QMetaObject::invokeMethod(this, &FakePostReply::respond, Qt::QueuedConnection);
    }

    Q_INVOKABLE virtual void respond() {
        if (aborted) {
            setError(OperationCanceledError, QStringLiteral("Operation Canceled"));
            Q_EMIT metaDataChanged();
            checkedFinished();
            return;
        }
        setHeader(QNetworkRequest::ContentLengthHeader, payload->size());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        Q_EMIT metaDataChanged();
        if (bytesAvailable())
            Q_EMIT readyRead();
        checkedFinished();
    }

    void abort() override {
        aborted = true;
    }

    void checkedFinished() {
        if (!isFinished()) {
            setFinished(true);
            Q_EMIT finished();
        }
    }

    qint64 bytesAvailable() const override {
        if (aborted)
            return 0;
        return payload->bytesAvailable();
    }

    qint64 readData(char *data, qint64 maxlen) override {
        return payload->read(data, maxlen);
    }
};

// Reply with a small delay
class SlowFakePostReply : public FakePostReply {
    Q_OBJECT
public:
    using FakePostReply::FakePostReply;
    void respond() override {
        // override of FakePostReply::respond, will call the real one with a delay.
        QTimer::singleShot(100ms, this, [this] { this->FakePostReply::respond(); });
    }
};


class OAuthTestCase : public QObject
{
    Q_OBJECT
    DesktopServiceHook desktopServiceHook;

protected:
    QString _expectedClientId = Theme::instance()->oauthClientId();

public:
    enum State { StartState,
        StatusPhpState,
        BrowserOpened,
        TokenAsked,
        UserInfoFetched,
        CustomState } state = StartState;
    Q_ENUM(State);

    QString localHost = QStringLiteral("localhost");
    bool replyToBrowserOk = false;
    bool gotAuthOk = false;
    virtual bool done() const { return replyToBrowserOk && gotAuthOk; }

    FakeAM *fakeAm = nullptr;
    QNetworkAccessManager realQNAM;
    QPointer<QNetworkReply> browserReply = nullptr;
    QString code = QString::fromUtf8(generateEtag());
    OCC::Account *account;

    std::unique_ptr<AccountBasedOAuth> oauth;

    virtual std::unique_ptr<AccountBasedOAuth> prepareOauth()
    {
        fakeAm = new FakeAM(nullptr);
        account = new Account(QUuid::createUuid(), "admin", sOpenIdBaseURL);

        // the account seizes ownership over the qnam in account->setCredentials(...) by keeping a shared pointer on it
        // therefore, we should never call fakeAm->setThis(...)
        // Lisa todo: ???
        account->setCredentials(new FakeCredentials(account, fakeAm, account));
        fakeAm->setOverride([this](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *device) {
            if (req.url().path().endsWith(QLatin1String(".well-known/openid-configuration"))) {
                return this->wellKnownReply(op, req);
            }
            if (req.url().path().endsWith(QLatin1String("status.php"))) {
                return this->statusPhpReply(op, req);
            }
            if (req.url().path().endsWith(QLatin1String("ocs/v2.php/cloud/user")) && req.url().query() == QLatin1String("format=json")) {
                return this->userInfoReply(op, req);
            }
            OC_ASSERT(device);
            OC_ASSERT(device && device->bytesAvailable() > 0); // OAuth2 always sends around POST data.
            return this->tokenReply(op, req, device);
        });

        QObject::connect(&desktopServiceHook, &DesktopServiceHook::hooked, this, &OAuthTestCase::openBrowserHook);

        AppConfig config;
        auto out = std::make_unique<AccountBasedOAuth>(account, config.openIdConfig(), nullptr);
        QObject::connect(out.get(), &OAuth::result, this, &OAuthTestCase::oauthResult);
        return out;
    }

    virtual void test()
    {
        runTest();

        QTRY_VERIFY(done());
    }

    virtual void runTest()
    {
        oauth = prepareOauth();
        oauth->startAuthentication();

        QSignalSpy spy(oauth.get(), &OCC::OAuth::authorisationLinkChanged);
        if (spy.wait()) {
            oauth->openBrowser();
        }
    }

    virtual void openBrowserHook(const QUrl &url) {
        QCOMPARE(state, StatusPhpState);
        state = BrowserOpened;
        QCOMPARE(url.path(), sOpenIdAuthURL.path());
        QVERIFY(url.toString().startsWith(sOpenIdBaseURL.toString()));
        QUrlQuery query(url);
        QCOMPARE(query.queryItemValue(QStringLiteral("response_type")), QLatin1String("code"));
        QCOMPARE(query.queryItemValue(QStringLiteral("client_id")), _expectedClientId);
        QUrl redirectUri(query.queryItemValue(QStringLiteral("redirect_uri")));
        QCOMPARE(redirectUri.host(), localHost);
        redirectUri.setQuery(QStringLiteral("code=%1&state=%2").arg(code, query.queryItemValue(QStringLiteral("state"))));
        createBrowserReply(QNetworkRequest(redirectUri));
    }

    virtual QNetworkReply *createBrowserReply(const QNetworkRequest &request) {
        auto r = request;
        // don't follow the redirect to owncloud://success
        r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        browserReply = realQNAM.get(r);
        QObject::connect(browserReply, &QNetworkReply::finished, this, &OAuthTestCase::browserReplyFinished);
        return browserReply;
    }

    virtual void browserReplyFinished() {
        QCOMPARE(sender(), browserReply.data());
        QCOMPARE(state, UserInfoFetched);
        browserReply->deleteLater();
        QCOMPARE(QNetworkReply::NoError, browserReply->error());
        QCOMPARE(browserReply->rawHeader("Location"), QByteArray("owncloud://success"));
        replyToBrowserOk = true;
    }

    virtual QNetworkReply *tokenReply(QNetworkAccessManager::Operation op, const QNetworkRequest &req, [[maybe_unused]] QIODevice *device)
    {
        OC_ASSERT(state == BrowserOpened);
        state = TokenAsked;
        OC_ASSERT(op == QNetworkAccessManager::PostOperation);

        auto payload = std::make_unique<QBuffer>();
        payload->setData(tokenReplyPayload());
        return new FakePostReply(op, req, std::move(payload), fakeAm);
    }

    virtual QNetworkReply *statusPhpReply(QNetworkAccessManager::Operation op, const QNetworkRequest &req)
    {
        OC_ASSERT(state == StartState);
        state = StatusPhpState;
        OC_ASSERT(op == QNetworkAccessManager::GetOperation);
        OC_ASSERT(req.url().toString().startsWith(sOpenIdBaseURL.toString()));
        OC_ASSERT(req.url().path() == sOpenIdBaseURL.path() + QStringLiteral("/status.php"));
        auto payload = std::make_unique<QBuffer>();
        payload->setData(statusPhpPayload());
        return new FakePostReply(op, req, std::move(payload), fakeAm);
    }

    virtual QNetworkReply *userInfoReply(QNetworkAccessManager::Operation op, const QNetworkRequest &req)
    {
        OC_ASSERT(state == TokenAsked);
        state = UserInfoFetched;
        OC_ASSERT(op == QNetworkAccessManager::GetOperation);
        OC_ASSERT(req.url().toString().startsWith(sOpenIdBaseURL.toString()));
        OC_ASSERT(req.url().path() == sOpenIdBaseURL.path() + QStringLiteral("/ocs/v2.php/cloud/user"));
        OC_ASSERT(req.url().query() == QStringLiteral("format=json"));
        auto payload = std::make_unique<QBuffer>();
        payload->setData(userInfoPayload());
        return new FakePostReply(op, req, std::move(payload), fakeAm);
    }

    virtual QNetworkReply * wellKnownReply(QNetworkAccessManager::Operation op, const QNetworkRequest & req) {
        OC_ASSERT(op == QNetworkAccessManager::GetOperation);
        QJsonDocument jsondata(QJsonObject{
            {QStringLiteral("authorization_endpoint"), sOpenIdAuthURL.toString()},
            {QStringLiteral("token_endpoint"), QStringLiteral("oauthtest://openidserver/token_endpoint")},
            {QStringLiteral("token_endpoint_auth_methods_supported"), QJsonArray{QStringLiteral("client_secret_post")}},
        });
        return new FakePayloadReply(op, req, jsondata.toJson(), fakeAm);
    }

    virtual QByteArray tokenReplyPayload() const {
        // the dummy server provides the user admin
        QJsonDocument jsondata(QJsonObject{{QStringLiteral("access_token"), QStringLiteral("123")}, {QStringLiteral("refresh_token"), QStringLiteral("456")},
            {QStringLiteral("message_url"), QStringLiteral("owncloud://success")}, {QStringLiteral("user_id"), QStringLiteral("admin")},
            {QStringLiteral("token_type"), QStringLiteral("Bearer")}});
        return jsondata.toJson();
    }

    virtual QByteArray statusPhpPayload() const
    {
        QJsonDocument jsondata(
            QJsonObject{{QStringLiteral("installed"), true}, {QStringLiteral("maintenance"), false}, {QStringLiteral("needsDbUpgrade"), false},
                {QStringLiteral("version"), QStringLiteral("10.5.0.10")}, {QStringLiteral("versionstring"), QStringLiteral("10.5.0")},
                {QStringLiteral("edition"), QStringLiteral("Enterprise")}, {QStringLiteral("productname"), QStringLiteral("ownCloud")}});
        return jsondata.toJson();
    }

    virtual QByteArray userInfoPayload() const
    {
        // the dummy server provides the user admin
        // we don't provide "meta" at the moment, since it is not used
        QJsonDocument jsonData(QJsonObject{{QStringLiteral("ocs"),
            QJsonObject{{QStringLiteral("data"),
                QJsonObject{
                    {QStringLiteral("display-name"), QStringLiteral("Admin")},
                    {QStringLiteral("id"), QStringLiteral("admin")},
                    {QStringLiteral("email"), QStringLiteral("admin@admin.admin")},

                }}}}});
        return jsonData.toJson();
    }

    virtual void oauthResult(OAuth::Result result, const QString &token, const QString &refreshToken)
    {
        QCOMPARE(result, OAuth::LoggedIn);
        QCOMPARE(state, UserInfoFetched);
        QCOMPARE(token, QStringLiteral("123"));
        QCOMPARE(refreshToken, QStringLiteral("456"));
        gotAuthOk = true;
    }
};

// Anmeldeseite des Assistenten gegen einen gefälschten Server: .well-known nennt die Endpunkte,
// der „Browser“ merkt sich die geöffneten Anmeldelinks
class OAuthPageFixture : public QObject
{
    Q_OBJECT

public:
    OAuthPageFixture()
        : fakeAm(new FakeAM(this))
        , controller(&page, fakeAm, nullptr)
    {
        fakeAm->setOverride([this](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *) -> QNetworkReply * {
            if (!req.url().path().endsWith(QLatin1String(".well-known/openid-configuration"))) {
                return nullptr;
            }
            const QJsonDocument json(QJsonObject{{QStringLiteral("authorization_endpoint"), sOpenIdAuthURL.toString()},
                {QStringLiteral("token_endpoint"), QStringLiteral("oauthtest://openidserver/token_endpoint")},
                {QStringLiteral("token_endpoint_auth_methods_supported"), QJsonArray{QStringLiteral("client_secret_post")}}});
            return new FakePayloadReply(op, req, json.toJson(), fakeAm);
        });
        controller.setServerUrl(sOpenIdBaseURL);
        controller.setAuthenticationUrl(sOpenIdBaseURL);
        QDesktopServices::setUrlHandler(QStringLiteral("oauthtest"), this, "browserOpened");
    }

    ~OAuthPageFixture() override { QDesktopServices::unsetUrlHandler(QStringLiteral("oauthtest")); }

    OAuth *oauth() const { return controller.findChild<OAuth *>(); }

    QList<QUrl> openedLinks;
    FakeAM *fakeAm;
    QWizardPage page;
    OAuthPageController controller;

public Q_SLOTS:
    void browserOpened(const QUrl &url) { openedLinks.append(url); }
};

// Anmeldeseite in einem echten Assistenten (davor die Adressseite) gegen einen gefälschten Server, der die Anmeldung bis
// zu den Capabilities beantwortet. onUserInfo läuft bei der Abfrage des Benutzers, also während der Prüfung des Kontos.
class OAuthWizardFixture : public QObject
{
    Q_OBJECT

public:
    OAuthWizardFixture()
        : fakeAm(new FakeAM(this))
    {
        auto *urlPage = new QWizardPage;
        auto *oauthPage = new QWizardPage;
        wizard.addPage(urlPage);
        wizard.addPage(oauthPage);
        controller = new OAuthPageController(oauthPage, fakeAm, this);
        controller->setServerUrl(sOpenIdBaseURL);
        controller->setAuthenticationUrl(sOpenIdBaseURL);
        fakeAm->setOverride([this](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *) -> QNetworkReply * {
            const QString path = req.url().path();
            if (path.endsWith(QLatin1String(".well-known/openid-configuration"))) {
                return new FakePayloadReply(op, req, openIdConfigurationPayload(), fakeAm);
            }
            if (req.url().host() == QLatin1String("openidserver")) {
                auto payload = std::make_unique<QBuffer>();
                payload->setData(tokenJson());
                return new FakePostReply(op, req, std::move(payload), fakeAm);
            }
            if (path.endsWith(QLatin1String("ocs/v2.php/cloud/user"))) {
                ++userInfoRequests;
                if (onUserInfo) {
                    onUserInfo();
                }
                return new FakePayloadReply(op, req, userInfoJson(), fakeAm);
            }
            if (path.endsWith(QLatin1String("ocs/v2.php/cloud/capabilities"))) {
                ++capabilitiesRequests;
                auto *reply = new FakePayloadReply(op, req,
                    QByteArrayLiteral("{\"ocs\":{\"data\":{\"capabilities\":{\"core\":{\"status\":{\"version\":\"11.0.0.0\",\"versionstring\":\"11.0.0\"}}}}}}"),
                    fakeAm);
                reply->setRawHeader(QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json; charset=utf-8"));
                return reply;
            }
            return nullptr;
        });
        QDesktopServices::setUrlHandler(QStringLiteral("oauthtest"), this, "browserOpened");
        // wie im Programm: von der Adressseite zur Anmeldeseite
        wizard.restart();
        wizard.next();
    }

    ~OAuthWizardFixture() override
    {
        // vor dem Assistenten: der Controller greift auf die Elemente der Seite zu
        delete controller;
        QDesktopServices::unsetUrlHandler(QStringLiteral("oauthtest"));
    }

    OAuth *oauth() const { return controller->findChild<OAuth *>(); }

    QList<QUrl> openedLinks;
    FakeAM *fakeAm;
    QWizard wizard;
    OAuthPageController *controller = nullptr;
    std::function<void()> onUserInfo;
    int userInfoRequests = 0;
    int capabilitiesRequests = 0;

public Q_SLOTS:
    void browserOpened(const QUrl &url) { openedLinks.append(url); }
};

// Wie im Programm bekommt jede Prüfung des Servers einen eigenen AccessManager: CheckServerJob übernimmt ihn und löscht
// ihn mit sich. Die FakeCredentials gäben immer denselben heraus, der zweite Anlauf griffe dann auf einen gelöschten zu.
class FreshAccessManagerCredentials : public FakeCredentials
{
public:
    FreshAccessManagerCredentials(Account *account, FakeAM *am, const FakeAM::Override &override, QObject *parent)
        : FakeCredentials(account, am, parent)
        , _am(am)
        , _override(override)
    {
    }

    OCC::AccessManager *createAccessManager() const override
    {
        // der erste ist der des Kontos (Account::setCredentials)
        if (!_handedOut) {
            _handedOut = true;
            return _am;
        }
        auto *am = new FakeAM(nullptr);
        am->setOverride(_override);
        return am;
    }

private:
    FakeAM *_am;
    FakeAM::Override _override;
    mutable bool _handedOut = false;
};

// Dialog „Anmeldung erforderlich“ eines bestehenden Kontos gegen einen gefälschten Server
class ReauthFixture : public QObject
{
    Q_OBJECT

public:
    ReauthFixture()
        : fakeAm(new FakeAM(nullptr))
        , account(new Account(QUuid::createUuid(), QStringLiteral("admin"), sOpenIdBaseURL, this))
        , controller(new RequestAuthenticationController(this))
    {
        const FakeAM::Override server = [this](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *) -> QNetworkReply * {
            const QString path = req.url().path();
            if (path.endsWith(QLatin1String("status.php"))) {
                ++statusRequests;
                if (serverDown) {
                    return new FakeErrorReply(op, req, fakeAm, 503);
                }
                return new FakePayloadReply(op, req,
                    QByteArrayLiteral("{\"installed\":true,\"maintenance\":false,\"needsDbUpgrade\":false,\"version\":\"11.0.0.0\",\"versionstring\":"
                                      "\"11.0.0\",\"edition\":\"Community\",\"productname\":\"owncloud.online\"}"),
                    fakeAm);
            }
            if (path.endsWith(QLatin1String(".well-known/openid-configuration"))) {
                return new FakePayloadReply(op, req, openIdConfigurationPayload(), fakeAm);
            }
            if (req.url().host() == QLatin1String("openidserver")) {
                auto payload = std::make_unique<QBuffer>();
                payload->setData(tokenJson());
                return new FakePostReply(op, req, std::move(payload), fakeAm);
            }
            if (path.endsWith(QLatin1String("ocs/v2.php/cloud/user"))) {
                return new FakePayloadReply(op, req, userInfoJson(), fakeAm);
            }
            return nullptr;
        };
        fakeAm->setOverride(server);
        // die Anmeldedaten übernehmen den FakeAM
        account->setCredentials(new FreshAccessManagerCredentials(account, fakeAm, server, account));
        QObject::connect(controller, &RequestAuthenticationController::requestAccountModal, this, [this](AccountModalWidget *widget) { modal.reset(widget); });
        QDesktopServices::setUrlHandler(QStringLiteral("oauthtest"), this, "browserOpened");
    }

    ~ReauthFixture() override
    {
        delete controller;
        QDesktopServices::unsetUrlHandler(QStringLiteral("oauthtest"));
    }

    QPushButton *button(const char *name) const { return modal ? modal->findChild<QPushButton *>(QString::fromLatin1(name)) : nullptr; }
    OAuth *oauth() const { return controller->findChild<OAuth *>(); }

    QList<QUrl> openedLinks;
    FakeAM *fakeAm;
    Account *account;
    RequestAuthenticationController *controller;
    std::unique_ptr<AccountModalWidget> modal;
    bool serverDown = false;
    int statusRequests = 0;

public Q_SLOTS:
    void browserOpened(const QUrl &url) { openedLinks.append(url); }
};

class TestOAuth: public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testBasic()
    {
        OAuthTestCase test;
        test.test();
    }


    void testWrongUser()
    {
        struct Test : OAuthTestCase {
            QByteArray tokenReplyPayload() const override {
                // the dummy server provides the user admin
                QJsonDocument jsondata(QJsonObject{{QStringLiteral("access_token"), QStringLiteral("123")},
                    {QStringLiteral("refresh_token"), QStringLiteral("456")}, {QStringLiteral("message_url"), QStringLiteral("owncloud://success")},
                    {QStringLiteral("user_id"), QStringLiteral("wrong_user")}, {QStringLiteral("token_type"), QStringLiteral("Bearer")}});
                return jsondata.toJson();
            }

            void browserReplyFinished() override {
                QCOMPARE(sender(), browserReply.data());
                QCOMPARE(state, TokenAsked);
                browserReply->deleteLater();
                QCOMPARE(QNetworkReply::AuthenticationRequiredError, browserReply->error());
            }

            bool done() const override{
                return true;
            }
        };
        Test test;
        test.test();
    }

    // Test for https://github.com/owncloud/client/pull/6057
    void testCloseBrowserDontCrash()
    {
        struct Test : OAuthTestCase {
            QNetworkReply *tokenReply(QNetworkAccessManager::Operation op, const QNetworkRequest &req, [[maybe_unused]] QIODevice *device) override
            {
                OC_ASSERT(browserReply);
                // simulate the fact that the browser is closing the connection
                browserReply->abort();

                OC_ASSERT(state == BrowserOpened);
                state = TokenAsked;

                auto payload = std::make_unique<QBuffer>();
                payload->setData(tokenReplyPayload());
                return new SlowFakePostReply(op, req, std::move(payload), fakeAm);
            }

            void browserReplyFinished() override
            {
                QCOMPARE(sender(), browserReply.data());
                QCOMPARE(browserReply->error(), QNetworkReply::OperationCanceledError);
                replyToBrowserOk = true;
            }
        } test;
        test.test();
    }

    void testRandomConnections()
    {
        // Test that we can send random garbage to the litening socket and it does not prevent the connection
        struct Test : OAuthTestCase {
            QNetworkReply *createBrowserReply(const QNetworkRequest &request) override {
                QTimer::singleShot(0, this, [this, request] {
                    auto port = request.url().port();
                    state = CustomState;
                    const QVector<QByteArray> payloads = {
                        "GET FOFOFO HTTP 1/1\n\n",
                        "GET /?code=invalie HTTP 1/1\n\n",
                        "GET /?code=xxxxx&bar=fff",
                        QByteArray("\0\0\0", 3),
                        QByteArray("GET \0\0\0 \n\n\n\n\n\0", 14),
                        QByteArray("GET /?code=éléphant\xa5 HTTP\n"),
                        QByteArray("\n\n\n\n"),
                    };
                    for (const auto &x : payloads) {
                        auto socket = new QTcpSocket(this);
                        socket->connectToHost(QStringLiteral("localhost"), port);
                        QVERIFY(socket->waitForConnected());
                        socket->write(x);
                    }

                    // Do the actual request a bit later
                    QTimer::singleShot(100ms, this, [this, request] {
                        QCOMPARE(state, CustomState);
                        state = BrowserOpened;
                        this->OAuthTestCase::createBrowserReply(request);
                    });
               });
               return nullptr;
            }

            QNetworkReply *tokenReply(QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *device) override
            {
                if (state == CustomState)
                    return new FakeErrorReply{op, req, this, 500};
                return OAuthTestCase::tokenReply(op, req, device);
            }

            void oauthResult(OAuth::Result result, const QString &token, const QString &refreshToken) override
            {
                if (state != CustomState) {
                    return OAuthTestCase::oauthResult(result, token, refreshToken);
                }
                QCOMPARE(result, OAuth::Error);
            }
        } test;
        test.test();
    }

    void testWellKnown() {
        struct Test : OAuthTestCase {
            Test()
            {
                localHost = QStringLiteral("localhost");
            }

            void openBrowserHook(const QUrl & url) override {
                OC_ASSERT(url.host() == QStringLiteral("auth.example.com"));
                QUrl url2 = url;
                url2.setHost(sOpenIdBaseURL.host());
                OAuthTestCase::openBrowserHook(url2);
            }

            /*
            QNetworkReply *tokenReply(QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *device) override
            {
                OC_ASSERT(browserReply);
                OC_ASSERT(request.url().toString().startsWith(QStringLiteral("oauthtest://openidserver/token_endpoint")));
                auto req = request;
                req.setUrl(QUrl(request.url().toString().replace(QLatin1String("oauthtest://openidserver/token_endpoint"),
                    sOpenIdBaseURL.toString() + QStringLiteral("/index.php/apps/oauth2/api/v1/token"))));
                return OAuthTestCase::tokenReply(op, req, device);
            }*/
        } test;
        test.test();
    }

    void testWellKnown307() {
        struct Test : OAuthTestCase {
            Test()
            {
                localHost = QStringLiteral("localhost");
            }

            QNetworkReply *wellKnownReply(QNetworkAccessManager::Operation op, const QNetworkRequest &req) override
            {
                return new FakeErrorReply(op, req, fakeAm, 404);
            }

            void oauthResult(OAuth::Result result, const QString &token, const QString &refreshToken) override
            {
                OAuthTestCase::oauthResult(result, token, refreshToken);
            }

            void openBrowserHook(const QUrl & url) override {
                QCOMPARE(state, StatusPhpState);
                state = BrowserOpened;
                QCOMPARE(url.host(), sOpenIdBaseURL.host());
                QCOMPARE(url.path(), QStringLiteral("/index.php/apps/oauth2/authorize"));
                QUrlQuery query(url);
                QCOMPARE(query.queryItemValue(QStringLiteral("response_type")), QLatin1String("code"));
                QCOMPARE(query.queryItemValue(QStringLiteral("client_id")), _expectedClientId);
                QUrl redirectUri(query.queryItemValue(QStringLiteral("redirect_uri")));
                QCOMPARE(redirectUri.host(), localHost);
                redirectUri.setQuery(QStringLiteral("code=%1&state=%2").arg(code, query.queryItemValue(QStringLiteral("state"))));
                createBrowserReply(QNetworkRequest(redirectUri));
            }
        } test;
        test.test();
    }

    // owncloud.online: authorization_endpoint mit „//“ im Pfad und einem eigenen Parameter des Anbieters (wie „p=…“ bei Azure
    // AD B2C). Utility::concatUrlPath() brach daran mit Q_ASSERT ab; jetzt bleibt der Endpunkt, wie er ist, samt Parameter.
    void testAuthEndpointWithDoubledSlashAndQuery()
    {
        struct Test : OAuthTestCase {
            QNetworkReply *wellKnownReply(QNetworkAccessManager::Operation op, const QNetworkRequest &req) override
            {
                const QJsonObject data{
                    {QStringLiteral("authorization_endpoint"), QStringLiteral("oauthtest://auth.example.com//realms/test/auth?p=b2c_1_signin")},
                    {QStringLiteral("token_endpoint"), QStringLiteral("oauthtest://openidserver/token_endpoint")},
                    {QStringLiteral("token_endpoint_auth_methods_supported"), QJsonArray{QStringLiteral("client_secret_post")}}};
                return new FakePayloadReply(op, req, QJsonDocument(data).toJson(), fakeAm);
            }

            void openBrowserHook(const QUrl &url) override
            {
                QCOMPARE(state, StatusPhpState);
                state = BrowserOpened;
                QCOMPARE(url.host(), sOpenIdBaseURL.host());
                QCOMPARE(url.path(), QStringLiteral("//realms/test/auth"));
                QUrlQuery query(url);
                QCOMPARE(query.queryItemValue(QStringLiteral("p")), QStringLiteral("b2c_1_signin"));
                QCOMPARE(query.queryItemValue(QStringLiteral("response_type")), QLatin1String("code"));
                QCOMPARE(query.queryItemValue(QStringLiteral("client_id")), _expectedClientId);
                QUrl redirectUri(query.queryItemValue(QStringLiteral("redirect_uri")));
                QCOMPARE(redirectUri.host(), localHost);
                redirectUri.setQuery(QStringLiteral("code=%1&state=%2").arg(code, query.queryItemValue(QStringLiteral("state"))));
                createBrowserReply(QNetworkRequest(redirectUri));
            }
        } test;
        test.test();
    }

    void testTimeout()
    {
        struct Test : OAuthTestCase
        {
            QScopedValueRollback<std::chrono::seconds> rollback;

            Test()
                : rollback(AbstractNetworkJob::httpTimeout, 1s)
            {
                localHost = QStringLiteral("localhost");
            }

            QNetworkReply *statusPhpReply(QNetworkAccessManager::Operation op, const QNetworkRequest &req) override
            {
                OC_ASSERT(op == QNetworkAccessManager::GetOperation);
                return new FakeHangingReply(op, req, fakeAm);
            }

            void oauthResult(OAuth::Result result, const QString &token, const QString &refreshToken) override
            {
                Q_UNUSED(token);
                Q_UNUSED(refreshToken);

                QCOMPARE(state, StartState);
                QCOMPARE(result, OAuth::Error);
                gotAuthOk = true;
                replyToBrowserOk = true;
            }
        } test;
        test.test();
    }

    void testCallbackOverLoopback_data()
    {
        QTest::addColumn<QHostAddress>("address");

        QTest::newRow("IPv4") << QHostAddress(QHostAddress::LocalHost);
        QTest::newRow("IPv6") << QHostAddress(QHostAddress::LocalHostIPv6);
    }

    // Browser lösen localhost je nach System als 127.0.0.1 oder als ::1 auf: der Rückruf muss über beide ankommen.
    // Nach dem Rückruf ist der Listener auf beiden Adressen zu.
    void testCallbackOverLoopback()
    {
        QFETCH(QHostAddress, address);
        if (address.protocol() == QAbstractSocket::IPv6Protocol && !ipv6LoopbackAvailable()) {
            QSKIP("no IPv6 loopback on this system");
        }

        struct Test : OAuthTestCase
        {
            QHostAddress browserAddress;
            quint16 callbackPort = 0;
            QByteArray browserResponse;

            // simulierter Browser: ruft http://localhost:<port>/?code=…&state=… über die gewählte Loopback-Adresse auf
            QNetworkReply *createBrowserReply(const QNetworkRequest &request) override
            {
                callbackPort = static_cast<quint16>(request.url().port());
                const QByteArray get = "GET /?" + request.url().query(QUrl::FullyEncoded).toUtf8() + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
                auto *socket = new QTcpSocket(this);
                QObject::connect(socket, &QTcpSocket::connected, socket, [socket, get] { socket->write(get); });
                QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket] { browserResponse += socket->readAll(); });
                QObject::connect(socket, &QTcpSocket::disconnected, this, [this] { replyToBrowserOk = browserResponse.startsWith("HTTP/1.1 303"); });
                socket->connectToHost(browserAddress, callbackPort);
                return nullptr;
            }
        } test;
        test.browserAddress = address;

        test.test();

        QVERIFY(!acceptsConnections(QHostAddress(QHostAddress::LocalHost), test.callbackPort));
        if (ipv6LoopbackAvailable()) {
            QVERIFY(!acceptsConnections(QHostAddress(QHostAddress::LocalHostIPv6), test.callbackPort));
        }
    }

    // „Browser erneut öffnen“ darf die wartende Anmeldung nicht verwerfen: Port und state bleiben gleich,
    // ein zuerst geöffneter oder kopierter Link führt weiter zum Client
    void testReopenBrowserKeepsPendingSignIn()
    {
        // Arrange
        OAuthPageFixture f;
        QVERIFY(!f.controller.validate());
        QTRY_COMPARE(f.openedLinks.size(), 1);

        // Act
        QVERIFY(!f.controller.validate());

        // Assert
        QTRY_COMPARE(f.openedLinks.size(), 2);
        QCOMPARE(f.openedLinks.at(1), f.openedLinks.at(0));
        QVERIFY(acceptsConnections(QHostAddress(QHostAddress::LocalHost), redirectPort(f.openedLinks.at(0))));
    }

    // Lässt sich der Browser nicht öffnen, wartet die Anmeldung weiter: der Link wird kopiert und von Hand geöffnet
    void testBrowserFailureKeepsSignInOpen()
    {
        // Arrange: openUrl() schlägt fehl, weil der Handler keine passende Methode hat
        OAuthPageFixture f;
        QDesktopServices::setUrlHandler(QStringLiteral("oauthtest"), &f, "noSuchMethod");
        QSignalSpy failure(&f.controller, &OAuthPageController::failure);

        // Act
        QVERIFY(!f.controller.validate());
        QTRY_COMPARE(failure.size(), 1);

        // Assert
        const QPointer<OAuth> oauth = f.oauth();
        QVERIFY(oauth);
        QVERIFY(acceptsConnections(QHostAddress(QHostAddress::LocalHost), redirectPort(oauth->authorisationLink())));
    }

    // ausdrücklich abgebrochen: mit dem Assistenten schließt auch der Listener
    void testClosingWizardClosesListener()
    {
        // Arrange
        auto f = std::make_unique<OAuthPageFixture>();
        QVERIFY(!f->controller.validate());
        QTRY_COMPARE(f->openedLinks.size(), 1);
        const quint16 port = redirectPort(f->openedLinks.at(0));

        // Act
        f.reset();

        // Assert
        QVERIFY(!acceptsConnections(QHostAddress(QHostAddress::LocalHost), port));
    }

    // Ohne nutzbare Angabe in token_endpoint_auth_methods_supported gilt client_secret_basic. Bisher beendete hier ein
    // OC_ASSERT_X mit FORCE_ASSERTS (Beta) das Programm schon wegen der Serverantwort.
    void testWellKnownWithoutUsableAuthMethod_data()
    {
        QTest::addColumn<QJsonValue>("authMethods");
        QTest::newRow("missing") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("empty") << QJsonValue(QJsonArray{});
        QTest::newRow("unknown only") << QJsonValue(QJsonArray{QStringLiteral("private_key_jwt")});
    }

    void testWellKnownWithoutUsableAuthMethod()
    {
        // Arrange
        QFETCH(QJsonValue, authMethods);
        struct Test : OAuthTestCase {
            QJsonValue authMethods;
            QByteArray tokenAuthorization;
            QByteArray tokenBody;

            QNetworkReply *wellKnownReply(QNetworkAccessManager::Operation op, const QNetworkRequest &req) override
            {
                QJsonObject data{{QStringLiteral("authorization_endpoint"), sOpenIdAuthURL.toString()},
                    {QStringLiteral("token_endpoint"), QStringLiteral("oauthtest://openidserver/token_endpoint")}};
                if (!authMethods.isUndefined()) {
                    data.insert(QStringLiteral("token_endpoint_auth_methods_supported"), authMethods);
                }
                return new FakePayloadReply(op, req, QJsonDocument(data).toJson(), fakeAm);
            }

            QNetworkReply *tokenReply(QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *device) override
            {
                tokenAuthorization = req.rawHeader("Authorization");
                tokenBody = device->peek(device->bytesAvailable());
                return OAuthTestCase::tokenReply(op, req, device);
            }
        } test;
        test.authMethods = authMethods;

        // Act
        test.test();

        // Assert: Client-Kennung und Geheimnis im Basic-Header, nicht im Formular
        QVERIFY(test.tokenAuthorization.startsWith("Basic "));
        QVERIFY(!test.tokenBody.contains("client_secret"));
    }

    // „Anmelden“ geklickt, bevor der Link feststeht (nach einem Fehler beginnt die Anmeldung neu), oder nachdem der Browser
    // schon zurückgekehrt ist: nichts öffnen und kein Ergebnis melden. authorisationLink() setzt beides voraus; mit
    // FORCE_ASSERTS (Beta) brach das Programm hier bisher ab.
    void testOpenBrowserWithoutValidLinkDoesNothing()
    {
        struct Test : OAuthTestCase {
            int results = 0;
            int browserOpened = 0;

            void openBrowserHook(const QUrl &url) override
            {
                ++browserOpened;
                OAuthTestCase::openBrowserHook(url);
            }

            QNetworkReply *tokenReply(QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *device) override
            {
                // der Rückruf ist da, der Listener geschlossen
                oauth->openBrowser();
                return OAuthTestCase::tokenReply(op, req, device);
            }

            void runTest() override
            {
                oauth = prepareOauth();
                QObject::connect(oauth.get(), &OAuth::result, this, [this] { ++results; });
                oauth->startAuthentication();
                QSignalSpy linkChanged(oauth.get(), &OAuth::authorisationLinkChanged);

                // zu früh: die Endpunkte werden noch abgefragt
                oauth->openBrowser();
                QCOMPARE(results, 0);
                QCOMPARE(browserOpened, 0);

                QVERIFY(linkChanged.wait());
                oauth->openBrowser();
            }
        } test;

        // Act
        test.test();

        // Assert: genau ein Browserfenster, genau ein Ergebnis (angemeldet)
        QCOMPARE(test.browserOpened, 1);
        QCOMPARE(test.results, 1);
    }

    // Ist [::1] mit dem Port schon vergeben, landete ein Browser, der localhost als ::1 auflöst, bei dem anderen Programm:
    // dann einen anderen Port nehmen. Bisher lauschte der Client bei festen Ports sofort nur noch auf 127.0.0.1. Gibt es
    // keinen anderen Port, bleibt es wie bisher bei IPv4.
    void testListenerAvoidsPortTakenOnIpv6_data()
    {
        QTest::addColumn<bool>("withAlternative");
        QTest::newRow("next configured port") << true;
        QTest::newRow("no other port") << false;
    }

    void testListenerAvoidsPortTakenOnIpv6()
    {
        // Arrange
        QFETCH(bool, withAlternative);
        if (!ipv6LoopbackAvailable()) {
            QSKIP("no IPv6 loopback on this system");
        }
        QTcpServer otherProgram;
        QVERIFY(otherProgram.listen(QHostAddress::LocalHostIPv6, 0));
        const quint16 taken = otherProgram.serverPort();
        {
            QTcpServer probe;
            if (!probe.listen(QHostAddress::LocalHost, taken)) {
                QSKIP("the port is taken on 127.0.0.1 as well");
            }
        }
        FakeAM fakeAm(nullptr);
        fakeAm.setOverride([&fakeAm](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *) -> QNetworkReply * {
            return new FakePayloadReply(op, req, openIdConfigurationPayload(), &fakeAm);
        });
        const OpenIdConfig defaults = AppConfig().openIdConfig();
        const QList<quint16> ports = withAlternative ? QList<quint16>{taken, 0} : QList<quint16>{taken};
        OAuth oauth(sOpenIdBaseURL, {}, OpenIdConfig(defaults.clientId(), defaults.clientSecret(), ports, defaults.scopes(), defaults.prompt()), &fakeAm,
            nullptr);
        QSignalSpy linkChanged(&oauth, &OAuth::authorisationLinkChanged);

        // Act
        oauth.startAuthentication();
        QVERIFY(linkChanged.wait());

        // Assert
        const quint16 port = redirectPort(oauth.authorisationLink());
        QVERIFY(acceptsConnections(QHostAddress(QHostAddress::LocalHost), port));
        if (withAlternative) {
            QVERIFY(port != taken);
            QVERIFY(acceptsConnections(QHostAddress(QHostAddress::LocalHostIPv6), port));
        } else {
            QCOMPARE(port, taken);
        }
    }

    // Kommt der Rückruf, nachdem der Nutzer zur Adressseite zurückgegangen ist (davor oder während der Prüfung des Kontos),
    // gilt das Ergebnis nicht mehr: Sonst prüfte setCurrentId() die Adressseite mit der inzwischen eingetragenen Adresse,
    // und das Konto dafür bekäme Token, Refresh-Token und Benutzer dieses Servers.
    void testSignInResultAfterGoingBackIsDiscarded_data()
    {
        QTest::addColumn<bool>("backDuringAccountCheck");
        QTest::newRow("back before the callback") << false;
        QTest::newRow("back while the account is checked") << true;
    }

    void testSignInResultAfterGoingBackIsDiscarded()
    {
        // Arrange
        QFETCH(bool, backDuringAccountCheck);
        OAuthWizardFixture f;
        QSignalSpy success(f.controller, &OAuthPageController::success);
        QVERIFY(!f.controller->validate());
        QTRY_COMPARE(f.openedLinks.size(), 1);
        if (backDuringAccountCheck) {
            f.onUserInfo = [&f] { QTimer::singleShot(0, &f.wizard, &QWizard::back); };
        } else {
            f.wizard.back();
        }

        // Act: der Browser kehrt zurück
        browserCallback(f.openedLinks.at(0), &f);

        // Assert: verworfen, die Anmeldung ist beendet
        QTRY_VERIFY(!f.oauth());
        QCOMPARE(f.wizard.currentId(), 0);
        QCOMPARE(success.size(), 0);
        QCOMPARE(f.userInfoRequests, backDuringAccountCheck ? 1 : 0);
        QVERIFY(!acceptsConnections(QHostAddress(QHostAddress::LocalHost), redirectPort(f.openedLinks.at(0))));

        // zurück auf der Anmeldeseite beginnt eine neue Anmeldung, statt das verworfene Ergebnis zu melden
        f.wizard.next();
        QVERIFY(!f.controller->validate());
        QTRY_COMPARE(f.openedLinks.size(), 2);
        QVERIFY(f.oauth());
        QCOMPARE(success.size(), 0);
    }

    // Gegenprobe: auf der Anmeldeseite geht das Ergebnis durch
    void testSignInResultOnSignInPageIsAccepted()
    {
        OAuthWizardFixture f;
        QSignalSpy success(f.controller, &OAuthPageController::success);
        QVERIFY(!f.controller->validate());
        QTRY_COMPARE(f.openedLinks.size(), 1);
        const qint64 before = QDateTime::currentSecsSinceEpoch();

        browserCallback(f.openedLinks.at(0), &f);

        QTRY_COMPARE(success.size(), 1);
        const auto results = success.at(0).at(0).value<OAuthPageResults>();
        QCOMPARE(results.token, QStringLiteral("123"));
        QCOMPARE(results.userId, QStringLiteral("admin"));
        QCOMPARE(f.capabilitiesRequests, 1);
        // Ablaufzeitpunkt aus expires_in des Code-Tauschs: Das Konto entsteht erst einige Seiten später
        QVERIFY2(results.accessTokenExpiresAt >= before + 3600 && results.accessTokenExpiresAt <= QDateTime::currentSecsSinceEpoch() + 3600,
            qPrintable(QString::number(results.accessTokenExpiresAt - before)));
    }

    // Zurück und eine andere Adresse (onUrlValidationCompleted): Die Anmeldung für die vorige Adresse endet sofort, ihr
    // Listener nimmt keinen Rückruf mehr an. Dieselbe Adresse lässt sie bestehen („Browser erneut öffnen“).
    void testSignInEndsWhenAddressChanges()
    {
        // Arrange
        OAuthWizardFixture f;
        QSignalSpy success(f.controller, &OAuthPageController::success);
        QVERIFY(!f.controller->validate());
        QTRY_COMPARE(f.openedLinks.size(), 1);
        const QPointer<OAuth> first = f.oauth();
        const quint16 firstPort = redirectPort(f.openedLinks.at(0));
        f.wizard.back();

        // Act 1: dieselbe Adresse
        f.controller->setAuthenticationUrl(sOpenIdBaseURL);

        // Assert 1: die Anmeldung wartet weiter
        QTest::qWait(50);
        QVERIFY(first);
        QVERIFY(first->isListening());

        // Act 2: eine andere Adresse
        const QUrl otherServer(QStringLiteral("oauthtest://other.example.com"));
        f.controller->setServerUrl(otherServer);
        f.controller->setAuthenticationUrl(otherServer);

        // Assert 2: beendet, der Port ist zu
        QTRY_VERIFY(!first);
        QVERIFY(!f.oauth());
        QVERIFY(!acceptsConnections(QHostAddress(QHostAddress::LocalHost), firstPort));

        // zurück auf der Anmeldeseite beginnt die Anmeldung für die neue Adresse und geht durch
        f.wizard.next();
        QVERIFY(!f.controller->validate());
        QTRY_COMPARE(f.openedLinks.size(), 2);
        browserCallback(f.openedLinks.at(1), &f);
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(success.at(0).at(0).value<OAuthPageResults>().userId, QStringLiteral("admin"));
    }

    // Zurück während der Prüfung, eine andere Adresse und wieder zur Anmeldeseite: validate() lehnt während der Prüfung ab.
    // Nach dem Verwerfen beginnt die Anmeldung für die neue Adresse von selbst, statt die Seite ohne Anmeldung stehen zu lassen.
    void testSignInForNewAddressStartsAfterDiscardedCheck()
    {
        // Arrange
        OAuthWizardFixture f;
        QSignalSpy success(f.controller, &OAuthPageController::success);
        QSignalSpy failure(f.controller, &OAuthPageController::failure);
        QVERIFY(!f.controller->validate());
        QTRY_COMPARE(f.openedLinks.size(), 1);
        const QUrl otherServer(QStringLiteral("oauthtest://other.example.com"));
        bool switched = false;
        bool validateRefused = false;
        f.onUserInfo = [&] {
            if (switched) {
                return;
            }
            switched = true;
            QTimer::singleShot(0, &f.wizard, [&] {
                // wie im Programm: Zurück, neue Adresse geprüft (onUrlValidationCompleted), Anmeldeseite prüft automatisch
                f.wizard.back();
                f.controller->setServerUrl(otherServer);
                f.controller->setAuthenticationUrl(otherServer);
                f.wizard.next();
                validateRefused = !f.controller->validate();
            });
        };

        // Act
        browserCallback(f.openedLinks.at(0), &f);

        // Assert: das Ergebnis des ersten Servers ist verworfen, für den neuen öffnet sich der Browser
        QTRY_COMPARE(f.openedLinks.size(), 2);
        QVERIFY(validateRefused);
        QCOMPARE(f.wizard.currentId(), 1);
        QCOMPARE(success.size(), 0);
        QCOMPARE(failure.size(), 0);

        // die neue Anmeldung geht durch
        browserCallback(f.openedLinks.at(1), &f);
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(f.userInfoRequests, 2);
    }

    // Neuanmeldung: Lässt sich der Browser nicht öffnen, bleibt die Anmeldung samt Listener, Port und state bestehen; die
    // Meldung fordert ja zum Kopieren des Links auf. Bisher begann sie neu, der kopierte Link führte auf einen geschlossenen Port.
    void testReauthBrowserFailureKeepsSignIn()
    {
        // Arrange: openUrl() schlägt fehl, weil der Handler keine passende Methode hat
        ReauthFixture f;
        QDesktopServices::setUrlHandler(QStringLiteral("oauthtest"), &f, "noSuchMethod");
        QSignalSpy succeeded(f.controller, &RequestAuthenticationController::authenticationSucceeded);
        f.controller->startAuthentication(f.account);
        QTRY_VERIFY(f.button("SignInButton") && f.button("SignInButton")->isEnabled());
        const QPointer<OAuth> oauth = f.oauth();
        QVERIFY(oauth);
        const QUrl link = oauth->authorisationLink();

        // Act: zweimal „Anmelden“
        f.button("SignInButton")->click();
        f.button("SignInButton")->click();
        QTest::qWait(100);

        // Assert: dieselbe Anmeldung wartet weiter, kein neuer Anlauf
        QVERIFY(oauth);
        QCOMPARE(f.oauth(), oauth.data());
        QCOMPARE(oauth->authorisationLink(), link);
        QCOMPARE(f.statusRequests, 1);

        // der kopierte Link führt die Anmeldung zu Ende
        browserCallback(link, &f);
        QTRY_COMPARE(succeeded.size(), 1);
        QCOMPARE(succeeded.at(0).at(0).toString(), QStringLiteral("123"));
        QCOMPARE(succeeded.at(0).at(1).toString(), QStringLiteral("456"));
    }

    // Neuanmeldung: Die Lebensdauer des Access-Tokens aus dem Code-Tausch (expires_in) geht mit, damit die Credentials es vor
    // dem Ablauf erneuern
    void testReauthPassesTheTokenLifetime()
    {
        // Arrange
        ReauthFixture f;
        QSignalSpy succeeded(f.controller, &RequestAuthenticationController::authenticationSucceeded);
        f.controller->startAuthentication(f.account);
        QTRY_VERIFY(f.button("SignInButton") && f.button("SignInButton")->isEnabled());
        f.button("SignInButton")->click();
        QTRY_COMPARE(f.openedLinks.size(), 1);

        // Act
        browserCallback(f.openedLinks.at(0), &f);

        // Assert
        QTRY_COMPARE(succeeded.size(), 1);
        QCOMPARE(succeeded.at(0).at(0).toString(), QStringLiteral("123"));
        QCOMPARE(succeeded.at(0).at(1).toString(), QStringLiteral("456"));
        QCOMPARE(succeeded.at(0).at(2).toLongLong(), 3600);
    }

    // Neuanmeldung ohne erreichbaren Server: nicht sofort neu beginnen (bisher eine enge Schleife aus status.php-Anfragen,
    // solange der Dialog offen ist), sondern nach einer Pause; „Abgemeldet bleiben“ bleibt möglich
    void testReauthRetriesWithPauseWhenServerIsDown()
    {
        // Arrange
        ReauthFixture f;
        f.serverDown = true;

        // Act
        f.controller->startAuthentication(f.account);

        // Assert
        QTRY_COMPARE(f.statusRequests, 1);
        QTest::qWait(1000);
        QCOMPARE(f.statusRequests, 1);
        QVERIFY(f.button("StayLoggedOutButton")->isEnabled());
        QVERIFY(!f.button("SignInButton")->isEnabled());
        QVERIFY(!f.button("CopyUrlToClipboardButton")->isEnabled());
        // auch ein direkt ausgelöstes „Anmelden“ öffnet ohne laufende Anmeldung nichts
        Q_EMIT f.modal->findChild<RequestAuthenticationWidget *>()->connectClicked();
        QCOMPARE(f.openedLinks.size(), 0);

        // nach der Pause ein neuer Anlauf; ist der Server wieder da, kommt der Link
        f.serverDown = false;
        QTRY_COMPARE_WITH_TIMEOUT(f.statusRequests, 2, 5000);
        QTRY_VERIFY(f.button("SignInButton")->isEnabled());
        QVERIFY(f.button("CopyUrlToClipboardButton")->isEnabled());
        f.button("SignInButton")->click();
        QTRY_COMPARE(f.openedLinks.size(), 1);
    }

    void testReauthRestartDelay()
    {
        using namespace std::chrono_literals;
        QCOMPARE(RequestAuthenticationController::restartDelay(1), 2000ms);
        QCOMPARE(RequestAuthenticationController::restartDelay(2), 4000ms);
        QCOMPARE(RequestAuthenticationController::restartDelay(5), 32000ms);
        QCOMPARE(RequestAuthenticationController::restartDelay(6), 60000ms);
        QCOMPARE(RequestAuthenticationController::restartDelay(1000), 60000ms);
    }
};


QTEST_MAIN(TestOAuth)
#include "testoauth.moc"
