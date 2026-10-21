/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// Anmeldung mit Benutzername und Kennwort (Server ohne die App „OAuth2“): Kennwortseite des Assistenten und Dialog
// „Anmeldung erforderlich“ der Kontoansicht

#include <QtTest/QtTest>

#include "accountsgui/accountmodalwidget.h"
#include "creds/basicauthenticationwidget.h"
#include "creds/httpcredentialsgui.h"
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
};

QTEST_MAIN(TestBasicAuth)
#include "testbasicauth.moc"
