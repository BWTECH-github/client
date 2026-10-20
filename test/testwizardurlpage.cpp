/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// Adressseite des Kontoassistenten (UrlPageController) im Assistenten fester Größe: Die Meldung zu einem Server ohne die
// App „OAuth2“ ist in jeder Sprache vollständig lesbar (in v11.0.0-beta3 schnitt die Knopfzeile sie ab). Eine Adresse
// mit doppeltem Schema oder „//“ im Pfad führt zum gemeinten Server, statt in Utility::concatUrlPath abzubrechen.

#include "libsync/httplogger.h"
#include "networkadapters/serverdiagnostics.h"
#include "newaccountwizard/urlpagecontroller.h"
#include "testutils/syncenginetestutils.h"

#include <QDir>
#include <QLabel>
#include <QScopeGuard>
#include <QTest>
#include <QTranslator>
#include <QWizard>
#include <QWizardPage>

using namespace OCC;

namespace {

/**
 * owncloud.online-Server: mit der App „OAuth2“ wie testoo.bw.tech (DAV verlangt einen Bearer-Token) oder ohne sie wie
 * testool.bw.tech (nur Basic, der Token-Endpunkt leitet zur Anmeldeseite um). WebFinger und OpenID-Konfiguration gibt es
 * auf beiden nicht. Merkt sich jede Anfrage.
 */
class WizardServerAM : public FakeAM
{
public:
    explicit WizardServerAM(bool hasOAuth2)
        : FakeAM(nullptr)
    {
        setOverride([this, hasOAuth2](Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            requests.append(request.url());
            const QString path = request.url().path();
            if (path.endsWith(QLatin1String("/status.php"))) {
                return new FakePayloadReply(op, request,
                    QByteArrayLiteral(
                        "{\"installed\":true,\"maintenance\":false,\"needsDbUpgrade\":false,\"version\":\"11.0.0.0\",\"versionstring\":\"11.0.0\","
                        "\"productname\":\"owncloud.online\"}"),
                    this);
            }
            if (HttpLogger::requestVerb(op, request) == "PROPFIND") {
                auto *reply = new FakeErrorReply(op, request, this, 401);
                reply->setRawHeader(QByteArrayLiteral("WWW-Authenticate"),
                    hasOAuth2 ? QByteArrayLiteral("Bearer realm=\"owncloud.online\"")
                              : QByteArrayLiteral("Basic realm=\"owncloud.online\", charset=\"UTF-8\""));
                return reply;
            }
            if (path.endsWith(QLatin1String("/index.php/apps/oauth2/api/v1/token"))) {
                auto *reply = new FakePayloadReply(op, request, QByteArrayLiteral("<!DOCTYPE html><html><body></body></html>"), this);
                reply->setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 302);
                reply->setRawHeader(QByteArrayLiteral("Location"), QByteArrayLiteral("https://testool.example.org/index.php/login"));
                return reply;
            }
            return new FakeErrorReply(op, request, this, 404);
        });
    }

    QList<QUrl> requests;
};

/** wie NewAccountWizardController::setupWizard für die Adressseite: feste Größe, ModernStyle, Knopfzeile „Anmelden“, „Abbrechen“ */
void setUpLikeAccountWizard(QWizard *wizard)
{
    wizard->setFixedSize(600, 450);
    wizard->setWizardStyle(QWizard::ModernStyle);
    wizard->setOptions(wizard->options() | QWizard::IndependentPages | QWizard::NoBackButtonOnStartPage);
    wizard->setOption(QWizard::NoCancelButton, false);
    wizard->setOption(QWizard::HaveCustomButton1, true);
    wizard->setButtonLayout({QWizard::Stretch, QWizard::NextButton, QWizard::CancelButton});
}

QLabel *labelWithText(QWidget *page, const QString &text)
{
    const auto labels = page->findChildren<QLabel *>();
    for (auto *label : labels) {
        if (label->text() == text) {
            return label;
        }
    }
    return nullptr;
}

/** Steht das Feld ganz im sichtbaren Teil des Fensters (kein Vorfahre schneidet es ab) und ist es hoch genug für seinen umbrochenen Text? */
bool isFullyVisible(QLabel *label, QWidget *window, QString *detail)
{
    const QRect rect(label->mapTo(window, QPoint(0, 0)), label->size());
    QRect visible = rect;
    for (QWidget *ancestor = label->parentWidget(); ancestor && ancestor != window; ancestor = ancestor->parentWidget()) {
        visible &= QRect(ancestor->mapTo(window, QPoint(0, 0)), ancestor->size());
    }
    const int neededHeight = label->heightForWidth(label->width());
    const auto describe = [](const QRect &r) { return QStringLiteral("x=%1..%2 y=%3..%4").arg(r.left()).arg(r.right()).arg(r.top()).arg(r.bottom()); };
    *detail = QStringLiteral("Meldung %1, davon sichtbar %2, Höhe %3 von nötigen %4, Inhalt %5 breit")
                  .arg(describe(rect), describe(visible))
                  .arg(label->height())
                  .arg(neededHeight)
                  .arg(label->parentWidget()->width());
    return label->isVisible() && visible == rect && label->height() >= neededHeight;
}

} // namespace

