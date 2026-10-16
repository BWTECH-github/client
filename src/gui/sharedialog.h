/*
 * Copyright (C) by BW-Tech GmbH for owncloud.online.
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

#include "shareapi.h"

#include <QDialog>
#include <QPointer>
#include <QTimer>

class QCheckBox;
class QDateEdit;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPushButton;
class QRadioButton;
class QTabWidget;
class QVBoxLayout;

namespace OCC {

class Account;
class AccountState;

/**
 * @brief Nativer Teilen-Dialog für oC10/owncloud.online (OCS-Share-API v1).
 *
 * Reiter "Nutzer und Gruppen": Sharee-Suche, Freigabe anlegen, Rechte
 * (lesen/bearbeiten/anlegen/löschen/teilen) ändern, Freigabe entfernen.
 * Reiter "Öffentliche Links": mehrere Links mit Name, Rolle, Passwort und
 * Ablaufdatum anlegen, bearbeiten, kopieren, löschen; die Richtlinien des
 * Servers (erzwungenes Passwort/Ablaufdatum) stehen schon im Formular.
 * Geöffnet über das Explorer-Kontextmenü und aus dem Client.
 *
 * @ingroup gui
 */
class ShareDialog : public QDialog
{
    Q_OBJECT
public:
    /// remotePath is the server-relative path of the item ("/Documents/report.odt")
    ShareDialog(AccountState *accountState, const QUrl &davUrl, const QString &remotePath, const QString &localPath,
        ShareDialogStartPage startPage = ShareDialogStartPage::UsersAndGroups, QWidget *parent = nullptr);

    /// Meldung unter den Reitern, z.B. warum der Dialog statt einer direkten Aktion erscheint
    void showError(const QString &message);

private:
    // die eine laufende Schreibanfrage (Anlegen/Ändern/Löschen); der Dialog ist währenddessen gesperrt
    enum class Pending { None, CreateUserShare, CreateLink, UpdateUserShare, UpdateLink, DeleteUserShare, DeleteLink };

    Account *account() const;
    const Share *findShare(const QString &shareId) const;
    QWidget *buildUserTab();
    QWidget *buildLinkTab();

    void onItemInfo(bool isFolder, SharePermissions maxPermissions);
    void onSharesFetched(const QList<Share> &shares);
    void onShareCreated(const Share &share);
    void onShareChanged();
    void onShareesFetched(const QString &search, const QList<Sharee> &sharees);
    void onRequestFailed(ShareApi::Operation operation, const QString &serverMessage, const QString &shareId);

    void reloadShares();
    void searchSharees();
    void onShareeChosen(QListWidgetItem *item);
    void hideShareeResults();
    void rebuildUserShares();
    void rebuildLinkShares();
    void changeUserSharePermission(const Share &share, SharePermission permission, bool enabled);
    void confirmAndDelete(const Share &share);

    void startEditingLink(const Share &share);
    void resetLinkEditor();
    void updateLinkPolicy();
    void saveLink();
    SharePermissions selectedLinkPermissions() const;
    QString linkRoleText(SharePermissions permissions) const;
    QString shareeText(int type, const QString &displayName, const QString &additionalInfo = {}) const;

    void setBusy(bool busy);
    void showStatus(const QString &message, bool isError);
    void copyToClipboard(const QString &url);
    /// Öffnen, Kopieren, Direkt-Download kopieren, per E-Mail senden
    QMenu *createLinkMenu(const Share &share, QWidget *parent);

    QPointer<AccountState> _accountState;
    ShareApi *_api = nullptr;
    QString _remotePath;
    QString _itemName;
    bool _isFolder = false;
    SharePermissions _maxPermissions = allSharePermissions();
    QList<Share> _shares;
    QList<Sharee> _sharees;
    bool _sharesLoaded = false;
    bool _busy = false;
    Pending _pending = Pending::None;
    QString _pendingShareeText;
    QString _editingLinkId;
    QTimer _searchTimer;

    QTabWidget *_tabs = nullptr;
    QLabel *_statusLabel = nullptr;

    // Nutzer und Gruppen
    QLineEdit *_shareeSearch = nullptr;
    QListWidget *_shareeResults = nullptr;
    QLabel *_shareeHint = nullptr;
    QVBoxLayout *_userSharesLayout = nullptr;

    // öffentliche Links
    QVBoxLayout *_linkSharesLayout = nullptr;
    QGroupBox *_linkEditor = nullptr;
    QLineEdit *_linkName = nullptr;
    QWidget *_linkRoleBox = nullptr;
    QRadioButton *_roleReadOnly = nullptr;
    QRadioButton *_roleUpload = nullptr;
    QRadioButton *_roleEdit = nullptr;
    QRadioButton *_roleUploadOnly = nullptr;
    QCheckBox *_linkPasswordCheck = nullptr;
    QLineEdit *_linkPassword = nullptr;
    QCheckBox *_linkExpireCheck = nullptr;
    QDateEdit *_linkExpireDate = nullptr;
    QLabel *_linkPolicyHint = nullptr;
    QPushButton *_linkSaveButton = nullptr;
    QPushButton *_linkCancelButton = nullptr;
};

} // namespace OCC
