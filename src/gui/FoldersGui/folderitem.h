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

#pragma once

#include "QStandardItem"

// #include "foldererroritem.h"
#include "folder.h"
#include "progressdispatcher.h"

#include <QList>
#include <QPointer>

#include <chrono>

// #include "folderstatusupdater.h"

namespace OCC {

class FolderItemUpdater;

enum FolderItemRoles { DataRole = Qt::UserRole, StatusIconRole, StatusStringRole, StatusInfoRole, SortPriorityRole, ItemKindRole, RemotePathRole, DetailStringRole,
    // Prozent (0-100) für den Fortschrittsbalken der Ordnerzeile, nur solange übertragen wird; sonst ungültig
    ProgressRole };

// Discriminates the child rows below a FolderItem. Error rows predate the kind role, so
// Error doubles as the value an untagged row reports (an unset role decodes to 0).
// BrowserMoreFiles: letzte Zeile eines Ordners mit mehr Dateien, als die Ansicht zeigt („… und N weitere Dateien“).
enum class FolderTreeItemKind { Error = 0, BrowserFolder = 1, BrowserPlaceholder = 2, BrowserFile = 3, BrowserMoreFiles = 4 };

class FolderItem : public QStandardItem
{
    Q_DECLARE_TR_FUNCTIONS(FolderItem)

public:
    FolderItem(Folder *folder);
    ~FolderItem() override;

    QVariant data(int role) const override;

    void refresh();
    void setProgress(const ProgressInfo &progress);
    /**
     * Übertragungen der Plattform statt der Sync-Engine (macOS: die Erweiterung des Dateianbieters lädt hoch oder herunter,
     * die Engine steht still). Solange sie laufen, zeigt die Zeile Fortschritt, Raten, Sync-Symbol und Balken wie bei einem
     * Lauf; Gesamtzahlen 0 in beiden Richtungen beenden die Anzeige. Die Raten kommen aus den fortlaufend gezählten
     * bewegten Bytes, nicht aus dem Fortschritt der Runde; eine Restzeit nur, wenn die Runde aus einer Operation besteht.
     */
    void setTransferProgress(const VfsTransferProgress &progress);
    void updateImage();

    Folder *folder();

private:
    QPointer<Folder> _folder;
    FolderItemUpdater *_updater = nullptr;

    quint64 _totalSize = 0;
    quint64 _completedSize = 0;
    quint64 _estimatedUpBw = 0;
    quint64 _estimatedDownBw = 0;
    quint64 _estimatedEta = 0; //!< estimated remaining time in ms
    int _percentComplete = 0;
    // die Plattform überträgt gerade (setTransferProgress()); die Zahlen stehen in _totalSize/_completedSize
    bool _platformTransfer = false;
    // letzter Stand der bewegten Bytes für die Raten der Plattform-Übertragung
    qint64 _lastSent = 0;
    qint64 _lastReceived = 0;
    std::chrono::steady_clock::time_point _lastTransferSample;

    QString _statusString;
    QIcon _image;
    // ProgressInfo _progress;
    //  void updateProgress(OCC::Folder *folder, const OCC::ProgressInfo &progress);

    QString statusIconName() const;
    QString statusAsString() const;
    // „Synchronisiere 120 MB von 1,1 GB (11 %, …)“ aus den Zahlen des laufenden Laufs oder der Übertragung der Plattform
    QString progressString() const;
    // die Übertragung der Plattform bestimmt Symbol, Text und Balken (nicht bei Fehler oder nicht verfügbarem Ordner)
    bool showsPlatformTransfer() const;
    void updateStatusString();
};
}
