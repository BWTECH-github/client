/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QtTest>

#include "libsync/theme.h"
#include "resources/iconresources.h"

using namespace OCC;

class TestTheme : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Regression: nach dem Merge von upstream 7.2 lieferte applicationIconName()
    // "owncloud.online-icon" (APPLICATION_SHORTNAME), die Ressource heißt aber
    // owncloud-online-icon.svg (APPLICATION_ICON_NAME). Alle Fenster, der Tray-Eintrag
    // und die Update-Dialoge zeigten dadurch ein Platzhaltersymbol.
    void testBrandingIconsExist_data()
    {
        QTest::addColumn<QString>("name");
        const auto *theme = Theme::instance();
        QTest::newRow("applicationIconName") << theme->applicationIconName();
        QTest::newRow("aboutIconName") << theme->aboutIconName();
        QTest::newRow("wizardHeaderLogoName") << theme->wizardHeaderLogoName();
    }

    void testBrandingIconsExist()
    {
        QFETCH(QString, name);
        QVERIFY2(!IconResources::findIconPath(QStringLiteral("universal"), name).isEmpty(), qPrintable(name));
    }

    // Hilfe und Konflikthilfe führen ins Handbuch des Desktop-Clients; die frühere Seite owncloud.online/conflicts.html
    // gab es nie (404), die Startseite von owncloud.online ist keine Hilfe
    void testHelpLinksPointToClientManual()
    {
        const QUrl help(Theme::instance()->helpUrl());
        QCOMPARE(help.scheme(), QStringLiteral("https"));
        QCOMPARE(help.host(), QStringLiteral("docs.owncloud.online"));
        QCOMPARE(help.path(), QStringLiteral("/user/desktop-client"));

        const QUrl conflicts(Theme::instance()->conflictHelpUrl());
        QCOMPARE(conflicts.adjusted(QUrl::RemoveFragment), help);
        QCOMPARE(conflicts.fragment(), QStringLiteral("wenn-dieselbe-datei-zweimal-geaendert-wurde"));
    }

    // ein Theme ohne Hilfe zeigt auch keinen Konfliktlink
    void testNoConflictLinkWithoutHelp()
    {
        class NoHelpTheme : public Theme
        {
        public:
            QString helpUrl() const override { return {}; }
        };
        const NoHelpTheme theme;
        QVERIFY(theme.conflictHelpUrl().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestTheme)
#include "testtheme.moc"
