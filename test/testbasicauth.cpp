/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// Anmeldung mit Benutzername und Kennwort (Server ohne die App „OAuth2“): Kennwortseite des Assistenten und Dialog
// „Anmeldung erforderlich“ der Kontoansicht

#include <QtTest/QtTest>

#include "accessmanager.h"
#include "accountsgui/accountmodalwidget.h"
#include "creds/basicauthenticationwidget.h"
#include "creds/credentialssupport.h"
#include "creds/httpcredentialsgui.h"
#include "networkjobs.h"
#include "newaccountwizard/basiccredentialspagecontroller.h"
#include "testutils/syncenginetestutils.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWizard>
#include <QWizardPage>

#include <algorithm>
#include <memory>

using namespace OCC;

namespace {

const QUrl serverUrl(QStringLiteral("https://cloud.example.org/owncloud"));

QByteArray capabilitiesJson()
{
    return QByteArrayLiteral("{\"ocs\":{\"data\":{\"capabilities\":{\"core\":{\"status\":{\"version\":\"11.0.0.0\",\"versionstring\":\"11.0.0\"}}}}}}");
}

// Kennwortseite in einem echten Assistenten (davor die Adressseite) gegen einen Server, der nur anna mit „secret“ annimmt
class BasicPageFixture
{
public:
    BasicPageFixture()
        : fakeAm(new FakeAM(nullptr))
    {
        auto *urlPage = new QWizardPage;
        page = new QWizardPage;
        wizard.addPage(urlPage);
        wizard.addPage(page);
        controller = new BasicCredentialsPageController(page, fakeAm, nullptr);
        controller->setServerUrl(serverUrl);
        fakeAm->setOverride([this](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *) -> QNetworkReply * {
            const QString path = req.url().path();
            if (path.contains(QLatin1String("/ocs/"))) {
                ocsApiRequestHeaders.append(req.rawHeader("OCS-APIREQUEST"));
            }
            if (path.endsWith(QLatin1String("ocs/v2.php/cloud/user"))) {
                authorizations.append(req.rawHeader("Authorization"));
                if (req.rawHeader("Authorization") != basicAuthorizationHeader(QStringLiteral("anna"), QStringLiteral("secret"))) {
                    return new FakeErrorReply(op, req, fakeAm, 401);
                }
                return new FakePayloadReply(op, req, QByteArrayLiteral("{\"ocs\":{\"data\":{\"id\":\"anna\",\"display-name\":\"Anna Beispiel\"}}}"), fakeAm);
            }
            if (path.endsWith(QLatin1String("ocs/v2.php/cloud/capabilities"))) {
                auto *reply = new FakePayloadReply(op, req, capabilitiesJson(), fakeAm);
                reply->setRawHeader(QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json; charset=utf-8"));
                return reply;
            }
            return nullptr;
        });
        // wie im Programm: von der Adressseite zur Kennwortseite
        wizard.restart();
        wizard.next();
    }

    ~BasicPageFixture()
    {
        // vor dem Assistenten: der Controller greift auf die Elemente der Seite zu
        delete controller;
        delete fakeAm;
    }

    QLineEdit *userField() const { return page->findChild<QLineEdit *>(QStringLiteral("LoginUserLineEdit")); }
    QLineEdit *passwordField() const { return page->findChild<QLineEdit *>(QStringLiteral("PasswordLineEdit")); }
    QString error() const { return page->findChild<QLabel *>(QStringLiteral("ErrorLabel"))->text(); }

    FakeAM *fakeAm;
    QWizard wizard;
    QWizardPage *page = nullptr;
    BasicCredentialsPageController *controller = nullptr;
    QList<QByteArray> authorizations;
    QList<QByteArray> ocsApiRequestHeaders;
};

// Dialog der Kontoansicht; das Kennwort liegt im Speicher statt im Schlüsselbund
class MemoryHttpCredentialsGui : public HttpCredentialsGui
{
public:
    using HttpCredentialsGui::HttpCredentialsGui;

