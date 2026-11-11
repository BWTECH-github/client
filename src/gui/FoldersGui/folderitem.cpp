/*
 * Copyright (C) Lisa Reese <lisa.reese@kiteworks.com>
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

#include "folderitem.h"

#include "configfile.h"
#include "folderitemupdater.h"
#include "progressdispatcher.h"
#include "resources/iconresources.h"

#include <QCoreApplication>

namespace OCC {


FolderItem::FolderItem(Folder *folder)
    : QStandardItem()
    , _folder(folder)
{
    if (_folder == nullptr)
        return;

    setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

    _updater = new FolderItemUpdater(this);
    updateStatusString();
    updateImage();
}

FolderItem::~FolderItem()
{
    if (_updater) {
        delete _updater;
        _updater = nullptr;
    }
}

void FolderItem::refresh()
{
    updateStatusString();
    emitDataChanged();
}

void FolderItem::setProgress(const ProgressInfo &progress)
{
    if (progress.status() == ProgressInfo::Done) {
        // this might be premature - if so use one of the completion states from folder sync result
        _totalSize = 0;
        _completedSize = 0;
        _percentComplete = 0;
        _estimatedDownBw = 0;
        _estimatedUpBw = 0;
        return;
    }

    if (progress.totalSize() == 0) {
        // nothing is going to happen, visually so ditch - with vfs this will always be the case?
        return;
    }

    // total size is literal - meaning if there was nothing to actually sync, total size = 0
    // this can happen if there are no files/folders in a space
    // or of the space is already 100% up to date, so no diffs,
    // or if there is nothing to "move" between server/client (move or delete)
    _totalSize = progress.totalSize();
    _completedSize = progress.completedSize();
    // the old impl added extra "bytes" to the calculation to account for "contentless" operations (move, delete, possibly vfs placeholder creation)
    // but I'm going with the simple calc because my strong gut feeling is that these tiny updates won't take long enough to substantially change the
    // info in the gui. If someone complains we can make it more complicated again.
    _percentComplete = qBound(0, qRound(double(_completedSize) / double(_totalSize) * 100.0), 100);

    _estimatedUpBw = 0;
    _estimatedDownBw = 0;

    for (const auto &citm : progress._currentItems) {
        // the idea here is the total available bandwidth will be "shared" among multiple items. So summing their individual bw consumption reveals
        // the total bw up or down.
        if (citm._item._direction == SyncFileItem::Up) {
            _estimatedUpBw += progress.fileProgress(citm._item).estimatedBandwidth;
        } else {
            _estimatedDownBw += progress.fileProgress(citm._item).estimatedBandwidth;
        }
    }

    _estimatedEta = progress.totalProgress().estimatedEta;

    refresh();
}

void FolderItem::resetRunProgress()
{
    // die Erweiterung des Dateianbieters (macOS) überträgt gerade: ihre Zahlen stehen in denselben Feldern und bleiben
    if (_platformTransfer)
        return;
    _totalSize = 0;
    _completedSize = 0;
    _percentComplete = 0;
    _estimatedUpBw = 0;
    _estimatedDownBw = 0;
    _estimatedEta = 0;
}

void FolderItem::setTransferProgress(const VfsTransferProgress &progress)
{
    const qint64 uploadBytes = std::max<qint64>(progress.uploadBytes, 0);
    const qint64 downloadBytes = std::max<qint64>(progress.downloadBytes, 0);
    const qint64 uploadedBytes = std::clamp<qint64>(progress.uploadedBytes, 0, uploadBytes);
    const qint64 downloadedBytes = std::clamp<qint64>(progress.downloadedBytes, 0, downloadBytes);
    const qint64 sentBytes = progress.sentBytes;
    const qint64 receivedBytes = progress.receivedBytes;
    const qint64 total = uploadBytes + downloadBytes;
    if (total == 0) {
        if (!_platformTransfer) {
            return;
        }
        // fertig: zurück zum Zustand des Ordners („Synchronisiert“)
        _platformTransfer = false;
        _totalSize = 0;
        _completedSize = 0;
        _percentComplete = 0;
        _estimatedUpBw = 0;
        _estimatedDownBw = 0;
        _estimatedEta = 0;
        _lastTransferSample = {};
        refresh();
        return;
    }

    // Raten aus den bewegten Bytes seit dem letzten Stand, geglättet. Nicht aus dem Fortschritt der Runde: Der springt bei
    // einer Wiederholung zurück. Fallen die Zähler, hat die Erweiterung neu begonnen: wieder bei 0.
    const auto now = std::chrono::steady_clock::now();
    if (!_platformTransfer || _lastTransferSample == std::chrono::steady_clock::time_point{}) {
        _estimatedUpBw = 0;
        _estimatedDownBw = 0;
        _lastTransferSample = now;
        _lastSent = sentBytes;
        _lastReceived = receivedBytes;
    } else {
        const double seconds = std::chrono::duration<double>(now - _lastTransferSample).count();
        if (seconds >= 0.5) {
            const auto rate = [seconds](qint64 current, qint64 before, quint64 previousRate) -> quint64 {
                if (current < before) {
                    return 0;
                }
                const double measured = double(current - before) / seconds;
                return static_cast<quint64>(previousRate == 0 ? measured : 0.7 * double(previousRate) + 0.3 * measured);
            };
            _estimatedUpBw = rate(sentBytes, _lastSent, _estimatedUpBw);
            _estimatedDownBw = rate(receivedBytes, _lastReceived, _estimatedDownBw);
            _lastTransferSample = now;
            _lastSent = sentBytes;
            _lastReceived = receivedBytes;
        }
    }

    _platformTransfer = true;
    _totalSize = static_cast<quint64>(total);
    _completedSize = static_cast<quint64>(uploadedBytes + downloadedBytes);
    // abgerundet: 100 % erst, wenn alles übertragen ist
    _percentComplete = static_cast<int>(_completedSize * 100 / _totalSize);
    // Eine Restzeit nur für eine einzelne Operation: Bei mehreren kennt die Runde nur die Dateien, die das System schon
    // begonnen hat (weitere reicht es erst nach, wenn eine fertig ist), die Restzeit wäre viel zu kurz.
    const bool single = std::max<qint64>(progress.uploadOperations, 0) + std::max<qint64>(progress.downloadOperations, 0) <= 1;
    const quint64 bandwidth = _estimatedUpBw + _estimatedDownBw;
    _estimatedEta = single && bandwidth > 0 ? static_cast<quint64>(double(_totalSize - _completedSize) / double(bandwidth) * 1000.0) : 0;
    refresh();
}

Folder *FolderItem::folder()
{
    return _folder;
}

QString FolderItem::statusIconName() const
{
    if (!_folder || !_folder->accountState())
        return {};
    SyncResult status = _folder->syncResult();
    if (showsPlatformTransfer()) {
        // die Erweiterung überträgt: dasselbe Symbol wie ein Sync-Lauf
        status.setStatus(SyncResult::SyncRunning);
    }
    if (!_folder->isConnected()) {
        status.setStatus(SyncResult::Status::Offline);
    } else if (_folder->syncPaused() || NetworkInformation::instance()->isBehindCaptivePortal()
        || (NetworkInformation::instance()->isMetered() && ConfigFile().pauseSyncWhenMetered())) {
        status.setStatus(SyncResult::Status::Paused);
    }

    return QString("states/%1").arg(status.iconNameForStatus());
}

QString FolderItem::statusAsString() const
{
    if (!_folder || !_folder->accountState())
        return {};

    if (!_folder->isConnected())
        return tr("Offline");

    // this can get out of sorts wrt sync state if we were offline then go on - no sync is ever attempted if the folder is already
    // marked unavailable, so hopefully this catches the corner cases
    if (!_folder->isAvailable())
        return tr("Unavailable: the space is no longer available on the server");

    // when reloading folders from config, no sync on paused folders is attempted, so we can't rely on the sync status
    if (_folder->syncPaused())
        return tr("Sync paused");

    // Die Erweiterung des Dateianbieters überträgt (macOS), die Sync-Engine steht still: wie ein Lauf zeigen
    if (showsPlatformTransfer())
        return progressString();

    SyncResult status = _folder->syncResult();

    switch (status.status()) {
    case SyncResult::SyncPrepare:
    case SyncResult::Undefined:
    case SyncResult::SyncAbortRequested:
        return {};
    case SyncResult::Success:
        return tr("Synced");
    case SyncResult::Unavailable:
        return tr("Unavailable: the space is no longer available on the server");
    case SyncResult::Problem:
    case SyncResult::Error:
    case SyncResult::SetupError:
        return tr("Sync failed"); // todo: I presume this collection of states needs refinement, will cover it when I get to the error handling
    case SyncResult::Paused:
        return tr("Sync paused");
    case SyncResult::Offline:
        return tr("Offline");
    case SyncResult::NotYetStarted:
        return tr("Sync pending");
    case SyncResult::SyncRunning:
        return progressString();
    };

    return {};
}

bool FolderItem::showsPlatformTransfer() const
{
    if (!_platformTransfer || _totalSize == 0 || !_folder || !_folder->isAvailable())
        return false;
    // ein Fehler des Ordners geht vor (Symbol und Text), die Übertragung der Erweiterung überdeckt ihn nicht
    switch (_folder->syncResult().status()) {
    case SyncResult::Error:
    case SyncResult::SetupError:
    case SyncResult::Problem:
    case SyncResult::Unavailable:
        return false;
    default:
        return true;
    }
}

QString FolderItem::progressString() const
{
    // In diesem Lauf noch keine Meldung mit Gesamtgröße, oder er überträgt nichts (Verschieben, Löschen, Platzhalter):
    // keine Zahlen („0 B von 0 B“). Der Text stammt aus dem Infobereich und ist dort in allen Katalogen übersetzt.
    if (_totalSize == 0)
        return QCoreApplication::translate("OCC::TrayMenuController", "Syncing");

    QString completedFormatted = Utility::octetsToString(_completedSize);
    QString totalFormatted = Utility::octetsToString(_totalSize);
    QString percentFormatted = QString::number(_percentComplete);
    QString progress = tr("Syncing %1 of %2 (%3 %").arg(completedFormatted, totalFormatted, percentFormatted);

    if (_estimatedDownBw > 0) {
        QString formattedDownBw = Utility::octetsToString(_estimatedDownBw);
        progress.append(tr(", ⬇️ %1/s").arg(formattedDownBw));
    }
    if (_estimatedUpBw > 0) {
        QString formattedUpBw = Utility::octetsToString(_estimatedUpBw);
        progress.append(tr(", ⬆️ %1/s").arg(formattedUpBw));
    }
    if (_estimatedEta > 0) {
        progress.append(tr(", %1 left").arg(Utility::durationToDescriptiveString1(std::chrono::milliseconds(_estimatedEta))));
    }
    progress.append(")");
    return progress;
}

void FolderItem::updateStatusString()
{
    // the idea here is that we only want to update the string if the current status is something we care to show. If
    // a new status string is empty, just use the LAST status string until we get something new that we care about
    QString newStatusString = statusAsString();
    if (!newStatusString.isEmpty())
        _statusString = newStatusString;
}

void FolderItem::updateImage()
{
    QIcon spaceIcon;
    if (_folder && _folder->space() && _folder->space()->image())
        spaceIcon = _folder->space()->image()->image();

    if (spaceIcon.isNull())
        spaceIcon = IconResources::getCoreIcon("defaultSpaceImage");

    // sure I would like to see if they are equal before the set, but apparently there are no available ==/!= operators.
    _image = spaceIcon;
    // ensure repaint!
    emitDataChanged();
}

QVariant FolderItem::data(int role) const
{
    if (!_folder)
        return QVariant();

    switch (role) {
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
        return _folder->displayName();
    case Qt::AccessibleTextRole: {
        //: Accessible text, read out by a screen reader.
        //~ The first argument is the displayed name of the folder.
        return tr("%1, sync status: %2").arg(_folder->displayName(), _statusString);
    }
    case Qt::DecorationRole:
        return _image;
    case FolderItemRoles::StatusIconRole:
        return IconResources::getCoreIcon(statusIconName());
    case FolderItemRoles::StatusStringRole:
        return _statusString;
    case FolderItemRoles::ProgressRole:
        // Balken nur, solange der Ordner wirklich überträgt; die Zahl stammt aus setProgress() (Sync-Lauf) oder
        // setTransferProgress() (die Erweiterung des Dateianbieters unter macOS)
        if (_totalSize > 0 && _folder->isConnected() && !_folder->syncPaused()
            && (showsPlatformTransfer() || _folder->syncResult().status() == SyncResult::SyncRunning))
            return _percentComplete;
        return QVariant();
    case FolderItemRoles::SortPriorityRole:
        // everything will be sorted in descending order, multiply the priority by 100 and prefer A over Z by applying a negative factor
        return QVariant::fromValue(
            _folder->sortPriority() * 100 - (_folder->displayName().isEmpty() ? 0 : static_cast<int64_t>(_folder->displayName().at(0).toLower().unicode())));
    }
    return QStandardItem::data(role);
}
}
