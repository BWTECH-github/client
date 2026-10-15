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

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <QVector>

namespace OCC {

/**
 * Ein Eintrag der Liste „Zuletzt geändert“ im Tray-Menü: eine auf diesem Rechner synchronisierte Datei
 * oder eine Aktivität vom Server (OCS-Aktivitäten-API).
 */
struct OWNCLOUDGUI_EXPORT TrayActivity
{
    enum class Source { Local, Server };

    Source source = Source::Local;
    QUuid accountId;
    QString id; ///< nur Server: activity_id
    QString subject; ///< lokal: Dateiname, Server: Betreff
    QString detail; ///< lokal: Art der Änderung, Server: Nachricht
    QDateTime time;
    QString localPath; ///< nur lokal: absoluter Pfad der Datei
    QString relativePath; ///< nur lokal: Pfad im Sync-Ordner
    QUrl link; ///< nur Server: Verweis in die Weboberfläche
};

/**
 * Zuletzt geänderte Dateien, neueste zuerst. Lokale Einträge sind auf maxLocalItems begrenzt und je Datei
 * eindeutig, Server-Aktivitäten werden je Konto als Ganzes ersetzt.
 */
class OWNCLOUDGUI_EXPORT RecentActivityModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles { SourceRole = Qt::UserRole + 1, AccountRole, DetailRole, TimeRole, LocalPathRole, LinkRole };
    Q_ENUM(Roles)

    explicit RecentActivityModel(int maxLocalItems, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    const TrayActivity &activity(int row) const;

    void addLocalActivity(const TrayActivity &activity);
    void setServerActivities(const QUuid &accountId, const QVector<TrayActivity> &activities);
    void removeAccount(const QUuid &accountId);

    /** wertet das data-Feld der Antwort von ocs/v2.php/cloud/activity aus */
    static QVector<TrayActivity> parseServerActivities(const QJsonArray &data, const QUuid &accountId);

private:
    int insertPosition(const QDateTime &time) const;
    void removeRowAt(int row);

    int _maxLocalItems;
    QVector<TrayActivity> _items; ///< nach Zeit absteigend sortiert
};

struct OWNCLOUDGUI_EXPORT ServerNotificationAction
{
    QString label;
    QUrl link;
    QByteArray verb; ///< GET, POST, PUT, DELETE oder WEB (im Browser öffnen)
    bool primary = false;
};

struct OWNCLOUDGUI_EXPORT ServerNotification
{
    QUuid accountId;
    QString id;
    QString app;
    QString subject;
    QString message;
    QUrl link;
    QDateTime time;
    QVector<ServerNotificationAction> actions;
};

/**
 * Benachrichtigungen aller Konten (OCS-Benachrichtigungen-API), neueste zuerst.
 */
class OWNCLOUDGUI_EXPORT ServerNotificationModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles { AccountRole = Qt::UserRole + 1, IdRole, MessageRole, TimeRole, LinkRole };
    Q_ENUM(Roles)

    explicit ServerNotificationModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    const ServerNotification &notification(int row) const;

    /**
     * Ersetzt die Benachrichtigungen eines Kontos und liefert die, die seit dem letzten Aufruf neu
     * hinzugekommen sind (für Desktop-Hinweise).
     */
    QVector<ServerNotification> setNotifications(const QUuid &accountId, const QVector<ServerNotification> &notifications);
    bool removeNotification(const QUuid &accountId, const QString &id);
    void removeAccount(const QUuid &accountId);

    /** wertet das data-Feld der Antwort von ocs/v2.php/apps/notifications/api/v1/notifications aus */
    static QVector<ServerNotification> parseNotifications(const QJsonArray &data, const QUuid &accountId);

private:
    QVector<ServerNotification> _items; ///< nach Zeit absteigend sortiert
    QHash<QUuid, QSet<QString>> _knownIds; ///< je Konto bereits gemeldete IDs
};

} // namespace OCC