    QStringList stored;

protected:
    void readPassword(const ReadCallback &done) override { done(KeychainResult::NotFound, {}, {}); }
    void storePassword(const QString &password, const StoreCallback &done) override
    {
        stored.append(password);
        done(true, {});
    }
    void removePassword() override { }
};

// Konto mit Kennwort gegen einen Server, der nur anna mit „secret“ annimmt; die Anfragen tragen wie im Programm den Kopf aus
// withAuthorization(), und zwar zum Zeitpunkt des Sendens. setStoredPassword() spielt das Lesen des Schlüsselbunds nach.
class ServerHttpCredentials : public MemoryHttpCredentialsGui
{
public:
    using MemoryHttpCredentialsGui::MemoryHttpCredentialsGui;

    void setStoredPassword(const QString &password)
    {
        _password = password;
        _storedPassword = password;
        _ready = true;
    }

    OCC::AccessManager *createAccessManager() const override
    {
        // wie das echte: das Konto löscht es mit den Zugangsdaten
        auto *am = new FakeAM(FileInfo::A12_B12_C12_S12(), const_cast<ServerHttpCredentials *>(this));
        const auto seen = authorizations;
        am->setOverride([this, seen, am](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            const QNetworkRequest authorized = withAuthorization(request);
            seen->append(authorized.rawHeader("Authorization"));
            if (authorized.rawHeader("Authorization") != basicAuthorizationHeader(QStringLiteral("anna"), QStringLiteral("secret"))) {
                return new FakeErrorReply(op, authorized, am, 401);
            }
            // der Dateibaum antwortet
            return nullptr;
        });
        return am;
    }

    // die Authorization-Köpfe der Anfragen in der Reihenfolge des Sendens
    std::shared_ptr<QList<QByteArray>> authorizations = std::make_shared<QList<QByteArray>>();
};
}

class TestBasicAuth : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { qRegisterMetaType<BasicCredentialsPageResults>(); }

    // Kennwortseite: maskiertes Kennwort, ohne Eingabe keine Anfrage
    void testBasicPageNeedsInput()
    {
        BasicPageFixture f;
        QCOMPARE(f.wizard.currentPage(), f.page);
        QVERIFY(f.userField());
        QCOMPARE(f.passwordField()->echoMode(), QLineEdit::Password);

        QSignalSpy failure(f.controller, &BasicCredentialsPageController::failure);
        QVERIFY(!f.controller->validate());
        QCOMPARE(failure.size(), 1);
        QCOMPARE(f.error(), QStringLiteral("Please enter your user name and password."));
        QVERIFY(f.authorizations.isEmpty());
    }

    // abgelehntes Kennwort (401): klare Meldung, das Kennwort ist zum erneuten Eingeben leer, der Benutzername bleibt
    void testBasicPageRejectsWrongPassword()
    {
        BasicPageFixture f;
        QSignalSpy success(f.controller, &BasicCredentialsPageController::success);
        f.userField()->setText(QStringLiteral("anna"));
        f.passwordField()->setText(QStringLiteral("wrong"));

        QVERIFY(!f.controller->validate());
        QCOMPARE(success.size(), 0);
        QCOMPARE(f.error(), QStringLiteral("The user name or password is not correct."));
        QCOMPARE(f.authorizations, QList<QByteArray>{basicAuthorizationHeader(QStringLiteral("anna"), QStringLiteral("wrong"))});
        QVERIFY(f.passwordField()->text().isEmpty());
        QCOMPARE(f.userField()->text(), QStringLiteral("anna"));
    }

    void testBasicPageAcceptsPassword()
    {
        BasicPageFixture f;
        QSignalSpy success(f.controller, &BasicCredentialsPageController::success);
        f.controller->presetLoginUser(QStringLiteral(" anna "));
        f.passwordField()->setText(QStringLiteral("secret"));

        QVERIFY(f.controller->validate());
        QCOMPARE(success.size(), 1);
        const auto results = success.first().first().value<BasicCredentialsPageResults>();
        QCOMPARE(results.loginUser, QStringLiteral("anna"));
        QCOMPARE(results.password, QStringLiteral("secret"));
        QCOMPARE(results.userId, QStringLiteral("anna"));
        QCOMPARE(results.displayName, QStringLiteral("Anna Beispiel"));
        QVERIFY(f.error().isEmpty());
        // Benutzer und Capabilities wie im 6.0.3-Client mit OCS-APIREQUEST: die Sitzung des Servers gilt sonst nicht für OCS
        QCOMPARE(f.ocsApiRequestHeaders, (QList<QByteArray>{"true", "true"}));
    }

