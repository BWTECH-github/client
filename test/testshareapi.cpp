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

#include <QtTest>

#include "gui/guiutility.h"
#include "gui/shareapi.h"
#include "libsync/abstractnetworkjob.h"
#include "libsync/capabilities.h"
#include "libsync/httplogger.h"

#include "testutils/syncenginetestutils.h"
#include "testutils/testutils.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>

using namespace std::chrono_literals;
using namespace OCC;

namespace {
// Antworten wie von oC10/owncloud.online (OCS v1: immer HTTP 200, Status im Körper)
QByteArray ocsReply(const QJsonValue &data, int statusCode = 100, const QString &message = QStringLiteral("OK"))
{
    const QJsonObject meta{{QStringLiteral("status"), statusCode == 100 ? QStringLiteral("ok") : QStringLiteral("failure")},
        {QStringLiteral("statuscode"), statusCode}, {QStringLiteral("message"), message}};
    return QJsonDocument(QJsonObject{{QStringLiteral("ocs"), QJsonObject{{QStringLiteral("meta"), meta}, {QStringLiteral("data"), data}}}})
        .toJson(QJsonDocument::Compact);
}

QJsonObject linkShareJson()
{
    return QJsonObject{{QStringLiteral("id"), 17}, {QStringLiteral("share_type"), 3}, {QStringLiteral("permissions"), 15},
        {QStringLiteral("share_with"), QStringLiteral("$2y$10$passwordhash")}, {QStringLiteral("share_with_displayname"), QStringLiteral("")},
        {QStringLiteral("name"), QStringLiteral("Team drop")}, {QStringLiteral("url"), QStringLiteral("https://cloud.example/s/AbC")},
        {QStringLiteral("token"), QStringLiteral("AbC")}, {QStringLiteral("expiration"), QStringLiteral("2026-12-24 00:00:00")},
        {QStringLiteral("item_type"), QStringLiteral("folder")}};
}

QJsonObject userShareJson()
{
    return QJsonObject{{QStringLiteral("id"), QStringLiteral("18")}, {QStringLiteral("share_type"), 0}, {QStringLiteral("permissions"), 19},
        {QStringLiteral("share_with"), QStringLiteral("alice")}, {QStringLiteral("share_with_displayname"), QStringLiteral("Alice A.")},
        {QStringLiteral("expiration"), QJsonValue::Null}, {QStringLiteral("item_type"), QStringLiteral("folder")}};
}

QJsonObject groupShareJson()
{
    // ältere Server liefern Zahlen als Zeichenketten
    return QJsonObject{{QStringLiteral("id"), QStringLiteral("19")}, {QStringLiteral("share_type"), QStringLiteral("1")},
        {QStringLiteral("permissions"), QStringLiteral("1")}, {QStringLiteral("share_with"), QStringLiteral("team")},
        {QStringLiteral("share_with_displayname"), QStringLiteral("Team")}, {QStringLiteral("item_type"), QStringLiteral("file")}};
}

QJsonObject publicLinkWithoutPassword()
{
    auto share = linkShareJson();
    share.insert(QStringLiteral("id"), QStringLiteral("20"));
    share.insert(QStringLiteral("share_with"), QJsonValue::Null);
    share.insert(QStringLiteral("expiration"), QJsonValue::Null);
    share.insert(QStringLiteral("permissions"), 1);
    return share;
}

QJsonObject shareeJson(const QString &label, int shareType, const QString &shareWith, const QString &additionalInfo = {})
{
    QJsonObject value{{QStringLiteral("shareType"), shareType}, {QStringLiteral("shareWith"), shareWith}};
    if (!additionalInfo.isEmpty())
        value.insert(QStringLiteral("shareWithAdditionalInfo"), additionalInfo);
    return QJsonObject{{QStringLiteral("label"), label}, {QStringLiteral("value"), value}};
}

QJsonObject shareesData()
{
    const QJsonObject exact{{QStringLiteral("users"), QJsonArray{shareeJson(QStringLiteral("Alice A."), 0, QStringLiteral("alice"), QStringLiteral("alice@example.com"))}},
        {QStringLiteral("groups"), QJsonArray{}}, {QStringLiteral("remotes"), QJsonArray{}}};
    return QJsonObject{{QStringLiteral("exact"), exact},
        // derselbe Nutzer noch einmal unter den ungenauen Treffern: darf nur einmal erscheinen
        {QStringLiteral("users"), QJsonArray{shareeJson(QStringLiteral("Alice A."), 0, QStringLiteral("alice")), shareeJson(QStringLiteral("Alina"), 0, QStringLiteral("alina"))}},
        {QStringLiteral("groups"), QJsonArray{shareeJson(QStringLiteral("All staff"), 1, QStringLiteral("all"))}},
        {QStringLiteral("remotes"), QJsonArray{shareeJson(QStringLiteral("al@other.example"), 6, QStringLiteral("al@other.example"))}}};
}

// nur hineinschauen: der HTTP-Logger liest den Körper danach selbst ab Position 0
QByteArray peekBody(QIODevice *outgoingData)
{
    return outgoingData ? outgoingData->peek(outgoingData->size()) : QByteArray();
}

QUrlQuery bodyQuery(QIODevice *outgoingData)
{
    return QUrlQuery(QString::fromUtf8(peekBody(outgoingData)));
}

QString queryValue(const QUrlQuery &query, const QString &key)
{
    return query.queryItemValue(key, QUrl::FullyDecoded);
}

Capabilities sharingCapabilities(const QVariantMap &filesSharing)
{
    return Capabilities(QUrl(QStringLiteral("https://cloud.example")), {{QStringLiteral("files_sharing"), filesSharing}});
}

// PROPFIND-Antwort: 207 mit XML-Inhaltstyp, sonst verwirft PropfindJob sie
class MultiStatusReply : public FakePayloadReply
{
public:
    MultiStatusReply(QNetworkAccessManager::Operation op, const QNetworkRequest &request, const QByteArray &body, QObject *parent)
        : FakePayloadReply(op, request, body, parent)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 207);
        setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/xml; charset=utf-8"));
    }
};

