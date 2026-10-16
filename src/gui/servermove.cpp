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
#include "servermove.h"

#include "accountmanager.h"
#include "accountstate.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
#include "common/utility.h"
#include "config/appconfig.h"
#include "creds/credentials.h"
#include "creds/oauth.h"
#include "folder.h"
#include "folderman.h"
#include "libsync/accessmanager.h"
#include "libsync/account.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QXmlStreamReader>

namespace OCC {

Q_LOGGING_CATEGORY(lcServerMove, "gui.servermove", QtInfoMsg)

namespace {

    constexpr auto transferTimeoutMs = 30 * 1000;

    int defaultPort(const QString &scheme)
    {
        return scheme == QLatin1String("https") ? 443 : 80;
    }

    QString displayUrl(const QUrl &url)
    {
        return url.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment).toDisplayString();
    }

    bool isRedirect(int httpStatus)
    {
        return httpStatus == 301 || httpStatus == 302 || httpStatus == 303 || httpStatus == 307 || httpStatus == 308;
    }
}

QUrl ServerMove::normalizeServerUrl(const QString &input, QString *error)
{
    Q_ASSERT(error);
    QString text = input.trimmed();
    if (text.isEmpty()) {
        *error = tr("Please enter the new address of your server.");
        return {};
    }
    if (!text.contains(QLatin1String("://"))) {
        text.prepend(QStringLiteral("https://"));
    }

    QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty()) {
        *error = tr("%1 is not a valid address.").arg(input.trimmed());
        return {};
    }
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http")) {
        *error = tr("The address must start with https://.");
        return {};
    }
    if (!url.userInfo().isEmpty()) {
        *error = tr("The address must not contain a user name or password.");
        return {};
    }
    if (url.hasQuery() || url.hasFragment()) {
        *error = tr("The address must not contain “?” or “#”.");
        return {};
    }

    url.setScheme(scheme);
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    for (const auto suffix : {QLatin1String("/status.php"), QLatin1String("/index.php")}) {
        if (path.endsWith(suffix)) {
            path.chop(suffix.size());
            break;
        }
    }
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    url.setPath(path);
    return url;
}

QUrl ServerMove::serverUrlFromStatusUrl(const QUrl &statusUrl)
{
    if (!statusUrl.isValid() || statusUrl.host().isEmpty()) {
        return {};
    }
    const QString statusSuffix = QStringLiteral("/status.php");
    QString path = statusUrl.path();
    if (!path.endsWith(statusSuffix)) {
        return {};
    }
    path.chop(statusSuffix.size());
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    QUrl serverUrl = statusUrl.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment);
    serverUrl.setPath(path);
    return serverUrl;
}

bool ServerMove::isSafeRedirect(const QUrl &from, const QUrl &to)
{
    if (!to.isValid() || to.host().isEmpty() || !to.userInfo().isEmpty()) {
        return false;
    }
    if (QString::compare(from.host(), to.host(), Qt::CaseInsensitive) != 0) {
        return false;
    }
    const QString fromScheme = from.scheme().toLower();
    const QString toScheme = to.scheme().toLower();
    if (toScheme != QLatin1String("https") && toScheme != QLatin1String("http")) {
        return false;
    }
    // nie von einer verschlüsselten zu einer unverschlüsselten Verbindung
    if (fromScheme == QLatin1String("https") && toScheme == QLatin1String("http")) {
        return false;
    }
    const int fromPort = from.port(defaultPort(fromScheme));
    const int toPort = to.port(defaultPort(toScheme));
    if (fromScheme == toScheme) {
        return fromPort == toPort;
    }
    // HTTP zu HTTPS: von den Standard-Ports oder auf demselben Port
    return (fromPort == 80 && toPort == 443) || fromPort == toPort;
}

QString ServerMove::instanceIdFromFileId(const QByteArray &fileId)
{
    // ownCloud 10: sprintf('%08d', fileid) . instanceid
    qsizetype digits = 0;
    while (digits < fileId.size() && fileId.at(digits) >= '0' && fileId.at(digits) <= '9') {
        ++digits;
    }
    if (digits < 8 || digits == fileId.size()) {
        return {};
    }
    const QByteArray instanceId = fileId.mid(digits);
    for (const char c : instanceId) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!allowed) {
            return {};
        }
    }
    return QString::fromLatin1(instanceId);
}

