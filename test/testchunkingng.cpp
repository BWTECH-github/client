/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include "testutils/syncenginetestutils.h"
#include "testutils/testutils.h"

#include "common/syncjournaldb.h"
#include "libsync/filesystem.h"
#include "libsync/syncengine.h"

#include <QtTest>

using namespace std::chrono_literals;
using namespace OCC::FileSystem::SizeLiterals;
using namespace OCC;

namespace {

// Fixed chunk size, so a 3 MiB file is sent in exactly three chunks
void setChunkSize(SyncEngine &engine, qint64 size)
{
    SyncOptions options = engine.syncOptions();
    options._maxChunkSize = size;
    options._initialChunkSize = size;
    options._minChunkSize = size;
    engine.setSyncOptions(options);
}

bool isUploadRequest(const QNetworkRequest &request)
{
    return request.url().path().startsWith(chunkingNgUploadPath());
}

bool isVerb(const QNetworkRequest &request, const QByteArray &verb)
{
    return request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == verb;
}

/* Pretend an earlier, interrupted run left a chunked upload session for the
 * (already existing) local file behind: a journal entry pointing at an upload
 * directory on the server that holds the given chunks (name = byte offset). */
void seedUploadSession(FakeFolder &fakeFolder, const QString &file, uint transferId, const QList<QPair<QString, quint64>> &chunks)
{
    const QString localPath = fakeFolder.localPath() + file;
    SyncJournalDb::UploadInfo info;
    info._valid = true;
    info._transferid = transferId;
    info._size = QFileInfo(localPath).size();
    info._modtime = FileSystem::getModTime(localPath);
    fakeFolder.syncJournal()->setUploadInfo(file, info);
    fakeFolder.syncJournal()->commit(QStringLiteral("test"));

    const QString uploadDir = QString::number(transferId);
    fakeFolder.uploadState().createDir(uploadDir);
    for (const auto &chunk : chunks) {
        fakeFolder.uploadState().insert(uploadDir + QLatin1Char('/') + chunk.first, chunk.second);
    }
}

} // anonymous namespace