class TestWizardUrlPage : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testOAuth2WarningFullyVisible_data()
    {
        QTest::addColumn<QString>("language");

        // alle eingebauten Kataloge (CMakeLists von src/resources), darunter die längste Fassung der Meldung (fr)
        const auto catalogs = QDir(QStringLiteral(":/client/translations")).entryList({QStringLiteral("client_*.qm")}, QDir::Files, QDir::Name);
        for (const auto &catalog : catalogs) {
            const QString language = catalog.mid(7).chopped(3);
            QTest::newRow(qPrintable(language)) << language;
        }
    }

    // Server ohne die App „OAuth2“: die Meldung steht vollständig über der Knopfzeile, ohne dass man rollen muss
    void testOAuth2WarningFullyVisible()
    {
        // Arrange
        QFETCH(QString, language);
        QTranslator translator;
        QVERIFY(translator.load(QStringLiteral(":/client/translations/client_%1.qm").arg(language)));
        QCoreApplication::installTranslator(&translator);
        const auto removeTranslator = qScopeGuard([&translator] { QCoreApplication::removeTranslator(&translator); });
        WizardServerAM server(false);
        QWizard wizard;
        setUpLikeAccountWizard(&wizard);
        auto *page = new QWizardPage;
        UrlPageController controller(page, &server, nullptr);
        wizard.addPage(page);
        controller.presetUrl(QStringLiteral("https://testool.example.org"));
        wizard.show();
        QVERIFY(QTest::qWaitForWindowExposed(&wizard));

        // Act
        const bool accepted = controller.validate();

        // Assert
        QVERIFY(!accepted);
        const QString message = ServerDiagnostics::message(ServerDiagnostics::Issue::OAuth2AppMissing, QUrl(QStringLiteral("https://testool.example.org")));
        QLabel *messageLabel = labelWithText(page, message);
        QVERIFY2(messageLabel, qPrintable(message));
        QString detail;
        QTRY_VERIFY2(isFullyVisible(messageLabel, &wizard, &detail), qPrintable(detail));
    }

    void testDoubledSchemeOrSlashes_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("serverPath");

        // eingefügt in ein Feld, in dem schon „https://“ stand, oder mit „/“ zu viel
        QTest::newRow("doubled scheme") << QStringLiteral("https://https://testoo.example.org/") << QString();
        QTest::newRow("doubled slash at the end") << QStringLiteral("https://testoo.example.org//") << QString();
        QTest::newRow("doubled slashes in the path") << QStringLiteral("https://testoo.example.org//owncloud//") << QStringLiteral("/owncloud");
        QTest::newRow("spaces around") << QStringLiteral("  testoo.example.org/owncloud \n") << QStringLiteral("/owncloud");
    }

    // die Adresse führt zum gemeinten Server; keine Anfrage mit „//“ im Pfad
    void testDoubledSchemeOrSlashes()
    {
        // Arrange
        QFETCH(QString, input);
        QFETCH(QString, serverPath);
        WizardServerAM server(true);
        QWizard wizard;
        setUpLikeAccountWizard(&wizard);
        auto *page = new QWizardPage;
        UrlPageController controller(page, &server, nullptr);
        wizard.addPage(page);
        controller.presetUrl(input);
        QUrl serverUrl;
        connect(&controller, &UrlPageController::success, this, [&serverUrl](const UrlPageResults &results) { serverUrl = results.baseServerUrl; });

        // Act
        const bool accepted = controller.validate();

        // Assert
        QVERIFY(accepted);
        QCOMPARE(serverUrl.host(), QStringLiteral("testoo.example.org"));
        QCOMPARE(serverUrl.path().chopped(serverUrl.path().endsWith(QLatin1Char('/')) ? 1 : 0), serverPath);
        QVERIFY(!server.requests.isEmpty());
        for (const auto &url : std::as_const(server.requests)) {
            QCOMPARE(url.host(), QStringLiteral("testoo.example.org"));
            QVERIFY2(!url.path().contains(QLatin1String("//")), qPrintable(url.toString()));
        }
    }
};

QTEST_MAIN(TestWizardUrlPage)
#include "testwizardurlpage.moc"