QString ServerMove::knownInstanceId(const QUuid &accountId)
{
    FolderMan *folderMan = FolderMan::instance();
    if (!folderMan) {
        return {};
    }
    for (Folder *folder : folderMan->foldersForAccount(accountId)) {
        if (!folder->spaceId().isEmpty() || !folder->journalDb()) {
            continue;
        }
        // die oberste Ebene genügt: alle Einträge tragen dieselbe Instanz-ID
        QString instanceId;
        folder->journalDb()->listFilesInPath(QByteArray(), [&instanceId](const SyncJournalFileRecord &record) {
            if (instanceId.isEmpty()) {
                instanceId = instanceIdFromFileId(record._fileId);
            }
        });
        if (!instanceId.isEmpty()) {
            return instanceId;
        }
    }
    return {};
}

QByteArray ServerMove::fileIdFromPropfind(const QByteArray &xml)
{
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() == QXmlStreamReader::StartElement && reader.namespaceUri() == QLatin1String("http://owncloud.org/ns")
            && reader.name() == QLatin1String("id")) {
            return reader.readElementText().trimmed().toUtf8();
        }
    }
    return {};
}

std::optional<QUrl> ServerMove::rebaseUrl(const QUrl &url, const QUrl &oldBase, const QUrl &newBase)
{
    const QString scheme = url.scheme().toLower();
    if (scheme != oldBase.scheme().toLower() || QString::compare(url.host(), oldBase.host(), Qt::CaseInsensitive) != 0
        || url.port(defaultPort(scheme)) != oldBase.port(defaultPort(scheme))) {
        return {};
    }
    const QString basePath = Utility::stripTrailingSlash(oldBase.path());
    const QString path = url.path();
    if (path != basePath && !path.startsWith(basePath + QLatin1Char('/'))) {
        return {};
    }
    QUrl rebased = newBase.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment);
    rebased.setPath(Utility::stripTrailingSlash(newBase.path()) + path.mid(basePath.size()));
    return rebased;
}

QString ServerMove::applyNewServerUrl(AccountState *accountState, const QUrl &newUrl, const QString &accessToken, const QString &refreshToken)
{
    if (!accountState || !accountState->account()) {
        return tr("The account no longer exists.");
    }
    Account *account = accountState->account();
    if (account->capabilities().spacesSupport().enabled) {
        return tr("The server address can only be changed for accounts without Spaces.");
    }
    const QUrl oldUrl = account->url();
    if (Utility::urlEqual(oldUrl, newUrl)) {
        return tr("This is already the address of the account.");
    }
    qCInfo(lcServerMove) << "moving account" << account->displayNameWithHost() << "from" << oldUrl << "to" << newUrl;

    // die alten Zugangsdaten liegen im Schlüsselbund unter dem alten Host: erst löschen, dann umstellen
    account->credentials()->forgetSensitiveData();
    account->setUrl(newUrl);

    if (FolderMan *folderMan = FolderMan::instance()) {
        folderMan->rebuildFoldersForAccount(accountState, [&oldUrl, &newUrl, account](FolderDefinition &definition) {
            if (const auto rebased = rebaseUrl(definition.webDavUrl(), oldUrl, newUrl)) {
                definition.setWebDavUrl(*rebased);
            } else {
                qCWarning(lcServerMove) << "unexpected WebDAV address" << definition.webDavUrl() << "for" << definition.localPath();
                definition.setWebDavUrl(account->davUrl());
            }
        });
    }

    // die neuen Token unter dem neuen Host speichern; schließt auch eine offene Neuanmeldung für die alte Adresse
    if (auto *credentials = qobject_cast<Credentials *>(account->credentials())) {
        credentials->replaceTokens(accessToken, refreshToken);
    }
    Q_EMIT account->wantsAccountSaved(account);

    if (accountState->isSignedOut()) {
        accountState->signIn();
    }
    accountState->checkConnectivity(true);
    return {};
}

ServerStatusCheck::ServerStatusCheck(QNetworkAccessManager *accessManager, QObject *parent)
    : QObject(parent)
    , _accessManager(accessManager)
{
}

void ServerStatusCheck::start(const QUrl &serverUrl)
{
    request(serverUrl, maxRedirects);
}

void ServerStatusCheck::request(const QUrl &serverUrl, int redirectsLeft)
{
    if (!_accessManager) {
        Result result;
        result.serverUrl = serverUrl;
        result.error = tr("No server could be reached at %1.").arg(displayUrl(serverUrl));
        Q_EMIT finished(result);
        return;
    }

    QNetworkRequest request(Utility::concatUrlPath(serverUrl, QStringLiteral("status.php")));
    // Weiterleitungen prüft handleReply() selbst: nie automatisch auf einen anderen Host
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(transferTimeoutMs);
    QNetworkReply *reply = _accessManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, serverUrl, redirectsLeft, this] { handleReply(reply, serverUrl, redirectsLeft); });
}

