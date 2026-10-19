/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include "gui/FoldersGui/buttondelegate.h"
#include "gui/translations.h"

#include <QDialogButtonBox>
#include <QGuiApplication>
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

    void testLoadsQtCatalogForChineseScriptVariants_data()
    {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("qtCatalog");

        // Qt liefert Chinesisch nur als zh_CN (Kurzzeichen) und zh_TW (Langzeichen)
        QTest::newRow("Simplified zh-Hans") << QStringLiteral("zh-Hans") << QStringLiteral("qtbase_zh_CN.qm");
        QTest::newRow("Traditional zh_HK") << QStringLiteral("zh_HK") << QStringLiteral("qtbase_zh_TW.qm");
    }

    // ohne Zuordnung blieben die Standardknöpfe in zh-Hans und zh_HK englisch
    void testLoadsQtCatalogForChineseScriptVariants()
    {
        QFETCH(QString, language);
        QFETCH(QString, qtCatalog);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QFile::copy(QStringLiteral(TEST_QTBASE_DE_QM), QDir(dir.path()).filePath(qtCatalog)));

        QTranslator translator;
        QVERIFY(Translations::loadQtTranslation(&translator, qtCatalogPrefixes, language, {dir.path()}));
        QVERIFY(!translator.isEmpty());
    }

    void testReportsMissingCatalog()
    {
        QTemporaryDir empty;
        QVERIFY(empty.isValid());

        QTranslator translator;
        QVERIFY(!Translations::loadQtTranslation(&translator, qtCatalogPrefixes, QStringLiteral("de"), {empty.path()}));
        QVERIFY(translator.isEmpty());
    }

    // Sprachumfang wie der letzte Community-Client 6.0.3 (dazu pl und sv aus upstream 7.x); upstream hatte elf davon
    // gestrichen, sie sind wieder eingebaut und landen über die Ressourcen in jedem Paket
    void testCommunityLanguagesAreShipped()
    {
        const QStringList expected = {QStringLiteral("ar"), QStringLiteral("bg_BG"), QStringLiteral("cs"), QStringLiteral("de"),
            QStringLiteral("de-informal"), QStringLiteral("en"), QStringLiteral("en_GB"), QStringLiteral("en_US"), QStringLiteral("es"),
            QStringLiteral("et"), QStringLiteral("fr"), QStringLiteral("it"), QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("lo"),
            QStringLiteral("nl"), QStringLiteral("pl"), QStringLiteral("pt"), QStringLiteral("pt_BR"), QStringLiteral("ru"), QStringLiteral("sq"),
            QStringLiteral("sv"), QStringLiteral("tr"), QStringLiteral("ug"), QStringLiteral("zh-Hans"), QStringLiteral("zh_CN"),
            QStringLiteral("zh_HK"), QStringLiteral("zh_TW")};

        const auto available = Translations::listAvailableTranslations();

        for (const auto &language : expected) {
            QVERIFY2(available.contains(language), qPrintable(language));
            QTranslator translator;
            QVERIFY2(translator.load(Translations::translationsFilePrefix() + language, Translations::translationsDirectoryPath()),
                qPrintable(language));
            QVERIFY2(!translator.isEmpty(), qPrintable(language));
        }
    }

    void testLayoutDirectionFollowsLanguage_data()
    {
        QTest::addColumn<QString>("language");
        QTest::addColumn<Qt::LayoutDirection>("direction");

        QTest::newRow("Arabic") << QStringLiteral("ar") << Qt::RightToLeft;
        // für Uigurisch liefert Qt keinen eigenen Katalog, die Richtung kommt allein aus client_ug
        QTest::newRow("Uyghur") << QStringLiteral("ug") << Qt::RightToLeft;
        QTest::newRow("German") << QStringLiteral("de") << Qt::LeftToRight;
        QTest::newRow("Russian") << QStringLiteral("ru") << Qt::LeftToRight;
        QTest::newRow("Chinese") << QStringLiteral("zh_CN") << Qt::LeftToRight;
    }

    // Qt spiegelt die Oberfläche, wenn eine installierte Übersetzung „QT_LAYOUT_DIRECTION“ (Kontext QGuiApplication)
    // mit „RTL“ übersetzt; ohne Qt-Katalog muss das der eigene Katalog leisten
    void testLayoutDirectionFollowsLanguage()
    {
        QFETCH(QString, language);
        QFETCH(Qt::LayoutDirection, direction);

        QTranslator translator;
        QVERIFY(translator.load(Translations::translationsFilePrefix() + language, Translations::translationsDirectoryPath()));
        QVERIFY(QCoreApplication::installTranslator(&translator));
        QCOMPARE(QGuiApplication::layoutDirection(), direction);

        QVERIFY(QCoreApplication::removeTranslator(&translator));
        QCOMPARE(QGuiApplication::layoutDirection(), Qt::LeftToRight);
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

    // Klassen ohne Q_OBJECT übersetzen nur mit eigenem Kontext: ButtonDelegate (Optionsknopf der Ordnerliste) suchte
    // seine Texte unter „QItemDelegate“ und blieb für Screenreader in jeder Sprache englisch
    void testButtonDelegateTextsAreTranslated()
    {
        QTranslator translator;
        QVERIFY(translator.load(Translations::translationsFilePrefix() + QStringLiteral("de"), Translations::translationsDirectoryPath()));
        QVERIFY(QCoreApplication::installTranslator(&translator));

        const QString translated = ButtonDelegate::tr("%1 options button");

        QVERIFY(QCoreApplication::removeTranslator(&translator));
        QCOMPARE(translated, QStringLiteral("%1-Optionen-Schaltfläche"));
    }
};

QTEST_MAIN(TestTranslations)
#include "testtranslations.moc"
