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

#pragma once

#include "owncloudpropagator.h"

#include <QPointer>
#include <QVector>

namespace OCC {

Q_DECLARE_LOGGING_CATEGORY(lcBulkPropagatorJob)

class PropfindJob;
class SimpleNetworkJob;

/**
 * @brief Uploads many small files in a single multipart/related POST to the
 * server's bulk endpoint (POST /remote.php/dav/bulk), instead of one request per
 * file. Only used when the server advertises capabilities.dav.bulkupload.
 *
 * The job collects a batch of small "upload" SyncFileItems, sends them in one
 * request, then completes each item from the per-file JSON result returned by the
 * server (etag/fileid on success, or a soft error so the file retries individually
 * on the next sync). It mirrors PropagateItemJob::done() per item so the sync
 * journal is updated exactly as for normal uploads.
 *
 * Das Ergebnis enthält keine Rechte; eine bestätigte Datei wird daher erst nach
 * einem Depth-0-PROPFIND auf sie abgeschlossen (wie im Einzel-PUT-Pfad). Schlägt
 * die Abfrage fehl, endet die Datei mit einem weichen Fehler und die nächste
 * Discovery trägt sie ein, ohne sie erneut zu übertragen.
 * @ingroup libsync
 */
class BulkPropagatorJob : public PropagatorJob
{
    Q_OBJECT

public:
    /** Only files up to this size are eligible for bulk upload; larger files use a
     *  normal PUT or chunked upload. */
    static constexpr qint64 maxBulkFileSize = 1 * 1024 * 1024; // 1 MiB
    /** Maximum number of files batched into a single bulk request. */
    static constexpr int maxBulkBatchCount = 100;
    /** Maximum total payload of a single bulk request (well under the server cap). */
    static constexpr qint64 maxBulkBatchSize = 10 * 1024 * 1024; // 10 MiB

    BulkPropagatorJob(OwncloudPropagator *propagator, QVector<SyncFileItemPtr> &&items);

    bool scheduleSelfOrChild() override;
    JobParallelism parallelism() override { return FullParallelism; }

private:
    void markDone();
    bool _countedDone = false;

public:
    void abort(PropagatorJob::AbortType abortType) override;

private Q_SLOTS:
    void slotUploadFinished();

private:
    /** Eine vom Server im Bulk-Ergebnis bestätigte Datei. Ihr Journal-Eintrag
     *  kann erst geschrieben werden, wenn ihre Rechte auf dem Server bekannt sind. */
    struct ConfirmedItem
    {
        SyncFileItemPtr item;
        QString etag;
        QByteArray fileId;
    };

    /** Die Bulk-Antwort enthält keine Rechte, das Journal verlangt sie aber
     *  (SyncJournalDb::setFileRecord prüft _remotePerm per OC_ASSERT). Holt sie wie
     *  PropagateUploadCommon::finalize() mit einem Depth-0-PROPFIND je Datei nach,
     *  höchstens maximumActiveTransferJob() gleichzeitig, und schließt jede
     *  bestätigte Datei mit ihrer Antwort ab. */
    void fetchPermissions(QVector<ConfirmedItem> &&confirmed);
    void startPermissionLookups();
    void permissionLookupFinished(const ConfirmedItem &confirmed, bool ok);

    /** Schreibt die Journal-Einträge des Batches fest und beendet den Job mit dem
     *  schlechtesten Status seiner Dateien; ein FatalError stoppt den ganzen Lauf. */
    void finishBatch();

    /** Complete a single item exactly once: set status/etag/fileid, update the
     *  blacklist, and notify the engine via itemCompleted so the journal is written. */
    void completeItem(const SyncFileItemPtr &item, SyncFileItem::Status status,
        const QString &etag, const QByteArray &fileId, const QString &errorString);

    void finishJob(SyncFileItem::Status status);

    QVector<SyncFileItemPtr> _items; //!< the whole batch handed to this job
    QVector<SyncFileItemPtr> _sentItems; //!< items actually included in the request body
    QPointer<SimpleNetworkJob> _job;
    QVector<ConfirmedItem> _awaitingPermissions; //!< bestätigte Dateien, deren Rechte-Abfrage noch nicht läuft
    QVector<QPointer<PropfindJob>> _permissionJobs; //!< gestartete Rechte-Abfragen, werden mit dem Job abgebrochen
    int _runningPermissionLookups = 0;
    bool _started = false;
};

}