void ServerStatusCheck::handleReply(QNetworkReply *reply, const QUrl &serverUrl, int redirectsLeft)
{
    reply->deleteLater();

    Result result;
    result.serverUrl = serverUrl;
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QUrl location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();

    if (isRedirect(httpStatus) && location.isValid()) {
        const QUrl target = reply->url().resolved(location);
        if (!ServerMove::isSafeRedirect(reply->url(), target)) {
            qCWarning(lcServerMove) << "not following redirect from" << reply->url() << "to" << target;
            const QUrl targetServer = ServerMove::serverUrlFromStatusUrl(target);
            result.foreignRedirect = targetServer.isValid() ? targetServer : target.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment);
            result.error = tr("The address %1 redirects to another server (%2). For your security, this redirect is not followed automatically. "
                              "If you trust that server, enter its address directly.")
                               .arg(displayUrl(serverUrl), displayUrl(result.foreignRedirect));
            Q_EMIT finished(result);
            return;
        }
        const QUrl targetServer = ServerMove::serverUrlFromStatusUrl(target);
        if (!targetServer.isValid()) {
            result.error = tr("The address %1 redirects to %2, which is not the address of a server.").arg(displayUrl(serverUrl), displayUrl(target));
            Q_EMIT finished(result);
            return;
        }
        if (redirectsLeft <= 0) {
            result.error = tr("The address %1 redirects too often.").arg(displayUrl(serverUrl));
            Q_EMIT finished(result);
            return;
        }
        qCInfo(lcServerMove) << "following redirect from" << reply->url() << "to" << target;
        request(targetServer, redirectsLeft - 1);
        return;
    }

    if (reply->error() != QNetworkReply::NoError && httpStatus == 0) {
        result.error = tr("No server could be reached at %1: %2").arg(displayUrl(serverUrl), reply->errorString());
        Q_EMIT finished(result);
        return;
    }

    QJsonParseError parseError;
    const QJsonObject status = QJsonDocument::fromJson(reply->read(64 * 1024), &parseError).object();
    if (httpStatus != 200 || parseError.error != QJsonParseError::NoError || !status.contains(QStringLiteral("installed"))) {
        result.error = tr("No server was found at %1.").arg(displayUrl(serverUrl));
        Q_EMIT finished(result);
        return;
    }
    // der Server sendet true/false teils als Zeichenkette, daher über QVariant
    if (!status.value(QStringLiteral("installed")).toVariant().toBool()) {
        result.error = tr("The server at %1 is not set up yet.").arg(displayUrl(serverUrl));
        Q_EMIT finished(result);
        return;
    }
    if (status.value(QStringLiteral("maintenance")).toVariant().toBool() || status.value(QStringLiteral("needsDbUpgrade")).toVariant().toBool()) {
        result.error = tr("The server at %1 is in maintenance mode. Please try again later.").arg(displayUrl(serverUrl));
        Q_EMIT finished(result);
        return;
    }

    result.ok = true;
    result.version = status.value(QStringLiteral("versionstring")).toString();
    if (result.version.isEmpty()) {
        result.version = status.value(QStringLiteral("version")).toString();
    }
    Q_EMIT finished(result);
}

ServerMoveController::ServerMoveController(AccountState *accountState, QObject *parent)
    : QObject(parent)
    , _accountState(accountState)
{
    _accessManagerFactory = [accountState = QPointer<AccountState>(accountState)](QObject *owner) -> QNetworkAccessManager * {
        auto *accessManager = new AccessManager(owner);
        // selbst bestätigte Zertifikate des Kontos gelten weiter, die Zugangsdaten nicht
        if (accountState && accountState->account()) {
            accessManager->setCustomTrustedCaCertificates(accountState->account()->approvedCerts());
        }
        return accessManager;
    };
}

ServerMoveController::~ServerMoveController()
{
    reset();
}

void ServerMoveController::setAccessManagerFactory(const AccessManagerFactory &factory)
{
    _accessManagerFactory = factory;
}

QNetworkAccessManager *ServerMoveController::accessManager()
{
    if (!_accessManager) {
        _accessManager = _accessManagerFactory(this);
    }
    return _accessManager;
}

bool ServerMoveController::isBusy() const
{
    return _statusCheck || _oauth || _verificationReply || _applying;
}