    // Kontoansicht niedriger als der Dialog (1024×768, Meldung): Das Logo behält seine Höhe („ONLINE“ war abgeschnitten), der
    // Inhalt rollt, die Knöpfe bleiben im Dialog sichtbar
    void testReauthDialogKeepsLogo()
    {
        // wie das Hauptfenster: eine eigene Mindestgröße, die nicht mit dem Inhalt wächst
        QWidget accountView;
        accountView.setMinimumSize(708, 300);
        accountView.resize(708, 300);
        auto *viewLayout = new QVBoxLayout(&accountView);
        viewLayout->setContentsMargins(0, 0, 0, 0);
        auto *widget = new BasicAuthenticationWidget(serverUrl, QStringLiteral("anna"), &accountView);
        widget->setErrorMessage(QStringLiteral("The user name or password is not correct."));
        viewLayout->addWidget(widget);
        accountView.show();
        QVERIFY(QTest::qWaitForWindowExposed(&accountView));
        QCOMPARE(widget->height(), 300);

        QLabel *logo = nullptr;
        for (auto *label : widget->findChildren<QLabel *>()) {
            if (label->accessibleName().startsWith(QLatin1String("Application Name Logo"))) {
                logo = label;
            }
        }
        QVERIFY(logo);
        QVERIFY(!logo->pixmap().isNull());
        QCOMPARE(logo->height(), logo->sizeHint().height());

        auto *scrollArea = widget->findChild<QScrollArea *>();
        QVERIFY(scrollArea);
        QVERIFY(scrollArea->verticalScrollBar()->maximum() > 0);

        auto *signIn = widget->findChild<QPushButton *>(QStringLiteral("SignInButton"));
        QVERIFY(signIn->isVisible());
        QVERIFY(widget->rect().contains(signIn->geometry()));
    }

    // Eine lange Meldung (der Hinweis zur Zwei-Faktor-Anmeldung) in einer niedrigen Kontoansicht: Der Dialog rollt zu ihr, sie
    // steht ganz im sichtbaren Bereich, das Kennwortfeld darüber auch; ebenso, wenn sie erst im offenen Dialog erscheint
    void testReauthDialogShowsLongMessage_data()
    {
        QTest::addColumn<bool>("beforeShow");
        QTest::newRow("vor dem Anzeigen") << true;
        QTest::newRow("im offenen Dialog") << false;
    }

    void testReauthDialogShowsLongMessage()
    {
        QFETCH(bool, beforeShow);
        const QString message = QStringLiteral(
            "The server requires two-factor authentication for this account and no longer accepts the password alone. Create an app "
            "password in the web interface under Settings → Security and enter it here, or remove the account and add it again to "
            "sign in in the browser.");
        QWidget accountView;
        accountView.setMinimumSize(708, 300);
        accountView.resize(708, 300);
        auto *viewLayout = new QVBoxLayout(&accountView);
        viewLayout->setContentsMargins(0, 0, 0, 0);
        auto *widget = new BasicAuthenticationWidget(serverUrl, QStringLiteral("anna"), &accountView);
        if (beforeShow) {
            widget->setErrorMessage(message);
        }
        viewLayout->addWidget(widget);
        accountView.show();
        QVERIFY(QTest::qWaitForWindowExposed(&accountView));
        if (!beforeShow) {
            widget->setErrorMessage(message);
        }

        auto *scrollArea = widget->findChild<QScrollArea *>();
        QVERIFY(scrollArea);
        const auto inView = [scrollArea](QWidget *child) {
            return scrollArea->viewport()->rect().contains(QRect(child->mapTo(scrollArea->viewport(), QPoint(0, 0)), child->size()));
        };
        auto *error = widget->findChild<QLabel *>(QStringLiteral("ErrorLabel"));
        QTRY_VERIFY(scrollArea->verticalScrollBar()->maximum() > 0);
        QTRY_VERIFY(inView(error));
        QVERIFY(inView(widget->findChild<QLineEdit *>(QStringLiteral("PasswordLineEdit"))));
    }

