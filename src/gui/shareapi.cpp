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
#include "shareapi.h"

#include "libsync/account.h"
#include "libsync/capabilities.h"
#include "libsync/networkjobs/jsonjob.h"

#include <QJsonArray>
#include <QNetworkReply>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSet>

using namespace std::chrono_literals;

namespace OCC {

Q_LOGGING_CATEGORY(lcShareApi, "gui.shareapi", QtInfoMsg)

namespace {
    const QString sharesEndpoint = QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/shares");
    const QString shareesEndpoint = QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/sharees");

    // je nach Server kommen Zahlen als Zahl oder als Zeichenkette
    int toInt(const QJsonValue &value)
    {
        return value.toVariant().toInt();
    }

    QJsonValue ocsData(const QJsonObject &ocsDocument)
    {
        return ocsDocument.value(QLatin1String("ocs")).toObject().value(QLatin1String("data"));
    }

    QString dateArgument(const QDate &date)
    {
        return date.isValid() ? date.toString(QStringLiteral("yyyy-MM-dd")) : QString();
    }
}

ShareApi::ShareApi(Account *account, QObject *parent)
    : QObject(parent)
    , _account(account)
{
}

JsonApiJob *ShareApi::startJob(Operation operation, const QString &path, const QByteArray &verb, const SimpleNetworkJob::UrlQuery &arguments,
    const QString &shareId, std::function<void(JsonApiJob *)> onSuccess)
{
    if (!_account) {
        Q_EMIT requestFailed(operation, QString(), shareId);
        return nullptr;
    }
    auto *job = new JsonApiJob(_account, path, verb, arguments, QNetworkRequest(), this);
    connect(job, &JsonApiJob::finishedSignal, this, [this, job, operation, shareId, onSuccess = std::move(onSuccess)] {
        if (!job->ocsSuccess()) {
            // die OCS-Meldung erklärt z.B. eine Passwort- oder Ablauf-Richtlinie; ohne sie den Netzfehler zeigen
            QString message = job->ocsMessage();
            if (message.isEmpty() && job->reply() && job->reply()->error() != QNetworkReply::NoError)
                message = job->errorString();
            qCWarning(lcShareApi) << operation << "failed:" << job->ocsStatus() << message;
            Q_EMIT requestFailed(operation, message, shareId);
            return;
        }
        onSuccess(job);
    });
    job->start();
    return job;
}

void ShareApi::fetchItemInfo(const QUrl &davUrl, const QString &path)
{
    if (!_account) {
        Q_EMIT requestFailed(Operation::ItemInfo, QString(), QString());
        return;
    }
    auto *job = new PropfindJob(_account, davUrl, path, PropfindJob::Depth::Zero, this);
    job->setProperties({QByteArrayLiteral("resourcetype"), QByteArrayLiteral("http://open-collaboration-services.org/ns:share-permissions")});
    job->setTimeout(30s);
    connect(job, &PropfindJob::directoryListingIterated, this, [this](const QString &, const QMap<QString, QString> &properties) {
        const bool isFolder = properties.value(QStringLiteral("resourcetype")).contains(QLatin1String("collection"));
        // ältere Server liefern share-permissions leer: dann entscheidet der Server beim Anlegen
        bool ok = false;
        const int maxPermissions = properties.value(QStringLiteral("share-permissions")).toInt(&ok);
        Q_EMIT itemInfoFetched(isFolder, ok ? SharePermissions::fromInt(maxPermissions) & allSharePermissions() : allSharePermissions());
    });
    connect(job, &PropfindJob::finishedWithError, this, [this, job] {
        Q_EMIT requestFailed(Operation::ItemInfo, job->errorString(), QString());
    });
    job->start();
}

void ShareApi::fetchShares(const QString &path)
{
    // nur die jüngste Antwort zählt: ein verspätetes älteres Ergebnis darf die Liste nicht zurückdrehen
    const quint64 generation = ++_fetchGeneration;
    startJob(Operation::Fetch, sharesEndpoint, QByteArrayLiteral("GET"),
        {{QStringLiteral("path"), path}, {QStringLiteral("reshares"), QStringLiteral("false")}}, QString(), [this, generation](JsonApiJob *job) {
            if (generation == _fetchGeneration)
                Q_EMIT sharesFetched(parseShares(job->data()));
        });
}

void ShareApi::createLinkShare(const QString &path, const LinkSettings &settings)
{
    startJob(Operation::Create, sharesEndpoint, QByteArrayLiteral("POST"), linkCreateArguments(path, settings), QString(),
        [this](JsonApiJob *job) { Q_EMIT shareCreated(parseShare(ocsData(job->data()).toObject())); });
}

void ShareApi::createShare(const QString &path, const Sharee &sharee, SharePermissions permissions)
{
    const SimpleNetworkJob::UrlQuery arguments = {
        {QStringLiteral("path"), path},
        {QStringLiteral("shareType"), QString::number(sharee.type)},
        {QStringLiteral("shareWith"), sharee.shareWith},
        {QStringLiteral("permissions"), QString::number(permissions.toInt())},
    };
    startJob(Operation::Create, sharesEndpoint, QByteArrayLiteral("POST"), arguments, QString(),
        [this](JsonApiJob *job) { Q_EMIT shareCreated(parseShare(ocsData(job->data()).toObject())); });
}

void ShareApi::updateShare(const QString &shareId, const SimpleNetworkJob::UrlQuery &changes)
{
    startJob(Operation::Update, sharesEndpoint + QLatin1Char('/') + shareId, QByteArrayLiteral("PUT"), changes, shareId, [this, shareId](JsonApiJob *job) {
        Share share = parseShare(ocsData(job->data()).toObject());
        // ältere Server antworten ohne Freigabedaten: die ID trotzdem melden, damit neu geladen wird
        if (share.id.isEmpty())
            share.id = shareId;
        Q_EMIT shareUpdated(share);
    });
}

void ShareApi::deleteShare(const QString &shareId)
{
    startJob(Operation::Delete, sharesEndpoint + QLatin1Char('/') + shareId, QByteArrayLiteral("DELETE"), {}, shareId,
        [this, shareId](JsonApiJob *) { Q_EMIT shareDeleted(shareId); });
}

void ShareApi::searchSharees(const QString &search, bool isFolder)
{
    const SimpleNetworkJob::UrlQuery arguments = {
        {QStringLiteral("search"), search},
        {QStringLiteral("itemType"), isFolder ? QStringLiteral("folder") : QStringLiteral("file")},
        {QStringLiteral("page"), QStringLiteral("1")},
        {QStringLiteral("perPage"), QStringLiteral("50")},
    };
    startJob(Operation::Search, shareesEndpoint, QByteArrayLiteral("GET"), arguments, QString(),
        [this, search](JsonApiJob *job) { Q_EMIT shareesFetched(search, parseSharees(job->data())); });
}

Share ShareApi::parseShare(const QJsonObject &data)
{
    Share share;
    // "id" war früher eine Zahl, heute eine Zeichenkette
    share.id = data.value(QLatin1String("id")).toVariant().toString();
    share.type = toInt(data.value(QLatin1String("share_type")));
    share.permissions = SharePermissions::fromInt(toInt(data.value(QLatin1String("permissions")))) & allSharePermissions();
    share.shareWith = data.value(QLatin1String("share_with")).toVariant().toString();
    share.shareWithDisplayName = data.value(QLatin1String("share_with_displayname")).toString();
    share.name = data.value(QLatin1String("name")).toString();
    share.url = data.value(QLatin1String("url")).toString();
    // bei Links steht in share_with der Passwort-Hash, ohne Passwort ist es null
    share.hasPassword = share.isLink() && !share.shareWith.isEmpty();
    // "2026-10-22 00:00:00" oder null
    share.expireDate = QDate::fromString(data.value(QLatin1String("expiration")).toString().left(10), Qt::ISODate);
    share.isFolder = data.value(QLatin1String("item_type")).toString() == QLatin1String("folder");
    return share;
}

QList<Share> ShareApi::parseShares(const QJsonObject &ocsDocument)
{
    QList<Share> shares;
    const QJsonArray items = ocsData(ocsDocument).toArray();
    for (const auto &item : items) {
        Share share = parseShare(item.toObject());
        if (!share.id.isEmpty())
            shares.append(share);
    }
    return shares;
}

QList<Sharee> ShareApi::parseSharees(const QJsonObject &ocsDocument)
{
    const QJsonObject data = ocsData(ocsDocument).toObject();
    QList<Sharee> sharees;
    QSet<QString> seen;
    const auto appendFrom = [&](const QJsonObject &bucket) {
        for (const auto &key : {QLatin1String("users"), QLatin1String("groups"), QLatin1String("remotes")}) {
            const QJsonArray entries = bucket.value(key).toArray();
            for (const auto &entry : entries) {
                const QJsonObject object = entry.toObject();
                const QJsonObject value = object.value(QLatin1String("value")).toObject();
                Sharee sharee;
                sharee.type = toInt(value.value(QLatin1String("shareType")));
                sharee.shareWith = value.value(QLatin1String("shareWith")).toVariant().toString();
                sharee.displayName = object.value(QLatin1String("label")).toString();
                sharee.additionalInfo = value.value(QLatin1String("shareWithAdditionalInfo")).toString();
                if (sharee.shareWith.isEmpty())
                    continue;
                if (sharee.displayName.isEmpty())
                    sharee.displayName = sharee.shareWith;
                // exakte Treffer zuerst; derselbe Eintrag kann in beiden Listen stehen
                const QString identity = QString::number(sharee.type) + QLatin1Char(':') + sharee.shareWith;
                if (seen.contains(identity))
                    continue;
                seen.insert(identity);
                sharees.append(sharee);
            }
        }
    };
    appendFrom(data.value(QLatin1String("exact")).toObject());
    appendFrom(data);
    return sharees;
}

SimpleNetworkJob::UrlQuery ShareApi::linkCreateArguments(const QString &path, const LinkSettings &settings)
{
    SimpleNetworkJob::UrlQuery arguments = {
        {QStringLiteral("path"), path},
        {QStringLiteral("shareType"), QString::number(Share::Link)},
        {QStringLiteral("permissions"), QString::number(settings.permissions.toInt())},
    };
    if (!settings.name.isEmpty())
        arguments.append({QStringLiteral("name"), settings.name});
    if (settings.passwordProtected && !settings.password.isEmpty())
        arguments.append({QStringLiteral("password"), settings.password});
    if (settings.expireDate.isValid())
        arguments.append({QStringLiteral("expireDate"), dateArgument(settings.expireDate)});
    return arguments;
}

SimpleNetworkJob::UrlQuery ShareApi::linkUpdateArguments(const Share &current, const LinkSettings &wanted)
{
    // oC10 nimmt mehrere Felder in einem PUT an; leere Werte entfernen Passwort bzw. Ablaufdatum
    SimpleNetworkJob::UrlQuery arguments;
    if (wanted.name != current.name)
        arguments.append({QStringLiteral("name"), wanted.name});
    if (wanted.permissions != current.permissions)
        arguments.append({QStringLiteral("permissions"), QString::number(wanted.permissions.toInt())});
    if (wanted.passwordProtected) {
        if (!wanted.password.isEmpty())
            arguments.append({QStringLiteral("password"), wanted.password});
    } else if (current.hasPassword) {
        arguments.append({QStringLiteral("password"), QString()});
    }
    if (wanted.expireDate != current.expireDate)
        arguments.append({QStringLiteral("expireDate"), dateArgument(wanted.expireDate)});
    return arguments;
}

SharePermissions ShareApi::maskForItem(SharePermissions permissions, bool isFolder, SharePermissions maxPermissions)
{
    if (!isFolder)
        permissions &= ~SharePermissions(SharePermission::Create | SharePermission::Delete);
    // Lesen ist für jede Freigabe Pflicht
    return (permissions & maxPermissions) | SharePermission::Read;
}

SharePermissions ShareApi::defaultUserSharePermissions(const Capabilities &capabilities, bool isFolder, SharePermissions maxPermissions)
{
    SharePermissions permissions = SharePermissions::fromInt(capabilities.defaultPermissions()) & allSharePermissions();
    if (!capabilities.shareResharing())
        permissions &= ~SharePermissions(SharePermission::Share);
    return maskForItem(permissions, isFolder, maxPermissions);
}

bool ShareApi::linkPasswordEnforced(const Capabilities &capabilities, SharePermissions linkPermissions)
{
    const SharePermissions readWrite = SharePermission::Read | SharePermission::Create;
    const SharePermissions readWriteDelete = SharePermission::Read | SharePermission::Update | SharePermission::Create | SharePermission::Delete;
    if (linkPermissions == SharePermissions(SharePermission::Create))
        return capabilities.sharePublicLinkEnforcePasswordForUploadOnly();
    if (linkPermissions == readWriteDelete)
        return capabilities.sharePublicLinkEnforcePasswordForReadWriteDelete();
    if (linkPermissions == readWrite)
        return capabilities.sharePublicLinkEnforcePasswordForReadWrite();
    return capabilities.sharePublicLinkEnforcePasswordForReadOnly();
}

QUrl ShareApi::directDownloadUrl(const Share &share)
{
    // eine Datei-Ablage (nur hochladen) erlaubt kein Herunterladen
    if (!share.isLink() || !share.permissions.testFlag(SharePermission::Read))
        return {};
    QUrl url(share.url);
    // nur die oC10-Form …/s/<token>; andere Adressen kennen /download nicht
    static const QRegularExpression publicLinkPath(QStringLiteral("/s/[^/]+/?$"));
    const bool isWeb = url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http");
    if (!url.isValid() || !isWeb || url.hasQuery() || url.hasFragment() || !publicLinkPath.match(url.path()).hasMatch())
        return {};
    QString path = url.path();
    if (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    url.setPath(path + QStringLiteral("/download"));
    return url;
}

PublicLinkCreator::PublicLinkCreator(Account *account, const QString &path, QObject *parent)
    : QObject(parent)
    , _account(account)
    , _api(new ShareApi(account, this))
    , _path(path.startsWith(QLatin1Char('/')) ? path : QLatin1Char('/') + path)
{
    connect(_api, &ShareApi::sharesFetched, this, &PublicLinkCreator::onSharesFetched);
    connect(_api, &ShareApi::shareCreated, this, [this](const Share &share) {
        if (share.url.isEmpty()) {
            // ohne Adresse gibt es nichts zu kopieren; der Teilen-Dialog zeigt den Link trotzdem
            qCWarning(lcShareApi) << "link share created without url for" << _path;
            Q_EMIT failed(QString());
            return;
        }
        Q_EMIT linkReady(share.url);
    });
    connect(_api, &ShareApi::requestFailed, this, [this](ShareApi::Operation, const QString &serverMessage) { Q_EMIT failed(serverMessage); });
}

bool PublicLinkCreator::needsUserInput(const Capabilities &capabilities)
{
    return ShareApi::linkPasswordEnforced(capabilities, SharePermission::Read) || capabilities.sharePublicLinkEnforceExpireDate();
}

QString PublicLinkCreator::linkName(const Capabilities &capabilities, const QDate &today)
{
    if (capabilities.sharePublicLinkDefaultExpire())
        return tr("Context menu link %1").arg(today.toString(Qt::ISODate));
    return tr("Context menu link");
}

void PublicLinkCreator::start()
{
    if (!_account) {
        Q_EMIT failed(QString());
        return;
    }
    _name = linkName(_account->capabilities(), QDate::currentDate());
    _api->fetchShares(_path);
}

void PublicLinkCreator::onSharesFetched(const QList<Share> &shares)
{
    if (!_account) {
        Q_EMIT failed(QString());
        return;
    }
    const Share *firstLink = nullptr;
    for (const auto &share : shares) {
        if (!share.isLink() || share.url.isEmpty())
            continue;
        // nur einen unveränderten Link wiederverwenden: wer ihn im Dialog auf Bearbeiten gestellt oder mit Passwort versehen hat, bekommt einen neuen
        if (share.name == _name && share.permissions == SharePermissions(SharePermission::Read) && !share.hasPassword) {
            Q_EMIT linkReady(share.url);
            return;
        }
        if (!firstLink)
            firstLink = &share;
    }
    if (firstLink && !_account->capabilities().sharePublicLinkMultiple()) {
        Q_EMIT linkReady(firstLink->url);
        return;
    }
    LinkSettings settings;
    settings.name = _name;
    _api->createLinkShare(_path, settings);
}

} // namespace OCC
