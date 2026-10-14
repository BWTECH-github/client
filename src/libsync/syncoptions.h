/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
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

#include "owncloudlib.h"
#include "common/filesystembase.h"
#include "common/vfs.h"

#include <QPointer>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QString>

#include <chrono>


namespace OCC {

/**
 * Value class containing the options given to the sync engine
 */
class OWNCLOUDSYNC_EXPORT SyncOptions
{
public:
    // the VFS pointer must be stored in a QPointer as it may go out of scope from "above"
    // it's owned by the Folder so if the folder dies, so does the vfs pointer.
    // use isValid to see if the options should still be used, or test the vfs pointer directly to see if it's null or not.
    explicit SyncOptions(Vfs *vfs = nullptr);
    ~SyncOptions();

    Vfs *vfs() const { return _vfs; }

    /** Size limit in bytes above which a NEW remote folder needs user confirmation
     * before it is synced. Negative = feature disabled. Only used while virtual
     * files are off. */
    qint64 _newBigFolderSizeLimit = -1;

    /** Ask for confirmation before syncing folders from external storages. Only
     * used while virtual files are off. */
    bool _confirmExternalStorage = false;

    /** The initial un-adjusted chunk size in bytes for chunked uploads, which also
     * classifies an item as "to be chunked" (item size above this value).
     *
     * In chunking-NG, when dynamic chunk size adjustments are done, this is the
     * starting value and is then gradually adjusted within the
     * _minChunkSize / _maxChunkSize bounds.
     */
    qint64 _initialChunkSize = 10 * 1000 * 1000; // 10MB

    /** Size threshold (bytes) above which an upload uses the resumable chunked-NG
     * path instead of a single PUT. Decoupled from _initialChunkSize (the chunk
     * *size*): a file only needs one chunk yet still benefits from resume and from
     * bypassing the server's single-request body limit. Kept equal to the client
     * bulk-upload per-file ceiling (BulkPropagatorJob::maxBulkFileSize = 1 MiB) so
     * there is no gap: new files <= 1 MiB batch via bulk, everything larger (and any
     * overwrite > 1 MiB) chunks resumably, and nothing falls back to a bare PUT. */
    qint64 _chunkUploadThreshold = 1 * 1024 * 1024; // 1 MiB, matches maxBulkFileSize

    /** The minimum chunk size in bytes for chunked uploads */
    qint64 _minChunkSize = 1 * 1000 * 1000; // 1MB

    /** The maximum chunk size in bytes for chunked uploads */
    qint64 _maxChunkSize = 100 * 1000 * 1000; // 100MB

    /** The target duration of chunk uploads for dynamic chunk sizing.
     *
     * Set to 0 it will disable dynamic chunk sizing.
     */
    std::chrono::milliseconds _targetChunkUploadDuration = std::chrono::minutes(1);

    /** The maximum number of active jobs in parallel  */
    int _parallelNetworkJobs = 6;

    /** Reads the chunk sizes (OWNCLOUD_CHUNK_SIZE, OWNCLOUD_MIN_CHUNK_SIZE, OWNCLOUD_MAX_CHUNK_SIZE,
     * OWNCLOUD_TARGET_CHUNK_UPLOAD_DURATION) and OWNCLOUD_MAX_PARALLEL from the environment where set. */
    void fillFromEnvironmentVariables();

    /** Ensure min <= initial <= max.
     *
     * Adjusts _minChunkSize / _maxChunkSize to always include _initialChunkSize,
     * so env overrides can't produce an inconsistent configuration.
     */
    void verifyChunkSizes();

    /**
     * @brief isValid indicates if the options are complete
     * @return true if vfs is non-null, else false
     */
    bool isValid() const;

private:
    /** Create a virtual file for new files instead of downloading. If vfs is null, isValid will return false indicating you should not use it
     * implementation note: of course you can pass a nullptr to the ctr or it can be deleted from above so this is the "sanest" way to
     *  handle it.
     */
    QPointer<Vfs> _vfs;
};

}
