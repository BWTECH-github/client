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
#include "models/trayactivitymodels.h"
#include "networkjobs/jsonjob.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QTimer>

namespace OCC {

class AccountState;

/**
 * Holt Aktivitäten (ocs/v2.php/cloud/activity) und Benachrichtigungen (OCS-Benachrichtigungen-API) der
 * verbundenen Konten in die Modelle des Tray-Menüs und führt Aktionen an Benachrichtigungen aus.
 *
 * Benachrichtigungen nur, wenn die Capability sie ankündigt; die Aktivitäten-App meldet keine Capability,
 * ihr Endpunkt wird deshalb versucht und bei Fehlern eine Stunde lang nicht mehr abgefragt.
 */
class OWNCLOUDGUI_EXPORT ServerActivityFetcher : public QObject
{
    Q_OBJECT
public:
    ServerActivityFetcher(RecentActivityModel *activities, ServerNotificationModel *notifications, QObject *parent = nullptr);

    /** Aktivitäten aller verbundenen Konten, je Konto höchstens einmal pro Minute */
    void refreshActivities();
    /** Benachrichtigungen aller verbundenen Konten */
    void refreshNotifications();

    void dismissNotification(const QUuid &accountId, const QString &id);
    void runNotificationAction(const QUuid &accountId, const QString &id, const OCC::ServerNotificationAction &action);

    /**
     * Pfad (relativ zur Server-Adresse) und Parameter eines Aktionslinks. false für Links auf einen anderen
     * Server oder außerhalb der Server-Adresse: dorthin gehen keine Anmeldedaten.
     */
    static bool apiPathForLink(const QUrl &accountUrl, const QUrl &link, QString *path, QList<QPair<QString, QString>> *query);

    /** nur http(s)-Verweise dürfen den Browser öffnen, keine file:- oder Programm-Schemata */
    static bool isWebLink(const QUrl &url);

Q_SIGNALS:
    void newNotifications(const QUuid &accountId, const QVector<OCC::ServerNotification> &notifications);
    void notificationRequestFailed(const QString &message);

private:
    struct AccountData
    {
        QPointer<JsonApiJob> activityJob;
        QPointer<JsonApiJob> notificationJob;
        QDateTime lastActivityFetch;
        QDateTime activityPausedUntil;
        QDateTime notificationsPausedUntil;
    };

    void onAccountAdded(AccountState *accountState);
    void onAccountRemoved(AccountState *accountState);
    void fetchActivities(AccountState *accountState, bool force);
    void fetchNotifications(AccountState *accountState);

    RecentActivityModel *_activities;
    ServerNotificationModel *_notifications;
    QHash<QUuid, AccountData> _accountData;
    QTimer _pollTimer;
};

} // namespace OCC
