/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// owncloud.online: systemweite Ausschlussliste der Installation (sync-exclude.lst neben dem Programm wie in 6.0.3)

#include "configfile.h"
#include "csync_exclude.h"
#include "testutils/testutils.h"

#include <QtTest>

using namespace OCC;

namespace {

// Legt die Liste dort ab, wo die Plattform sie neben dem Programm erwartet, und gibt den Programmordner zurück
QString writeInstallationList(const QTemporaryDir &root, const QByteArray &content)
{
#if defined(Q_OS_MAC)
    const QString appDir = root.filePath(QStringLiteral("Contents/MacOS"));
    const QString listDir = root.filePath(QStringLiteral("Contents/Resources"));
#else
    const QString appDir = root.filePath(QStringLiteral("bin"));
    const QString listDir = appDir;
#endif
    if (!QDir().mkpath(appDir) || !QDir().mkpath(listDir)) {
        return {};
    }
    QFile list(QDir(listDir).filePath(QStringLiteral("sync-exclude.lst")));
    if (!list.open(QIODevice::WriteOnly) || list.write(content) != content.size()) {
        return {};
    }
    return appDir;
}

bool isExcludedFile(const ExcludedFiles &excluded, const QString &path)
{
    return excluded.traversalPatternMatch(path, ItemTypeFile) != CSYNC_NOT_EXCLUDED;
}

}

class TestInstallationExcludes : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testNoListWithoutFile()
    {
        const auto root = TestUtils::createTempDir();
        const QString appDir = root.filePath(QStringLiteral("bin"));
        QVERIFY(QDir().mkpath(appDir));

        QVERIFY(ConfigFile::excludeFileFromInstallation(appDir).isEmpty());
    }

    void testFindsListNextToProgram()
    {
        const auto root = TestUtils::createTempDir();
        const QString appDir = writeInstallationList(root, QByteArrayLiteral("*.admin-blocked\n"));
        QVERIFY(!appDir.isEmpty());

        const QString list = ConfigFile::excludeFileFromInstallation(appDir);
        QVERIFY(!list.isEmpty());
        QCOMPARE(QFileInfo(list).fileName(), QStringLiteral("sync-exclude.lst"));
        QFile file(list);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArrayLiteral("*.admin-blocked\n"));
    }

    // Die Liste der Installation ergänzt die eingebaute Liste
    void testInstallationListIsApplied()
    {
        const auto root = TestUtils::createTempDir();
        const QString appDir = writeInstallationList(root, QByteArrayLiteral("# Vorgabe der IT\n*.admin-blocked\nAdminOnly/\n"));
        QVERIFY(!appDir.isEmpty());

        ExcludedFiles excluded;
        ConfigFile::setupDefaultExcludeFilePaths(excluded, appDir);
        QVERIFY(excluded.reloadExcludeFiles());

        QVERIFY(isExcludedFile(excluded, QStringLiteral("Bericht.admin-blocked")));
        QVERIFY(excluded.traversalPatternMatch(QStringLiteral("AdminOnly"), ItemTypeDirectory) != CSYNC_NOT_EXCLUDED);
        QVERIFY(!isExcludedFile(excluded, QStringLiteral("Bericht.txt")));
        // eingebaute Liste
        QVERIFY(isExcludedFile(excluded, QStringLiteral("~$Bericht.docx")));
    }

    void testWithoutInstallationListOnlyBuiltIn()
    {
        const auto root = TestUtils::createTempDir();
        const QString appDir = root.filePath(QStringLiteral("bin"));
        QVERIFY(QDir().mkpath(appDir));

        ExcludedFiles excluded;
        ConfigFile::setupDefaultExcludeFilePaths(excluded, appDir);
        QVERIFY(excluded.reloadExcludeFiles());

        QVERIFY(!isExcludedFile(excluded, QStringLiteral("Bericht.admin-blocked")));
        QVERIFY(isExcludedFile(excluded, QStringLiteral("~$Bericht.docx")));
    }
};

QTEST_GUILESS_MAIN(TestInstallationExcludes)
#include "testinstallationexcludes.moc"