class TestChunkingNG : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Baseline: a file above the chunking threshold goes through the NG endpoint
    // (MKCOL, chunk PUTs, assembling MOVE) and ends up on the server.
    void testChunkedUpload()
    {
        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12());
        setChunkSize(*fakeFolder.syncEngine(), 1_MiB);
        const auto size = 3_MiB;
        OperationCounter counter(fakeFolder);

        fakeFolder.localModifier().insert(QStringLiteral("A/big"), size);
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("A/big"))->contentSize, size);
        QCOMPARE(fakeFolder.uploadState().children.count(), 1); // one upload session
        QCOMPARE(counter.nPUT, 3); // three chunks
        QCOMPARE(counter.nMOVE, 1);
    }

    // Resuming a session whose stale chunk cannot be deleted (server error on the
    // DELETE) starts the upload over. The job had already left the propagator's
    // active list at that point, so startNewUpload() hit its
    // OC_ASSERT(_activeJobList.count(this) == 1) - fatal with QT_FORCE_ASSERTS.
    void testResumeDeleteErrorStartsOver()
    {
        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12());
        setChunkSize(*fakeFolder.syncEngine(), 1_MiB);
        const auto size = 3_MiB;

        fakeFolder.localModifier().insert(QStringLiteral("A/big"), size);
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());
        // a chunk at an impossible offset: it cannot be reused and has to be deleted
        const uint transferId = 12345;
        seedUploadSession(fakeFolder, QStringLiteral("A/big"), transferId, {{QStringLiteral("10000"), size}});

        int nDELETE = 0;
        QObject parent;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (op == QNetworkAccessManager::DeleteOperation && isUploadRequest(request)) {
                ++nDELETE;
                return new FakeErrorReply(op, request, &parent, 500);
            }
            return nullptr;
        });

        ItemCompletedSpy completeSpy(fakeFolder);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(nDELETE, 1);
        const auto item = completeSpy.findItem(QStringLiteral("A/big"));
        QVERIFY(item);
        QCOMPARE(item->_status, SyncFileItem::Success);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("A/big"))->contentSize, size);
        // a fresh session was used; the old one is left to the server's cleanup
        QCOMPARE(fakeFolder.uploadState().children.count(), 2);
        QVERIFY(fakeFolder.uploadState().children.contains(QString::number(transferId)));
        // and the journal no longer points at any session
        QVERIFY(!fakeFolder.syncJournal()->getUploadInfo(QStringLiteral("A/big"))._valid);
    }

    // The server answers the MKCOL of the upload directory with a 2xx other than
    // 201 Created. That is no network error, so it must not be fed into
    // classifyError() (which asserts on NoError); the transfer fails with a
    // normal error and succeeds on the next run.
    void testMkColUnexpectedStatus()
    {
        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12());
        setChunkSize(*fakeFolder.syncEngine(), 1_MiB);
        const auto size = 3_MiB;

        int nMKCOL = 0;
        QObject parent;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (isVerb(request, QByteArrayLiteral("MKCOL")) && isUploadRequest(request)) {
                ++nMKCOL;
                // "200 OK" without creating anything
                return new FakePayloadReply(op, request, QByteArray(), &parent);
            }
            return nullptr;
        });

        ItemCompletedSpy completeSpy(fakeFolder);
        fakeFolder.localModifier().insert(QStringLiteral("A/big"), size);
        QVERIFY(!fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(nMKCOL, 1);
        const auto item = completeSpy.findItem(QStringLiteral("A/big"));
        QVERIFY(item);
        QCOMPARE(item->_status, SyncFileItem::NormalError);
        QVERIFY2(item->_errorString.contains(QStringLiteral("(200)")), qPrintable(item->_errorString));
        QVERIFY(!fakeFolder.currentRemoteState().find(QStringLiteral("A/big")));

        // next run against a regular server: the upload goes through
        fakeFolder.setServerOverride(nullptr);
        fakeFolder.syncJournal()->wipeErrorBlacklist();
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("A/big"))->contentSize, size);
    }

    // Same class of defect on the resume PROPFIND: a 2xx that is no 207 (proxy
    // page, non-XML body) arrives without a network error. The session must
    // survive in the journal and be resumed on the next run.
    void testResumePropfindUnexpectedStatus()
    {
        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12());
        setChunkSize(*fakeFolder.syncEngine(), 1_MiB);
        const auto size = 3_MiB;

        fakeFolder.localModifier().insert(QStringLiteral("A/big"), size);
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());
        // the first chunk is already on the server and can be reused
        const uint transferId = 23456;
        seedUploadSession(fakeFolder, QStringLiteral("A/big"), transferId, {{QStringLiteral("0000000000000000"), 1_MiB}});

        int nPROPFIND = 0;
        QObject parent;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (isVerb(request, QByteArrayLiteral("PROPFIND")) && isUploadRequest(request)) {
                ++nPROPFIND;
                return new FakePayloadReply(op, request, QByteArray(), &parent);
            }
            return nullptr;
        });

        ItemCompletedSpy completeSpy(fakeFolder);
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(nPROPFIND, 1);
        const auto item = completeSpy.findItem(QStringLiteral("A/big"));
        QVERIFY(item);
        QCOMPARE(item->_status, SyncFileItem::NormalError);
        QVERIFY2(item->_errorString.contains(QStringLiteral("(200)")), qPrintable(item->_errorString));
        // the session is kept for the next run
        const auto info = fakeFolder.syncJournal()->getUploadInfo(QStringLiteral("A/big"));
        QVERIFY(info._valid);
        QCOMPARE(info._transferid, transferId);

        // next run: the resume works and the existing chunk is reused
        fakeFolder.setServerOverride(nullptr);
        fakeFolder.syncJournal()->wipeErrorBlacklist();
        OperationCounter counter(fakeFolder);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(counter.nPUT, 2); // only the two missing chunks
        QCOMPARE(counter.nMOVE, 1);
        QCOMPARE(fakeFolder.uploadState().children.count(), 1); // the same session
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("A/big"))->contentSize, size);
    }
};

QTEST_GUILESS_MAIN(TestChunkingNG)
#include "testchunkingng.moc"