void ServerMoveController::start(const QString &input)
{
    if (isBusy()) {
        return;
    }
    if (!_accountState || !_accountState->account()) {
        fail(ServerMove::tr("The account no longer exists."));
        return;
    }
    Account *account = _accountState->account();
    if (account->capabilities().spacesSupport().enabled) {
        fail(ServerMove::tr("The server address can only be changed for accounts without Spaces."));
        return;
    }

    QString error;
    const QUrl url = ServerMove::normalizeServerUrl(input, &error);
    if (!url.isValid()) {
        fail(error);
        return;
    }
    if (Utility::urlEqual(url, account->url())) {
        fail(ServerMove::tr("This is already the address of the account."));
        return;
    }
    if (account->url().scheme() == QLatin1String("https") && url.scheme() == QLatin1String("http")) {
        fail(tr("The new address must use an encrypted connection (https://), like the current one."));
        return;
    }

    _statusCheck = new ServerStatusCheck(accessManager(), this);
    connect(_statusCheck, &ServerStatusCheck::finished, this, &ServerMoveController::onStatusChecked);
    Q_EMIT progress(tr("Checking %1…").arg(displayUrl(url)));
    _statusCheck->start(url);
}

void ServerMoveController::onStatusChecked(const ServerStatusCheck::Result &result)
{
    if (_statusCheck) {
        _statusCheck->deleteLater();
        _statusCheck = nullptr;
    }
    if (!result.ok) {
        fail(result.error);
        return;
    }
    if (!_accountState || !_accountState->account()) {
        fail(ServerMove::tr("The account no longer exists."));
        return;
    }
    Account *account = _accountState->account();
    if (Utility::urlEqual(result.serverUrl, account->url())) {
        fail(ServerMove::tr("This is already the address of the account."));
        return;
    }
    // kein zweites Konto mit derselben Adresse und demselben Benutzer
    for (AccountState *other : AccountManager::instance()->accounts()) {
        if (other == _accountState || !other->account()) {
            continue;
        }
        if (Utility::urlEqual(other->account()->url(), result.serverUrl) && other->account()->davUser().compare(account->davUser(), Qt::CaseInsensitive) == 0) {
            fail(tr("Another account already uses this address with the same user."));
            return;
        }
    }

    _newUrl = result.serverUrl;
    startSignIn(result.serverUrl);
}

void ServerMoveController::startSignIn(const QUrl &newUrl)
{
    Account *account = _accountState->account();
    const QString davUser = account->davUser();
    AppConfig appConfig;
    // ein eigener Netzwerkzugang ohne die Zugangsdaten des Kontos; mit dem Benutzer prüft OAuth, dass sich derselbe anmeldet
    _oauth = new OAuth(newUrl, davUser, appConfig.openIdConfig(), accessManager(), this);
    connect(_oauth, &OAuth::authorisationLinkChanged, this, [this] {
        if (!_oauth) {
            return;
        }
        Q_EMIT signInLinkChanged(_oauth->authorisationLink());
        _oauth->openBrowser();
    });
    connect(_oauth, &OAuth::result, this, [davUser, this](OAuth::Result result, const QString &accessToken, const QString &refreshToken) {
        switch (result) {
        case OAuth::LoggedIn: {
            const QUrl url = _newUrl;
            // erst später löschen: das Signal kommt aus dem OAuth-Objekt selbst
            _oauth->disconnect(this);
            _oauth->deleteLater();
            _oauth = nullptr;
            Q_EMIT progress(tr("Checking that this is the same server and the same user…"));
            verifyAndApply(url, accessToken, refreshToken);
            break;
        }
        case OAuth::NotSupported:
            // der Browser ließ sich nicht öffnen; der Link bleibt gültig und lässt sich kopieren
            Q_EMIT progress(tr("The browser could not be opened. Copy the sign-in link and open it in your browser."));
            break;
        case OAuth::ErrorInsecureUrl:
            fail(tr("The sign-in page of the new address does not use a secure connection (HTTPS)."));
            break;
        case OAuth::ErrorIdPUnreachable:
            fail(tr("The sign-in service of the new address cannot be reached. Please try again later."));
            break;
        case OAuth::Error:
            fail(tr("Signing in at the new address failed. Please sign in with the same user as before: %1.").arg(davUser));
            break;
        }
    });
    Q_EMIT progress(tr("Please sign in at the new address in your browser, with the same user as before: %1.").arg(davUser));
    _oauth->startAuthentication();
}

void ServerMoveController::openBrowserAgain()
{
    if (_oauth && _oauth->isListening()) {
        _oauth->openBrowser();
    }
}

