/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QLabel>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QtTest>

#include "accountstate.h"
#include "common/syncjournaldb.h"
#include "folder.h"
#include "folderman.h"
#include "folderwizard/folderwizard.h"
#include "folderwizard/folderwizard_p.h"
#include "folderwizard/folderwizardremotepath.h"
#include "libsync/theme.h"
#include "testutils/testutils.h"

#include <syncenginetestutils.h>

using namespace OCC;

namespace {
// Fähigkeiten eines ownCloud-10-Servers (owncloud.online): ohne Spaces
QVariantMap oc10Capabilities()
{
    auto caps = TestUtils::testCapabilities();
    caps.remove(QStringLiteral("spaces"));
    return caps;
}

FakeAM *fakeAccessManager(Account *account)
{
    return dynamic_cast<FakeAM *>(account->accessManager());
}

QTreeWidget *folderTree(FolderWizardRemotePath *page)
{
    return page->findChild<QTreeWidget *>(QStringLiteral("folderTreeWidget"));
}

QStringList childNames(QTreeWidgetItem *item)
{
    QStringList names;
    for (int i = 0; i < item->childCount(); ++i) {
        names.append(item->child(i)->text(0));
    }
    names.sort();
    return names;
}

// startet den Assistenten und wartet, bis die Serverordner der obersten Ebene eingetragen sind
FolderWizardRemotePath *startWizard(FolderWizard &wizard)
{
    auto *page = qobject_cast<FolderWizardRemotePath *>(wizard.page(FolderWizard::Page_Target));
    if (!page) {
        return nullptr;
    }
    QSignalSpy listed(page, &FolderWizardRemotePath::listingFinished);
    wizard.restart();
    if (!listed.wait()) {
        return nullptr;
    }
    return page;
}
}

