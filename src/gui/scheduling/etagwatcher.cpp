/*
 * Copyright (C) by Hannah von Reth <hannah.vonreth@owncloud.com>
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

#include "gui/scheduling/etagwatcher.h"

#include "accountstate.h"
#include "folder.h"
#include "libsync/configfile.h"
#include "libsync/networkjobs.h"
#include "libsync/syncengine.h"

#include <QTimer>


using namespace std::chrono_literals;

using namespace OCC;

Q_LOGGING_CATEGORY(lcEtagWatcher, "gui.scheduler.etagwatcher", QtInfoMsg)

ETagWatcher::ETagWatcher(QObject *parent)
    : QObject(parent)
{
    // owncloud.online/ownCloud 10 kennt keine Spaces, und nur deren Änderungen meldet der Server von selbst (slotSpaceChanged).
    // Ohne Abfrage kämen Dateien vom Server erst mit der vollständigen Synchronisation nach gut einer Stunde an: wie der
    // klassische Client alle remotePollInterval (Vorgabe 30 s) das ETag jedes Serverordners abfragen.
    auto *pollTimer = new QTimer(this);
    pollTimer->setInterval(ConfigFile().remotePollInterval(0s));
    connect(pollTimer, &QTimer::timeout, this, &ETagWatcher::pollFoldersWithoutSpace);
    pollTimer->start();
}

void ETagWatcher::pollFoldersWithoutSpace()
{
    for (auto accountIt = _lastEtagJobForSpace.cbegin(); accountIt != _lastEtagJobForSpace.cend(); ++accountIt) {
        const QUuid accountId = accountIt.key();
        for (auto it = accountIt->cbegin(); it != accountIt->cend(); ++it) {
            Folder *folder = it->folder;
            // vor dem ersten Sync fehlt das ETag zum Vergleich; ein laufender Sync bringt das neue ETag selbst mit
            if (!folder || !folder->spaceId().isEmpty() || it->etag.isEmpty() || !folder->canSync() || folder->isSyncRunning()) {
                continue;
            }
            const QString key = it.key();
            const QString pollKey = accountId.toString() + QLatin1Char('/') + key;
            if (_runningPolls.contains(pollKey)) {
                continue;
            }
            // der Ordner als Elternobjekt: wird er entfernt (etwa mit dem Konto), endet die Abfrage mit ihm
            auto *job = new RequestEtagJob(folder->accountState()->account(), folder->webDavUrl(), folder->remotePath(), folder);
            _runningPolls.insert(pollKey);
            connect(job, &QObject::destroyed, this, [pollKey, this] { _runningPolls.remove(pollKey); });
            connect(job, &RequestEtagJob::finishedWithoutError, this, [job, accountId, key, this] { updateEtag(accountId, key, job->etag()); });
            job->start();
        }
    }
}

// todo: #48 - this seems to be triggered on every timer driven sync check for each folder. Investigate.
void ETagWatcher::slotSpaceChanged(GraphApi::Space *space)
{
    QString spaceId = space->id();
    QUuid account = space->accountId();
    if (_lastEtagJobForSpace.contains(account) && _lastEtagJobForSpace[account].contains(spaceId)) {
        QString etag = Utility::normalizeEtag(space->eTag());
        updateEtag(account, spaceId, etag);
    }
}

void ETagWatcher::slotFolderListChanged(const QUuid &accountId, const QList<Folder *> folders)
{
    if (folders.isEmpty()) {
        // if the list is empty and there is no accountId, it means ALL folders have been removed, eg on app shutdown
        if (accountId.isNull())
            _lastEtagJobForSpace.clear();
        else if (_lastEtagJobForSpace.contains(accountId))
            _lastEtagJobForSpace.remove(accountId);
        return;
    }

    for (Folder *f : folders)
        onFolderAdded(accountId, f);
}

void ETagWatcher::onFolderAdded(const QUuid &accountId, Folder *folder)
{
    Q_UNUSED(accountId);

    // bei Spaces die Space-ID, ohne Spaces die Ordner-ID: mehrere Ordnerpaare eines ownCloud-10-Kontos
    const QString key = folder->mapKey();
    if (_lastEtagJobForSpace.contains(accountId) && _lastEtagJobForSpace[accountId].contains(key))
        return;

    _lastEtagJobForSpace[accountId].insert(key, ETagInfo{{}, folder});

    connect(folder->syncEngine(), &SyncEngine::rootEtagDiscovered, this, [accountId, folder, this](const QString &etag, const QDateTime &time) {
        const QString key = folder->mapKey();
        if (_lastEtagJobForSpace.contains(accountId) && _lastEtagJobForSpace[accountId].contains(key)) {
            auto &info = _lastEtagJobForSpace[accountId][key];
            info.etag = etag;
            if (folder->accountState()) {
                folder->accountState()->tagLastSuccessfulETagRequest(time);
            }
        }
    });
}

void ETagWatcher::onFolderRemoved(const QUuid &accountId, Folder *folder)
{
    Q_UNUSED(accountId);
    if (folder && _lastEtagJobForSpace.contains(accountId))
        _lastEtagJobForSpace[accountId].remove(folder->mapKey());
}

void ETagWatcher::updateEtag(const QUuid &accountId, const QString &spaceId, const QString &etag)
{
    // the server must provide a valid etag but there might be bugs
    // https://github.com/owncloud/ocis/issues/7160
    if (!etag.isEmpty() && _lastEtagJobForSpace.contains(accountId) && _lastEtagJobForSpace[accountId].contains(spaceId)) {
        auto &info = _lastEtagJobForSpace[accountId][spaceId];
        if (info.folder && info.folder->canSync() && info.etag != etag) {
            const bool folderWasSyncedOnce = !info.etag.isEmpty();
            info.etag = etag;

            if (folderWasSyncedOnce) {
                // Only schedule a sync if the folder finished its initial sync.
                qCDebug(lcEtagWatcher) << "Scheduling sync of" << info.folder->displayName() << info.folder->path() << "due to an etag change";
                emit requestEnqueueFolder(info.folder);
            }
        }
    } else {
        qCWarning(lcEtagWatcher) << "Invalid empty etag received for space" << spaceId;
    }
}
