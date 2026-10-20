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
#include "creds/httpcredentials.h"
#include "creds/oauth.h"
#include "folder.h"
#include "folderman.h"
#include "libsync/abstractcorejob.h"
#include "libsync/accessmanager.h"
#include "libsync/account.h"
#include "networkadapters/serverdiagnostics.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QXmlStreamReader>

#include <utility>

namespace OCC {

Q_LOGGING_CATEGORY(lcServerMove, "gui.servermove", QtInfoMsg)

namespace {

    constexpr auto transferTimeoutMs = 30 * 1000;
    // dieselben Pfade wie der Assistent (DetermineAuthTypeAdapter)
    const QString authProbePathC = QStringLiteral("remote.php/webdav/");
    const QString openIdConfigurationPathC = QStringLiteral(".well-known/openid-configuration");

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

    // „//“ im Pfad (Eingabe oder Weiterleitung des Servers) zu „/“ zusammenfassen und „/“ am Ende entfernen:
    // Utility::concatUrlPath() verlangt Pfade ohne „//“ (Q_ASSERT, in Beta-Builds aktiv)
    QString cleanServerPath(const QString &path)
    {
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            return {};
        }
        return QLatin1Char('/') + parts.join(QLatin1Char('/'));
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
    // auch http nicht: Die Anmeldung (OAuth2) verlangt https und scheiterte sonst erst im Browser
    if (scheme != QLatin1String("https")) {
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
    // „:443“ ist dieselbe Adresse: sonst hielte Utility::urlEqual() sie für eine andere und es folgte ein sinnloser Umzug
    if (url.port() == defaultPort(scheme)) {
        url.setPort(-1);
    }
    QString path = cleanServerPath(url.path());
    for (const auto suffix : {QLatin1String("/status.php"), QLatin1String("/index.php")}) {
        if (path.endsWith(suffix)) {
            path.chop(suffix.size());
            break;
        }
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
    QString path = cleanServerPath(statusUrl.path());
    if (!path.endsWith(statusSuffix)) {
        return {};
    }
    path.chop(statusSuffix.size());
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
    return applyNewServerUrl(accountState, newUrl, SignIn::withTokens(accessToken, refreshToken));
}

QString ServerMove::applyNewServerUrl(AccountState *accountState, const QUrl &newUrl, const SignIn &signIn)
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
    // Die Verbindungen des Kontos (Abmelden, Anmeldedialog, Schlüsselbund-Hinweis) hängen am Credentials-Objekt: Es bleibt,
    // also auch seine Anmeldeart. Ein Konto mit Kennwort bekommt nur ein Kennwort, eines mit Browser-Anmeldung nur Token.
    const bool accountUsesPassword = qobject_cast<const HttpCredentials *>(account->credentials()) != nullptr;
    if ((signIn.type == AuthenticationType::Basic) != accountUsesPassword) {
        return accountUsesPassword
            ? tr("The new address signs in in the browser, but this account uses a user name and password. The address was not changed.")
            : tr("The new address asks for a user name and password, but this account signs in in the browser. The address was not changed.");
    }
    FolderMan *folderMan = FolderMan::instance();
    // ein laufender Ordner darf nicht umgebaut werden; ServerMoveController wartet vorher, bis alle Läufe beendet sind
    if (folderMan && folderMan->isAnySyncRunning(account->uuid())) {
        return tr("A synchronization of the account is still running. The address was not changed, please try again.");
    }
    qCInfo(lcServerMove) << "moving account" << account->displayNameWithHost() << "from" << oldUrl << "to" << newUrl;

    // Ab hier ohne Ereignisschleife, zuerst die Kontoadresse, dann die Ordnerpaare, zuletzt die neuen Token. Bricht der
    // Prozess dazwischen ab, verlangt der Client höchstens eine neue Anmeldung an der neuen Adresse.
    // Die alten Zugangsdaten liegen im Schlüsselbund unter dem alten Host: erst löschen, dann umstellen.
    account->credentials()->forgetSensitiveData();
    account->setUrl(newUrl);
    Q_EMIT account->wantsAccountSaved(account);

    // ohne laufenden Lauf (oben geprüft, seitdem keine Ereignisschleife) baut FolderMan immer um
    const bool rebuilt = !folderMan || folderMan->rebuildFoldersForAccount(accountState, [&oldUrl, &newUrl, account](FolderDefinition &definition) {
        if (const auto rebased = rebaseUrl(definition.webDavUrl(), oldUrl, newUrl)) {
            definition.setWebDavUrl(*rebased);
        } else {
            qCWarning(lcServerMove) << "unexpected WebDAV address" << definition.webDavUrl() << "for" << definition.localPath();
            definition.setWebDavUrl(account->davUrl());
        }
    });
    if (!rebuilt) {
        // die Ordnerpaare tragen noch die alte Adresse; ein erneuter Umzug stellt sie über rebaseUrl() um
        qCCritical(lcServerMove) << "the folders of" << account->displayNameWithHost() << "were not rebuilt for the new address";
    }

    // die neuen Zugangsdaten unter dem neuen Host speichern; schließt auch eine offene Neuanmeldung für die alte Adresse
    if (auto *oauthCredentials = qobject_cast<Credentials *>(account->credentials())) {
        oauthCredentials->replaceTokens(signIn.accessToken, signIn.refreshToken);
    } else if (auto *httpCredentials = qobject_cast<HttpCredentials *>(account->credentials())) {
        httpCredentials->replacePassword(signIn.password);
    }

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
    if (accountState && accountState->account()) {
        _trustedCertificates = accountState->account()->approvedCerts();
    }
}

ServerMoveController::~ServerMoveController()
{
    reset();
}

void ServerMoveController::setAccessManagerFactory(const AccessManagerFactory &factory)
{
    _accessManagerFactory = factory;
}

void ServerMoveController::setTlsPrompt(const TlsPrompt &prompt)
{
    _tlsPrompt = prompt;
}

QNetworkAccessManager *ServerMoveController::accessManager()
{
    if (!_accessManager) {
        _accessManager = _accessManagerFactory(this);
        connect(_accessManager, &QNetworkAccessManager::sslErrors, this, &ServerMoveController::onSslErrors);
    }
    return _accessManager;
}

void ServerMoveController::onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
    // bestätigen lassen sich nur Fehler mit Zertifikat (wie bei der Kontoeinrichtung); alles andere scheitert wie bisher
    bool allTrusted = true;
    for (const QSslError &error : errors) {
        if (error.certificate().isNull()) {
            return;
        }
        allTrusted = allTrusted && _trustedCertificates.contains(error.certificate());
    }
    if (allTrusted) {
        reply->ignoreSslErrors(errors);
        return;
    }
    // nicht hier fragen: ein Dialog mitten im Verbindungsaufbau bräuchte eine verschachtelte Ereignisschleife. Die Prüfung
    // scheitert, onStatusChecked() fragt danach und prüft dieselbe Adresse noch einmal.
    _pendingTlsErrors = errors;
    _pendingTlsHost = reply->url().host();
}

bool ServerMoveController::isBusy() const
{
    return _statusCheck || _authTypeReply || _oauth || _awaitingPassword || _verificationReply || _applying;
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

    startStatusCheck(url);
}

void ServerMoveController::startStatusCheck(const QUrl &url)
{
    _statusTarget = url;
    _pendingTlsErrors.clear();
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
    const QList<QSslError> tlsErrors = std::exchange(_pendingTlsErrors, {});
    if (!result.ok && !tlsErrors.isEmpty() && _tlsPrompt) {
        // unbekanntes Zertifikat (selbst signiert, eigene CA): wie bei der Kontoeinrichtung nachfragen. Die Rückfrage ist
        // modal; schließt sich dabei der Dialog samt Controller, darf danach nichts mehr auf Mitglieder zugreifen.
        QPointer<ServerMoveController> self(this);
        const TlsPrompt prompt = _tlsPrompt;
        const QString host = _pendingTlsHost;
        const bool trusted = prompt(tlsErrors, host);
        if (!self) {
            return;
        }
        if (trusted) {
            QSet<QSslCertificate> certificates;
            for (const QSslError &error : tlsErrors) {
                certificates.insert(error.certificate());
            }
            _trustedCertificates.unite(certificates);
            _confirmedCertificates.unite(certificates);
            // Anmeldung und Prüfung laufen über denselben Netzwerkzugang
            if (auto *ocAccessManager = qobject_cast<AccessManager *>(accessManager())) {
                ocAccessManager->addCustomTrustedCaCertificates(certificates);
            }
            qCInfo(lcServerMove) << "certificate of" << host << "confirmed, checking again";
            startStatusCheck(_statusTarget);
            return;
        }
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
    if (usesPassword(account)) {
        // Benutzername und Kennwort: erst prüfen, ob die neue Adresse das annimmt
        startAuthTypeCheck(result.serverUrl);
    } else {
        // Anmeldung im Browser wie bisher; nimmt die neue Adresse sie nicht an, meldet das die Anmeldung selbst
        startSignIn(result.serverUrl);
    }
}

void ServerMoveController::startAuthTypeCheck(const QUrl &newUrl)
{
    // wie der Assistent: ein PROPFIND ohne Zugangsdaten, die Ablehnung nennt das Verfahren
    QNetworkRequest request = AbstractCoreJobFactory::makeRequest(Utility::concatUrlPath(newUrl, authProbePathC));
    request.setAttribute(DontAddCredentialsAttribute, true);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(transferTimeoutMs);
    QNetworkReply *reply = accessManager()->sendCustomRequest(request, QByteArrayLiteral("PROPFIND"));
    _authTypeReply = reply;
    Q_EMIT progress(tr("Checking how to sign in at %1…").arg(displayUrl(newUrl)));
    connect(reply, &QNetworkReply::finished, this, [reply, newUrl, this] { finishAuthTypeCheck(reply, newUrl); });
}

void ServerMoveController::finishAuthTypeCheck(QNetworkReply *reply, const QUrl &newUrl)
{
    reply->deleteLater();
    _authTypeReply = nullptr;
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // nur eine Ablehnung (401) nennt das Verfahren; alles andere bleibt unklar (Unknown)
    const AuthenticationType type =
        httpStatus == 401 ? authenticationTypeFromChallenge(reply->rawHeader(QByteArrayLiteral("www-authenticate"))) : AuthenticationType::Unknown;
    qCInfo(lcServerMove) << "sign-in check of" << newUrl << "answered" << httpStatus << "->" << static_cast<int>(type);
    if (type == AuthenticationType::Basic) {
        probeOpenIdConnect(newUrl);
        return;
    }
    continueWithAuthType(type);
}

void ServerMoveController::probeOpenIdConnect(const QUrl &newUrl)
{
    // wie der Assistent: ohne Zugangsdaten, Weiterleitungen (etwa zu index.php) gelten
    QNetworkRequest request = AbstractCoreJobFactory::makeRequest(Utility::concatUrlPath(newUrl, openIdConfigurationPathC));
    request.setAttribute(DontAddCredentialsAttribute, true);
    request.setTransferTimeout(transferTimeoutMs);
    QNetworkReply *reply = accessManager()->get(request);
    _authTypeReply = reply;
    connect(reply, &QNetworkReply::finished, this, [reply, newUrl, this] {
        reply->deleteLater();
        _authTypeReply = nullptr;
        const bool openIdConnect = reply->error() == QNetworkReply::NoError && ServerDiagnostics::isOpenIdConfiguration(reply->read(64 * 1024));
        continueWithAuthType(openIdConnect ? AuthenticationType::OAuth : AuthenticationType::Basic);
    });
}

void ServerMoveController::continueWithAuthType(AuthenticationType type)
{
    if (!_accountState || !_accountState->account()) {
        fail(ServerMove::tr("The account no longer exists."));
        return;
    }
    // nur Konten mit Benutzername und Kennwort prüfen die Anmeldeart der neuen Adresse (onStatusChecked())
    switch (type) {
    case AuthenticationType::Basic:
        askForPassword();
        return;
    case AuthenticationType::OAuth:
        fail(ServerMove::tr("The new address signs in in the browser, but this account uses a user name and password. The address was not changed."));
        return;
    case AuthenticationType::Unknown:
        fail(tr("The new address does not accept a user name and password. The address was not changed."));
        return;
    }
}

QString ServerMoveController::loginUser() const
{
    if (!_accountState || !_accountState->account()) {
        return {};
    }
    const Account *account = _accountState->account();
    const QString login = account->credentials() ? account->credentials()->loginUser() : QString();
    return login.isEmpty() ? account->davUser() : login;
}

bool ServerMoveController::usesPassword(const Account *account)
{
    return account && qobject_cast<const HttpCredentials *>(account->credentials());
}

void ServerMoveController::askForPassword()
{
    const QString login = loginUser();
    _awaitingPassword = true;
    qCInfo(lcServerMove) << "the new address asks for user name and password, asking for the password of" << login;
    Q_EMIT progress(tr("Please enter the password of %1 for the new address.").arg(login));
    Q_EMIT passwordRequired(login);
}

void ServerMoveController::signInWithPassword(const QString &password)
{
    if (!_awaitingPassword || password.isEmpty()) {
        return;
    }
    _awaitingPassword = false;
    const QUrl url = _newUrl;
    Q_EMIT progress(tr("Checking that this is the same server and the same user…"));
    verifyAndApply(url, ServerMove::SignIn::withPassword(password));
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
    verifyAndApply(newUrl, ServerMove::SignIn::withTokens(accessToken, refreshToken));
}

void ServerMoveController::verifyAndApply(const QUrl &newUrl, const ServerMove::SignIn &signIn)
{
    if (!_accountState || !_accountState->account()) {
        fail(ServerMove::tr("The account no longer exists."));
        return;
    }
    Account *account = _accountState->account();

    QNetworkRequest request(Utility::concatUrlPath(newUrl, account->davPath()));
    if (signIn.type == AuthenticationType::Basic) {
        request.setRawHeader(QByteArrayLiteral("Authorization"), basicAuthorizationHeader(loginUser(), signIn.password));
    } else {
        request.setRawHeader(QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + signIn.accessToken.toUtf8());
    }
    request.setRawHeader(QByteArrayLiteral("Depth"), QByteArrayLiteral("0"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/xml; charset=utf-8"));
    // den Token nie einer Weiterleitung hinterherschicken
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(transferTimeoutMs);
    static const QByteArray body = QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                                     "<d:propfind xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\"><d:prop><oc:id/></d:prop></d:propfind>");
    QNetworkReply *reply = accessManager()->sendCustomRequest(request, QByteArrayLiteral("PROPFIND"), body);
    _verificationReply = reply;
    connect(reply, &QNetworkReply::finished, this, [reply, newUrl, signIn, this] { finishVerification(reply, newUrl, signIn); });
}

void ServerMoveController::finishVerification(QNetworkReply *reply, const QUrl &newUrl, const ServerMove::SignIn &signIn)
{
    reply->deleteLater();
    _verificationReply = nullptr;
    if (!_accountState || !_accountState->account()) {
        fail(ServerMove::tr("The account no longer exists."));
        return;
    }
    Account *account = _accountState->account();

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus == 401 && signIn.type == AuthenticationType::Basic) {
        // vertippt: noch einmal nach dem Kennwort fragen, statt den ganzen Umzug neu zu beginnen
        qCInfo(lcServerMove) << "the new address rejected the password, asking again";
        Q_EMIT progress(tr("The user name or password is not correct."));
        _awaitingPassword = true;
        Q_EMIT passwordRequired(loginUser());
        return;
    }
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

    // Gleiche Instanz? Die Datei-IDs von ownCloud 10 enden auf die Instanz-ID. Eine solche ID muss die neue Adresse immer
    // melden, auch wenn noch nichts synchronisiert wurde: sonst ginge etwa ein Infinite-Scale-Server (Datei-IDs
    // „storage$space!id“) durch, dessen Ordner nicht zu diesem Konto passen.
    // Die Antwort kommt von einem noch nicht bestätigten Server: nur so viel lesen, wie ein PROPFIND mit Tiefe 0 braucht.
    const QString newInstance = ServerMove::instanceIdFromFileId(ServerMove::fileIdFromPropfind(reply->read(256 * 1024)));
    if (newInstance.isEmpty()) {
        fail(tr("The new address does not report which installation it belongs to, so it cannot be confirmed that it is the same server. "
                "The address was not changed."));
        return;
    }
    const QString knownInstance = ServerMove::knownInstanceId(account->uuid());
    if (!knownInstance.isEmpty() && newInstance != knownInstance) {
        qCWarning(lcServerMove) << "instance mismatch: known" << knownInstance << "new" << newInstance;
        fail(tr("The server at %1 is a different installation than the one this account was set up with. To protect your files, the address was "
                "not changed.")
                 .arg(displayUrl(newUrl)));
        return;
    }

    // Erst umstellen, wenn kein Lauf des Kontos mehr läuft, und bevor sich etwas ändert: ohne verschachtelte Ereignisschleife
    // (Beenden über das Tray) und nie einen laufenden Ordner löschen
    _applying = true;
    if (FolderMan *folderMan = FolderMan::instance()) {
        folderMan->setSyncEnabled(false);
        _syncsHeld = true;
        for (Folder *folder : folderMan->foldersForAccount(account->uuid())) {
            if (folder->isSyncRunning()) {
                folder->slotTerminateSync(ServerMove::tr("change of the account's server address"));
            }
        }
    }
    _syncStopTime.start();
    _syncStopPoll = new QTimer(this);
    _syncStopPoll->setInterval(100);
    connect(_syncStopPoll, &QTimer::timeout, this, [newUrl, signIn, this] { applyWhenSyncsStopped(newUrl, signIn); });
    _syncStopPoll->start();
    applyWhenSyncsStopped(newUrl, signIn);
}

void ServerMoveController::applyWhenSyncsStopped(const QUrl &newUrl, const ServerMove::SignIn &signIn)
{
    if (!_accountState || !_accountState->account()) {
        fail(ServerMove::tr("The account no longer exists."));
        return;
    }
    FolderMan *folderMan = FolderMan::instance();
    if (folderMan && folderMan->isAnySyncRunning(_accountState->account()->uuid())) {
        if (_syncStopTime.elapsed() < syncStopTimeoutMs) {
            return;
        }
        fail(tr("The synchronization of the account could not be stopped. The address was not changed, please try again."));
        return;
    }
    // erst umstellen, dann die Synchronisation freigeben (reset() bzw. fail()): FolderMan::setSyncEnabled(true) startet
    // sofort den nächsten Lauf aus der Warteschlange, womöglich einen Ordner dieses Kontos
    const QString error = ServerMove::applyNewServerUrl(_accountState, newUrl, signIn);
    if (!error.isEmpty()) {
        fail(error);
        return;
    }
    // im Dialog bestätigte Zertifikate gelten danach für das Konto, sonst fragte dessen erste Verbindung gleich noch einmal
    // (die Verbindung startet erst in der Ereignisschleife, also nach diesem Aufruf)
    if (!_confirmedCertificates.isEmpty() && _accountState && _accountState->account()) {
        _accountState->account()->addApprovedCerts(_confirmedCertificates);
    }
    reset();
    Q_EMIT succeeded(newUrl);
}

void ServerMoveController::releaseSyncs()
{
    if (_syncStopPoll) {
        _syncStopPoll->stop();
        _syncStopPoll->deleteLater();
        _syncStopPoll = nullptr;
    }
    if (std::exchange(_syncsHeld, false)) {
        if (FolderMan *folderMan = FolderMan::instance()) {
            folderMan->setSyncEnabled(true);
        }
    }
    _applying = false;
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
    if (_authTypeReply) {
        _authTypeReply->disconnect(this);
        _authTypeReply->abort();
        _authTypeReply->deleteLater();
        _authTypeReply = nullptr;
    }
    if (_oauth) {
        // erst später löschen: reset() kann aus einem Signal des OAuth-Objekts heraus kommen
        _oauth->disconnect(this);
        _oauth->deleteLater();
        _oauth = nullptr;
    }
    _awaitingPassword = false;
    if (_verificationReply) {
        _verificationReply->disconnect(this);
        _verificationReply->abort();
        _verificationReply->deleteLater();
        _verificationReply = nullptr;
    }
    releaseSyncs();
    _newUrl.clear();
    // bestätigte Zertifikate bleiben bis zum Schließen des Dialogs: ein neuer Versuch fragt nicht noch einmal
    _pendingTlsErrors.clear();
}

}