struct Recorded
{
    QString verb;
    QString path;
    QUrlQuery urlQuery;
    QUrlQuery body;
    QByteArray ocsHeader;
};
}

class TestShareApi : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { AbstractNetworkJob::httpTimeout = 10s; }

    void testParseShares()
    {
        const auto shares =
            ShareApi::parseShares(QJsonDocument::fromJson(ocsReply(QJsonArray{linkShareJson(), userShareJson(), groupShareJson(), publicLinkWithoutPassword()})).object());
        QCOMPARE(shares.size(), 4);

        const Share &link = shares.at(0);
        QCOMPARE(link.id, QStringLiteral("17"));
        QVERIFY(link.isLink());
        QCOMPARE(link.permissions, SharePermission::Read | SharePermission::Update | SharePermission::Create | SharePermission::Delete);
        QVERIFY(link.hasPassword);
        QCOMPARE(link.name, QStringLiteral("Team drop"));
        QCOMPARE(link.url, QStringLiteral("https://cloud.example/s/AbC"));
        QCOMPARE(link.expireDate, QDate(2026, 12, 24));
        QVERIFY(link.isFolder);

        const Share &user = shares.at(1);
        QCOMPARE(user.id, QStringLiteral("18"));
        QCOMPARE(user.type, int(Share::User));
        QCOMPARE(user.shareWith, QStringLiteral("alice"));
        QCOMPARE(user.shareWithDisplayName, QStringLiteral("Alice A."));
        QCOMPARE(user.permissions, SharePermission::Read | SharePermission::Update | SharePermission::Share);
        QVERIFY(!user.hasPassword);
        QVERIFY(!user.expireDate.isValid());

        const Share &group = shares.at(2);
        QCOMPARE(group.type, int(Share::Group));
        QCOMPARE(group.permissions, SharePermissions(SharePermission::Read));
        QVERIFY(!group.isFolder);

        const Share &plainLink = shares.at(3);
        QVERIFY(!plainLink.hasPassword);
        QVERIFY(!plainLink.expireDate.isValid());
    }

    void testParseSharees()
    {
        const auto sharees = ShareApi::parseSharees(QJsonDocument::fromJson(ocsReply(shareesData())).object());
        QCOMPARE(sharees.size(), 4);
        // exakte Treffer zuerst, Dubletten entfernt
        QCOMPARE(sharees.at(0).shareWith, QStringLiteral("alice"));
        QCOMPARE(sharees.at(0).additionalInfo, QStringLiteral("alice@example.com"));
        QCOMPARE(sharees.at(1).shareWith, QStringLiteral("alina"));
        QCOMPARE(sharees.at(2).type, int(Share::Group));
        QCOMPARE(sharees.at(2).displayName, QStringLiteral("All staff"));
        QCOMPARE(sharees.at(3).type, int(Share::Remote));
    }

    void testLinkArguments()
    {
        LinkSettings settings;
        settings.name = QStringLiteral("Für Kunden & Partner");
        settings.permissions = SharePermission::Create;
        settings.passwordProtected = true;
        settings.password = QStringLiteral("s3cret+/=");
        settings.expireDate = QDate(2026, 11, 30);
        const auto create = ShareApi::linkCreateArguments(QStringLiteral("/Docs"), settings);
        const auto value = [](const SimpleNetworkJob::UrlQuery &query, const QString &key) {
            for (const auto &item : query) {
                if (item.first == key)
                    return item.second;
            }
            return QStringLiteral("<missing>");
        };
        QCOMPARE(value(create, QStringLiteral("path")), QStringLiteral("/Docs"));
        QCOMPARE(value(create, QStringLiteral("shareType")), QStringLiteral("3"));
        QCOMPARE(value(create, QStringLiteral("permissions")), QStringLiteral("4"));
        QCOMPARE(value(create, QStringLiteral("name")), settings.name);
        QCOMPARE(value(create, QStringLiteral("password")), settings.password);
        QCOMPARE(value(create, QStringLiteral("expireDate")), QStringLiteral("2026-11-30"));

        // ohne Passwort/Ablauf werden die Felder gar nicht erst gesendet
        const auto plain = ShareApi::linkCreateArguments(QStringLiteral("/Docs"), LinkSettings());
        QCOMPARE(value(plain, QStringLiteral("password")), QStringLiteral("<missing>"));
        QCOMPARE(value(plain, QStringLiteral("expireDate")), QStringLiteral("<missing>"));
        QCOMPARE(value(plain, QStringLiteral("permissions")), QStringLiteral("1"));

        // Bearbeiten: nur Geändertes; Passwort leer gelassen = behalten, abgewählt = entfernen
        const Share current = ShareApi::parseShare(linkShareJson());
        LinkSettings unchanged;
        unchanged.name = current.name;
        unchanged.permissions = current.permissions;
        unchanged.passwordProtected = true;
        unchanged.expireDate = current.expireDate;
        QVERIFY(ShareApi::linkUpdateArguments(current, unchanged).isEmpty());

        LinkSettings changed = unchanged;
        changed.passwordProtected = false;
        changed.expireDate = QDate();
        changed.permissions = SharePermission::Read;
        const auto update = ShareApi::linkUpdateArguments(current, changed);
        QCOMPARE(update.size(), 3);
        QCOMPARE(value(update, QStringLiteral("password")), QString());
        QCOMPARE(value(update, QStringLiteral("expireDate")), QString());
        QCOMPARE(value(update, QStringLiteral("permissions")), QStringLiteral("1"));

        LinkSettings newPassword = unchanged;
        newPassword.password = QStringLiteral("neu");
        const auto passwordOnly = ShareApi::linkUpdateArguments(current, newPassword);
        QCOMPARE(passwordOnly.size(), 1);
        QCOMPARE(value(passwordOnly, QStringLiteral("password")), QStringLiteral("neu"));
    }

    void testPermissionMasks()
    {
        const SharePermissions all = allSharePermissions();
        // Dateien: kein Anlegen/Löschen
        QCOMPARE(ShareApi::maskForItem(all, false, all), SharePermission::Read | SharePermission::Update | SharePermission::Share);
        // nie mehr als die eigenen Rechte, Lesen immer
        QCOMPARE(ShareApi::maskForItem(all, true, SharePermission::Read | SharePermission::Share), SharePermission::Read | SharePermission::Share);
        QCOMPARE(ShareApi::maskForItem(SharePermissions(), true, all), SharePermissions(SharePermission::Read));

        const auto noResharing = sharingCapabilities({{QStringLiteral("default_permissions"), 31}, {QStringLiteral("resharing"), false}});
        QCOMPARE(ShareApi::defaultUserSharePermissions(noResharing, true, all),
            SharePermission::Read | SharePermission::Update | SharePermission::Create | SharePermission::Delete);
        const auto defaults = sharingCapabilities({{QStringLiteral("default_permissions"), 31}});
        QCOMPARE(ShareApi::defaultUserSharePermissions(defaults, false, all), SharePermission::Read | SharePermission::Update | SharePermission::Share);
    }

    void testCapabilities()
    {
        const QVariantMap publicCaps{{QStringLiteral("enabled"), true}, {QStringLiteral("upload"), true}, {QStringLiteral("supports_upload_only"), true},
            {QStringLiteral("multiple"), false},
            {QStringLiteral("password"),
                QVariantMap{{QStringLiteral("enforced"), false},
                    {QStringLiteral("enforced_for"),
                        QVariantMap{{QStringLiteral("read_only"), false}, {QStringLiteral("read_write"), true}, {QStringLiteral("upload_only"), true}}}}},
            {QStringLiteral("expire_date"), QVariantMap{{QStringLiteral("enabled"), true}, {QStringLiteral("days"), QStringLiteral("7")}, {QStringLiteral("enforced"), true}}}};
        const auto caps = sharingCapabilities({{QStringLiteral("api_enabled"), true}, {QStringLiteral("public"), publicCaps},
            {QStringLiteral("resharing"), false}, {QStringLiteral("search_min_length"), 3}});
        QVERIFY(caps.sharePublicLink());
        QVERIFY(caps.sharePublicLinkAllowUpload());
        QVERIFY(caps.sharePublicLinkSupportsUploadOnly());
        QVERIFY(!caps.sharePublicLinkMultiple());
        QVERIFY(!caps.shareResharing());
        QCOMPARE(caps.shareSearchMinLength(), 3);
        QVERIFY(caps.sharePublicLinkDefaultExpire());
        QVERIFY(caps.sharePublicLinkEnforceExpireDate());
        QCOMPARE(caps.sharePublicLinkDefaultExpireDateDays(), 7);

        QVERIFY(!ShareApi::linkPasswordEnforced(caps, SharePermission::Read));
        QVERIFY(ShareApi::linkPasswordEnforced(caps, SharePermission::Read | SharePermission::Create));
        // read_write_delete fehlt (Server vor 10.1): dann gilt read_write
        QVERIFY(ShareApi::linkPasswordEnforced(caps, SharePermission::Read | SharePermission::Update | SharePermission::Create | SharePermission::Delete));
        QVERIFY(ShareApi::linkPasswordEnforced(caps, SharePermission::Create));

        // ohne Angaben: großzügige Vorgaben, kein erzwungenes Passwort
        const auto empty = sharingCapabilities({});
        QVERIFY(empty.sharePublicLinkMultiple());
        QVERIFY(empty.shareResharing());
        QCOMPARE(empty.shareSearchMinLength(), 2);
        QVERIFY(!ShareApi::linkPasswordEnforced(empty, SharePermission::Read));
        QVERIFY(ShareApi::linkPasswordEnforced(
            sharingCapabilities({{QStringLiteral("public"), QVariantMap{{QStringLiteral("password"), QVariantMap{{QStringLiteral("enforced"), true}}}}}}),
            SharePermission::Read));
    }

    void testFetchShares()
    {
        FakeFolder fakeFolder({});
        Recorded recorded;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (!request.url().path().endsWith(QLatin1String("/ocs/v1.php/apps/files_sharing/api/v1/shares")))
                return nullptr;
            recorded = {QString::fromUtf8(HttpLogger::requestVerb(op, request)), request.url().path(), QUrlQuery(request.url()), {},
                request.rawHeader("OCS-APIREQUEST")};
            return new FakePayloadReply(op, request, ocsReply(QJsonArray{linkShareJson(), userShareJson()}), this);
        });

        ShareApi api(fakeFolder.account());
        QList<Share> fetched;
        connect(&api, &ShareApi::sharesFetched, this, [&fetched](const QList<Share> &shares) { fetched = shares; });
        QSignalSpy spy(&api, &ShareApi::sharesFetched);
        api.fetchShares(QStringLiteral("/Docs/Q4 & Plan.odt"));
        QVERIFY(spy.wait());

        QCOMPARE(recorded.verb, QStringLiteral("GET"));
        QCOMPARE(queryValue(recorded.urlQuery, QStringLiteral("path")), QStringLiteral("/Docs/Q4 & Plan.odt"));
        QCOMPARE(queryValue(recorded.urlQuery, QStringLiteral("reshares")), QStringLiteral("false"));
        QCOMPARE(queryValue(recorded.urlQuery, QStringLiteral("format")), QStringLiteral("json"));
        QCOMPARE(recorded.ocsHeader, QByteArrayLiteral("true"));
        QCOMPARE(fetched.size(), 2);
        QCOMPARE(fetched.at(1).shareWith, QStringLiteral("alice"));
    }

    void testLateFetchIsDropped()
    {
        // die erste Antwort kommt nach der zweiten an: die Liste muss beim jüngeren Stand bleiben
        FakeFolder fakeFolder({});
        int requests = 0;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (!request.url().path().endsWith(QLatin1String("/apps/files_sharing/api/v1/shares")))
                return nullptr;
            if (++requests == 1)
                return new DelayedReply<FakePayloadReply>(300ms, op, request, ocsReply(QJsonArray{linkShareJson()}), this);
            return new FakePayloadReply(op, request, ocsReply(QJsonArray{userShareJson()}), this);
        });

        ShareApi api(fakeFolder.account());
        QList<QList<Share>> emitted;
        connect(&api, &ShareApi::sharesFetched, this, [&emitted](const QList<Share> &shares) { emitted.append(shares); });
        api.fetchShares(QStringLiteral("/Team"));
        api.fetchShares(QStringLiteral("/Team"));
        QTRY_COMPARE(emitted.size(), 1);
        QTest::qWait(500);
        QCOMPARE(requests, 2);
        QCOMPARE(emitted.size(), 1);
        QCOMPARE(emitted.first().first().shareWith, QStringLiteral("alice"));
    }

    void testCreateLinkShare()
    {
        FakeFolder fakeFolder({});
        Recorded recorded;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *outgoingData) -> QNetworkReply * {
            if (!request.url().path().endsWith(QLatin1String("/apps/files_sharing/api/v1/shares")))
                return nullptr;
            recorded = {QString::fromUtf8(HttpLogger::requestVerb(op, request)), request.url().path(), QUrlQuery(request.url()), bodyQuery(outgoingData), {}};
            return new FakePayloadReply(op, request, ocsReply(linkShareJson()), this);
        });

        ShareApi api(fakeFolder.account());
        Share created;
        connect(&api, &ShareApi::shareCreated, this, [&created](const Share &share) { created = share; });
        QSignalSpy spy(&api, &ShareApi::shareCreated);
        LinkSettings settings;
        settings.name = QStringLiteral("Team drop");
        settings.permissions = SharePermission::Read | SharePermission::Update | SharePermission::Create | SharePermission::Delete;
        settings.passwordProtected = true;
        settings.password = QStringLiteral("a&b=c d+e");
        settings.expireDate = QDate(2026, 12, 24);
        api.createLinkShare(QStringLiteral("/Team"), settings);
        QVERIFY(spy.wait());

        QCOMPARE(recorded.verb, QStringLiteral("POST"));
        // Sonderzeichen im Passwort überstehen die Formular-Kodierung
        QCOMPARE(queryValue(recorded.body, QStringLiteral("password")), QStringLiteral("a&b=c d+e"));
        QCOMPARE(queryValue(recorded.body, QStringLiteral("shareType")), QStringLiteral("3"));
        QCOMPARE(queryValue(recorded.body, QStringLiteral("permissions")), QStringLiteral("15"));
        QCOMPARE(queryValue(recorded.body, QStringLiteral("expireDate")), QStringLiteral("2026-12-24"));
        QCOMPARE(queryValue(recorded.body, QStringLiteral("name")), QStringLiteral("Team drop"));
        QCOMPARE(queryValue(recorded.urlQuery, QStringLiteral("format")), QStringLiteral("json"));
        QCOMPARE(created.url, QStringLiteral("https://cloud.example/s/AbC"));
    }

    void testCreateUserShare()
    {
        FakeFolder fakeFolder({});
        Recorded recorded;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *outgoingData) -> QNetworkReply * {
            if (!request.url().path().endsWith(QLatin1String("/apps/files_sharing/api/v1/shares")))
                return nullptr;
            recorded = {QString::fromUtf8(HttpLogger::requestVerb(op, request)), request.url().path(), {}, bodyQuery(outgoingData), {}};
            return new FakePayloadReply(op, request, ocsReply(userShareJson()), this);
        });

        ShareApi api(fakeFolder.account());
        QSignalSpy spy(&api, &ShareApi::shareCreated);
        Sharee alice;
        alice.shareWith = QStringLiteral("alice");
        alice.type = Share::User;
        api.createShare(QStringLiteral("/Team"), alice, SharePermission::Read | SharePermission::Share);
        QVERIFY(spy.wait());
        QCOMPARE(recorded.verb, QStringLiteral("POST"));
        QCOMPARE(queryValue(recorded.body, QStringLiteral("shareWith")), QStringLiteral("alice"));
        QCOMPARE(queryValue(recorded.body, QStringLiteral("shareType")), QStringLiteral("0"));
        QCOMPARE(queryValue(recorded.body, QStringLiteral("permissions")), QStringLiteral("17"));
        QCOMPARE(queryValue(recorded.body, QStringLiteral("path")), QStringLiteral("/Team"));
    }

    void testServerMessageReachesCaller()
    {
        // eine Passwort-Richtlinie lehnt ab: OCS v1 meldet das mit HTTP 200 und statuscode 403
        FakeFolder fakeFolder({});
        Recorded recorded;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *outgoingData) -> QNetworkReply * {
            if (!request.url().path().contains(QLatin1String("/apps/files_sharing/api/v1/shares/")))
                return nullptr;
            recorded = {QString::fromUtf8(HttpLogger::requestVerb(op, request)), request.url().path(), {}, bodyQuery(outgoingData), {}};
            return new FakePayloadReply(op, request, ocsReply(QJsonArray{}, 403, QStringLiteral("The password must contain at least 8 characters")), this);
        });

        ShareApi api(fakeFolder.account());
        ShareApi::Operation failedOperation = ShareApi::Operation::Fetch;
        QString message;
        QString failedId;
        connect(&api, &ShareApi::requestFailed, this, [&](ShareApi::Operation operation, const QString &serverMessage, const QString &shareId) {
            failedOperation = operation;
            message = serverMessage;
            failedId = shareId;
        });
        QSignalSpy updated(&api, &ShareApi::shareUpdated);
        QSignalSpy failed(&api, &ShareApi::requestFailed);
        api.updateShare(QStringLiteral("17"), {{QStringLiteral("password"), QStringLiteral("kurz")}});
        QVERIFY(failed.wait());
        QCOMPARE(recorded.verb, QStringLiteral("PUT"));
        QVERIFY(recorded.path.endsWith(QLatin1String("/shares/17")));
        QCOMPARE(queryValue(recorded.body, QStringLiteral("password")), QStringLiteral("kurz"));
        QCOMPARE(failedOperation, ShareApi::Operation::Update);
        QCOMPARE(message, QStringLiteral("The password must contain at least 8 characters"));
        QCOMPARE(failedId, QStringLiteral("17"));
        QCOMPARE(updated.count(), 0);
    }

    void testUpdateAndDeleteShare()
    {
        FakeFolder fakeFolder({});
        QStringList verbs;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (!request.url().path().endsWith(QLatin1String("/apps/files_sharing/api/v1/shares/18")))
                return nullptr;
            const auto verb = QString::fromUtf8(HttpLogger::requestVerb(op, request));
            verbs.append(verb);
            if (verb == QLatin1String("DELETE"))
                return new FakePayloadReply(op, request, ocsReply(QJsonValue::Null), this);
            auto share = userShareJson();
            share.insert(QStringLiteral("permissions"), 1);
            return new FakePayloadReply(op, request, ocsReply(share), this);
        });

        ShareApi api(fakeFolder.account());
        Share updatedShare;
        connect(&api, &ShareApi::shareUpdated, this, [&updatedShare](const Share &share) { updatedShare = share; });
        QSignalSpy updated(&api, &ShareApi::shareUpdated);
        api.updateShare(QStringLiteral("18"), {{QStringLiteral("permissions"), QStringLiteral("1")}});
        QVERIFY(updated.wait());
        QCOMPARE(updatedShare.id, QStringLiteral("18"));
        QCOMPARE(updatedShare.permissions, SharePermissions(SharePermission::Read));

        QString deletedId;
        connect(&api, &ShareApi::shareDeleted, this, [&deletedId](const QString &shareId) { deletedId = shareId; });
        QSignalSpy deleted(&api, &ShareApi::shareDeleted);
        api.deleteShare(QStringLiteral("18"));
        QVERIFY(deleted.wait());
        QCOMPARE(deletedId, QStringLiteral("18"));
        QCOMPARE(verbs, QStringList({QStringLiteral("PUT"), QStringLiteral("DELETE")}));
    }

    void testSearchSharees()
    {
        FakeFolder fakeFolder({});
        QUrlQuery query;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (!request.url().path().endsWith(QLatin1String("/ocs/v1.php/apps/files_sharing/api/v1/sharees")))
                return nullptr;
            query = QUrlQuery(request.url());
            return new FakePayloadReply(op, request, ocsReply(shareesData()), this);
        });

        ShareApi api(fakeFolder.account());
        QString answeredSearch;
        QList<Sharee> found;
        connect(&api, &ShareApi::shareesFetched, this, [&](const QString &search, const QList<Sharee> &sharees) {
            answeredSearch = search;
            found = sharees;
        });
        QSignalSpy spy(&api, &ShareApi::shareesFetched);
        api.searchSharees(QStringLiteral("al"), true);
        QVERIFY(spy.wait());
        QCOMPARE(queryValue(query, QStringLiteral("search")), QStringLiteral("al"));
        QCOMPARE(queryValue(query, QStringLiteral("itemType")), QStringLiteral("folder"));
        QCOMPARE(answeredSearch, QStringLiteral("al"));
        QCOMPARE(found.size(), 4);
    }

    void testItemInfo_data()
    {
        QTest::addColumn<QString>("resourceType");
        QTest::addColumn<QString>("sharePermissions");
        QTest::addColumn<bool>("isFolder");
        QTest::addColumn<int>("maxPermissions");

        QTest::newRow("own file") << QString() << QStringLiteral("19") << false << 19;
        QTest::newRow("own folder") << QStringLiteral("<d:collection/>") << QStringLiteral("31") << true << 31;
        QTest::newRow("read-only reshare") << QStringLiteral("<d:collection/>") << QStringLiteral("1") << true << 1;
        // ältere Server ohne share-permissions: nichts sperren, der Server entscheidet
        QTest::newRow("no share-permissions") << QStringLiteral("<d:collection/>") << QString() << true << 31;
    }

    void testItemInfo()
    {
        QFETCH(QString, resourceType);
        QFETCH(QString, sharePermissions);
        QFETCH(bool, isFolder);
        QFETCH(int, maxPermissions);

        FakeFolder fakeFolder({});
        QByteArray requestBody;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *outgoingData) -> QNetworkReply * {
            // nur die eigene Abfrage beantworten, nicht etwa eine Verbindungsprüfung des Kontos
            if (HttpLogger::requestVerb(op, request) != "PROPFIND" || !request.url().path().endsWith(QLatin1String("/Docs/item")))
                return nullptr;
            requestBody = peekBody(outgoingData);
            const QString permissionsProp =
                sharePermissions.isEmpty() ? QString() : QStringLiteral("<ocs:share-permissions>%1</ocs:share-permissions>").arg(sharePermissions);
            const QString xml = QStringLiteral(
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                "<d:multistatus xmlns:d=\"DAV:\" xmlns:ocs=\"http://open-collaboration-services.org/ns\">"
                "<d:response><d:href>%1</d:href><d:propstat><d:prop><d:resourcetype>%2</d:resourcetype>%3</d:prop>"
                "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>")
                                    .arg(request.url().path(), resourceType, permissionsProp);
            return new MultiStatusReply(op, request, xml.toUtf8(), this);
        });

        ShareApi api(fakeFolder.account());
        bool receivedFolder = !isFolder;
        SharePermissions receivedPermissions;
        connect(&api, &ShareApi::itemInfoFetched, this, [&](bool folder, SharePermissions permissions) {
            receivedFolder = folder;
            receivedPermissions = permissions;
        });
        QSignalSpy spy(&api, &ShareApi::itemInfoFetched);
        api.fetchItemInfo(fakeFolder.account()->davUrl(), QStringLiteral("/Docs/item"));
        QVERIFY(spy.wait());
        QVERIFY(requestBody.contains("share-permissions"));
        QCOMPARE(receivedFolder, isFolder);
        QCOMPARE(receivedPermissions.toInt(), maxPermissions);
    }

    void testDirectDownloadUrl_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<int>("type");
        QTest::addColumn<int>("permissions");
        QTest::addColumn<QString>("expected");

        const int link = Share::Link;
        QTest::newRow("file link") << QStringLiteral("https://cloud.example/index.php/s/AbC") << link << 1
                                   << QStringLiteral("https://cloud.example/index.php/s/AbC/download");
        QTest::newRow("folder link with upload") << QStringLiteral("https://cloud.example/s/AbC/") << link << 5
                                                 << QStringLiteral("https://cloud.example/s/AbC/download");
        // Datei-Ablage: Empfänger dürfen nichts herunterladen
        QTest::newRow("upload only") << QStringLiteral("https://cloud.example/s/AbC") << link << 4 << QString();
        QTest::newRow("no public link address") << QStringLiteral("https://cloud.example/index.php/apps/files/?dir=/") << link << 1 << QString();
        QTest::newRow("token in query") << QStringLiteral("https://cloud.example/s/AbC?x=1") << link << 1 << QString();
        QTest::newRow("user share") << QStringLiteral("https://cloud.example/s/AbC") << int(Share::User) << 1 << QString();
        QTest::newRow("empty") << QString() << link << 1 << QString();
        QTest::newRow("not http") << QStringLiteral("ftp://cloud.example/s/AbC") << link << 1 << QString();
    }

    void testDirectDownloadUrl()
    {
        QFETCH(QString, url);
        QFETCH(int, type);
        QFETCH(int, permissions);
        QFETCH(QString, expected);

        Share share;
        share.type = type;
        share.url = url;
        share.permissions = SharePermissions::fromInt(permissions);
        QCOMPARE(ShareApi::directDownloadUrl(share).toString(), expected);
    }

    // „Per E-Mail senden“: Dateinamen mit & # % + dürfen die mailto-Adresse nicht zerlegen
    void testEmailComposerUrl()
    {
        const QString subject = QStringLiteral("I shared Q4 & #1 50%25 + mehr with you");
        const QString body = QStringLiteral("https://cloud.example/s/AbC?x=1&y=2");
        const QUrl url = Utility::emailComposerUrl(subject, body);
        QVERIFY(url.isValid());
        QCOMPARE(url.scheme(), QStringLiteral("mailto"));
        QVERIFY(!url.hasFragment());
        const QUrlQuery query(url);
        QCOMPARE(query.queryItems().size(), 2);
        QCOMPARE(query.queryItemValue(QStringLiteral("subject"), QUrl::FullyDecoded), subject);
        QCOMPARE(query.queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded), body);
    }

    void testPublicLinkNeedsUserInput()
    {
        const auto publicCaps = [](const QVariantMap &values) { return sharingCapabilities({{QStringLiteral("public"), values}}); };
        const auto passwordFor = [](const QString &role) {
            return QVariantMap{{QStringLiteral("password"), QVariantMap{{QStringLiteral("enforced_for"), QVariantMap{{role, true}}}}}};
        };
        QVERIFY(!PublicLinkCreator::needsUserInput(sharingCapabilities({})));
        QVERIFY(PublicLinkCreator::needsUserInput(publicCaps(passwordFor(QStringLiteral("read_only")))));
        // nur für Upload-Links vorgeschrieben: der Leselink kommt ohne Passwort aus
        QVERIFY(!PublicLinkCreator::needsUserInput(publicCaps(passwordFor(QStringLiteral("upload_only")))));
        QVERIFY(PublicLinkCreator::needsUserInput(publicCaps({{QStringLiteral("expire_date"),
            QVariantMap{{QStringLiteral("enabled"), true}, {QStringLiteral("days"), QStringLiteral("7")}, {QStringLiteral("enforced"), true}}}})));

        // ein vorgegebenes, nicht erzwungenes Ablaufdatum setzt der Server selbst: dann je Tag ein eigener Link
        const auto defaultExpire =
            publicCaps({{QStringLiteral("expire_date"), QVariantMap{{QStringLiteral("enabled"), true}, {QStringLiteral("days"), QStringLiteral("7")}}}});
        QVERIFY(!PublicLinkCreator::needsUserInput(defaultExpire));
        QCOMPARE(PublicLinkCreator::linkName(defaultExpire, QDate(2026, 10, 16)), QStringLiteral("Context menu link 2026-10-16"));
        QCOMPARE(PublicLinkCreator::linkName(sharingCapabilities({}), QDate(2026, 10, 16)), QStringLiteral("Context menu link"));
    }

    void testPublicLinkCreator_data()
    {
        QTest::addColumn<QJsonArray>("existing");
        QTest::addColumn<bool>("multipleLinks");
        QTest::addColumn<QStringList>("verbs");
        QTest::addColumn<QString>("copiedUrl");

        const QString name = PublicLinkCreator::linkName(sharingCapabilities({}), QDate::currentDate());
        const auto contextMenuLink = [&name](const QString &id, int permissions, bool withPassword, const QString &url) {
            auto share = publicLinkWithoutPassword();
            share.insert(QStringLiteral("id"), id);
            share.insert(QStringLiteral("name"), name);
            share.insert(QStringLiteral("permissions"), permissions);
            share.insert(QStringLiteral("url"), url);
            if (withPassword)
                share.insert(QStringLiteral("share_with"), QStringLiteral("$2y$10$passwordhash"));
            return share;
        };
        const QString created = QStringLiteral("https://cloud.example/s/New");
        const QStringList fetchAndCreate{QStringLiteral("GET"), QStringLiteral("POST")};
        const QStringList fetchOnly{QStringLiteral("GET")};

        // Nutzer-Shares und anders benannte Links zählen nicht
        QTest::newRow("no own link yet") << QJsonArray{userShareJson(), linkShareJson()} << true << fetchAndCreate << created;
        // ein im Dialog auf Bearbeiten gestellter Link wird übergangen, der unveränderte wiederverwendet
        QTest::newRow("reuses unchanged link") << QJsonArray{contextMenuLink(QStringLiteral("30"), 15, false, QStringLiteral("https://cloud.example/s/Edited")),
            contextMenuLink(QStringLiteral("31"), 1, false, QStringLiteral("https://cloud.example/s/Old"))}
                                               << true << fetchOnly << QStringLiteral("https://cloud.example/s/Old");
        // nachträglich mit Passwort versehen: ein neuer Link, sonst bekäme der Empfänger eine Passwortabfrage
        QTest::newRow("password added later") << QJsonArray{contextMenuLink(QStringLiteral("32"), 1, true, QStringLiteral("https://cloud.example/s/Locked"))}
                                              << true << fetchAndCreate << created;
        // nur ein Link je Element erlaubt: der vorhandene gilt
        QTest::newRow("single link server") << QJsonArray{linkShareJson()} << false << fetchOnly << QStringLiteral("https://cloud.example/s/AbC");
        QTest::newRow("single link server without link") << QJsonArray{userShareJson()} << false << fetchAndCreate << created;
    }

    void testPublicLinkCreator()
    {
        QFETCH(QJsonArray, existing);
        QFETCH(bool, multipleLinks);
        QFETCH(QStringList, verbs);
        QFETCH(QString, copiedUrl);

        // Arrange
        FakeFolder fakeFolder({});
        auto capabilities = TestUtils::testCapabilities();
        capabilities.insert(QStringLiteral("files_sharing"), QVariantMap{{QStringLiteral("public"), QVariantMap{{QStringLiteral("multiple"), multipleLinks}}}});
        fakeFolder.account()->setCapabilities({fakeFolder.account()->url(), capabilities});
        QStringList requestedVerbs;
        QUrlQuery fetchQuery;
        QUrlQuery createBody;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *outgoingData) -> QNetworkReply * {
            if (!request.url().path().endsWith(QLatin1String("/apps/files_sharing/api/v1/shares")))
                return nullptr;
            const QString verb = QString::fromUtf8(HttpLogger::requestVerb(op, request));
            requestedVerbs.append(verb);
            if (verb == QLatin1String("GET")) {
                fetchQuery = QUrlQuery(request.url());
                return new FakePayloadReply(op, request, ocsReply(existing), this);
            }
            createBody = bodyQuery(outgoingData);
            auto share = publicLinkWithoutPassword();
            share.insert(QStringLiteral("id"), QStringLiteral("40"));
            share.insert(QStringLiteral("name"), queryValue(createBody, QStringLiteral("name")));
            share.insert(QStringLiteral("url"), QStringLiteral("https://cloud.example/s/New"));
            return new FakePayloadReply(op, request, ocsReply(share), this);
        });

        // Act: Pfad ohne führenden Schrägstrich, wie ihn ein Aufrufer liefern könnte
        PublicLinkCreator creator(fakeFolder.account(), QStringLiteral("Docs/Q4 & Plan.odt"));
        QSignalSpy ready(&creator, &PublicLinkCreator::linkReady);
        QSignalSpy failed(&creator, &PublicLinkCreator::failed);
        creator.start();

        // Assert
        QVERIFY(ready.wait());
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.first().first().toString(), copiedUrl);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(requestedVerbs, verbs);
        QCOMPARE(queryValue(fetchQuery, QStringLiteral("path")), QStringLiteral("/Docs/Q4 & Plan.odt"));
        if (verbs.contains(QStringLiteral("POST"))) {
            // Standardrechte: nur lesen, ohne Passwort und ohne eigenes Ablaufdatum
            QCOMPARE(queryValue(createBody, QStringLiteral("path")), QStringLiteral("/Docs/Q4 & Plan.odt"));
            QCOMPARE(queryValue(createBody, QStringLiteral("shareType")), QStringLiteral("3"));
            QCOMPARE(queryValue(createBody, QStringLiteral("permissions")), QStringLiteral("1"));
            QCOMPARE(queryValue(createBody, QStringLiteral("name")), PublicLinkCreator::linkName(fakeFolder.account()->capabilities(), QDate::currentDate()));
            QVERIFY(!createBody.hasQueryItem(QStringLiteral("password")));
            QVERIFY(!createBody.hasQueryItem(QStringLiteral("expireDate")));
        }
    }

    void testPublicLinkCreatorReportsServerMessage()
    {
        // eine Passwort-Richtlinie, die der Server nicht in den Capabilities meldet, lehnt den Link ab
        FakeFolder fakeFolder({});
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (!request.url().path().endsWith(QLatin1String("/apps/files_sharing/api/v1/shares")))
                return nullptr;
            if (HttpLogger::requestVerb(op, request) == "GET")
                return new FakePayloadReply(op, request, ocsReply(QJsonArray{}), this);
            return new FakePayloadReply(op, request, ocsReply(QJsonArray{}, 403, QStringLiteral("Passwords are enforced for link shares")), this);
        });

        PublicLinkCreator creator(fakeFolder.account(), QStringLiteral("/Docs/item"));
        QSignalSpy ready(&creator, &PublicLinkCreator::linkReady);
        QSignalSpy failed(&creator, &PublicLinkCreator::failed);
        creator.start();

        QVERIFY(failed.wait());
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.first().first().toString(), QStringLiteral("Passwords are enforced for link shares"));
        QCOMPARE(ready.size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestShareApi)
#include "testshareapi.moc"