    // Kontoansicht: Dialog mit maskiertem Kennwort, Enter meldet an; fragt das Konto danach erneut (Server lehnte ab),
    // nennt der Dialog den Grund; „Abgemeldet bleiben“ meldet das Konto ab
    void testReauthDialog()
    {
        auto accountState = createDummyAccount();
        auto *credentials = new MemoryHttpCredentialsGui(accountState->account(), QStringLiteral("anna"), QString());
        QVERIFY(!credentials->ready());
        std::unique_ptr<AccountModalWidget> modal;
        connect(credentials, &AbstractCredentials::requestAccountModal, this, [&modal](AccountModalWidget *widget) { modal.reset(widget); });
        QSignalSpy fetched(credentials, &AbstractCredentials::fetched);
        QSignalSpy logout(credentials, &AbstractCredentials::requestLogout);

        credentials->askFromUser();
        QTRY_VERIFY(modal);
        auto *widget = modal->findChild<BasicAuthenticationWidget *>();
        QVERIFY(widget);
        auto *passwordField = widget->findChild<QLineEdit *>(QStringLiteral("PasswordLineEdit"));
        QCOMPARE(passwordField->echoMode(), QLineEdit::Password);
        QVERIFY(widget->findChild<QLabel *>(QStringLiteral("ErrorLabel"))->text().isEmpty());
        // ein zweiter Aufruf öffnet keinen zweiten Dialog
        AccountModalWidget *first = modal.get();
        credentials->askFromUser();
        QTest::qWait(10);
        QCOMPARE(modal.get(), first);

        passwordField->setText(QStringLiteral("typo"));
        QTest::keyClick(passwordField, Qt::Key_Return);
        QCOMPARE(fetched.size(), 1);
        QVERIFY(credentials->ready());
        QCOMPARE(credentials->stored, QStringList{QStringLiteral("typo")});
        QCOMPARE(credentials->loginUser(), QStringLiteral("anna"));

        // die Kontoansicht schließt den beendeten Dialog
        modal.reset();
        credentials->askFromUser();
        QTRY_VERIFY(modal);
        widget = modal->findChild<BasicAuthenticationWidget *>();
        QCOMPARE(widget->findChild<QLabel *>(QStringLiteral("ErrorLabel"))->text(), QStringLiteral("The user name or password is not correct."));

        widget->findChild<QPushButton *>(QStringLiteral("StayLoggedOutButton"))->click();
        QCOMPARE(logout.size(), 1);
        QCOMPARE(fetched.size(), 1);
        modal.reset();
    }

    // Beim Start ging eine Abfrage hinaus, bevor das Kennwort aus dem Schlüsselbund kam. Ihr 401 (ohne Authorization-Kopf)
    // darf das eben gelesene, richtige Kennwort nicht verwerfen, sonst fragt der Client nach jedem Start danach. Ebenso
    // eine Antwort auf ein inzwischen ersetztes Kennwort. Nur ein 401 auf das aktuelle Kennwort gilt als Ablehnung.
    void testUnauthorizedWithoutCurrentPasswordKeepsCredentials()
    {
        auto accountState = createDummyAccount();
        auto *credentials = new MemoryHttpCredentialsGui(accountState->account(), QStringLiteral("anna"), QString());
        credentials->replacePassword(QStringLiteral("app-kennwort"));
        QVERIFY(credentials->ready());

        const QNetworkRequest request(QUrl(serverUrl.toString() + QStringLiteral("/remote.php/dav/files/anna")));
        const auto unauthorized = [](const QNetworkRequest &sent) {
            return std::make_unique<FakeErrorReply>(QNetworkAccessManager::CustomOperation, sent, nullptr, 401);
        };

        auto beforeKeychain = unauthorized(request);
        QVERIFY(!beforeKeychain->request().hasRawHeader(QByteArrayLiteral("Authorization")));
        QVERIFY(credentials->stillValid(beforeKeychain.get()));

        QNetworkRequest previous(request);
        previous.setRawHeader(QByteArrayLiteral("Authorization"), basicAuthorizationHeader(QStringLiteral("anna"), QStringLiteral("altes-kennwort")));
        auto withPrevious = unauthorized(previous);
        QVERIFY(credentials->stillValid(withPrevious.get()));

        auto withCurrent = unauthorized(credentials->withAuthorization(request));
        QCOMPARE(withCurrent->request().rawHeader(QByteArrayLiteral("Authorization")),
            basicAuthorizationHeader(QStringLiteral("anna"), QStringLiteral("app-kennwort")));
        QVERIFY(!credentials->stillValid(withCurrent.get()));

        // ohne Kennwort (noch nicht gelesen) ist keine Antwort ein Urteil über ein Kennwort
        credentials->invalidateToken();
        QVERIFY(!credentials->ready());
        QVERIFY(credentials->stillValid(withCurrent.get()));
    }