void ServerMoveController::verifyAndApply(const QUrl &newUrl, const QString &accessToken, const QString &refreshToken)
{
    if (!_accountState || !_accountState->account()) {
        fail(ServerMove::tr("The account no longer exists."));
        return;
    }
    Account *account = _accountState->account();

    QNetworkRequest request(Utility::concatUrlPath(newUrl, account->davPath()));
    request.setRawHeader(QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + accessToken.toUtf8());
    request.setRawHeader(QByteArrayLiteral("Depth"), QByteArrayLiteral("0"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/xml; charset=utf-8"));
    // den Token nie einer Weiterleitung hinterherschicken
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(transferTimeoutMs);
    static const QByteArray body = QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                                     "<d:propfind xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\"><d:prop><oc:id/></d:prop></d:propfind>");
    QNetworkReply *reply = accessManager()->sendCustomRequest(request, QByteArrayLiteral("PROPFIND"), body);
    _verificationReply = reply;
    connect(reply, &QNetworkReply::finished, this,
        [reply, newUrl, accessToken, refreshToken, this] { finishVerification(reply, newUrl, accessToken, refreshToken); });
}

void ServerMoveController::finishVerification(QNetworkReply *reply, const QUrl &newUrl, const QString &accessToken, const QString &refreshToken)
{
    reply->deleteLater();
    _verificationReply = nullptr;
    if (!_accountState || !_accountState->account()) {
        fail(ServerMove::tr("The account no longer exists."));
        return;
    }
    Account *account = _accountState->account();

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus == 401 || httpStatus == 403) {
        fail(tr("The new address did not accept the sign-in. The address was not changed."));
        return;
    }
    if (httpStatus == 404) {
        fail(tr("The user %1 does not exist at the new address. The address was not changed.").arg(account->davUser()));
        return;
    }
    if (isRedirect(httpStatus)) {
        fail(tr("The new address redirected the check to another address. The address was not changed."));
        return;
    }
    if (httpStatus != 207) {
        fail(tr("The new address could not be checked: %1").arg(reply->errorString()));
        return;
    }

    // Gleiche Instanz? Die Datei-IDs von ownCloud 10 enden auf die Instanz-ID. Ohne synchronisierte Einträge gibt es nichts
    // zu vergleichen, aber auch nichts, was ein fremder Server als gelöscht erscheinen lassen könnte.
    const QString knownInstance = ServerMove::knownInstanceId(account->uuid());
    if (!knownInstance.isEmpty()) {
        // die Antwort kommt von einem noch nicht bestätigten Server: nur so viel lesen, wie ein PROPFIND mit Tiefe 0 braucht
        const QString newInstance = ServerMove::instanceIdFromFileId(ServerMove::fileIdFromPropfind(reply->read(256 * 1024)));
        if (newInstance.isEmpty()) {
            fail(tr("The new address does not report which installation it belongs to, so it cannot be confirmed that it is the same server. "
                    "The address was not changed."));
            return;
        }
        if (newInstance != knownInstance) {
            qCWarning(lcServerMove) << "instance mismatch: known" << knownInstance << "new" << newInstance;
            fail(tr("The server at %1 is a different installation than the one this account was set up with. To protect your files, the address was "
                    "not changed.")
                     .arg(displayUrl(newUrl)));
            return;
        }
    }

    _applying = true;
    const QString error = ServerMove::applyNewServerUrl(_accountState, newUrl, accessToken, refreshToken);
    _applying = false;
    if (!error.isEmpty()) {
        fail(error);
        return;
    }
    reset();
    Q_EMIT succeeded(newUrl);
}

void ServerMoveController::cancel()
{
    reset();
}

void ServerMoveController::fail(const QString &message)
{
    qCWarning(lcServerMove) << "server move failed:" << message;
    reset();
    Q_EMIT failed(message);
}

void ServerMoveController::reset()
{
    if (_statusCheck) {
        _statusCheck->disconnect(this);
        _statusCheck->deleteLater();
        _statusCheck = nullptr;
    }
    if (_oauth) {
        // erst später löschen: reset() kann aus einem Signal des OAuth-Objekts heraus kommen
        _oauth->disconnect(this);
        _oauth->deleteLater();
        _oauth = nullptr;
    }
    if (_verificationReply) {
        _verificationReply->disconnect(this);
        _verificationReply->abort();
        _verificationReply->deleteLater();
        _verificationReply = nullptr;
    }
    _newUrl.clear();
}

}
