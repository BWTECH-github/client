/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include "gui/translations.h"

#include <QDialogButtonBox>
#include <QLibraryInfo>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

using namespace OCC;

namespace {

const QStringList qtCatalogPrefixes = {QStringLiteral("qt_"), QStringLiteral("qtbase_")};

// Beschriftung ohne Kürzelzeichen, macOS zeigt "&" nicht an
QString buttonText(const QDialogButtonBox &box, QDialogButtonBox::StandardButton button)
{
    return box.button(button)->text().remove(QLatin1Char('&'));
}

}

class TestTranslations : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSearchPathsCoverPackageLayouts()
    {
        const QString appDir = QStringLiteral("/opt/app/bin");

        const QStringList paths = Translations::qtTranslationsSearchPaths(QStringLiteral("/opt/app/bin/translations"), appDir);

        // QLibraryInfo zuerst; der windeployqt-Ort bin/translations ist hier derselbe und steht nur einmal drin
        const QStringList expected = {QStringLiteral("/opt/app/bin/translations"), QStringLiteral("/opt/app/translations"),
            QStringLiteral("/opt/app/Resources/translations"), QStringLiteral("/opt/app/share/qt6/translations")};
        QCOMPARE(paths, expected);
    }

    void testLoadsQtCatalogFromInstallerLayout_data()
    {
        QTest::addColumn<QString>("language");

        QTest::newRow("system locale") << QStringLiteral("de_DE");
        QTest::newRow("language only") << QStringLiteral("de");
        QTest::newRow("informal variant") << QStringLiteral("de-informal");
    }

    // Windows-Installer: <Programmordner>/bin/owncloud.online.exe, Qt-Kataloge unter <Programmordner>/translations.
    // QLibraryInfo meldet dort <Programmordner>/bin/translations, das es nicht gibt (Ja/Nein blieben englisch).
    void testLoadsQtCatalogFromInstallerLayout()
    {
        QFETCH(QString, language);

        // Arrange
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QDir rootDir(root.path());
        QVERIFY(rootDir.mkpath(QStringLiteral("bin")));
        QVERIFY(rootDir.mkpath(QStringLiteral("translations")));
        QVERIFY(QFile::copy(QStringLiteral(TEST_QTBASE_DE_QM), rootDir.filePath(QStringLiteral("translations/qtbase_de.qm"))));
        const QString appDir = rootDir.filePath(QStringLiteral("bin"));
        const QStringList searchPaths = Translations::qtTranslationsSearchPaths(appDir + QStringLiteral("/translations"), appDir);

        // Act
        QTranslator translator;
        QVERIFY(Translations::loadQtTranslation(&translator, qtCatalogPrefixes, language, searchPaths));
        QVERIFY(QCoreApplication::installTranslator(&translator));

        // Assert
        const QDialogButtonBox box(QDialogButtonBox::Yes | QDialogButtonBox::No);
        QCOMPARE(buttonText(box, QDialogButtonBox::Yes), QStringLiteral("Ja"));
        QCOMPARE(buttonText(box, QDialogButtonBox::No), QStringLiteral("Nein"));
    }

    void testReportsMissingCatalog()
    {
        QTemporaryDir empty;
        QVERIFY(empty.isValid());

        QTranslator translator;
        QVERIFY(!Translations::loadQtTranslation(&translator, qtCatalogPrefixes, QStringLiteral("de"), {empty.path()}));
        QVERIFY(translator.isEmpty());
    }

    // der echte Qt-Katalog der Bauumgebung, so wie ihn das Paket mitliefert
    void testQtCatalogTranslatesStandardButtons()
    {
        const QStringList searchPaths =
            Translations::qtTranslationsSearchPaths(QLibraryInfo::path(QLibraryInfo::TranslationsPath), QCoreApplication::applicationDirPath());
        QTranslator translator;
        if (!Translations::loadQtTranslation(&translator, qtCatalogPrefixes, QStringLiteral("de"), searchPaths)) {
            QSKIP("no German Qt catalog in this build environment");
        }
        QVERIFY(QCoreApplication::installTranslator(&translator));

        const QDialogButtonBox box(QDialogButtonBox::Yes | QDialogButtonBox::No);
        QCOMPARE(buttonText(box, QDialogButtonBox::Yes), QStringLiteral("Ja"));
        QCOMPARE(buttonText(box, QDialogButtonBox::No), QStringLiteral("Nein"));
    }
};

QTEST_MAIN(TestTranslations)
#include "testtranslations.moc"
