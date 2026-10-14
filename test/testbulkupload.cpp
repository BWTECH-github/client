/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include "testutils/syncenginetestutils.h"
#include "testutils/testutils.h"

#include "common/syncjournalfilerecord.h"
#include "libsync/syncengine.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <functional>

using namespace std::chrono_literals;
using namespace OCC::FileSystem::SizeLiterals;
using namespace OCC;

namespace {

// Capabilities of a server that offers the bulk endpoint (owncloud.online >= 11.0.14)
QVariantMap bulkUploadCapabilities()
{
    auto cap = TestUtils::testCapabilities();
    auto dav = cap[QStringLiteral("dav")].toMap();
    dav.insert(QStringLiteral("bulkupload"), QStringLiteral("1.0"));
    cap[QStringLiteral("dav")] = dav;
    return cap;
}

/**
 * Fake of the server's POST /remote.php/dav/bulk endpoint, installed as the
 * server override of a FakeFolder: it stores every part of the multipart/related
 * request body as a file in the remote tree and answers with the per-file JSON
 * result. Like the real endpoint it returns etag and OC-FileID only - no
 * permissions. It also records the PROPFINDs the client sends after the first
 * bulk request, i.e. the permission lookups. Everything else is left to the
 * default fake server.
 */
class FakeBulkEndpoint : public QObject
{
public:
    explicit FakeBulkEndpoint(FakeFolder &folder)
        : _folder(folder)
    {
        folder.setServerOverride([this](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *device) -> QNetworkReply * {
            return handle(op, request, device);
        });
    }

    int nPOST = 0;
    int nPUT = 0;
    /// PROPFINDs sent once the first bulk request went out, as "<remote path>|<Depth header>"
    QStringList lookups;
    /// lookups the client had on the wire at the same time (current and peak)
    int lookupsInFlight = 0;
    int maxLookupsInFlight = 0;
    /// permissions the fake server reports for a stored file, keyed by its remote path
    QHash<QString, QString> permissions;
    /// optional custom answer for the lookups
    std::function<QNetworkReply *(QNetworkAccessManager::Operation, const QNetworkRequest &)> onLookup;

private:
    QNetworkReply *handle(QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *device)
    {
        const auto verb = request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
        if (op == QNetworkAccessManager::PutOperation) {
            ++nPUT;
            return nullptr;
        }
        if (verb == QByteArrayLiteral("PROPFIND")) {
            if (nPOST == 0) {
                return nullptr;
            }
            lookups.append(getFilePathFromUrl(request.url()) + QLatin1Char('|') + QString::fromLatin1(request.rawHeader(QByteArrayLiteral("Depth"))));
            QNetworkReply *reply = onLookup ? onLookup(op, request) : new FakePropfindReply(_folder.remoteModifier(), op, request, this);
            ++lookupsInFlight;
            maxLookupsInFlight = qMax(maxLookupsInFlight, lookupsInFlight);
            connect(reply, &QNetworkReply::finished, this, [this] { --lookupsInFlight; });
            return reply;
        }
        if (op != QNetworkAccessManager::PostOperation || !request.url().path().endsWith(QLatin1String("/remote.php/dav/bulk"))) {
            return nullptr;
        }
        ++nPOST;

        const QByteArray contentType = request.header(QNetworkRequest::ContentTypeHeader).toByteArray();
        const qsizetype boundaryPos = contentType.indexOf("boundary=");
        Q_ASSERT(boundaryPos >= 0);
        const QByteArray delimiter = "--" + contentType.mid(boundaryPos + qsizetype(qstrlen("boundary=")));
        const QByteArray body = device ? device->readAll() : QByteArray();

        QJsonObject results;
        qsizetype pos = body.indexOf(delimiter);
        while (pos >= 0) {
            pos += delimiter.size();
            if (body.mid(pos, 2) == "--") {
                break; // closing delimiter
            }
            pos += 2; // CRLF after the delimiter
            const qsizetype headersEnd = body.indexOf("\r\n\r\n", pos);
            Q_ASSERT(headersEnd > pos);
            QHash<QByteArray, QByteArray> headers;
            const QList<QByteArray> headerLines = body.mid(pos, headersEnd - pos).split('\n');
            for (const QByteArray &line : headerLines) {
                const qsizetype colon = line.indexOf(':');
                if (colon > 0) {
                    headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
                }
            }
            const qsizetype length = headers.value("content-length").toLongLong();
            const QByteArray data = body.mid(headersEnd + 4, length);
            pos = body.indexOf(delimiter, headersEnd + 4 + length);

            // X-File-Path is relative to the user's files root, with a leading slash
            const QString remotePath = QString::fromUtf8(headers.value("x-file-path"));
            Q_ASSERT(remotePath.startsWith(QLatin1Char('/')));
            const QString relativePath = remotePath.mid(1);
            FileInfo *file = _folder.remoteModifier().create(relativePath, data.size(), data.isEmpty() ? 'W' : data.at(0));
            file->setLastModifiedFromSecondsUTC(headers.value("x-oc-mtime").toLongLong());
            if (permissions.contains(relativePath)) {
                file->permissions = RemotePermissions::fromServerString(permissions.value(relativePath));
            }

            QJsonObject entry;
            entry.insert(QStringLiteral("error"), false);
            entry.insert(QStringLiteral("etag"), QString::fromUtf8(file->etag));
            entry.insert(QStringLiteral("OC-FileID"), QString::fromUtf8(file->fileId));
            results.insert(remotePath, entry);
        }
        return new FakePayloadReply(op, request, QJsonDocument(results).toJson(QJsonDocument::Compact), this);
    }

