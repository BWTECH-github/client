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

#include "trayactivitymodels.h"

#include <QJsonObject>

#include <algorithm>

namespace OCC {

namespace {
    // ungültige Zeitangaben vom Server sortieren ans Ende statt die Reihenfolge zu stören
    QDateTime parseServerTime(const QString &value)
    {
        const auto time = QDateTime::fromString(value, Qt::ISODate);
        return time.isValid() ? time : QDateTime::fromSecsSinceEpoch(0);
    }

    template <typename T>
    void sortNewestFirst(QVector<T> &items)
    {
        std::stable_sort(items.begin(), items.end(), [](const T &a, const T &b) { return a.time > b.time; });
    }
}

RecentActivityModel::RecentActivityModel(int maxLocalItems, QObject *parent)
    : QAbstractListModel(parent)
    , _maxLocalItems(maxLocalItems)
{
}

int RecentActivityModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(_items.size());
}

QVariant RecentActivityModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return {};
    }
    const auto &item = _items.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return item.subject;
    case Qt::ToolTipRole:
        return item.source == TrayActivity::Source::Local ? item.relativePath : item.detail;
    case SourceRole:
        return static_cast<int>(item.source);
    case AccountRole:
        return QVariant::fromValue(item.accountId);
    case DetailRole:
        return item.detail;
    case TimeRole:
        return item.time;
    case LocalPathRole:
        return item.localPath;
    case LinkRole:
        return item.link;
    default:
        return {};
    }
}

const TrayActivity &RecentActivityModel::activity(int row) const
{
    return _items.at(row);
}

int RecentActivityModel::insertPosition(const QDateTime &time) const
{
    // neuer Eintrag vor gleich alten: bei gleicher Zeit steht der zuletzt gemeldete oben
    const auto it = std::find_if(_items.cbegin(), _items.cend(), [&time](const TrayActivity &item) { return item.time <= time; });
    return static_cast<int>(std::distance(_items.cbegin(), it));
}

void RecentActivityModel::removeRowAt(int row)
{
    beginRemoveRows(QModelIndex(), row, row);
    _items.removeAt(row);
    endRemoveRows();
}

void RecentActivityModel::addLocalActivity(const TrayActivity &activity)
{
    TrayActivity item = activity;
    item.source = TrayActivity::Source::Local;

    // dieselbe Datei nur einmal: der ältere Eintrag weicht dem neuen
    if (!item.localPath.isEmpty()) {
        for (int i = 0; i < _items.size(); ++i) {
            const auto &existing = _items.at(i);
            if (existing.source == TrayActivity::Source::Local && existing.localPath == item.localPath) {
                removeRowAt(i);
                break;
            }
        }
    }

    const int row = insertPosition(item.time);
    beginInsertRows(QModelIndex(), row, row);
    _items.insert(row, item);
    endInsertRows();

    // über der Grenze fallen die ältesten lokalen Einträge weg
    auto localCount = std::count_if(_items.cbegin(), _items.cend(), [](const TrayActivity &a) { return a.source == TrayActivity::Source::Local; });
    for (int i = static_cast<int>(_items.size()) - 1; i >= 0 && localCount > _maxLocalItems; --i) {
        if (_items.at(i).source == TrayActivity::Source::Local) {
            removeRowAt(i);
            --localCount;
        }
    }
}

void RecentActivityModel::setServerActivities(const QUuid &accountId, const QVector<TrayActivity> &activities)
{
    beginResetModel();
    _items.erase(std::remove_if(_items.begin(), _items.end(),
                     [&accountId](const TrayActivity &a) { return a.source == TrayActivity::Source::Server && a.accountId == accountId; }),
        _items.end());
    for (auto activity : activities) {
        activity.source = TrayActivity::Source::Server;
        activity.accountId = accountId;
        _items.append(activity);
    }
    sortNewestFirst(_items);
    endResetModel();
}

void RecentActivityModel::removeAccount(const QUuid &accountId)
{
    for (int i = static_cast<int>(_items.size()) - 1; i >= 0; --i) {
        if (_items.at(i).accountId == accountId) {
            removeRowAt(i);
        }
    }
}

