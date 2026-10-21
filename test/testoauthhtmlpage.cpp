/*
*    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QtTest>

#include "creds/oauthhtmlpage.h"

using namespace OCC;

class TestOAuthHtmlPage : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSuccessPage() {
        const QString title = tr("Successfully signed in");
        const QString message = tr("You can close this window.");

        auto oauth = OAuthHtmlPage::buildPage(true, title, message);
        QVERIFY(oauth.contains(title));
        QVERIFY(oauth.contains(message));
        QVERIFY(oauth.contains(QStringLiteral("49851C")));
    }

    void testErrorPage() {
        const QString title = tr("Login error");
        const QString message = tr("The reply from the server did not contain all expected fields\n:\tError: Missing field access_token");

        auto oauth = OAuthHtmlPage::buildPage(false, title, message);
        QVERIFY(oauth.contains(title));
        QVERIFY(oauth.contains(message));
        QVERIFY(oauth.contains(QStringLiteral("E50101")));
    }

    // Die Fehlerbeschreibung kommt vom Server: Platzhalter darin liefen endlos bzw. brachen mit FORCE_ASSERTS ab
    void testPlaceholdersFromServer()
    {
        const QString message = QStringLiteral("Error returned from the server: <em>@{MESSAGE} @{UNKNOWN}</em>");

        const auto page = OAuthHtmlPage::buildPage(false, QStringLiteral("Login Error @{TITLE}"), message);
        QVERIFY(page.contains(QStringLiteral("<em>&#64;{MESSAGE} &#64;{UNKNOWN}</em>")));
        QVERIFY(page.contains(QStringLiteral("Login Error &#64;{TITLE}")));
    }
};

QTEST_MAIN(TestOAuthHtmlPage)
#include "testoauthhtmlpage.moc"