    FakeFolder &_folder;
};

} // anonymous namespace

class TestBulkUpload : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // The bulk response carries no permissions, but the sync journal requires
    // them (SyncJournalDb::setFileRecord asserts on a null _remotePerm, fatal
    // with QT_FORCE_ASSERTS). Like the single-PUT path the job looks them up
    // with a Depth-0 PROPFIND per file before the records are written - never
    // with a listing of the parent folder, whose answer grows with the folder
    // and would be repeated for every batch of a large folder.
    void testBulkUploadWritesRemotePermissions()
    {
        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12());
        fakeFolder.account()->setCapabilities({fakeFolder.account()->url(), bulkUploadCapabilities()});
        QVERIFY(fakeFolder.account()->capabilities().bulkUpload());

        FakeBulkEndpoint bulk(fakeFolder);
        // distinct permissions per file, so a mix-up between files or with the folder would show
        bulk.permissions.insert(QStringLiteral("A/a3"), QStringLiteral("RDNVW"));
        bulk.permissions.insert(QStringLiteral("A/a 4"), QStringLiteral("DNVW"));
        bulk.permissions.insert(QStringLiteral("B/b3"), QStringLiteral("RW"));
        bulk.permissions.insert(QStringLiteral("top"), QStringLiteral("RDNV"));

        ItemCompletedSpy completeSpy(fakeFolder);
        fakeFolder.localModifier().insert(QStringLiteral("A/a3"), 8_B);
        fakeFolder.localModifier().insert(QStringLiteral("A/a 4"), 16_B);
        fakeFolder.localModifier().insert(QStringLiteral("B/b3"), 24_B);
        fakeFolder.localModifier().insert(QStringLiteral("top"), 32_B);
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());

        // Unveränderte Ordner (A, B) bekommen keinen eigenen Verzeichnis-Job, ihre
        // neuen Dateien landen im Wurzel-Job: ein Bulk-Request für alle vier
        // Dateien über Ordnergrenzen hinweg (X-File-Path trägt den vollen Pfad),
        // kein einzelner PUT
        QCOMPARE(bulk.nPOST, 1);
        QCOMPARE(bulk.nPUT, 0);
        // exactly one Depth-0 lookup per uploaded file
        bulk.lookups.sort();
        QCOMPARE(bulk.lookups,
            QStringList({QStringLiteral("A/a 4|0"), QStringLiteral("A/a3|0"), QStringLiteral("B/b3|0"), QStringLiteral("top|0")}));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        for (auto it = bulk.permissions.cbegin(); it != bulk.permissions.cend(); ++it) {
            const auto item = completeSpy.findItem(it.key());
            QVERIFY2(!item.isNull(), qPrintable(it.key()));
            QCOMPARE(item->_status, SyncFileItem::Success);

            SyncJournalFileRecord record;
            QVERIFY(fakeFolder.syncJournal().getFileRecord(it.key(), &record));
            QVERIFY2(record.isValid(), qPrintable(it.key()));
            QVERIFY2(!record._remotePerm.isNull(), qPrintable(it.key()));
            QCOMPARE(record._remotePerm.toString(), RemotePermissions::fromServerString(it.value()).toString());
            QCOMPARE(record._etag, fakeFolder.currentRemoteState().find(it.key())->etag);
        }

        // The records are complete: the next run has nothing to transfer or look up
        bulk.nPOST = 0;
        bulk.lookups.clear();
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(bulk.nPOST, 0);
        QCOMPARE(bulk.nPUT, 0);
        QVERIFY(bulk.lookups.isEmpty());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    // A batch holds up to 100 files. Their lookups run in parallel, but only as
    // many at a time as the propagator's transfer budget allows - with HTTP/2 a
    // whole batch would otherwise hit the server in one burst.
    void testPermissionLookupsStayWithinTheTransferBudget()
    {
        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12());
        fakeFolder.account()->setCapabilities({fakeFolder.account()->url(), bulkUploadCapabilities()});
        auto options = fakeFolder.syncEngine().syncOptions();
        options._parallelNetworkJobs = 4; // maximumActiveTransferJob() == 3
        fakeFolder.syncEngine().setSyncOptions(options);

        FakeBulkEndpoint bulk(fakeFolder);
        const int fileCount = 20;
        for (int i = 0; i < fileCount; ++i) {
            fakeFolder.localModifier().insert(QStringLiteral("A/new%1").arg(i), 1_KiB);
        }
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());

        QCOMPARE(bulk.nPOST, 1);
        QCOMPARE(bulk.lookups.size(), fileCount);
        QCOMPARE(bulk.maxLookupsInFlight, 3);
        QCOMPARE(bulk.lookupsInFlight, 0);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        for (int i = 0; i < fileCount; ++i) {
            const QString path = QStringLiteral("A/new%1").arg(i);
            SyncJournalFileRecord record;
            QVERIFY(fakeFolder.syncJournal().getFileRecord(path, &record));
            QVERIFY2(record.isValid(), qPrintable(path));
            QVERIFY2(!record._remotePerm.isNull(), qPrintable(path));
        }
    }

    // The upload succeeded, but the permission lookup fails: the files must not
    // be recorded without permissions. They end as soft errors and the next
    // discovery reconciles them (identical file on both sides) without a second
    // transfer and without a conflict.
    void testPermissionLookupFailure()
    {
        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12());
        fakeFolder.account()->setCapabilities({fakeFolder.account()->url(), bulkUploadCapabilities()});

        FakeBulkEndpoint bulk(fakeFolder);
        QObject parent;
        bulk.onLookup = [&parent](QNetworkAccessManager::Operation op, const QNetworkRequest &request) -> QNetworkReply * {
            return new FakeErrorReply(op, request, &parent, 500);
        };

        ItemCompletedSpy completeSpy(fakeFolder);
        fakeFolder.localModifier().insert(QStringLiteral("A/a3"), 8_B);
        fakeFolder.localModifier().insert(QStringLiteral("B/b3"), 8_B);
        QVERIFY(!fakeFolder.applyLocalModificationsAndSync());
        // beide Dateien in einem gemeinsamen Bulk-Request (siehe oben)
        QCOMPARE(bulk.nPOST, 1);
        QCOMPARE(bulk.lookups.size(), 2);

        for (const auto &path : {QStringLiteral("A/a3"), QStringLiteral("B/b3")}) {
            // stored on the server nevertheless ...
            QVERIFY2(fakeFolder.currentRemoteState().find(path), qPrintable(path));
            // ... but reported as a soft error and not recorded
            const auto item = completeSpy.findItem(path);
            QVERIFY2(!item.isNull(), qPrintable(path));
            QCOMPARE(item->_status, SyncFileItem::SoftError);
            SyncJournalFileRecord record;
            QVERIFY(fakeFolder.syncJournal().getFileRecord(path, &record));
            QVERIFY2(!record.isValid(), qPrintable(path));
        }

        // Next run with a healthy server: the discovery finds the identical file
        // on both sides and just records it - no upload, no conflict
        bulk.onLookup = nullptr;
        bulk.nPOST = 0;
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(bulk.nPOST, 0);
        QCOMPARE(bulk.nPUT, 0);
        FileInfo localState = fakeFolder.currentLocalState();
        QVERIFY(!findConflict(localState, QStringLiteral("A/a3")));
        QVERIFY(!findConflict(localState, QStringLiteral("B/b3")));
        QCOMPARE(localState, fakeFolder.currentRemoteState());
        for (const auto &path : {QStringLiteral("A/a3"), QStringLiteral("B/b3")}) {
            SyncJournalFileRecord record;
            QVERIFY(fakeFolder.syncJournal().getFileRecord(path, &record));
            QVERIFY2(record.isValid(), qPrintable(path));
            QVERIFY2(!record._remotePerm.isNull(), qPrintable(path));
        }
    }
};

QTEST_GUILESS_MAIN(TestBulkUpload)
#include "testbulkupload.moc"