    // Kundenfall auf Auftragsebene: Die Anfrage geht ohne Kennwort hinaus, das Kennwort kommt aus dem Schlüsselbund, während
    // sie unterwegs ist. Das Konto erfährt nichts von ihrem 401, die Wiederholung trägt das Kennwort und geht durch. Lehnt der
    // Server dagegen das aktuelle Kennwort ab, erfährt es das Konto, für jede solche Antwort.
    void testRejectionBeforePasswordKeepsIt()
    {
        auto accountState = createDummyAccount();
        Account *account = accountState->account();
        auto *credentials = new ServerHttpCredentials(account, QStringLiteral("anna"), QString());
        account->setCredentials(credentials);
        QVERIFY(!credentials->ready());
        std::unique_ptr<AccountModalWidget> modal;
        connect(credentials, &AbstractCredentials::requestAccountModal, this, [&modal](AccountModalWidget *widget) { modal.reset(widget); });
        QSignalSpy invalid(account, &Account::invalidCredentials);
        const QByteArray secret = basicAuthorizationHeader(QStringLiteral("anna"), QStringLiteral("secret"));

        auto *early = new PropfindJob(account, account->url(), QStringLiteral("remote.php/webdav/"), PropfindJob::Depth::Zero, this);
        early->setProperties({QByteArrayLiteral("getlastmodified")});
        QSignalSpy earlyDone(early, &PropfindJob::finishedWithoutError);
        early->start();
        // das Kennwort kommt aus dem Schlüsselbund, während die erste Anfrage unterwegs ist
        credentials->setStoredPassword(QStringLiteral("secret"));
        QTRY_COMPARE(earlyDone.size(), 1);
        QCOMPARE(invalid.size(), 0);
        QVERIFY(credentials->ready());
        QTest::qWait(20);
        QVERIFY(!modal);
        QCOMPARE(*credentials->authorizations, (QList<QByteArray>{QByteArray(), secret}));

        credentials->authorizations->clear();
        credentials->setStoredPassword(QStringLiteral("wrong"));
        auto *rejected = new PropfindJob(account, account->url(), QStringLiteral("remote.php/webdav/"), PropfindJob::Depth::Zero, this);
        rejected->setProperties({QByteArrayLiteral("getlastmodified")});
        QSignalSpy rejectedDone(rejected, &PropfindJob::finishedWithError);
        rejected->start();
        QTRY_COMPARE(rejectedDone.size(), 1);
        // jede Ablehnung des aktuellen Kennworts erfährt das Konto (im Programm verwirft es das Kennwort schon nach der ersten,
        // die Wiederholungen gehen dann ohne und melden nichts mehr)
        const QByteArray wrong = basicAuthorizationHeader(QStringLiteral("anna"), QStringLiteral("wrong"));
        QCOMPARE(credentials->authorizations->first(), wrong);
        QCOMPARE(invalid.size(), std::count(credentials->authorizations->cbegin(), credentials->authorizations->cend(), wrong));
        modal.reset();
    }

