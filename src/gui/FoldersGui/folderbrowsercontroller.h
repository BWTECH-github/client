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

#pragma once

#include "csync_exclude.h"
#include "gui/owncloudguilib.h"
#include "libsync/syncfileitem.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPersistentModelIndex>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUuid>

class QModelIndex;
class QPoint;
class QStandardItem;
class QStandardItemModel;
class QWidget;

namespace OCC {

class AccountState;
class Folder;
enum class FolderTreeItemKind;
class FolderItem;

/**
 * @brief Lets the user browse the remote directory structure directly in the folder list.
 *
 * Every folder row in the AccountFoldersView gets an expander; expanding a row runs a
 * Depth::One PROPFIND and inserts the subfolders as child rows (name + size), which are
 * themselves expandable, so the whole remote tree can be explored inside the client -
 * like the folder tree the classic 2.x client showed inline in its settings page.
 *
 * For folders synced without virtual files the rows carry selective-sync checkboxes.
 * Changes are collected as a pending blacklist per folder and only written to the sync
 * journal (and a sync forced) when the user confirms via the apply bar in the view;
 * discarding restores the journal state. For virtual-files folders the rows show the
 * current availability instead, and a context menu sets the pin state (always keep on
 * this device / free up space), mirroring the Explorer integration.
 */
class OWNCLOUDGUI_EXPORT FolderBrowserController : public QObject
{
    Q_OBJECT
public:
    explicit FolderBrowserController(QStandardItemModel *model, QObject *parent);

    // ein direkter Eintrag eines Serverordners
    struct ListingEntry
    {
        QString name;
        QString relPath;
        qint64 size = -1;
    };

    // So viele Dateizeilen zeigt ein Ordner höchstens, den Rest fasst die Zeile „… und N weitere Dateien“ zusammen.
    // Ordnerzeilen werden nie abgeschnitten, an ihnen hängt die Auswahl der selektiven Synchronisierung.
    static constexpr int maxFileRows = 500;

    /**
     * Bringt die Zeilen unter parentItem auf den Stand einer Serverauflistung: Ordner vor Dateien, beide nach Namen
     * sortiert. Vorhandene Zeilen bleiben samt aufgeklapptem Inhalt und Haken, neue kommen in wenigen Stücken hinzu
     * statt einzeln, Zeilen ohne Eintrag verschwinden. Ohne Bezug zum Sync-Ordner, damit Tests große Auflistungen
     * direkt einspeisen können; die Verfügbarkeit virtueller Dateien trägt der Controller danach nach.
     */
    static void applyListing(QStandardItem *parentItem, QList<ListingEntry> folders, QList<ListingEntry> files, bool vfsMode, const QSet<QString> &blackList);

public slots:
    void onItemExpanded(const QModelIndex &index);
    void applyPendingChanges();
    void discardPendingChanges();
    void popAvailabilityMenu(const QModelIndex &index, const QPoint &globalPos, QWidget *menuParent);

signals:
    void selectiveSyncPendingChanged(bool pending);

private slots:
    void onRowsInserted(const QModelIndex &parent, int first, int last);
    void onItemChanged(QStandardItem *item);
    void onFolderRemoved(const QUuid &accountId, OCC::Folder *folder);
    void onItemCompleted(OCC::Folder *folder, const OCC::SyncFileItemPtr &item);
    void onFilesTouched(OCC::Folder *folder, const QSet<QString> &relativePaths);
    void onFilesTouchedBelow(OCC::Folder *folder, const QString &relativeDir);

private:
    struct FolderSyncState
    {
        QSet<QString> journalBlackList;
        QSet<QString> pendingBlackList;
        bool vfsAtLoad = false;
    };

    void ensureSyncState(Folder *folder);
    /// returns true when the folder's browser subtree was rebuilt (VFS mode changed)
    bool maybeRefreshFolderState(Folder *folder, FolderItem *rootItem);
    void attachPlaceholder(QStandardItem *parentItem);
    void requestListing(Folder *folder, const QString &relPath);
    // Abfrage vormerken, bis das Konto verbunden ist, und danach nachholen
    void deferListing(Folder *folder, const QString &relPath);
    void runDeferredListings(AccountState *accountState);
    void populateListing(Folder *folder, const QString &parentRelPath, const QStringList &subfolders, const QStringList &files, const QHash<QString, qint64> &sizes);
    void onListingError(Folder *folder, const QString &parentRelPath, bool notFound);

    FolderItem *rootItemForFolder(Folder *folder) const;
    FolderItem *rootItemForIndex(const QModelIndex &index) const;
    QStandardItem *itemForPath(FolderItem *rootItem, const QString &relPath) const;
    static QStandardItem *placeholderChild(const QStandardItem *parentItem);
    static QList<QStandardItem *> placeholderRow();

    static Qt::CheckState stateForPath(const QSet<QString> &blackList, const QString &pathWithSlash);
    void propagateCheckChange(QStandardItem *item);
    QSet<QString> computeBlackList(const QStandardItem *parentItem, const QSet<QString> &previousBlackList) const;
    void recomputePending();
    void refreshCheckStates(QStandardItem *parentItem, const QSet<QString> &blackList);
    void refreshAvailability(Folder *folder, QStandardItem *item, bool recursive);
    QString availabilityText(Folder *folder, const QString &relPath) const;
    static void setRowDetail(QStandardItem *item, const QString &detail);
    void queueAvailability(const QList<QStandardItem *> &items);
    void processPendingAvailability();
    void noteChangedPath(Folder *folder, QString relativePath);
    void refreshChangedRows();

    static int itemKind(const QStandardItem *item);
    static QList<QStandardItem *> browserChildren(const QStandardItem *parentItem);
    static QList<QStandardItem *> childrenOfKind(const QStandardItem *parentItem, FolderTreeItemKind kind);
    static QString folderKey(const Folder *folder);

    QStandardItemModel *_model = nullptr;
    // keyed by folderKey(); tracks the journal blacklist and the (possibly dirty) pending edits
    QHash<QString, FolderSyncState> _syncStates;
    // folderKey() + '\x1f' + relPath of PROPFINDs in flight, to avoid duplicate jobs on re-expand
    QSet<QString> _pendingJobs;
    // folderKey() -> relPaths, die aufgeklappt wurden, bevor das Konto verbunden war (beim Start liest der Client das
    // Kennwort erst aus dem Schlüsselbund; eine frühere Abfrage ginge ohne Anmeldung hinaus)
    QHash<QString, QSet<QString>> _deferredListings;
    // Konten, deren Zustandswechsel schon beobachtet werden
    QSet<AccountState *> _watchedAccountStates;
    // true while this controller mutates the model, so onItemChanged only reacts to the user
    bool _updatingModel = false;
    ExcludedFiles _excludedFiles;
    // Zeilen, deren Verfügbarkeit (virtuelle Dateien) noch aussteht; abgearbeitet in Zeitscheiben
    QList<QPersistentModelIndex> _pendingAvailability;
    bool _availabilityScheduled = false;
    // Was sich seit der letzten Aktualisierung geändert hat, samt Elternordnern: Die Wahl setzt nur den Pin-Zustand, laden und
    // freigeben tut erst der Sync. Der Zeiger dient nur als Schlüssel, der Ordner kann längst weg sein.
    QHash<Folder *, QSet<QString>> _changedPaths;
    QTimer _changedPathsTimer;
};
}
