/*
 * Copyright (C) 2026 by BW-Tech GmbH (owncloud.online)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "bulkpropagatorjob.h"

#include "account.h"
#include "common/syncjournaldb.h"
#include "common/utility.h"
#include "filesystem.h"
#include "networkjobs.h"
#include "owncloudpropagator_p.h"
#include "syncengine.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>
#include <utility>

namespace OCC {

Q_LOGGING_CATEGORY(lcBulkPropagatorJob, "sync.propagator.bulkupload", QtInfoMsg)

namespace {
    // X-File-Path is the path of the file relative to the user's files root, which
    // is exactly what OwncloudPropagator::fullRemotePath() returns (relative to the
    // WebDAV files root). Make sure it has a single leading slash.
    QByteArray remoteFilePath(OwncloudPropagator *propagator, const SyncFileItemPtr &item)
    {
        QString path = propagator->fullRemotePath(item->_file);
        if (!path.startsWith(QLatin1Char('/'))) {
            path.prepend(QLatin1Char('/'));
        }
        return path.toUtf8();
    }
}

BulkPropagatorJob::BulkPropagatorJob(OwncloudPropagator *propagator, QVector<SyncFileItemPtr> &&items)
    : PropagatorJob(propagator, QStringLiteral("bulk"))
    , _items(std::move(items))
{
}

bool BulkPropagatorJob::scheduleSelfOrChild()
{
    if (state() == Finished || _started) {
        return false;
    }
    _started = true;
    setState(Running);
    // ins Transfer-Budget des Schedulers einbuchen (Gegenstueck in markDone())
    propagator()->bulkJobStarted();

    const QByteArray boundary = QByteArrayLiteral("owncloud_online_bulk_boundary_marker");
    QByteArray body;
    body.reserve(64 * 1024);

    for (const auto &item : std::as_const(_items)) {
        const QString localPath = propagator()->fullLocalPath(item->_file);

        if (FileSystem::isFileLocked(localPath, FileSystem::LockMode::SharedRead)) {
            Q_EMIT propagator()->seenLockedFile(localPath, FileSystem::LockMode::SharedRead);
            completeItem(item, SyncFileItem::SoftError, {}, {}, tr("%1 the file is currently in use").arg(localPath));
            continue;
        }

        QFile file(localPath);
        if (!file.open(QIODevice::ReadOnly)) {
            completeItem(item, SyncFileItem::SoftError, {}, {}, tr("Could not read %1").arg(localPath));
            continue;
        }
        const QByteArray data = file.readAll();
        file.close();

        // Skip files that changed since discovery; they will be re-picked next sync.
        if (FileSystem::fileChanged(QFileInfo(localPath), item->_size, item->_modtime)) {
            propagator()->_anotherSyncNeeded = true;
            completeItem(item, SyncFileItem::SoftError, {}, {}, tr("Local file changed during sync."));
            continue;
        }

        const QByteArray md5 = QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex();
        body += "--" + boundary + "\r\n";
        body += "X-File-Path: " + remoteFilePath(propagator(), item) + "\r\n";
        body += "X-File-Md5: " + md5 + "\r\n";
        body += "X-OC-Mtime: " + QByteArray::number(static_cast<qint64>(item->_modtime)) + "\r\n";
        body += "Content-Length: " + QByteArray::number(static_cast<qint64>(data.size())) + "\r\n\r\n";
        body += data;
        body += "\r\n";
        _sentItems.append(item);
    }
    body += "--" + boundary + "--\r\n";

    if (_sentItems.isEmpty()) {
        // Every item failed to read; they were already completed with an error.
        finishJob(SyncFileItem::SoftError);
        return true;
    }

    const QByteArray contentType = QByteArrayLiteral("multipart/related; boundary=") + boundary;
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);

    // POST to the DAV root endpoint /remote.php/dav/bulk (NOT the files WebDAV url).
    _job = new SimpleNetworkJob(propagator()->account(), propagator()->account()->url(),
        QStringLiteral("remote.php/dav/bulk"), "POST", std::move(body), request, this);
    connect(_job, &SimpleNetworkJob::finishedSignal, this, &BulkPropagatorJob::slotUploadFinished);
    qCInfo(lcBulkPropagatorJob) << "Bulk uploading" << _sentItems.size() << "files in one request";
    _job->start();
    return true;
}

void BulkPropagatorJob::slotUploadFinished()
{
    if (state() == Finished) {
        // Aborted in the meantime; sent items will be retried on the next sync.
        return;
    }

    const auto reply = _job ? _job->reply() : nullptr;
    const int httpCode = reply ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
    const auto netError = reply ? reply->error() : QNetworkReply::OperationCanceledError;

    if (netError != QNetworkReply::NoError || httpCode < 200 || httpCode >= 300) {
        const QString err = reply ? reply->errorString() : tr("Bulk upload failed");
        qCWarning(lcBulkPropagatorJob) << "Bulk upload failed, http" << httpCode << err;
        // Whole batch failed: soft-error every file so they retry individually.
        for (const auto &item : std::as_const(_sentItems)) {
            completeItem(item, SyncFileItem::SoftError, {}, {}, err);
        }
        propagator()->_anotherSyncNeeded = true;
        finishJob(SyncFileItem::SoftError);
        return;
    }

    const QJsonObject results = QJsonDocument::fromJson(reply->readAll()).object();
    QVector<ConfirmedItem> confirmed;
    for (const auto &item : std::as_const(_sentItems)) {
        const QString key = QString::fromUtf8(remoteFilePath(propagator(), item));
        const QJsonObject entry = results.value(key).toObject();
        if (entry.isEmpty() || entry.value(QStringLiteral("error")).toBool(true)) {
            const QString msg = entry.value(QStringLiteral("message")).toString(tr("Server did not confirm the upload"));
            propagator()->_anotherSyncNeeded = true;
            completeItem(item, SyncFileItem::SoftError, {}, {}, msg);
        } else {
            const QString etag = Utility::normalizeEtag(entry.value(QStringLiteral("etag")).toString());
            const QByteArray fileId = entry.value(QStringLiteral("OC-FileID")).toVariant().toByteArray();
            if (etag.isEmpty()) {
                // A success entry without an etag would be written to the journal with an
                // empty etag, making the next discovery re-download the just-uploaded file.
                // Retry it on the next sync instead of recording an incomplete record.
                propagator()->_anotherSyncNeeded = true;
                completeItem(item, SyncFileItem::SoftError, {}, {}, tr("Server confirmed the upload without an etag"));
            } else {
                // Bestätigt, aber noch nicht abgeschlossen: der Journal-Eintrag braucht
                // die Rechte auf dem Server, die die Bulk-Antwort nicht liefert.
                confirmed.append({item, etag, fileId});
            }
        }
    }

    if (confirmed.isEmpty()) {
        finishBatch();
        return;
    }
    fetchPermissions(std::move(confirmed));
}

void BulkPropagatorJob::fetchPermissions(QVector<ConfirmedItem> &&confirmed)
{
    // Bewusst keine Depth-1-Auflistung des Elternverzeichnisses: deren Antwort
    // wächst mit dem Verzeichnis, und jeder Batch eines großen Verzeichnisses
    // müsste es erneut auflisten - der Aufwand stiege quadratisch mit der Zahl
    // der Dateien. Ein Depth-0-PROPFIND je Datei bleibt linear und klein.
    _awaitingPermissions = std::move(confirmed);
    qCInfo(lcBulkPropagatorJob) << "Looking up the permissions of" << _awaitingPermissions.size() << "bulk uploaded files";
    startPermissionLookups();
}

void BulkPropagatorJob::startPermissionLookups()
{
    // Nicht den ganzen Batch (bis zu maxBulkBatchCount Dateien) auf einmal: mit
    // HTTP/2 ginge das als ein Schwall an den Server. So viele gleichzeitig, wie
    // der Propagator Transfers zulässt.
    const int maxRunning = std::max(1, propagator()->maximumActiveTransferJob());
    while (_runningPermissionLookups < maxRunning && !_awaitingPermissions.isEmpty()) {
        const ConfirmedItem entry = _awaitingPermissions.takeFirst();
        auto *job = new PropfindJob(propagator()->account(), propagator()->webDavUrl(),
            propagator()->fullRemotePath(entry.item->_file), PropfindJob::Depth::Zero, this);
        job->setProperties({ QByteArrayLiteral("http://owncloud.org/ns:permissions") });
        connect(job, &PropfindJob::directoryListingIterated, this, [item = entry.item](const QString &, const QMap<QString, QString> &properties) {
            const auto permissions = properties.constFind(QStringLiteral("permissions"));
            if (permissions != properties.cend()) {
                item->_remotePerm = RemotePermissions::fromServerString(permissions.value());
            }
        });
        connect(job, &PropfindJob::finishedWithoutError, this, [this, entry] { permissionLookupFinished(entry, true); });
        connect(job, &PropfindJob::finishedWithError, this, [this, entry] { permissionLookupFinished(entry, false); });
        _permissionJobs.append(job);
        ++_runningPermissionLookups;
        job->start();
    }
}

void BulkPropagatorJob::permissionLookupFinished(const ConfirmedItem &confirmed, bool ok)
{
    if (state() == Finished) {
        // inzwischen abgebrochen; die bestätigten Dateien gleicht die nächste Discovery ab
        return;
    }
    --_runningPermissionLookups;

    if (ok && !confirmed.item->_remotePerm.isNull()) {
        completeItem(confirmed.item, SyncFileItem::Success, confirmed.etag, confirmed.fileId, {});
    } else {
        // Die Datei liegt auf dem Server, aber ohne ihre Rechte kann der Journal-
        // Eintrag nicht geschrieben werden. Weicher Fehler: die nächste Discovery
        // sieht beidseitig dieselbe Datei (Größe und Mtime gleich) und trägt sie
        // nur noch ein.
        qCWarning(lcBulkPropagatorJob) << "No permissions for the bulk uploaded file" << confirmed.item->_file
                                       << "- retrying on the next sync";
        propagator()->_anotherSyncNeeded = true;
        completeItem(confirmed.item, SyncFileItem::SoftError, {}, {}, tr("The server did not provide the file permissions"));
    }

    if (_runningPermissionLookups == 0 && _awaitingPermissions.isEmpty()) {
        finishBatch();
    } else {
        startPermissionLookups();
    }
}

void BulkPropagatorJob::finishBatch()
{
    // Persist the journal records written by completeItem()->updateMetadata() now,
    // mirroring PropagateUploadCommon::finalize()'s per-file commit. Otherwise the
    // whole batch's records stay in the engine's long-running transaction and are
    // rolled back on a crash/kill before the final "All Finished" commit — the
    // just-uploaded files would then be re-discovered as new on the next sync.
    propagator()->_journal->commit(QStringLiteral("bulk upload"));

    // Der Batch endet mit dem schlechtesten Status seiner Dateien, so wie ein einzelner
    // PropagateItemJob seinen eigenen meldet: der übergeordnete Composite-Job weiß dann,
    // dass das Verzeichnis nicht sauber ist.
    // If completeItem() hit a FatalError (a journal write failure in updateMetadata),
    // mirror PropagateItemJob::done(): a failing journal must hard-stop the whole sync,
    // because items already marked done in memory are not persisted and could later be
    // seen as deletions. A SoftError on the other hand only schedules another sync.
    SyncFileItem::Status status = SyncFileItem::Success;
    for (const auto &item : std::as_const(_items)) {
        if (item->_status == SyncFileItem::FatalError) {
            status = SyncFileItem::FatalError;
            break;
        }
        if (item->_status == SyncFileItem::SoftError) {
            status = SyncFileItem::SoftError;
        }
    }
    if (status == SyncFileItem::FatalError) {
        // Emit finished BEFORE aborting (like PropagateItemJob::done): propagator()->abort()
        // cascades into this job's abort(), which sets the state to Finished and would make
        // a subsequent finishJob() a no-op, swallowing the finished signal and hanging the
        // parent composite job.
        finishJob(SyncFileItem::FatalError);
        propagator()->abort();
        return;
    }
    finishJob(status);
}

void BulkPropagatorJob::completeItem(const SyncFileItemPtr &item, SyncFileItem::Status status,
    const QString &etag, const QByteArray &fileId, const QString &errorString)
{
    item->_status = status;
    if (status == SyncFileItem::Success) {
        if (!etag.isEmpty()) {
            item->_etag = etag;
        }
        if (!fileId.isEmpty()) {
            item->_fileId = fileId;
        }
        item->_errorString.clear();
        if (item->_hasBlacklistEntry) {
            propagator()->_journal->wipeErrorBlacklistEntry(item->_file);
            item->_hasBlacklistEntry = false;
        }

        // Persist the sync journal record, exactly like PropagateUploadCommon::finalize()
        // does via updateMetadata(). Without this the file is re-discovered as new on the
        // next sync, which breaks no-op detection and delete propagation.
        // Der Eintrag verlangt die Rechte auf dem Server (setFileRecord prüft _remotePerm
        // per OC_ASSERT, mit QT_FORCE_ASSERTS fatal); die Bulk-Antwort liefert keine,
        // erfolgreiche Dateien kommen daher erst nach fetchPermissions() hierher.
        const auto result = propagator()->updateMetadata(*item);
        if (!result) {
            item->_status = SyncFileItem::FatalError;
            item->_errorString = tr("Error updating metadata: %1").arg(result.error());
            propagator()->_anotherSyncNeeded = true;
        } else {
            propagator()->_journal->setUploadInfo(item->_file, SyncJournalDb::UploadInfo());
            propagator()->reportProgress(*item, item->_size);
        }
    } else {
        if (item->_errorString.isEmpty()) {
            item->_errorString = errorString;
        }
    }
    // Mirror PropagateItemJob::done(): tell the engine so the sync journal is updated.
    Q_EMIT propagator()->itemCompleted(item);
}

void BulkPropagatorJob::finishJob(SyncFileItem::Status status)
{
    if (state() == Finished) {
        return;
    }
    setState(Finished);
    markDone();
    Q_EMIT finished(status);
}

void BulkPropagatorJob::markDone()
{
    // exakt einmal pro Running->Finished-Uebergang das Budget freigeben —
    // sowohl finishJob() als auch abort() (das ohne finished()-Emission
    // beendet) laufen hier durch
    if (_countedDone || !_started) {
        return;
    }
    _countedDone = true;
    propagator()->bulkJobFinished();
}

void BulkPropagatorJob::abort(PropagatorJob::AbortType abortType)
{
    // Stop further handling first so a cancelled reply doesn't double-complete items.
    if (state() != Finished) {
        setState(Finished);
        markDone();
    }
    if (_job && _job->reply()) {
        _job->reply()->abort();
    }
    // Laufende Rechte-Abfragen: ihre Antworten enden mit OperationCanceledError, und
    // permissionLookupFinished() übergeht sie, weil der Zustand schon Finished ist.
    for (const auto &job : std::as_const(_permissionJobs)) {
        if (job) {
            job->abort();
        }
    }
    _permissionJobs.clear();
    if (abortType == AbortType::Asynchronous) {
        Q_EMIT abortFinished();
    }
}

}