    // Zwei-Faktor-Anmeldung: Der Server lehnt auch das richtige Kennwort ab (WebDAV: 401 „password login forbidden“). Der
    // Dialog meldet dann kein falsches Kennwort, sondern nennt die Auswege; eine Ablehnung ohne den Hinweis (OCS) ändert
    // daran nichts, eine angenommene Anfrage mit dem aktuellen Kennwort oder das Abmelden schon
    void testReauthDialogExplainsTwoFactor()
    {
        auto accountState = createDummyAccount();
        auto *credentials = new MemoryHttpCredentialsGui(accountState->account(), QStringLiteral("anna"), QStringLiteral("secret"));
        std::unique_ptr<AccountModalWidget> modal;
        connect(credentials, &AbstractCredentials::requestAccountModal, this, [&modal](AccountModalWidget *widget) { modal.reset(widget); });
        const auto errorText = [&modal] {
            return modal->findChild<BasicAuthenticationWidget *>()->findChild<QLabel *>(QStringLiteral("ErrorLabel"))->text();
        };
        const auto dav = [](const QString &password) {
            QNetworkRequest request(QUrl(QStringLiteral("https://cloud.example.org/owncloud/remote.php/webdav/")));
            request.setRawHeader("Authorization", basicAuthorizationHeader(QStringLiteral("anna"), password));
            return request;
        };

        // so antwortet owncloud.online 11.0.49 mit erzwungener Zwei-Faktor-Anmeldung
        const QByteArray forbiddenBody = QByteArrayLiteral(
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<d:error xmlns:d=\"DAV:\" xmlns:s=\"http://sabredav.org/ns\" xmlns:o=\"http://owncloud.org/ns\">\n"
            "  <s:exception>OCA\\DAV\\Connector\\Sabre\\Exception\\PasswordLoginForbidden</s:exception>\n"
            "  <s:message/>\n"
            "  <o:hint xmlns:o=\"o:\">password login forbidden</o:hint>\n"
            "</d:error>\n");
        FakeErrorReply forbidden(QNetworkAccessManager::CustomOperation, dav(QStringLiteral("secret")), nullptr, 401, forbiddenBody);
        QVERIFY(!credentials->passwordLoginForbidden());
        QVERIFY(!credentials->stillValid(&forbidden));
        QVERIFY(credentials->passwordLoginForbidden());
        // der Auftrag liest die Antwort noch
        QCOMPARE(forbidden.readAll(), forbiddenBody);

        QNetworkRequest ocs(QUrl(QStringLiteral("https://cloud.example.org/owncloud/ocs/v2.php/cloud/user")));
        ocs.setRawHeader("Authorization", basicAuthorizationHeader(QStringLiteral("anna"), QStringLiteral("secret")));
        FakeErrorReply ocsRejected(QNetworkAccessManager::GetOperation, ocs, nullptr, 401,
            QByteArrayLiteral("{\"ocs\":{\"meta\":{\"status\":\"failure\",\"statuscode\":997,\"message\":\"Unauthorised\"}}}"));
        QVERIFY(!credentials->stillValid(&ocsRejected));
        QVERIFY(credentials->passwordLoginForbidden());

        credentials->askFromUser();
        QTRY_VERIFY(modal);
        QVERIFY(errorText().contains(QLatin1String("two-factor authentication")));
        QVERIFY(errorText().contains(QLatin1String("app password")));
        QVERIFY(!errorText().contains(QLatin1String("not correct")));
        // ein App-Passwort geht durch: Fragt das Konto später wieder, steht keine alte Meldung im Dialog
        modal->findChild<QLineEdit *>(QStringLiteral("PasswordLineEdit"))->setText(QStringLiteral("app-password"));
        QTest::keyClick(modal->findChild<QLineEdit *>(QStringLiteral("PasswordLineEdit")), Qt::Key_Return);
        modal.reset();
        // eine Antwort auf das alte Kennwort zählt nicht, weder als Ablehnung noch als Annahme
        FakeErrorReply old(QNetworkAccessManager::CustomOperation, dav(QStringLiteral("secret")), nullptr, 401, forbiddenBody);
        QVERIFY(credentials->stillValid(&old));
        QVERIFY(credentials->passwordLoginForbidden());
        FakePayloadReply accepted(QNetworkAccessManager::CustomOperation, dav(QStringLiteral("app-password")), QByteArrayLiteral("<d:multistatus xmlns:d=\"DAV:\"/>"), nullptr);
        QVERIFY(credentials->stillValid(&accepted));
        QVERIFY(!credentials->passwordLoginForbidden());
        credentials->askFromUser();
        QTRY_VERIFY(modal);
        QVERIFY(errorText().isEmpty());
        modal.reset();

        // Abmelden vergisst den Grund
        FakeErrorReply again(QNetworkAccessManager::CustomOperation, dav(QStringLiteral("app-password")), nullptr, 401, forbiddenBody);
        QVERIFY(!credentials->stillValid(&again));
        QVERIFY(credentials->passwordLoginForbidden());
        credentials->forgetSensitiveData();
        QVERIFY(!credentials->passwordLoginForbidden());
    }
};

QTEST_MAIN(TestBasicAuth)
#include "testbasicauth.moc"
