/*
 * Copyright (C) by Hannah von Reth <hannah.vonreth@owncloud.com>
 * Copyright (C) by Duncan Mac-Vicar P. <duncan@kde.org>
 * Copyright (C) by BW-Tech GmbH for owncloud.online.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */
#pragma once

#include "gui/folderwizard/folderwizard_p.h"

#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QUuid>

class QTreeWidgetItem;
class Ui_FolderWizardTargetPage;

namespace OCC {

class Account;
class PropfindJob;

/**
 * @brief Seite „Serverordner“ des Ordner-Assistenten für Konten ohne Spaces (ownCloud 10/owncloud.online)
 * @ingroup gui
 *
 * Zeigt den Ordnerbaum des Kontos (PROPFIND mit Tiefe 1, Unterordner werden beim Aufklappen nachgeladen) und legt
 * neue Ordner per MKCOL an. Weiter geht es nur mit einem Serverordner, der weder selbst noch als Ober- oder
 * Unterordner schon von einem anderen Ordnerpaar des Kontos gespiegelt wird (FolderMan::checkRemotePathValidity()).
 */
class FolderWizardRemotePath : public FolderWizardPage
{
    Q_OBJECT
public:
    FolderWizardRemotePath(FolderWizardPrivate *parent, Account *account, const QUrl &davUrl);
    ~FolderWizardRemotePath() override;

    bool isComplete() const override;
    void initializePage() override;
    void cleanupPage() override;

    /// der gewählte Serverordner mit führendem „/“, „/“ steht für das ganze Konto; leer, solange nichts gewählt ist
    QString targetPath() const;

    /// wählt den Ordner @p path aus, sofern er schon im Baum steht
    bool selectByPath(const QString &path);

    /**
     * Legt unter dem gewählten Ordner (ohne Auswahl unter der Wurzel) den Ordner @p name per MKCOL an und wählt ihn
     * danach aus. Ungültige Namen werden mit einer Meldung abgelehnt.
     */
    void createRemoteFolder(const QString &name);

    /// Fehler zu einem neuen Ordnernamen, oder leer, wenn der Name taugt
    static QString checkNewFolderName(const QString &name);

    /**
     * Vorschlag für den lokalen Ordner zu @p remotePath: Das erste Ordnerpaar liegt im Sync-Ordner des Kontos selbst, jedes
     * weitere daneben und trägt den Namen des Serverordners („owncloud.online - Dokumente“), damit kein Paar im anderen liegt.
     */
    static QString suggestLocalPath(const QString &defaultSyncRoot, const QString &remotePath, const QUuid &accountUuid);

    /// Name des Ordnerpaars in der Ordnerliste: der Name des Serverordners, leer für die Wurzel
    static QString displayNameForRemotePath(const QString &remotePath);

Q_SIGNALS:
    /// eine Ordnerliste ist eingetragen oder ihr Abruf gescheitert
    void listingFinished();
    /// MKCOL ist fertig; bei Erfolg ist der neue Ordner gewählt
    void remoteFolderCreated(bool success);

private:
    void askForNewFolderName();
    void refreshFolders();
    PropfindJob *runPropFindJob(const QString &path);
    void updateDirectories(const QString &listedPath, const QStringList &hrefs);
    void handleListingError(PropfindJob *job);
    void onItemExpanded(QTreeWidgetItem *item);
    void onCurrentItemChanged(QTreeWidgetItem *item);
    void onFolderEntryEdited(const QString &text);
    void listTypedFolder();
    QTreeWidgetItem *rootItem();
    void insertPath(QTreeWidgetItem *parent, const QStringList &pathTrail);
    QTreeWidgetItem *itemForPath(const QString &path) const;
    static QString pathForItem(const QTreeWidgetItem *item);
    void setInfo(const QString &message);
    void updateMessage() const;

    Ui_FolderWizardTargetPage *_ui;
    QPointer<Account> _account;
    QUrl _davUrl;
    QTimer _lscolTimer;
    // Hinweis zum letzten Vorgang (Ordner angelegt, Liste nicht ladbar)
    QString _info;
    // warum der gewählte Ordner nicht geht; isComplete() ist const, setzt ihn aber
    mutable QString _validityError;
};

}