class TestFolderWizard : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testRemoteFolderIsChosenFirst()
    {
        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});

        FolderWizard wizard(account, nullptr);
        // erst der Serverordner, dann der lokale Ordner, damit der Vorschlag zum Serverordner passt
        QCOMPARE(wizard.startId(), static_cast<int>(FolderWizard::Page_Target));
        QVERIFY(wizard.page(FolderWizard::Page_Source));
        QVERIFY(!wizard.page(FolderWizard::Page_Space));

        FolderWizardRemotePath *page = startWizard(wizard);
        QVERIFY(page);
        QTreeWidget *tree = folderTree(page);
        QVERIFY(tree);
        QTreeWidgetItem *root = tree->topLevelItem(0);
        QVERIFY(root);
        QCOMPARE(childNames(root), (QStringList{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("S")}));

        // nichts gewählt: weiter geht es nicht
        QVERIFY(!page->isComplete());
        QCOMPARE(wizard.d_func()->remotePath(), QString());

        QVERIFY(page->selectByPath(QStringLiteral("/B")));
        QCOMPARE(page->targetPath(), QStringLiteral("/B"));
        QVERIFY(page->isComplete());
        QCOMPARE(wizard.d_func()->remotePath(), QStringLiteral("/B"));
        // die Ordnerliste zeigt den Namen des Serverordners
        QCOMPARE(wizard.d_func()->displayName(), QStringLiteral("B"));
        // der vorgeschlagene lokale Ordner liegt neben dem Sync-Ordner des Kontos und trägt den Namen des Serverordners
        const QString suggestion = wizard.d_func()->initialLocalPath();
        QVERIFY2(suggestion.endsWith(QStringLiteral(" - B")), qPrintable(suggestion));
        QCOMPARE(FolderMan::instance()->checkPathValidity(suggestion, FolderMan::NewFolderType::SpacesFolder, account->uuid()), QString());

        // die Wurzel steht für das ganze Konto
        QVERIFY(page->selectByPath(QStringLiteral("/")));
        QCOMPARE(page->targetPath(), QStringLiteral("/"));
        QVERIFY(page->isComplete());
        QCOMPARE(wizard.d_func()->displayName(), Theme::instance()->appNameGUI());

        // Unterordner werden beim Aufklappen nachgeladen; ein Ordner ohne Unterordner verliert sein Aufklapp-Dreieck
        QSignalSpy listed(page, &FolderWizardRemotePath::listingFinished);
        QTreeWidgetItem *itemA = root->child(0);
        QCOMPARE(itemA->text(0), QStringLiteral("A"));
        itemA->setExpanded(true);
        QVERIFY(listed.wait());
        QCOMPARE(itemA->childCount(), 0);
        QCOMPARE(itemA->childIndicatorPolicy(), QTreeWidgetItem::DontShowIndicatorWhenChildless);
    }

    void testCreateRemoteFolder()
    {
        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        FakeAM *fakeAm = fakeAccessManager(account);
        QVERIFY(fakeAm);

        FolderWizard wizard(account, nullptr);
        FolderWizardRemotePath *page = startWizard(wizard);
        QVERIFY(page);

        // unter dem gewählten Ordner anlegen; danach ist der neue Ordner gewählt
        QVERIFY(page->selectByPath(QStringLiteral("/A")));
        QSignalSpy created(page, &FolderWizardRemotePath::remoteFolderCreated);
        page->createRemoteFolder(QStringLiteral("  Neuer Ordner  "));
        QVERIFY(created.wait());
        QCOMPARE(created.first().first().toBool(), true);
        QCOMPARE(page->targetPath(), QStringLiteral("/A/Neuer Ordner"));
        QVERIFY(page->isComplete());
        const FileInfo *newFolder = fakeAm->currentRemoteState().find(QStringLiteral("A/Neuer Ordner"));
        QVERIFY(newFolder);
        QVERIFY(newFolder->isDir);

        // ungültige Namen gehen gar nicht erst an den Server
        created.clear();
        page->createRemoteFolder(QStringLiteral("a/b"));
        QCOMPARE(created.size(), 1);
        QCOMPARE(created.first().first().toBool(), false);
        QVERIFY(!fakeAm->currentRemoteState().find(QStringLiteral("A/Neuer Ordner/a")));

        // der Server lehnt ab: Meldung, die Auswahl bleibt
        fakeAm->setOverride([fakeAm](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == "MKCOL") {
                return new FakeErrorReply(op, request, fakeAm, 403);
            }
            return nullptr;
        });
        created.clear();
        page->createRemoteFolder(QStringLiteral("Gesperrt"));
        QVERIFY(created.wait());
        QCOMPARE(created.first().first().toBool(), false);
        QCOMPARE(page->targetPath(), QStringLiteral("/A/Neuer Ordner"));
        QVERIFY(!page->findChild<QFrame *>(QStringLiteral("warningFrame"))->isHidden());
    }

    void testNewFolderNameValidation()
    {
        QVERIFY(FolderWizardRemotePath::checkNewFolderName(QStringLiteral("Dokumente")).isEmpty());
        QVERIFY(FolderWizardRemotePath::checkNewFolderName(QStringLiteral("Fotos & Videos 2026")).isEmpty());
        QVERIFY(FolderWizardRemotePath::checkNewFolderName(QStringLiteral("Übersicht")).isEmpty());
        for (const auto &invalid : {QString(), QStringLiteral("   "), QStringLiteral("."), QStringLiteral(".."), QStringLiteral("a/b"),
                 QStringLiteral("a\\b"), QStringLiteral("a:b"), QStringLiteral("frage?"), QStringLiteral("stern*"), QStringLiteral("<tag>"),
                 QStringLiteral("pipe|"), QStringLiteral("\"zitat\""), QStringLiteral("endet."), }) {
            QVERIFY2(!FolderWizardRemotePath::checkNewFolderName(invalid).isEmpty(), qPrintable(invalid));
        }
    }

    // ein Serverordner, der schon (auch als Teil der Wurzel) gespiegelt wird, sperrt „Weiter“ mit klarer Meldung
    void testBlocksAlreadySyncedServerFolder()
    {
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("root")));

        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});

        FolderDefinition definition(account->davUrl(), {}, {});
        definition.setLocalPath(QDir(dir.path()).canonicalPath() + QStringLiteral("/root"));
        definition.setTargetPath(QStringLiteral("/"));
        definition.setJournalPath(SyncJournalDb::makeDbName(definition.localPath()));
        Folder *rootFolder = FolderMan::instance()->addFolder(accountState.get(), definition);
        QVERIFY(rootFolder);

        FolderWizard wizard(account, nullptr);
        FolderWizardRemotePath *page = startWizard(wizard);
        QVERIFY(page);

        auto *warningFrame = page->findChild<QFrame *>(QStringLiteral("warningFrame"));
        auto *warningLabel = page->findChild<QLabel *>(QStringLiteral("warningLabel"));
        QVERIFY(warningFrame && warningLabel);

        for (const auto &path : {QStringLiteral("/"), QStringLiteral("/C")}) {
            QVERIFY(page->selectByPath(path));
            QVERIFY2(!page->isComplete(), qPrintable(path));
            QVERIFY(!warningFrame->isHidden());
            QVERIFY(warningLabel->text().contains(QDir::toNativeSeparators(rootFolder->cleanPath()).toHtmlEscaped()));
        }

        // ohne das Paar für die Wurzel ist der Ordner frei
        FolderMan::instance()->removeFolderFromGui(rootFolder);
        QVERIFY(page->selectByPath(QStringLiteral("/B")));
        QVERIFY(page->isComplete());
        QVERIFY(warningFrame->isHidden());
    }
};

QTEST_MAIN(TestFolderWizard)
#include "testfolderwizard.moc"
