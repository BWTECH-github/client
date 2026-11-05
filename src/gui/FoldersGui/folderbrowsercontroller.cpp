/*
 * Copyright (C) BW-Tech GmbH
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

#include "folderbrowsercontroller.h"

#include "common/pinstate.h"
#include "common/utility.h"
#include "common/vfs.h"
#include "accountstate.h"
#include "application.h"
#include "configfile.h"
#include "folder.h"
#include "folderitem.h"
#include "folderman.h"
#include "networkjobs.h"
#include "traymenucontroller.h"

#include "resources/iconresources.h"

#include <QTimer>
#include <QApplication>
#include <QCollator>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMenu>
#include <QNetworkReply>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyle>

#include <algorithm>
#include <functional>
#include <numeric>
#include <utility>
#include <vector>

namespace OCC {

Q_LOGGING_CATEGORY(lcFolderBrowser, "gui.accountfolders.browser", QtInfoMsg)

namespace {
    int kindOf(const QStandardItem *item)
    {
        return item ? item->data(FolderItemRoles::ItemKindRole).toInt() : 0;
    }

    // Reihenfolge der Zeilenarten unter einem Ordner: Fehler, Ordner, Dateien, „weitere Dateien“, Platzhalter
    int blockRank(int kind)
    {
        switch (static_cast<FolderTreeItemKind>(kind)) {
        case FolderTreeItemKind::Error:
            return 0;
        case FolderTreeItemKind::BrowserFolder:
            return 1;
        case FolderTreeItemKind::BrowserFile:
            return 2;
        case FolderTreeItemKind::BrowserMoreFiles:
            return 3;
        case FolderTreeItemKind::BrowserPlaceholder:
            return 4;
        }
        return 0;
    }

    // erste Zeile dieser Art oder einer späteren: dort beginnt der Block der Art
    int blockStart(const QStandardItem *parentItem, FolderTreeItemKind kind)
    {
        const int rank = blockRank(static_cast<int>(kind));
        for (int row = 0; row < parentItem->rowCount(); ++row) {
            if (blockRank(kindOf(parentItem->child(row, 0))) >= rank)
                return row;
        }
        return parentItem->rowCount();
    }

    QStandardItem *firstChildOfKind(const QStandardItem *parentItem, FolderTreeItemKind kind)
    {
        for (int row = 0; row < parentItem->rowCount(); ++row) {
            QStandardItem *child = parentItem->child(row, 0);
            if (child && kindOf(child) == static_cast<int>(kind))
                return child;
        }
        return nullptr;
    }

    QStandardItem *newFiller()
    {
        QStandardItem *filler = new QStandardItem();
        // enabled (but nothing else) so keyboard navigation in column 1 can step onto the row
        filler->setFlags(Qt::ItemIsEnabled);
        return filler;
    }

    // Fügt die Zeilen in einem Zug ein: ein rowsInserted für alle statt eines je Zeile. Ist in der Ansicht ein Editor
    // offen (der Optionen-Knopf der Ordnerzeile), legt sie nach jedem Einfügen den ganzen Baum neu aus; Zeile für Zeile
    // wurde das bei tausenden Einträgen quadratisch und hielt das Fenster minutenlang an.
    void insertRun(QStandardItem *parentItem, int row, QList<QStandardItem *> &run)
    {
        if (run.isEmpty())
            return;
        if (parentItem->columnCount() < 2)
            parentItem->setColumnCount(2);
        const qsizetype count = run.size();
        parentItem->insertRows(row, run);
        run.clear();
        // Spalte 1 der neuen Zeilen: setChild meldet je Zelle einen Layoutwechsel. Die Zeilen sind schon gemeldet und
        // die Zellen waren eben noch leer, die Ansichten brauchen dafür keine Nachricht.
        const QSignalBlocker blocker(parentItem->model());
        for (qsizetype i = 0; i < count; ++i)
            parentItem->setChild(row + static_cast<int>(i), 1, newFiller());
    }

    // entfernt die Zeilen von unten nach oben in zusammenhängenden Stücken
    void removeRuns(QStandardItem *parentItem, QList<int> rows)
    {
        std::sort(rows.begin(), rows.end());
        for (qsizetype last = rows.size() - 1; last >= 0;) {
            qsizetype first = last;
            while (first > 0 && rows.at(first - 1) == rows.at(first) - 1)
                --first;
            parentItem->removeRows(rows.at(first), rows.at(last) - rows.at(first) + 1);
            last = first - 1;
        }
    }

    // Sortiert nach Namen wie Utility::sortFilenames und behält die ersten keep Einträge; Namen, die die Sortierung gleich
    // einstuft („Bilder“ und „bilder“), nach Zeichencode, damit jede Auflistung dieselbe Reihenfolge ergibt.
    // Rückgabe: Zahl der weggelassenen Einträge
    qsizetype sortByName(QList<FolderBrowserController::ListingEntry> &entries, const QCollator &collator, qsizetype keep)
    {
#ifdef Q_OS_MACOS
        // Unter macOS kostet QCollator::compare (UCCompareText) so viel, dass 20.000 Namen eine Sekunde brauchten; ein
        // Schlüssel je Name (UCGetCollationKey) ordnet gleich und vergleicht sich billig. Nicht unter Windows: Dort ordnen
        // die Schlüssel Zahlen mit führenden Nullen anders als compare („00100“ vor „00002“).
        std::vector<QCollatorSortKey> keys;
        keys.reserve(entries.size());
        for (const FolderBrowserController::ListingEntry &entry : std::as_const(entries))
            keys.push_back(collator.sortKey(entry.name));
        const auto order = [&keys](qsizetype a, qsizetype b) { return keys[a].compare(keys[b]); };
#else
        const auto order = [&collator, &entries](qsizetype a, qsizetype b) { return collator.compare(entries.at(a).name, entries.at(b).name); };
#endif
        const auto lessThan = [&order, &entries](qsizetype a, qsizetype b) {
            const int result = order(a, b);
            return result != 0 ? result < 0 : entries.at(a).name < entries.at(b).name;
        };
        std::vector<qsizetype> indexes(entries.size());
        std::iota(indexes.begin(), indexes.end(), qsizetype(0));
        // nur die ersten keep: erst auswählen, dann nur diese sortieren
        if (static_cast<qsizetype>(indexes.size()) > keep) {
            std::nth_element(indexes.begin(), indexes.begin() + keep, indexes.end(), lessThan);
            indexes.resize(keep);
        }
        std::sort(indexes.begin(), indexes.end(), lessThan);
        QList<FolderBrowserController::ListingEntry> sorted;
        sorted.reserve(static_cast<qsizetype>(indexes.size()));
        for (const qsizetype index : indexes)
            sorted.append(entries.at(index));
        const qsizetype dropped = entries.size() - sorted.size();
        entries = std::move(sorted);
        return dropped;
    }
}

FolderBrowserController::FolderBrowserController(QStandardItemModel *model, QObject *parent)
    : QObject(parent)
    , _model(model)
{
    connect(_model, &QStandardItemModel::rowsInserted, this, &FolderBrowserController::onRowsInserted);
    connect(_model, &QStandardItemModel::itemChanged, this, &FolderBrowserController::onItemChanged);
    connect(FolderMan::instance(), &FolderMan::folderRemoved, this, &FolderBrowserController::onFolderRemoved);
    connect(ProgressDispatcher::instance(), &ProgressDispatcher::itemCompleted, this, &FolderBrowserController::onItemCompleted);
    connect(ProgressDispatcher::instance(), &ProgressDispatcher::filesTouched, this, &FolderBrowserController::onFilesTouched);

    // gesammelt: ein Sync schließt tausende Dateien in wenigen Sekunden ab
    _changedPathsTimer.setSingleShot(true);
    _changedPathsTimer.setInterval(500);
    connect(&_changedPathsTimer, &QTimer::timeout, this, &FolderBrowserController::refreshChangedRows);

    ConfigFile::setupDefaultExcludeFilePaths(_excludedFiles);
    _excludedFiles.reloadExcludeFiles();
}

int FolderBrowserController::itemKind(const QStandardItem *item)
{
    return item ? item->data(FolderItemRoles::ItemKindRole).toInt() : 0;
}

QList<QStandardItem *> FolderBrowserController::browserChildren(const QStandardItem *parentItem)
{
    return childrenOfKind(parentItem, FolderTreeItemKind::BrowserFolder);
}

QList<QStandardItem *> FolderBrowserController::childrenOfKind(const QStandardItem *parentItem, FolderTreeItemKind kind)
{
    QList<QStandardItem *> result;
    for (int row = 0; row < parentItem->rowCount(); ++row) {
        QStandardItem *child = parentItem->child(row, 0);
        if (child && itemKind(child) == static_cast<int>(kind))
            result.append(child);
    }
    return result;
}

QString FolderBrowserController::folderKey(const Folder *folder)
{
    return QString::fromUtf8(folder->id());
}

void FolderBrowserController::onRowsInserted(const QModelIndex &parent, int first, int last)
{
    // only the top-level folder rows need bootstrapping; the child rows are our own
    if (parent.isValid())
        return;

    for (int row = first; row <= last; ++row) {
        FolderItem *folderItem = dynamic_cast<FolderItem *>(_model->item(row, 0));
        if (!folderItem || !folderItem->folder())
            continue;
        ensureSyncState(folderItem->folder());
        // the placeholder gives the row its expander; the actual listing is fetched lazily on expand
        if (!placeholderChild(folderItem) && browserChildren(folderItem).isEmpty())
            attachPlaceholder(folderItem);
    }

    // a folder list rebuild destroys the browser rows and with them any unapplied checkbox
    // edits; recompute so stored pending state and the apply bar stay in agreement
    recomputePending();
}

void FolderBrowserController::ensureSyncState(Folder *folder)
{
    const QString key = folderKey(folder);
    if (_syncStates.contains(key))
        return;

    FolderSyncState state;
    bool ok = false;
    state.journalBlackList = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, ok);
    if (!ok) {
        qCWarning(lcFolderBrowser) << "Could not read selective sync list for" << folder->path();
        state.journalBlackList.clear();
    }
    state.pendingBlackList = state.journalBlackList;
    state.vfsAtLoad = folder->virtualFilesEnabled();
    _syncStates.insert(key, state);
}

void FolderBrowserController::onFolderRemoved(const QUuid &accountId, Folder *folder)
{
    Q_UNUSED(accountId);
    if (!folder)
        return;
    _syncStates.remove(folderKey(folder));
    recomputePending();
}

void FolderBrowserController::onItemCompleted(Folder *folder, const SyncFileItemPtr &item)
{
    if (!item->isDirectory()) {
        noteChangedPath(folder, item->destination());
    }
}

void FolderBrowserController::onFilesTouched(Folder *folder, const QSet<QString> &relativePaths)
{
    for (const QString &path : relativePaths) {
        noteChangedPath(folder, path);
    }
}

void FolderBrowserController::noteChangedPath(Folder *folder, QString relativePath)
{
    if (!folder || !folder->virtualFilesEnabled()) {
        return;
    }
    auto &paths = _changedPaths[folder];
    for (; !relativePath.isEmpty(); relativePath.truncate(std::max<qsizetype>(relativePath.lastIndexOf(QLatin1Char('/')), 0))) {
        paths.insert(relativePath);
    }
    if (!_changedPathsTimer.isActive()) {
        _changedPathsTimer.start();
    }
}

void FolderBrowserController::refreshChangedRows()
{
    const auto changed = std::exchange(_changedPaths, {});
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) {
        const FolderItem *rootItem = rootItemForFolder(it.key());
        if (!rootItem) {
            continue;
        }
        QList<QStandardItem *> rows;
        const auto keepRows = [&](const QList<QStandardItem *> &children) {
            for (QStandardItem *child : children) {
                if (it.value().contains(child->data(FolderItemRoles::RemotePathRole).toString())) {
                    rows.append(child);
                }
            }
        };
        QList<const QStandardItem *> parents{rootItem};
        while (!parents.isEmpty()) {
            const QStandardItem *parent = parents.takeLast();
            const QList<QStandardItem *> subfolders = browserChildren(parent);
            keepRows(subfolders);
            keepRows(childrenOfKind(parent, FolderTreeItemKind::BrowserFile));
            for (const QStandardItem *subfolder : subfolders) {
                parents.append(subfolder);
            }
        }
        queueAvailability(rows);
    }
}

QList<QStandardItem *> FolderBrowserController::placeholderRow()
{
    QStandardItem *placeholder = new QStandardItem(tr("Loading …"));
    placeholder->setFlags(Qt::ItemIsEnabled);
    placeholder->setData(static_cast<int>(FolderTreeItemKind::BrowserPlaceholder), FolderItemRoles::ItemKindRole);
    // keep placeholders below folder rows and error rows when the model gets (re)sorted
    placeholder->setData(-1, FolderItemRoles::SortPriorityRole);
    return {placeholder, newFiller()};
}

void FolderBrowserController::attachPlaceholder(QStandardItem *parentItem)
{
    QScopedValueRollback<bool> guard(_updatingModel, true);
    parentItem->appendRow(placeholderRow());
}

QStandardItem *FolderBrowserController::placeholderChild(const QStandardItem *parentItem)
{
    return firstChildOfKind(parentItem, FolderTreeItemKind::BrowserPlaceholder);
}

FolderItem *FolderBrowserController::rootItemForFolder(Folder *folder) const
{
    for (int row = 0; row < _model->rowCount(); ++row) {
        FolderItem *folderItem = dynamic_cast<FolderItem *>(_model->item(row, 0));
        if (folderItem && folderItem->folder() == folder)
            return folderItem;
    }
    return nullptr;
}

FolderItem *FolderBrowserController::rootItemForIndex(const QModelIndex &index) const
{
    QModelIndex top = index;
    while (top.parent().isValid())
        top = top.parent();
    return dynamic_cast<FolderItem *>(_model->item(top.row(), 0));
}

QStandardItem *FolderBrowserController::itemForPath(FolderItem *rootItem, const QString &relPath) const
{
    if (relPath.isEmpty())
        return rootItem;

    QStandardItem *current = rootItem;
    const QStringList segments = relPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString accumulated;
    for (const QString &segment : segments) {
        accumulated = accumulated.isEmpty() ? segment : accumulated + QLatin1Char('/') + segment;
        QStandardItem *next = nullptr;
        for (QStandardItem *child : browserChildren(current)) {
            if (child->data(FolderItemRoles::RemotePathRole).toString() == accumulated) {
                next = child;
                break;
            }
        }
        if (!next)
            return nullptr;
        current = next;
    }
    return current;
}

void FolderBrowserController::onItemExpanded(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    FolderItem *rootItem = rootItemForIndex(index);
    if (!rootItem || !rootItem->folder())
        return;
    Folder *folder = rootItem->folder();

    const QModelIndex contentIndex = index.siblingAtColumn(0);
    QStandardItem *item = _model->itemFromIndex(contentIndex);
    if (!item)
        return;

    // run the staleness checks on every expansion in the subtree, not only the root:
    // toggling virtual files invalidates all rows of the folder. When the subtree was
    // rebuilt, the expanded child row is gone - relist the (still expanded) root instead.
    if (maybeRefreshFolderState(folder, rootItem)) {
        requestListing(folder, QString());
        return;
    }

    QString relPath;
    if (item != rootItem) {
        if (itemKind(item) != static_cast<int>(FolderTreeItemKind::BrowserFolder))
            return;
        relPath = item->data(FolderItemRoles::RemotePathRole).toString();
        if (relPath.isEmpty())
            return;
    }

    requestListing(folder, relPath);
}

bool FolderBrowserController::maybeRefreshFolderState(Folder *folder, FolderItem *rootItem)
{
    const QString key = folderKey(folder);
    auto it = _syncStates.find(key);
    if (it == _syncStates.end())
        return false;

    // virtual files may have been toggled since the rows were built; the row mode
    // (checkboxes vs. availability) is then stale, so rebuild the subtree from scratch
    // and reload the journal state
    bool rebuilt = false;
    if (it->vfsAtLoad != folder->virtualFilesEnabled()) {
        {
            QScopedValueRollback<bool> guard(_updatingModel, true);
            QList<int> browserRows;
            for (int row = 0; row < rootItem->rowCount(); ++row) {
                const int kind = itemKind(rootItem->child(row, 0));
                if (kind == static_cast<int>(FolderTreeItemKind::BrowserFolder) || kind == static_cast<int>(FolderTreeItemKind::BrowserPlaceholder)
                    || kind == static_cast<int>(FolderTreeItemKind::BrowserFile) || kind == static_cast<int>(FolderTreeItemKind::BrowserMoreFiles))
                    browserRows.append(row);
            }
            // in Stücken statt Zeile für Zeile, aus demselben Grund wie beim Einfügen (insertRun)
            removeRuns(rootItem, browserRows);
        }
        attachPlaceholder(rootItem);
        _syncStates.remove(key);
        ensureSyncState(folder);
        // the rebuild dropped any pending edits, make the apply bar reflect that
        recomputePending();
        it = _syncStates.find(key);
        if (it == _syncStates.end())
            return true;
        rebuilt = true;
    }

    // pick up blacklist changes made elsewhere (e.g. the "Manage subfolder sync" modal),
    // but never while the user has unapplied edits here
    if (it->pendingBlackList == it->journalBlackList) {
        bool ok = false;
        const QSet<QString> journal = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, ok);
        if (ok && journal != it->journalBlackList) {
            it->journalBlackList = journal;
            it->pendingBlackList = journal;
            refreshCheckStates(rootItem, journal);
        }
    }
    return rebuilt;
}

void FolderBrowserController::requestListing(Folder *folder, const QString &relPath)
{
    if (!folder->accountState() || !folder->accountState()->account())
        return;

    const QString jobKey = folderKey(folder) + QLatin1Char('\x1f') + relPath;
    if (_pendingJobs.contains(jobKey))
        return;

    const QString folderPath = Utility::stripTrailingSlash(folder->remotePath());
    const QString propfindPath = relPath.isEmpty() ? folderPath : Utility::concatUrlPathItems({folderPath, relPath});

    auto *job = new PropfindJob(folder->accountState()->account(), folder->webDavUrl(), propfindPath, PropfindJob::Depth::One, this);
    job->setProperties({QByteArrayLiteral("resourcetype"), QByteArrayLiteral("http://owncloud.org/ns:size")});

    _pendingJobs.insert(jobKey);
    // catch-all cleanup; the result handlers below additionally remove the key up front
    // because the job is only destroyed via deleteLater - a relist triggered from inside
    // a result handler (VFS toggled mid-flight) must not be dropped by the dedup check
    connect(job, &QObject::destroyed, this, [this, jobKey] { _pendingJobs.remove(jobKey); });

    QPointer<Folder> folderGuard(folder);
    // non-collection responses are the files; directoryListingSubfolders alone
    // only carries the collections
    auto files = std::make_shared<QStringList>();
    connect(job, &PropfindJob::directoryListingIterated, this, [files](const QString &href, const QMap<QString, QString> &properties) {
        if (!properties.value(QStringLiteral("resourcetype")).contains(QLatin1String("collection")))
            files->append(href);
    });
    connect(job, &PropfindJob::directoryListingSubfolders, this, [this, job, jobKey, folderGuard, relPath, files](const QStringList &subfolders) {
        _pendingJobs.remove(jobKey);
        if (!folderGuard)
            return;
        populateListing(folderGuard, relPath, subfolders, *files, job->sizes());
    });
    connect(job, &PropfindJob::finishedWithError, this, [this, job, jobKey, folderGuard, relPath] {
        _pendingJobs.remove(jobKey);
        if (!folderGuard)
            return;
        const bool notFound = job->reply() && job->reply()->error() == QNetworkReply::ContentNotFoundError;
        onListingError(folderGuard, relPath, notFound);
    });
    job->start();
}

void FolderBrowserController::onListingError(Folder *folder, const QString &parentRelPath, bool notFound)
{
    FolderItem *rootItem = rootItemForFolder(folder);
    if (!rootItem)
        return;
    QStandardItem *parentItem = itemForPath(rootItem, parentRelPath);
    if (!parentItem)
        return;
    QStandardItem *placeholder = placeholderChild(parentItem);
    if (!placeholder)
        return;

    QScopedValueRollback<bool> guard(_updatingModel, true);
    // keep the placeholder so collapsing and re-expanding retries the listing
    placeholder->setText(notFound ? tr("This folder no longer exists on the server.")
                                  : tr("An error occurred while loading the folder contents."));
}

void FolderBrowserController::populateListing(Folder *folder, const QString &parentRelPath, const QStringList &subfolders, const QStringList &files, const QHash<QString, qint64> &sizes)
{
    FolderItem *rootItem = rootItemForFolder(folder);
    if (!rootItem)
        return;

    // the listing is async: virtual files may have been toggled while it was in flight,
    // in which case these results describe rows of the wrong mode - rebuild and relist
    if (maybeRefreshFolderState(folder, rootItem)) {
        requestListing(folder, QString());
        return;
    }

    QStandardItem *parentItem = itemForPath(rootItem, parentRelPath);
    if (!parentItem)
        return;

    const QString key = folderKey(folder);
    ensureSyncState(folder);
    FolderSyncState &syncState = _syncStates[key];

    const QString folderPath = Utility::stripTrailingSlash(folder->remotePath());
    const QString rootPath = Utility::ensureTrailingSlash(Utility::concatUrlPath(folder->webDavUrl(), folderPath).path());
    const bool ignoreHiddenFiles = FolderMan::instance()->ignoreHiddenFiles();
    const bool vfsMode = folder->virtualFilesEnabled();

    // collect the direct children of parentRelPath from the PROPFIND result
    const QString parentPrefix = parentRelPath.isEmpty() ? QString() : Utility::ensureTrailingSlash(parentRelPath);
    QList<ListingEntry> folderEntries;
    for (const QString &absPath : subfolders) {
        if (!absPath.startsWith(rootPath))
            continue;
        if (_excludedFiles.isExcludedRemote(absPath, rootPath, ignoreHiddenFiles, ItemTypeDirectory))
            continue;
        const QString relPath = Utility::stripTrailingSlash(absPath.mid(rootPath.size()));
        if (relPath.isEmpty() || !relPath.startsWith(parentPrefix))
            continue;
        const QString name = relPath.mid(parentPrefix.size());
        if (name.isEmpty() || name.contains(QLatin1Char('/')))
            continue;
        folderEntries.append({name, relPath, sizes.value(Utility::ensureTrailingSlash(absPath), -1)});
    }

    // the direct file children, shown as plain informational rows below the folders
    QList<ListingEntry> fileEntries;
    fileEntries.reserve(files.size());
    for (const QString &absPath : files) {
        if (!absPath.startsWith(rootPath))
            continue;
        if (_excludedFiles.isExcludedRemote(absPath, rootPath, ignoreHiddenFiles, ItemTypeFile))
            continue;
        const QString relPath = absPath.mid(rootPath.size());
        if (relPath.isEmpty() || !relPath.startsWith(parentPrefix))
            continue;
        const QString name = relPath.mid(parentPrefix.size());
        if (name.isEmpty() || name.contains(QLatin1Char('/')))
            continue;
        fileEntries.append({name, relPath, sizes.value(absPath, -1)});
    }

    // a blacklist of just "/" means "everything deselected"; concretize it to the actual
    // top-level folders as soon as they are known, exactly like the selective sync dialog
    if (parentRelPath.isEmpty() && syncState.journalBlackList.contains(QStringLiteral("/"))) {
        const bool wasClean = syncState.pendingBlackList == syncState.journalBlackList;
        syncState.journalBlackList.clear();
        for (const ListingEntry &entry : std::as_const(folderEntries))
            syncState.journalBlackList.insert(Utility::ensureTrailingSlash(entry.relPath));
        if (wasClean)
            syncState.pendingBlackList = syncState.journalBlackList;
    }

    {
        QScopedValueRollback<bool> guard(_updatingModel, true);
        applyListing(parentItem, std::move(folderEntries), std::move(fileEntries), vfsMode, syncState.pendingBlackList);
    }

    // die Verfügbarkeit fragt je Zeile das Journal und das Dateisystem:
    // nachgetragen in Zeitscheiben, damit ein großer Ordner das Fenster nicht anhält
    if (vfsMode)
        queueAvailability(browserChildren(parentItem) + childrenOfKind(parentItem, FolderTreeItemKind::BrowserFile));
}

void FolderBrowserController::applyListing(
    QStandardItem *parentItem, QList<ListingEntry> folders, QList<ListingEntry> files, bool vfsMode, const QSet<QString> &blackList)
{
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    sortByName(folders, collator, folders.size());
    const qsizetype hiddenFiles = sortByName(files, collator, maxFileRows);

    // Positionen in der Reihenfolge der Ansicht: Ordner, dann Dateien
    QHash<QString, qsizetype> folderPositions;
    folderPositions.reserve(folders.size());
    for (qsizetype i = 0; i < folders.size(); ++i)
        folderPositions.insert(folders.at(i).relPath, i);
    QHash<QString, qsizetype> filePositions;
    filePositions.reserve(files.size());
    for (qsizetype i = 0; i < files.size(); ++i)
        filePositions.insert(files.at(i).relPath, folders.size() + i);

    // Vorhandene Zeilen bleiben, wenn die Auflistung sie noch enthält und sie in deren Reihenfolge stehen (außer der Reihe
    // nur nach geänderter Sortierung: dann entsteht die Zeile an der richtigen Stelle neu). Alles andere geht.
    QList<QStandardItem *> kept;
    QList<int> staleRows;
    QStandardItem *moreRow = nullptr;
    QStandardItem *placeholder = nullptr;
    qsizetype lastPosition = -1;
    for (int row = 0; row < parentItem->rowCount(); ++row) {
        QStandardItem *child = parentItem->child(row, 0);
        const int kind = itemKind(child);
        if (kind == static_cast<int>(FolderTreeItemKind::BrowserFolder) || kind == static_cast<int>(FolderTreeItemKind::BrowserFile)) {
            const QHash<QString, qsizetype> &positions = kind == static_cast<int>(FolderTreeItemKind::BrowserFolder) ? folderPositions : filePositions;
            const auto it = positions.constFind(child->data(FolderItemRoles::RemotePathRole).toString());
            if (it != positions.cend() && *it > lastPosition) {
                kept.append(child);
                lastPosition = *it;
            } else {
                staleRows.append(row);
            }
        } else if (kind == static_cast<int>(FolderTreeItemKind::BrowserMoreFiles)) {
            if (hiddenFiles > 0 && !moreRow)
                moreRow = child;
            else
                staleRows.append(row);
        } else if (kind == static_cast<int>(FolderTreeItemKind::BrowserPlaceholder)) {
            if (folders.isEmpty() && files.isEmpty() && !placeholder)
                placeholder = child;
            else
                staleRows.append(row);
        }
    }
    // Erst entfernen, solange der Baum noch klein ist, dann in so wenigen Stücken wie möglich einfügen: Mit offenem Editor
    // legt die Ansicht den Baum nach jeder Strukturänderung neu aus, unter macOS samt der Höhe jeder Zeile.
    removeRuns(parentItem, staleRows);

    const QIcon folderIcon = IconResources::getCoreIcon(QStringLiteral("folder-sync"));
    // ein Symbol für alle Dateizeilen, nicht je Zeile beim Stil erfragt
    const QIcon fileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    const auto sizeText = [](qint64 size) { return size >= 0 ? Utility::octetsToString(size) : QString(); };

    // Neue Zeilen entstehen vollständig, bevor sie ins Modell kommen (Daten, Haken, Platzhalter): So lösen sie keine
    // Signale je Zeile aus.
    const auto newRow = [&](const ListingEntry &entry, FolderTreeItemKind kind) {
        QStandardItem *child = new QStandardItem(entry.name);
        child->setToolTip(entry.relPath);
        child->setData(static_cast<int>(kind), FolderItemRoles::ItemKindRole);
        child->setData(entry.relPath, FolderItemRoles::RemotePathRole);
        child->setData(0, FolderItemRoles::SortPriorityRole);
        if (kind == FolderTreeItemKind::BrowserFolder) {
            child->setIcon(folderIcon);
            Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if (!vfsMode)
                flags |= Qt::ItemIsUserCheckable;
            child->setFlags(flags);
            if (!vfsMode)
                child->setCheckState(stateForPath(blackList, Utility::ensureTrailingSlash(entry.relPath)));
            // give the new row its own expander; a Depth::One listing cannot tell
            // whether the subfolder has children, so assume it does until expanded
            child->appendRow(placeholderRow());
        } else {
            // file rows are informational only (selective sync stays folder-based)
            child->setIcon(fileIcon);
            child->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
        if (!vfsMode)
            setRowDetail(child, sizeText(entry.size));
        return child;
    };
    const auto updateRow = [&](QStandardItem *child, const ListingEntry &entry) {
        if (itemKind(child) == static_cast<int>(FolderTreeItemKind::BrowserFolder) && child->rowCount() == 0) {
            // the row was previously listed as empty and lost its placeholder; restore the
            // expander so content created on the server later remains discoverable
            // (a row with file children keeps its expander naturally)
            child->appendRow(placeholderRow());
        }
        if (!vfsMode)
            setRowDetail(child, sizeText(entry.size));
    };

    // Ordner und Dateien in einem Durchgang; neue Zeilen sammeln sich bis zur nächsten bleibenden Zeile
    int row = blockStart(parentItem, FolderTreeItemKind::BrowserFolder);
    qsizetype next = 0;
    QList<QStandardItem *> run;
    const auto mergeEntry = [&](const ListingEntry &entry, FolderTreeItemKind kind) {
        QStandardItem *keep = next < kept.size() ? kept.at(next) : nullptr;
        if (keep && itemKind(keep) == static_cast<int>(kind) && keep->data(FolderItemRoles::RemotePathRole).toString() == entry.relPath) {
            ++next;
            insertRun(parentItem, keep->row(), run);
            updateRow(keep, entry);
            row = keep->row() + 1;
        } else {
            run.append(newRow(entry, kind));
        }
    };
    for (const ListingEntry &entry : std::as_const(folders))
        mergeEntry(entry, FolderTreeItemKind::BrowserFolder);
    for (const ListingEntry &entry : std::as_const(files))
        mergeEntry(entry, FolderTreeItemKind::BrowserFile);

    // „… und N weitere Dateien“ unter den Dateizeilen; alle zeigt der Dateimanager
    if (hiddenFiles > 0) {
        //: Last row of a folder with more files than the folder view lists, %n is the number of files not shown
        const QString text = tr("… and %n more file(s)", nullptr, static_cast<int>(hiddenFiles));
        if (moreRow) {
            moreRow->setText(text);
        } else {
            moreRow = new QStandardItem(text);
            moreRow->setFlags(Qt::ItemIsEnabled);
            moreRow->setData(static_cast<int>(FolderTreeItemKind::BrowserMoreFiles), FolderItemRoles::ItemKindRole);
            // like the placeholder: stays below the folder and file rows when the model gets (re)sorted
            moreRow->setData(-1, FolderItemRoles::SortPriorityRole);
            run.append(moreRow);
        }
    }
    insertRun(parentItem, row, run);

    if (folders.isEmpty() && files.isEmpty()) {
        if (!placeholder) {
            // an earlier listing had content and removed the placeholder; re-attach
            // it so the row keeps its expander and can show the empty state
            parentItem->appendRow(placeholderRow());
            placeholder = placeholderChild(parentItem);
        }
        // keep it as an informational row: the expander stays (like in Explorer),
        // collapsing and re-expanding re-checks the server
        if (placeholder)
            placeholder->setText(tr("This folder is empty on the server."));
    }
}

void FolderBrowserController::setRowDetail(QStandardItem *item, const QString &detail)
{
    item->setData(detail, FolderItemRoles::DetailStringRole);

    QString accessibleFormat;
    if (itemKind(item) == static_cast<int>(FolderTreeItemKind::BrowserFile)) {
        //: Accessible (screen reader) text for a remote file row, %1 is the file name, %2 its size or availability
        accessibleFormat = tr("File %1, %2");
    } else {
        //: Accessible (screen reader) text for a remote folder row, %1 is the folder name, %2 its size or availability
        accessibleFormat = tr("Folder %1, %2");
    }
    const QString accessible = detail.isEmpty() ? item->text() : accessibleFormat.arg(item->text(), detail);
    item->setData(accessible, Qt::AccessibleTextRole);
}

QString FolderBrowserController::availabilityText(Folder *folder, const QString &relPath) const
{
    const auto availability = folder->vfs().availability(relPath);
    return availability ? Utility::enumToDisplayName(*availability) : QString();
}

Qt::CheckState FolderBrowserController::stateForPath(const QSet<QString> &blackList, const QString &pathWithSlash)
{
    for (const QString &entry : blackList) {
        // the entry blacklists this folder itself or one of its ancestors
        if (entry == QLatin1String("/") || pathWithSlash == entry || pathWithSlash.startsWith(entry))
            return Qt::Unchecked;
    }
    for (const QString &entry : blackList) {
        if (entry.startsWith(pathWithSlash))
            return Qt::PartiallyChecked;
    }
    return Qt::Checked;
}

void FolderBrowserController::onItemChanged(QStandardItem *item)
{
    if (_updatingModel || !item)
        return;
    if (itemKind(item) != static_cast<int>(FolderTreeItemKind::BrowserFolder) || !(item->flags() & Qt::ItemIsUserCheckable))
        return;

    propagateCheckChange(item);

    // only the edited folder's pending list is recomputed from its rows; the unloaded-subtree
    // fallback uses the previous pending list because that is what seeded the check states
    FolderItem *rootItem = rootItemForIndex(item->index());
    if (rootItem && rootItem->folder()) {
        auto it = _syncStates.find(folderKey(rootItem->folder()));
        if (it != _syncStates.end())
            it->pendingBlackList = computeBlackList(rootItem, it->pendingBlackList);
    }
    recomputePending();
}

void FolderBrowserController::propagateCheckChange(QStandardItem *item)
{
    QScopedValueRollback<bool> guard(_updatingModel, true);

    // downwards: a decisive state wins over whatever the loaded descendants had
    const Qt::CheckState state = item->checkState();
    if (state == Qt::Checked || state == Qt::Unchecked) {
        std::function<void(QStandardItem *)> setDescendants = [&](QStandardItem *parent) {
            for (QStandardItem *child : browserChildren(parent)) {
                if (child->checkState() != state)
                    child->setCheckState(state);
                setDescendants(child);
            }
        };
        setDescendants(item);
    }

    // upwards: a parent is fully checked only if all children are; it never becomes
    // unchecked automatically because the parent folder itself stays synced
    QStandardItem *parent = item->parent();
    while (parent && itemKind(parent) == static_cast<int>(FolderTreeItemKind::BrowserFolder) && (parent->flags() & Qt::ItemIsUserCheckable)) {
        bool allChecked = true;
        for (QStandardItem *child : browserChildren(parent)) {
            if (child->checkState() != Qt::Checked) {
                allChecked = false;
                break;
            }
        }
        const Qt::CheckState parentState = allChecked ? Qt::Checked : Qt::PartiallyChecked;
        if (parent->checkState() != parentState)
            parent->setCheckState(parentState);
        parent = parent->parent();
    }
}

QSet<QString> FolderBrowserController::computeBlackList(const QStandardItem *parentItem, const QSet<QString> &previousBlackList) const
{
    QSet<QString> result;
    for (QStandardItem *child : browserChildren(parentItem)) {
        const QString path = Utility::ensureTrailingSlash(child->data(FolderItemRoles::RemotePathRole).toString());
        switch (child->checkState()) {
        case Qt::Unchecked:
            result.insert(path);
            break;
        case Qt::Checked:
            break;
        case Qt::PartiallyChecked: {
            if (!browserChildren(child).isEmpty()) {
                result += computeBlackList(child, previousBlackList);
            } else {
                // subtree not loaded from the server; the previous list (which seeded
                // the check states) stays authoritative below this folder
                for (const QString &entry : previousBlackList) {
                    if (entry.startsWith(path))
                        result.insert(entry);
                }
            }
            break;
        }
        }
    }
    return result;
}

void FolderBrowserController::recomputePending()
{
    // the pending lists themselves are maintained in onItemChanged; this only decides
    // whether the apply bar shows, and drops edits the user can no longer see or change
    bool anyDirty = false;
    for (int row = 0; row < _model->rowCount(); ++row) {
        FolderItem *rootItem = dynamic_cast<FolderItem *>(_model->item(row, 0));
        if (!rootItem || !rootItem->folder())
            continue;
        Folder *folder = rootItem->folder();
        auto it = _syncStates.find(folderKey(folder));
        if (it == _syncStates.end())
            continue;
        if (folder->virtualFilesEnabled() || browserChildren(rootItem).isEmpty()) {
            // the rows are gone (folder list rebuild) or the folder switched to virtual
            // files: the edits have no visible representation anymore, discard them
            // instead of silently committing them on the next apply
            it->pendingBlackList = it->journalBlackList;
            continue;
        }
        if (it->pendingBlackList != it->journalBlackList)
            anyDirty = true;
    }
    emit selectiveSyncPendingChanged(anyDirty);
}

void FolderBrowserController::applyPendingChanges()
{
    for (int row = 0; row < _model->rowCount(); ++row) {
        FolderItem *rootItem = dynamic_cast<FolderItem *>(_model->item(row, 0));
        if (!rootItem || !rootItem->folder())
            continue;
        Folder *folder = rootItem->folder();
        auto it = _syncStates.find(folderKey(folder));
        if (it == _syncStates.end() || it->pendingBlackList == it->journalBlackList)
            continue;
        if (folder->virtualFilesEnabled()) {
            // selective sync does not apply under virtual files; stale edits from before
            // a mode switch must never reach the journal
            it->pendingBlackList = it->journalBlackList;
            continue;
        }

        // the journal can have changed underneath us (the "Manage subfolder sync" modal,
        // remote renames of blacklisted folders, ...). Never write our snapshot wholesale:
        // re-read the journal and apply only the user's delta on top of it.
        bool ok = false;
        QSet<QString> merged = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, ok);
        if (!ok) {
            qCWarning(lcFolderBrowser) << "Could not read selective sync list for" << folder->path() << "- not applying changes";
            continue;
        }
        const QSet<QString> added = it->pendingBlackList - it->journalBlackList;
        const QSet<QString> removed = it->journalBlackList - it->pendingBlackList;
        for (const QString &entry : removed)
            merged.remove(entry);
        for (const QString &entry : added) {
            // entries below a newly blacklisted folder are redundant
            for (auto m = merged.begin(); m != merged.end();) {
                if (m->startsWith(entry))
                    m = merged.erase(m);
                else
                    ++m;
            }
        }
        for (const QString &entry : added) {
            bool coveredByAncestor = false;
            for (const QString &m : std::as_const(merged)) {
                if (entry != m && entry.startsWith(m)) {
                    coveredByAncestor = true;
                    break;
                }
            }
            if (!coveredByAncestor)
                merged.insert(entry);
        }

        qCInfo(lcFolderBrowser) << "Applying selective sync changes for" << folder->path() << "- excluding" << added << "- re-including" << removed;
        folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, merged);
        // re-included folders were never stored in the journal; force their parents
        // to be re-discovered remotely, otherwise they stay invisible until an
        // unrelated remote change happens to bump the parent etag
        for (const QString &entry : removed) {
            folder->journalDb()->schedulePathForRemoteDiscovery(entry);
        }
        it->journalBlackList = merged;
        it->pendingBlackList = merged;
        // reflect entries that were changed externally in the loaded rows too
        refreshCheckStates(rootItem, merged);
        FolderMan::instance()->forceFolderSync(folder);
    }
    // not a plain 'false': a folder whose journal read failed above keeps its pending
    // edits, and the apply bar must keep showing them
    recomputePending();
}

void FolderBrowserController::discardPendingChanges()
{
    for (int row = 0; row < _model->rowCount(); ++row) {
        FolderItem *rootItem = dynamic_cast<FolderItem *>(_model->item(row, 0));
        if (!rootItem || !rootItem->folder())
            continue;
        auto it = _syncStates.find(folderKey(rootItem->folder()));
        if (it == _syncStates.end())
            continue;
        if (it->pendingBlackList != it->journalBlackList) {
            it->pendingBlackList = it->journalBlackList;
            refreshCheckStates(rootItem, it->journalBlackList);
        }
    }
    emit selectiveSyncPendingChanged(false);
}

void FolderBrowserController::refreshCheckStates(QStandardItem *parentItem, const QSet<QString> &blackList)
{
    QScopedValueRollback<bool> guard(_updatingModel, true);
    std::function<void(QStandardItem *)> refresh = [&](QStandardItem *parent) {
        for (QStandardItem *child : browserChildren(parent)) {
            if (child->flags() & Qt::ItemIsUserCheckable) {
                const Qt::CheckState state = stateForPath(blackList, Utility::ensureTrailingSlash(child->data(FolderItemRoles::RemotePathRole).toString()));
                if (child->checkState() != state)
                    child->setCheckState(state);
            }
            refresh(child);
        }
    };
    refresh(parentItem);
}

void FolderBrowserController::refreshAvailability(Folder *folder, QStandardItem *item, bool recursive)
{
    {
        QScopedValueRollback<bool> guard(_updatingModel, true);
        setRowDetail(item, availabilityText(folder, item->data(FolderItemRoles::RemotePathRole).toString()));
    }
    if (!recursive)
        return;
    // die geladenen Zeilen darunter wie nach dem Aufklappen in Zeitscheiben
    QList<QStandardItem *> rows;
    QList<const QStandardItem *> parents{item};
    while (!parents.isEmpty()) {
        const QStandardItem *parent = parents.takeLast();
        const QList<QStandardItem *> subfolders = browserChildren(parent);
        rows += subfolders;
        rows += childrenOfKind(parent, FolderTreeItemKind::BrowserFile);
        for (QStandardItem *subfolder : subfolders)
            parents.append(subfolder);
    }
    queueAvailability(rows);
}

void FolderBrowserController::queueAvailability(const QList<QStandardItem *> &items)
{
    for (QStandardItem *item : items)
        _pendingAvailability.append(QPersistentModelIndex(item->index()));
    processPendingAvailability();
}

void FolderBrowserController::processPendingAvailability()
{
    // Je Durchgang höchstens so lange, dann ist die Ereignisschleife wieder dran: Jede Zeile fragt das Dateisystem, ein ganz
    // angehefteter Ordner geht durch seinen Inhalt. Kleine Ordner sind im ersten Durchgang fertig.
    constexpr qint64 timeSliceMs = 30;
    QElapsedTimer timer;
    timer.start();
    {
        QScopedValueRollback<bool> guard(_updatingModel, true);
        while (!_pendingAvailability.isEmpty()) {
            const QPersistentModelIndex index = _pendingAvailability.takeFirst();
            QStandardItem *item = index.isValid() ? _model->itemFromIndex(index) : nullptr;
            FolderItem *rootItem = item ? rootItemForIndex(index) : nullptr;
            Folder *folder = rootItem ? rootItem->folder() : nullptr;
            // virtuelle Dateien inzwischen aus: die Zeilen entstehen beim nächsten Aufklappen neu
            if (folder && folder->virtualFilesEnabled())
                setRowDetail(item, availabilityText(folder, item->data(FolderItemRoles::RemotePathRole).toString()));
            if (timer.elapsed() >= timeSliceMs)
                break;
        }
    }
    if (!_pendingAvailability.isEmpty() && !_availabilityScheduled) {
        _availabilityScheduled = true;
        QTimer::singleShot(0, this, [this] {
            _availabilityScheduled = false;
            processPendingAvailability();
        });
    }
}

void FolderBrowserController::popAvailabilityMenu(const QModelIndex &index, const QPoint &globalPos, QWidget *menuParent)
{
    FolderItem *rootItem = rootItemForIndex(index);
    if (!rootItem || !rootItem->folder())
        return;
    const bool vfs = rootItem->folder()->virtualFilesEnabled();
    // owncloud.online: Teilen geht auch ohne virtuelle Dateien
    const bool canShare = rootItem->folder()->accountState()->account()->capabilities().shareAPI();
    if (!vfs && !canShare)
        return;

    QStandardItem *item = _model->itemFromIndex(index.siblingAtColumn(0));
    const auto isPinnable = [](const QStandardItem *i) {
        const int kind = itemKind(i);
        return kind == static_cast<int>(FolderTreeItemKind::BrowserFolder)
            || kind == static_cast<int>(FolderTreeItemKind::BrowserFile);
    };
    if (!item || !isPinnable(item))
        return;

    // menu.exec() spins a nested event loop: the folder can be removed and the row can be
    // pruned by an in-flight listing while the menu is open, so guard and re-resolve after
    QPointer<Folder> folder(rootItem->folder());
    const QPersistentModelIndex persistentIndex(index.siblingAtColumn(0));

    const QString relPathAtOpen = item->data(FolderItemRoles::RemotePathRole).toString();

    QMenu menu(menuParent);
    QAction *keepLocal = nullptr;
    QAction *freeUp = nullptr;
    QAction *reset = nullptr;
    if (vfs) {
        keepLocal = menu.addAction(tr("Always keep on this device"));
        freeUp = menu.addAction(tr("Free up space (online only)"));
        menu.addSeparator();
        reset = menu.addAction(tr("Reset to default"));
    }
    QAction *share = nullptr;
    if (canShare) {
        if (vfs)
            menu.addSeparator();
        share = menu.addAction(tr("Share…"));
        share->setEnabled(rootItem->folder()->isConnected());
    }

    QAction *chosen = menu.exec(globalPos);
    if (!chosen)
        return;
    if (chosen == share) {
        if (folder) {
            if (auto *app = ocAppOrNull())
                app->tray()->slotShowShareDialog(folder->remotePathTrailingSlash() + relPathAtOpen, folder->path() + relPathAtOpen);
        }
        return;
    }
    PinState state;
    if (chosen == keepLocal)
        state = PinState::AlwaysLocal;
    else if (chosen == freeUp)
        state = PinState::OnlineOnly;
    else if (chosen == reset)
        state = PinState::Inherited;
    else
        return;

    if (!folder || !folder->virtualFilesEnabled() || !persistentIndex.isValid())
        return;
    item = _model->itemFromIndex(QModelIndex(persistentIndex));
    if (!item || !isPinnable(item))
        return;

    const QString relPath = item->data(FolderItemRoles::RemotePathRole).toString();
    if (!folder->changeAvailability(relPath, state)) {
        qCWarning(lcFolderBrowser) << "Failed to set pin state" << static_cast<int>(state) << "for" << relPath;
        return;
    }
    // den Stand nach dem Laden und Freigeben tragen onItemCompleted() und refreshChangedRows() nach
    refreshAvailability(folder, item, true);
#ifdef Q_OS_MACOS
    // macOS lädt und entfernt im Hintergrund: den Stand kurz darauf noch einmal zeigen
    for (const int delay : {3000, 15000}) {
        QTimer::singleShot(delay, this, [this, folder, persistentIndex] {
            if (!folder || !persistentIndex.isValid()) {
                return;
            }
            if (auto *row = _model->itemFromIndex(QModelIndex(persistentIndex))) {
                refreshAvailability(folder, row, true);
            }
        });
    }
#endif
}
}
