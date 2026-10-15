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

#include "gui/owncloudguilib.h"
#include "libsync/networkjobs.h"

#include <QDate>
#include <QFlags>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include <functional>

namespace OCC {

class Account;
class Capabilities;
class JsonApiJob;

/// Freigabe-Rechte; die Bitwerte sind die der OCS-Share-API (oC10)
enum class SharePermission {
    Read = 1,
    Update = 2,
    Create = 4,
    Delete = 8,
    Share = 16,
};
Q_DECLARE_FLAGS(SharePermissions, SharePermission)
Q_DECLARE_OPERATORS_FOR_FLAGS(SharePermissions)

/// alle Rechte, die eine Freigabe überhaupt tragen kann
constexpr SharePermissions allSharePermissions()
{
    return SharePermission::Read | SharePermission::Update | SharePermission::Create | SharePermission::Delete | SharePermission::Share;
}

/// eine bestehende Freigabe, wie sie die OCS-Share-API liefert
struct OWNCLOUDGUI_EXPORT Share
{
    enum Type { User = 0, Group = 1, Link = 3, Remote = 6 };

    QString id;
    int type = User;
    SharePermissions permissions;
    /// Nutzer-/Gruppen-ID bzw. föderierte Adresse (bei Links der Passwort-Hash)
    QString shareWith;
    QString shareWithDisplayName;
    /// nur Links
    QString name;
    QString url;
    bool hasPassword = false;
    QDate expireDate;
    bool isFolder = false;

    bool isLink() const { return type == Link; }
};

/// ein Suchtreffer der Sharee-Suche (Nutzer, Gruppe, föderierter Nutzer)
struct OWNCLOUDGUI_EXPORT Sharee
{
    int type = Share::User;
    QString shareWith;
    QString displayName;
    QString additionalInfo;
};

/// gewünschter Zustand eines öffentlichen Links beim Anlegen oder Bearbeiten
struct OWNCLOUDGUI_EXPORT LinkSettings
{
    QString name;
    SharePermissions permissions = SharePermission::Read;
    bool passwordProtected = false;
    /// leer bei passwordProtected: vorhandenes Passwort bleibt
    QString password;
    /// ungültig: kein Ablaufdatum
    QDate expireDate;
};

/**
 * @brief Zugriff auf die OCS-Share-API v1 (oC10/owncloud.online) für den Teilen-Dialog.
 *
 * Bewusst v1: v2.php spiegelt OCS-Fehler in den HTTP-Status, dann parst JsonApiJob
 * die Antwort nicht und die Meldung des Servers (z.B. eine Passwort-Richtlinie)
 * erreicht den Nutzer nie. v1 antwortet immer mit HTTP 200 und statuscode/message.
 *
 * @ingroup gui
 */
class OWNCLOUDGUI_EXPORT ShareApi : public QObject
{
    Q_OBJECT
public:
    enum class Operation { ItemInfo, Fetch, Create, Update, Delete, Search };
    Q_ENUM(Operation)

    explicit ShareApi(Account *account, QObject *parent = nullptr);

    /// Art (Datei/Ordner) und höchste weitergebbare Rechte per PROPFIND (share-permissions)
    void fetchItemInfo(const QUrl &davUrl, const QString &path);
    void fetchShares(const QString &path);
    void createLinkShare(const QString &path, const LinkSettings &settings);
    void createShare(const QString &path, const Sharee &sharee, SharePermissions permissions);
    void updateShare(const QString &shareId, const SimpleNetworkJob::UrlQuery &changes);
    void deleteShare(const QString &shareId);
    void searchSharees(const QString &search, bool isFolder);

    static Share parseShare(const QJsonObject &data);
    static QList<Share> parseShares(const QJsonObject &ocsDocument);
    static QList<Sharee> parseSharees(const QJsonObject &ocsDocument);

    static SimpleNetworkJob::UrlQuery linkCreateArguments(const QString &path, const LinkSettings &settings);
    /// nur die geänderten Felder; leer, wenn nichts zu tun ist
    static SimpleNetworkJob::UrlQuery linkUpdateArguments(const Share &current, const LinkSettings &wanted);

    /// Dateien kennen kein Anlegen/Löschen; mehr als die eigenen Rechte lässt der Server nicht zu
    static SharePermissions maskForItem(SharePermissions permissions, bool isFolder, SharePermissions maxPermissions);
    static SharePermissions defaultUserSharePermissions(const Capabilities &capabilities, bool isFolder, SharePermissions maxPermissions);
    static bool linkPasswordEnforced(const Capabilities &capabilities, SharePermissions linkPermissions);

Q_SIGNALS:
    void itemInfoFetched(bool isFolder, OCC::SharePermissions maxPermissions);
    void sharesFetched(const QList<OCC::Share> &shares);
    void shareCreated(const OCC::Share &share);
    void shareUpdated(const OCC::Share &share);
    void shareDeleted(const QString &shareId);
    /// search: der Suchbegriff der Anfrage, damit veraltete Antworten verworfen werden können
    void shareesFetched(const QString &search, const QList<OCC::Sharee> &sharees);
    /// serverMessage: Meldung des Servers bzw. Netzfehler, kann leer sein
    void requestFailed(OCC::ShareApi::Operation operation, const QString &serverMessage, const QString &shareId);

private:
    JsonApiJob *startJob(Operation operation, const QString &path, const QByteArray &verb, const SimpleNetworkJob::UrlQuery &arguments,
        const QString &shareId, std::function<void(JsonApiJob *)> onSuccess);

    QPointer<Account> _account;
};

} // namespace OCC

Q_DECLARE_METATYPE(OCC::Share)
Q_DECLARE_METATYPE(OCC::Sharee)