QVector<TrayActivity> RecentActivityModel::parseServerActivities(const QJsonArray &data, const QUuid &accountId)
{
    QVector<TrayActivity> result;
    result.reserve(data.size());
    for (const auto &value : data) {
        const auto json = value.toObject();
        TrayActivity activity;
        activity.source = TrayActivity::Source::Server;
        activity.accountId = accountId;
        activity.id = json.value(QLatin1String("id")).toVariant().toString();
        activity.subject = json.value(QLatin1String("subject")).toString().trimmed();
        if (activity.subject.isEmpty()) {
            activity.subject = json.value(QLatin1String("file")).toString().trimmed();
        }
        if (activity.subject.isEmpty()) {
            continue;
        }
        activity.detail = json.value(QLatin1String("message")).toString().trimmed();
        activity.link = QUrl(json.value(QLatin1String("link")).toString());
        activity.time = parseServerTime(json.value(QLatin1String("date")).toString());
        result.append(activity);
    }
    return result;
}


ServerNotificationModel::ServerNotificationModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ServerNotificationModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(_items.size());
}

QVariant ServerNotificationModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return {};
    }
    const auto &item = _items.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return item.subject;
    case Qt::ToolTipRole:
    case MessageRole:
        return item.message;
    case AccountRole:
        return QVariant::fromValue(item.accountId);
    case IdRole:
        return item.id;
    case TimeRole:
        return item.time;
    case LinkRole:
        return item.link;
    default:
        return {};
    }
}

const ServerNotification &ServerNotificationModel::notification(int row) const
{
    return _items.at(row);
}

QVector<ServerNotification> ServerNotificationModel::setNotifications(const QUuid &accountId, const QVector<ServerNotification> &notifications)
{
    const auto known = _knownIds.value(accountId);
    QSet<QString> ids;
    QVector<ServerNotification> fresh;
    for (const auto &notification : notifications) {
        ids.insert(notification.id);
        if (!known.contains(notification.id)) {
            fresh.append(notification);
        }
    }
    // nur die aktuellen IDs merken: erledigte kommen nicht wieder, die Menge wächst so nicht unbegrenzt
    _knownIds.insert(accountId, ids);

    beginResetModel();
    _items.erase(std::remove_if(_items.begin(), _items.end(), [&accountId](const ServerNotification &n) { return n.accountId == accountId; }), _items.end());
    for (auto notification : notifications) {
        notification.accountId = accountId;
        _items.append(notification);
    }
    sortNewestFirst(_items);
    endResetModel();
    return fresh;
}

bool ServerNotificationModel::removeNotification(const QUuid &accountId, const QString &id)
{
    for (int i = 0; i < _items.size(); ++i) {
        if (_items.at(i).accountId == accountId && _items.at(i).id == id) {
            beginRemoveRows(QModelIndex(), i, i);
            _items.removeAt(i);
            endRemoveRows();
            return true;
        }
    }
    return false;
}

void ServerNotificationModel::removeAccount(const QUuid &accountId)
{
    _knownIds.remove(accountId);
    for (int i = static_cast<int>(_items.size()) - 1; i >= 0; --i) {
        if (_items.at(i).accountId == accountId) {
            beginRemoveRows(QModelIndex(), i, i);
            _items.removeAt(i);
            endRemoveRows();
        }
    }
}

QVector<ServerNotification> ServerNotificationModel::parseNotifications(const QJsonArray &data, const QUuid &accountId)
{
    QVector<ServerNotification> result;
    result.reserve(data.size());
    for (const auto &value : data) {
        const auto json = value.toObject();
        ServerNotification notification;
        notification.accountId = accountId;
        notification.id = json.value(QLatin1String("notification_id")).toVariant().toString();
        if (notification.id.isEmpty()) {
            continue;
        }
        notification.app = json.value(QLatin1String("app")).toString();
        notification.subject = json.value(QLatin1String("subject")).toString().trimmed();
        notification.message = json.value(QLatin1String("message")).toString().trimmed();
        if (notification.subject.isEmpty()) {
            notification.subject = notification.message;
            notification.message.clear();
        }
        notification.link = QUrl(json.value(QLatin1String("link")).toString());
        notification.time = parseServerTime(json.value(QLatin1String("datetime")).toString());

        const auto actions = json.value(QLatin1String("actions")).toArray();
        for (const auto &actionValue : actions) {
            const auto actionJson = actionValue.toObject();
            ServerNotificationAction action;
            // manche Apps liefern die Beschriftung prozentkodiert (so schon im Client 6.0.3 behandelt)
            action.label = QUrl::fromPercentEncoding(actionJson.value(QLatin1String("label")).toString().toUtf8()).trimmed();
            action.link = QUrl(actionJson.value(QLatin1String("link")).toString());
            action.verb = actionJson.value(QLatin1String("type")).toString().toUpper().toUtf8();
            action.primary = actionJson.value(QLatin1String("primary")).toBool();
            if (!action.label.isEmpty()) {
                notification.actions.append(action);
            }
        }
        result.append(notification);
    }
    return result;
}

} // namespace OCC
