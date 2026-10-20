/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
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

#include "oauth.h"

#include "accessmanager.h"
#include "common/asserts.h"
#include "creds/credentialssupport.h"
#include "gui/networkadapters/userinfoadapter.h"
#include "libsync/creds/credentialmanager.h"
#include "networkjobs/checkserverjobfactory.h"
#include "oauthhtmlpage.h"
#include "theme.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRandomGenerator>

using namespace std::chrono;
using namespace std::chrono_literals;

using namespace OCC;

// todo: #24 - this is such a mess. it needs to parted out into clear classes (each in their own files), every impl needs to be reviewed.
// I think there is some duplication here as well - just sort it out as much as possible.
Q_LOGGING_CATEGORY(lcOauth, "sync.credentials.oauth", QtInfoMsg)

namespace {

const QString wellKnownPathC = QStringLiteral("/.well-known/openid-configuration");
const QString ownCloudOAuthAuthorizePathC = QStringLiteral("/index.php/apps/oauth2/authorize");
const QString ownCloudOAuthTokenPathC = QStringLiteral("/index.php/apps/oauth2/api/v1/token");
// owncloud.online: Theme::oauthLocalhost() ist upstream entfallen (DC-308); die oC10-OAuth2-App erwartet localhost
const QString ownCloudOAuthRedirectUrlC = QStringLiteral("http://localhost");

OAuth::PromptValuesSupportedFlags defaultOauthPromptValue(const OpenIdConfig& config)
{
    static const auto promptValue = [config] {
        auto prompt = config.prompt();
        OAuth::PromptValuesSupportedFlags out = OAuth::PromptValuesSupported::none;
        // convert the legacy openIdConnectPrompt() to QFlags
        for (const auto &x : prompt.split(QLatin1Char(' '))) {
            out |= Utility::stringToEnum<OAuth::PromptValuesSupported>(x);
        }
        return out;
    }();
    return promptValue;
}

seconds defaultTimeout()
{
    // as the OAuth process can be interactive we don't want 5min of inactivity
    return qMin(30s, OCC::AbstractNetworkJob::httpTimeout);
}

int defaultTimeoutMs()
{
    return static_cast<int>(duration_cast<milliseconds>(defaultTimeout()).count());
}

// Anfragen an den Token-Endpunkt nicht nach 30 s abbrechen: Der Server tauscht die Tokens womöglich trotzdem aus (eine
// Bremse gegen Anmeldeversuche wartet bis zu 30 s, eine gesperrte Datenbank länger), die Antwort ginge verloren und
// mit ihr das neue Refresh-Token. Länger als 120 s aber auch nicht: Die oauth2-App ab 0.10.0 nimmt das vorige
// Refresh-Token noch 180 s an, die Wiederholung nach einer Zeitüberschreitung (30 s Pause) muss da hineinpassen.
int tokenRequestTimeoutMs()
{
    return static_cast<int>(duration_cast<milliseconds>(120s).count());
}

// Anfragen an .well-known und den Token-Endpunkt ohne Cookies des Kontos: Mit noch angemeldeter Sitzung wertet der
// Kern Client-Kennung und -Geheimnis als Anmeldeversuch eines Benutzers („Login failed“) und bremst weitere aus
void withoutCookies(QNetworkRequest &request)
{
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
}

// expires_in kommt je nach Server als Zahl oder als Zeichenkette
qint64 secondsValue(const QJsonValue &value)
{
    if (value.isDouble()) {
        return value.toInteger();
    }
    bool ok = false;
    const qint64 result = value.toString().toLongLong(&ok);
    return ok ? result : 0;
}

QVariant getRequiredField(const QVariantMap &json, const QString &s, QString *error)
{
    const auto out = json.constFind(s);
    if (out == json.constEnd()) {
        error->append(QStringLiteral("\tError: Missing field %1\n").arg(s));
        return {};
    }
    return *out;
}
}

OAuth::OAuth(const QUrl &serverUrl, const QString &davUser, const OpenIdConfig& openIdConfig, QNetworkAccessManager *networkAccessManager, QObject *parent)
    : QObject(parent)
    , _serverUrl(serverUrl)
    , _davUser(davUser)
    , _openIdConfig(openIdConfig)
    , _networkAccessManager(networkAccessManager)
    , _redirectUrl(QString("http://localhost"))
    , _supportedPromptValues(defaultOauthPromptValue(openIdConfig))
{
}

OAuth::~OAuth() = default;

void OAuth::startAuthentication()
{
    qCDebug(lcOauth) << "starting authentication";

    // Listen on the socket to get a port which will be used in the redirect_uri
    if (!listen()) {
        qCDebug(lcOauth) << "server is not listening";
        Q_EMIT result(Error, {});
        return;
    }

    _pkceCodeVerifier = generateRandomString(24);
    OC_ASSERT(_pkceCodeVerifier.size() == 128);
    _state = generateRandomString(8);

    connect(this, &OAuth::fetchWellKnownFinished, this, &OAuth::authorisationLinkChanged);

    fetchWellKnown();

    QObject::connect(&_server, &QTcpServer::newConnection, this, &OAuth::handleTcpConnection);
    QObject::connect(&_serverV6, &QTcpServer::newConnection, this, &OAuth::handleTcpConnection);
}

bool OAuth::listen()
{
    // Browser lösen localhost je nach System zuerst als ::1 oder als 127.0.0.1 auf und fallen nicht immer auf die
    // andere Adresse zurück: deshalb auf beiden mit demselben Port lauschen
    const QList<quint16> ports = _openIdConfig.ports();
    for (const quint16 port : ports) {
        // Port 0 wählt das System; ist der gewählte Port unter ::1 schon vergeben, mit einem anderen erneut versuchen
        const int attempts = port == 0 ? 10 : 1;
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            if (!_server.listen(QHostAddress::LocalHost, port)) {
                qCDebug(lcOauth) << "Creating local server on 127.0.0.1, port" << port << "failed. Error:" << _server.errorString();
                break;
            }
            if (_serverV6.listen(QHostAddress::LocalHostIPv6, _server.serverPort())) {
                qCInfo(lcOauth) << "Waiting for the browser on 127.0.0.1 and [::1], port" << _server.serverPort();
                return true;
            }
            // Nur ohne IPv6-Loopback (Protokoll fehlt oder ::1 ist nicht vergeben) reicht IPv4 wie bisher: Dann löst auch
            // kein Browser localhost als ::1 auf. Jeder andere Fehler (Port belegt, unter Windows exklusiv gebunden oder
            // reserviert, Ressourcen knapp) betrifft nur diesen Port; ein Browser, der localhost als ::1 auflöst, landete
            // sonst bei einem anderen Programm oder im Leeren: anderen Port nehmen.
            const auto v6Error = _serverV6.serverError();
            if (v6Error == QAbstractSocket::UnsupportedSocketOperationError || v6Error == QAbstractSocket::SocketAddressNotAvailableError) {
                qCInfo(lcOauth) << "Waiting for the browser on 127.0.0.1, port" << _server.serverPort() << "- no IPv6 loopback:" << _serverV6.errorString();
                return true;
            }
            qCInfo(lcOauth) << "Cannot listen on [::1], port" << _server.serverPort() << "-" << _serverV6.errorString();
            _server.close();
        }
    }
    // Kein Port ließ sich auf beiden Adressen öffnen: als letzter Ausweg nur IPv4, wie vor dem Lauschen auf ::1
    for (const quint16 port : ports) {
        if (_server.listen(QHostAddress::LocalHost, port)) {
            qCWarning(lcOauth) << "Waiting for the browser on 127.0.0.1 only, port" << _server.serverPort() << "- [::1] failed for every port";
            return true;
        }
    }
    return false;
}

bool OAuth::isListening() const
{
    return _server.isListening();
}

void OAuth::httpReplyAndClose(QPointer<QTcpSocket> socket, bool success, const QString &code, const QString &title, const QString &body, const QStringList &additionalHeader)
{
    if (!socket) {
        return; // socket can have been deleted if the browser was closed
    }

    const QByteArray content = OAuthHtmlPage::buildPage(success, title, body.isEmpty() ? title : body).toUtf8();
    QString header = QStringLiteral("HTTP/1.1 %1\r\n"
                                    "Content-Type: text/html; charset=utf-8\r\n"
                                    "Connection: close\r\n"
                                    "Content-Length: %2\r\n")
                         .arg(code, QString::number(content.length()));

    if (!additionalHeader.isEmpty()) {
        const QString nl = QStringLiteral("\r\n");
        header += additionalHeader.join(nl) + nl;
    }

    const QByteArray msg = header.toUtf8() + QByteArrayLiteral("\r\n") + content;

    qCDebug(lcOauth) << "replying with HTTP response and closing socket:" << msg;

    socket->write(msg);
    socket->disconnectFromHost();
    // this is super important as we may delete the oauth/server before the socket has completed its work
    // let the deleteLater on final disconnect take care of the cleanup
    socket->setParent(nullptr);
}

void OAuth::finalize(const QString &accessToken, const QString &refreshToken, const QUrl &messageUrl)
{
    const QString brand = Theme::instance()->appNameGUI();
    const QString loginSuccessfulTitle = tr("Successfully signed in");
    const QString loginSuccessfulHtml = tr("Now, explore %1 on desktop.").arg(brand);

    if (messageUrl.isValid()) {
        httpReplyAndClose(_socket, true, QStringLiteral("303 See Other"), loginSuccessfulTitle, loginSuccessfulHtml,
            {QStringLiteral("Location: %1").arg(QString::fromUtf8(messageUrl.toEncoded()))});
    } else {
        httpReplyAndClose(_socket, true, QStringLiteral("200 OK"), loginSuccessfulTitle, loginSuccessfulHtml);
    }
    Q_EMIT result(LoggedIn, accessToken, refreshToken, _expiresIn);
}

// the tcp server handling has to be done async while we wait for a suitable socket with which to communiciate with the
// authentication web page. The async handling for tcp includes this function + the handleSocketReadyRead
void OAuth::handleTcpConnection()
{
    for (QTcpServer *server : {&_server, &_serverV6}) {
        while (server->hasPendingConnections()) {
            auto socket = server->nextPendingConnection();

            qCDebug(lcOauth) << "accepted client connection from" << socket->peerAddress();

            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);

            QObject::connect(socket, &QIODevice::readyRead, this, &OAuth::handleSocketReadyRead);
        }
    }
}

void OAuth::handleSocketReadyRead()
{
    // we already have the socket we want to use to complete the auth communications so ignore any other incoming
    // sockets
    if (!_server.isListening())
        return;

    QTcpSocket *connected = qobject_cast<QTcpSocket *>(sender());
    if (!connected) {
        // something is really wrong
        return;
    }
    QPointer<QTcpSocket> candidateSocket(connected);

    Q_ASSERT(connected->state() == QAbstractSocket::ConnectedState);

    const QByteArray peek = candidateSocket->peek(qMin(connected->bytesAvailable(), 4000LL)); // The code should always be within the first 4K

    // wait until we find a \n
    if (!peek.contains('\n')) {
        return;
    }

    qCDebug(lcOauth) << "Server provided:" << peek;

    const auto getPrefix = QByteArrayLiteral("GET /?");
    if (!peek.startsWith(getPrefix)) {
        httpReplyAndClose(candidateSocket, false, QStringLiteral("404 Not Found"), QStringLiteral("404 Not Found"));
        return;
    }
    const auto endOfUrl = peek.indexOf(' ', getPrefix.length());

    _queryArgs = QUrlQuery(QUrl::fromPercentEncoding(peek.mid(getPrefix.length(), endOfUrl - getPrefix.length())));

    if (_queryArgs.queryItemValue(QStringLiteral("state")).toUtf8() != _state) {
        httpReplyAndClose(candidateSocket, false, QStringLiteral("400 Bad Request"), QStringLiteral("400 Bad Request"));
        return;
    }

    // basic checks passed, we now want to commit to this socket all other pending connections will effectively be
    // ignored from here out. To ensure we get as few stray connections as possible, just close the server
    _socket = candidateSocket;

    // server port cannot be queried any more after server has been closed, which we want to do as early as possible in the processing chain
    // therefore we have to store it beforehand
    _serverPort = _server.serverPort();
    _server.close();
    _serverV6.close();
    qCInfo(lcOauth) << "Browser callback received, stopped listening on port" << _serverPort;

    getTokens();
}

// todo: #24 - this can be converted to an adapter
void OAuth::getTokens()
{
    if(_tokenEndpoint.isEmpty()) {
        Q_EMIT result(Error);
        return;
    }

    auto postTokenReply = postTokenRequest({
        {QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
        {QStringLiteral("code"), _queryArgs.queryItemValue(QStringLiteral("code"))},
        {QStringLiteral("redirect_uri"), QStringLiteral("%1:%2").arg(_redirectUrl, QString::number(_serverPort))},
        {QStringLiteral("code_verifier"), QString::fromUtf8(_pkceCodeVerifier)},
    });

    connect(postTokenReply, &QNetworkReply::finished, this, [postTokenReply, this] {
        const auto jsonData = postTokenReply->readAll();
        QJsonParseError jsonParseError;
        const QJsonObject json = QJsonDocument::fromJson(jsonData, &jsonParseError).object();
        const auto data = json.toVariantMap();
        QString fieldsError;
        _accessToken = getRequiredField(data, QStringLiteral("access_token"), &fieldsError).toString();
        _refreshToken = getRequiredField(data, QStringLiteral("refresh_token"), &fieldsError).toString();
        // optional (RFC 6749, Abschnitt 5.1); ohne Angabe erneuern die Credentials erst nach einer Ablehnung
        _expiresIn = secondsValue(json.value(QStringLiteral("expires_in")));
        const QString tokenType = getRequiredField(data, QStringLiteral("token_type"), &fieldsError).toString().toLower();
        _messageUrl = QUrl::fromEncoded(data[QStringLiteral("message_url")].toByteArray());

        if (postTokenReply->error() != QNetworkReply::NoError || jsonParseError.error != QJsonParseError::NoError || !fieldsError.isEmpty()
            || tokenType != QLatin1String("bearer")) {
            // do we have error message suitable for users?
            QString errorReason = data[QStringLiteral("error_description")].toString();
            if (errorReason.isEmpty()) {
                // fall back to technical error
                errorReason = data[QStringLiteral("error")].toString();
            }
            if (!errorReason.isEmpty()) {
                errorReason = tr("Error returned from the server: <em>%1</em>").arg(errorReason.toHtmlEscaped());
            } else if (postTokenReply->error() != QNetworkReply::NoError) {
                errorReason = tr("There was an error accessing the 'token' endpoint: <br><em>%1</em>").arg(postTokenReply->errorString().toHtmlEscaped());
            } else if (jsonParseError.error != QJsonParseError::NoError) {
                errorReason = tr("Could not parse the JSON returned from the server: <br><em>%1</em>").arg(jsonParseError.errorString());
            } else if (tokenType != QStringLiteral("bearer")) {
                errorReason = tr("Unsupported token type: %1").arg(tokenType);
            } else if (!fieldsError.isEmpty()) {
                errorReason = tr("The reply from the server did not contain all expected fields\n:%1").arg(fieldsError);
            } else {
                errorReason = tr("Unknown Error");
            }
            qCWarning(lcOauth) << "Error when getting the accessToken" << errorReason;
            httpReplyAndClose(_socket, false, QStringLiteral("500 Internal Server Error"), tr("Login Error"), errorReason);
            Q_EMIT result(Error);
            return;
        }

        // this check is important when reauthenticating an existing account, where a davUser is already defined.
        // In this context, the check just validates that the user had not authenticated with a different username from the account
        if (!_davUser.isEmpty()) {
            checkUserInfo();
        } else { // davUser is empty so we are done:
            finalize(_accessToken, _refreshToken, _messageUrl);
        }
    });
}

void OAuth::checkUserInfo()
{
    Q_ASSERT(!_davUser.isEmpty());

    UserInfoAdapter infoAdapter(_networkAccessManager, _accessToken, _serverUrl);
    const UserInfoResult infoResult = infoAdapter.getResult();

    if (!infoResult.success()) {
        httpReplyAndClose(_socket, false, QStringLiteral("500 Internal Server Error"), tr("Login Error"), infoResult.error);
        Q_EMIT result(Error);
    } else {
        if (infoResult.userId.compare(_davUser, Qt::CaseInsensitive) != 0) {
            // Connected with the wrong user
            qCWarning(lcOauth) << "We expected the user" << _davUser << "but the server answered with user" << infoResult.userId;
            const QString message = tr("You logged-in with user <em>%1</em>, but must login with user <em>%2</em>.<br>"
                                       "Please return to the %3 client and restart the authentication.")
                                        .arg(infoResult.userId, _davUser, Theme::instance()->appNameGUI());
            httpReplyAndClose(_socket, false, QStringLiteral("403 Forbidden"), tr("Wrong user"), message);
            Q_EMIT result(Error);
        } else {
            finalize(_accessToken, _refreshToken, _messageUrl);
        }
    }
}

// todo: #24 - I think this should also be an adapter. I also have a vague recollection that we have a job that does this already
QNetworkReply *OAuth::postTokenRequest(QUrlQuery &&queryItems)
{
    const QUrl requestTokenUrl = _tokenEndpoint;
    QNetworkRequest req;
    req.setTransferTimeout(tokenRequestTimeoutMs());
    withoutCookies(req);
    // eine Weiterleitung (etwa auf die Anmeldeseite, wenn die oauth2-App fehlt) ist keine Antwort des Token-Endpunkts
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    switch (_endpointAuthMethod) {
    case TokenEndpointAuthMethods::client_secret_basic:
        req.setRawHeader("Authorization", "Basic " + QStringLiteral("%1:%2").arg(_openIdConfig.clientId(), _openIdConfig.clientSecret()).toUtf8().toBase64());
        break;
    case TokenEndpointAuthMethods::client_secret_post:
        queryItems.addQueryItem(QStringLiteral("client_id"), _openIdConfig.clientId());
        queryItems.addQueryItem(QStringLiteral("client_secret"), _openIdConfig.clientSecret());
        break;
    }
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
    req.setAttribute(DontAddCredentialsAttribute, true);

    queryItems.addQueryItem(QStringLiteral("scope"), QString::fromUtf8(QUrl::toPercentEncoding(_openIdConfig.scopes())));
    req.setUrl(requestTokenUrl);
    return _networkAccessManager->post(req, queryItems.toString(QUrl::FullyEncoded).toUtf8());
}

QByteArray OAuth::generateRandomString(size_t size) const
{
    // TODO: do we need a variable size?
    std::vector<quint32> buffer(size, 0);
    QRandomGenerator::global()->fillRange(buffer.data(), static_cast<qsizetype>(size));
    return QByteArray(reinterpret_cast<char *>(buffer.data()), static_cast<int>(size * sizeof(quint32))).toBase64(QByteArray::Base64UrlEncoding);
}

QUrl OAuth::authorisationLink() const
{
    Q_ASSERT(_server.isListening());
    Q_ASSERT(_wellKnownFinished);

    if (!_authEndpoint.isValid()) {
        return {}; // no auth endpoint, cannot continue
    }

    const QByteArray code_challenge =
        QCryptographicHash::hash(_pkceCodeVerifier, QCryptographicHash::Sha256).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    QUrlQuery query{{QStringLiteral("response_type"), QStringLiteral("code")}, {QStringLiteral("client_id"), _openIdConfig.clientId()},
        {QStringLiteral("redirect_uri"), QStringLiteral("%1:%2").arg(_redirectUrl, QString::number(_server.serverPort()))},
        {QStringLiteral("code_challenge"), QString::fromLatin1(code_challenge)}, {QStringLiteral("code_challenge_method"), QStringLiteral("S256")},
        {QStringLiteral("scope"), QString::fromUtf8(QUrl::toPercentEncoding(_openIdConfig.scopes()))},
        {QStringLiteral("prompt"), QString::fromUtf8(QUrl::toPercentEncoding(toString(_supportedPromptValues)))},
        {QStringLiteral("state"), QString::fromUtf8(_state)}};

    if (!_davUser.isEmpty()) {
        const QString davUser = QString::fromUtf8(QUrl::toPercentEncoding(_davUser)); // Issue #7762;
        // open id connect
        query.addQueryItem(QStringLiteral("login_hint"), davUser);
        // todo: #20 oc10 as fallback!
        query.addQueryItem(QStringLiteral("user"), davUser);
    }
    return Utility::concatUrlPath(_authEndpoint, {}, query);
}

void OAuth::fetchWellKnown()
{
    qCDebug(lcOauth) << "fetching" << wellKnownPathC;

    auto useOwnCloudOAuthFallback = [this]() {
        _authEndpoint = Utility::concatUrlPath(_serverUrl, ownCloudOAuthAuthorizePathC);
        _tokenEndpoint = Utility::concatUrlPath(_serverUrl, ownCloudOAuthTokenPathC);
        _redirectUrl = ownCloudOAuthRedirectUrlC;
        _endpointAuthMethod = TokenEndpointAuthMethods::client_secret_basic;
        _usesOpenIdConnect = false;
    };

    QNetworkRequest req;
    req.setAttribute(DontAddCredentialsAttribute, true);
    req.setUrl(Utility::concatUrlPath(_serverUrl, wellKnownPathC));
    req.setTransferTimeout(defaultTimeoutMs());
    withoutCookies(req);

    auto reply = _networkAccessManager->get(req);

    QObject::connect(reply, &QNetworkReply::finished, this, [reply, this, useOwnCloudOAuthFallback] {
        _wellKnownFinished = true;
        if (reply->error() != QNetworkReply::NoError) {
            qCDebug(lcOauth) << "failed to fetch .well-known reply, error:" << reply->error();
            // Modified by BW-Tech GmbH: ownCloud 10 / owncloud.online OAuth2
            // exposes no OpenID .well-known document. Use the legacy OAuth2
            // routes so branded clients can still authenticate.
            useOwnCloudOAuthFallback();
            Q_EMIT fetchWellKnownFinished();
            return;
        }
        QJsonParseError err = {};
        QJsonObject data = QJsonDocument::fromJson(reply->readAll(), &err).object();
        if (err.error == QJsonParseError::NoError) {
            _authEndpoint = QUrl::fromEncoded(data[QStringLiteral("authorization_endpoint")].toString().toUtf8());
            _tokenEndpoint = QUrl::fromEncoded(data[QStringLiteral("token_endpoint")].toString().toUtf8());
            _redirectUrl = ownCloudOAuthRedirectUrlC;
            // ohne brauchbare Endpunkte setzt der Rückfall unten das zurück
            _usesOpenIdConnect = true;

            const auto authMethods = data.value(QStringLiteral("token_endpoint_auth_methods_supported")).toArray();
            if (authMethods.contains(QStringLiteral("client_secret_basic"))) {
                _endpointAuthMethod = TokenEndpointAuthMethods::client_secret_basic;
            } else if (authMethods.contains(QStringLiteral("client_secret_post"))) {
                _endpointAuthMethod = TokenEndpointAuthMethods::client_secret_post;
            } else {
                // Die Angabe ist optional, ohne sie gilt client_secret_basic (OpenID Connect Discovery 1.0, Abschnitt 3). Auch bei
                // unbekannten Verfahren damit versuchen, statt abzubrechen: das OC_ASSERT_X hier beendete mit FORCE_ASSERTS (Beta)
                // das Programm schon wegen dieser Serverantwort
                if (!authMethods.isEmpty()) {
                    qCWarning(lcOauth) << "Unsupported token_endpoint_auth_methods_supported" << authMethods << "- trying client_secret_basic";
                }
                _endpointAuthMethod = TokenEndpointAuthMethods::client_secret_basic;
            }
            const auto promptValuesSupported = data.value(QStringLiteral("prompt_values_supported")).toArray();
            if (!promptValuesSupported.isEmpty()) {
                _supportedPromptValues = PromptValuesSupported::none;
                for (const auto &x : promptValuesSupported) {
                    const auto flag = Utility::stringToEnum<PromptValuesSupported>(x.toString());
                    // only use flags present in _openIdConfig->openIdConnectPrompt()
                    if (flag & defaultOauthPromptValue(_openIdConfig))
                        _supportedPromptValues |= flag;
                }
            }

            qCDebug(lcOauth) << "parsing .well-known reply successful, auth endpoint" << _authEndpoint << "and token endpoint" << _tokenEndpoint;
        } else if (err.error == QJsonParseError::IllegalValue) {
            qCDebug(lcOauth) << "failed to parse .well-known reply as JSON, server might not support OIDC";
            useOwnCloudOAuthFallback();
        } else {
            qCDebug(lcOauth) << "failed to parse .well-known reply, error:" << err.error;
            useOwnCloudOAuthFallback();
        }

        if (!_authEndpoint.isValid() || !_tokenEndpoint.isValid()) {
            qCDebug(lcOauth) << ".well-known response did not contain usable OAuth endpoints, falling back to ownCloud OAuth2 routes";
            useOwnCloudOAuthFallback();
        }
        Q_EMIT fetchWellKnownFinished();
    });
}

/**
 * Checks whether a URL returned by the server is valid.
 * @param url URL to validate
 * @return true if validation is successful, false otherwise
 */
bool isUrlSchemeValid(const QUrl &url)
{
    qCDebug(lcOauth()) << "Checking URL scheme for validity:" << url;

    // todo: #21 - I am not sure this check has much of a point to begin with as
    // we have already validated any URL via user input OR we take the url from the webfinger service,
    // but if this is in fact "needed" it should check more than just the scheme imo

    // the following allow list contains URL schemes accepted as valid
    // OAuth 2.0 URLs must be HTTPS to be in compliance with the specification
    // for unit tests, we also permit the non existing oauthtest scheme
    const QStringList allowedSchemes({QStringLiteral("https"), QStringLiteral("oauthtest")});
    return allowedSchemes.contains(url.scheme());
}

void OAuth::openBrowser()
{
    // Einen gültigen Link gibt es erst nach der Abfrage der Endpunkte und nur, solange der Listener wartet. Ein Klick auf
    // „Anmelden“, während die Anmeldung nach einem Fehler neu beginnt oder der Browser schon zurückgekehrt ist, öffnet
    // deshalb nichts: authorisationLink() setzt beides voraus, mit FORCE_ASSERTS (Beta) brach das Programm sonst ab
    if (!_wellKnownFinished || !_server.isListening()) {
        qCInfo(lcOauth) << "Not opening the browser: the sign-in link is not ready yet or the browser has already returned";
        return;
    }

    auto authorisationURL = authorisationLink();
    if (authorisationURL.isEmpty()) {
        qCWarning(lcOauth) << "Authorization URL is unknown - well-known/openid-configuration endpoint did not return usable information";
        Q_EMIT result(ErrorIdPUnreachable, QString());
        return;
    }
    qCDebug(lcOauth) << "opening browser";

    if (!isUrlSchemeValid(authorisationURL)) {
        qCWarning(lcOauth) << "URL validation failed";
        Q_EMIT result(ErrorInsecureUrl, QString());
        return;
    }

    if (!QDesktopServices::openUrl(authorisationURL)) {
        qCWarning(lcOauth) << "QDesktopServices::openUrl Failed";
        // We cannot open the browser, then we claim we don't support OAuth.
        Q_EMIT result(NotSupported, QString());
    }
}

// todo: I was contemplating how we can make sure the passed account isn't null before we use it
// to seed the OAuth ctr, and really, I'm not sure this should be a subclass of oauth in the first place. Instead it could simply use an
// oauth instance to complete the tasks it can't do itself -> this could possibly be a "has a" not an "is a" impl
AccountBasedOAuth::AccountBasedOAuth(Account *account, const OpenIdConfig& openIdConfig, QObject *parent)
    : OAuth(account->url(), account->davUser(), openIdConfig, account->accessManager(), parent)
    , _account(account)
{
}

void AccountBasedOAuth::startAuthentication()
{
    OAuth::startAuthentication();
}

void AccountBasedOAuth::fetchWellKnown()
{
    if (!_account) {
        qCWarning(lcOauth) << "Unable to fetch well known, account has been deleted";
        return;
    }

    qCDebug(lcOauth) << "starting CheckServerJob before fetching" << wellKnownPathC;

    auto *checkServerJob = CheckServerJobFactory::createFromAccount(_account, true).startJob(_serverUrl, this);

    connect(checkServerJob, &CoreJob::finished, this, [checkServerJob, this]() {
        if (checkServerJob->success()) {
            // Während der Wartung antwortet der Token-Endpunkt nur mit 503: abwarten, das Refresh-Token gilt weiter
            if (_isRefreshingToken
                && checkServerJob->result().value<CheckServerJobResult>().statusObject().value(QStringLiteral("maintenance")).toVariant().toBool()) {
                qCInfo(lcOauth) << "The server is in maintenance mode, not refreshing the token now";
                Q_EMIT refreshFailed(QString(), true, QString());
                return;
            }
            qCDebug(lcOauth) << "CheckServerJob succeeded, fetching" << wellKnownPathC;
            OAuth::fetchWellKnown();
        } else {
            qCDebug(lcOauth) << "CheckServerJob failed, error:" << checkServerJob->errorMessage();
            if (_isRefreshingToken) {
                const QNetworkReply *reply = checkServerJob->reply();
                Q_EMIT refreshFailed(reply && reply->error() != QNetworkReply::NoError ? networkReplyErrorString(*reply) : checkServerJob->errorMessage(), false,
                    QString());
            } else {
                Q_EMIT result(Error);
            }
        }
    });
}

void AccountBasedOAuth::refreshAuthentication(const QString &refreshToken)
{
    if (!OC_ENSURE(!_isRefreshingToken)) {
        qCDebug(lcOauth) << "already refreshing token, aborting";
        return;
    }

    // Jede Erneuerung bekommt ein eigenes Objekt (Credentials::startRefresh()): das bleibt gesetzt
    _isRefreshingToken = true;

    auto refresh = [this, refreshToken] {
        auto reply = postTokenRequest({{QStringLiteral("grant_type"), QStringLiteral("refresh_token")}, {QStringLiteral("refresh_token"), refreshToken}});
        connect(reply, &QNetworkReply::finished, this, [reply, this]() {
            reply->deleteLater();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QJsonParseError jsonParseError;
            const QJsonObject data = QJsonDocument::fromJson(reply->readAll(), &jsonParseError).object();
            // https://developer.okta.com/docs/reference/api/oidc/#response-properties-2
            const QString error = data.value(QStringLiteral("error")).toString();
            const QString description = data.value(QStringLiteral("error_description")).toString();
            if (error == QLatin1String("invalid_grant")) {
                // nur das heißt: dieses Refresh-Token gilt nicht mehr (abgelaufen, widerrufen oder schon verbraucht)
                qCWarning(lcOauth) << "The server rejected the refresh token:" << error << description;
                Q_EMIT refreshRejected(description.isEmpty() ? error : QStringLiteral("%1: %2").arg(error, description));
                return;
            }

            const QString accessToken = data.value(QStringLiteral("access_token")).toString();
            const QString newRefreshToken = data.value(QStringLiteral("refresh_token")).toString();
            const QString tokenType = data.value(QStringLiteral("token_type")).toString();
            if (reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300 && error.isEmpty() && !accessToken.isEmpty()
                && (tokenType.isEmpty() || tokenType.compare(QLatin1String("bearer"), Qt::CaseInsensitive) == 0)) {
                // RFC 6749, Abschnitt 6: ohne neues Refresh-Token gilt das bisherige weiter
                Q_EMIT refreshFinished(accessToken, newRefreshToken, secondsValue(data.value(QStringLiteral("expires_in"))));
                return;
            }

            // Alles andere ist kein Urteil über das Refresh-Token: Netz, Zeitüberschreitung, 5xx, eine Weiterleitung oder
            // HTML statt JSON (oauth2-App aus), ein deaktiviertes Konto (unauthorized_client) oder ein Fehler in der
            // Einrichtung (invalid_client, invalid_request). Die Meldung nennt die Ursache für den Kontostatus, die
            // technische Angabe steht in Klammern. Der Kontext steht ausdrücklich da: tr() in diesen verschachtelten
            // Lambdas ordnet das lupdate aus Qt 6.8 ohne Namensraum zu, zur Laufzeit gilt aber OCC::AccountBasedOAuth.
            const QString oauthError = description.isEmpty() ? error : QStringLiteral("%1: %2").arg(error, description);
            const QString httpDetail = jsonParseError.error != QJsonParseError::NoError
                ? QStringLiteral("HTTP %1: %2").arg(QString::number(httpStatus), jsonParseError.errorString())
                : QStringLiteral("HTTP %1").arg(QString::number(httpStatus));
            QString reason;
            if (error == QLatin1String("unauthorized_client") && !_usesOpenIdConnect) {
                // die oauth2-App: Konto deaktiviert oder gelöscht („user not enabled“)
                reason = QCoreApplication::translate("OCC::AccountBasedOAuth", "Your account on the server is disabled (%1). Please contact your system administrator.")
                             .arg(oauthError);
            } else if (error == QLatin1String("invalid_client") || error == QLatin1String("unauthorized_client")) {
                // bei einem OpenID-Connect-Anbieter heißt unauthorized_client: Diese Anwendung bekommt so kein Token (RFC 6749,
                // Abschnitt 5.2), über das Konto sagt das nichts
                reason = QCoreApplication::translate(
                    "OCC::AccountBasedOAuth", "The sign-in service of the server does not accept this application (%1). Please contact your system administrator.")
                             .arg(oauthError);
            } else if (error == QLatin1String("server_error") || error == QLatin1String("temporarily_unavailable") || (error.isEmpty() && httpStatus >= 500)) {
                // vorübergehende Störung: 5xx oder die Fehlercodes dafür aus RFC 6749
                reason = QCoreApplication::translate("OCC::AccountBasedOAuth", "The sign-in service of the server is temporarily unavailable (%1).")
                             .arg(error.isEmpty() ? QStringLiteral("HTTP %1").arg(QString::number(httpStatus)) : oauthError);
            } else if (!error.isEmpty()) {
                reason = QCoreApplication::translate("OCC::AccountBasedOAuth", "The sign-in service of the server rejected the request (%1).").arg(oauthError);
            } else if (httpStatus != 0) {
                // Weiterleitung auf die Anmeldeseite, HTML statt JSON, 404: die oauth2-App fehlt oder ist abgeschaltet
                reason = QCoreApplication::translate("OCC::AccountBasedOAuth", "The sign-in service of the server is not available (%1). Please contact your system administrator.")
                             .arg(httpDetail);
            } else {
                reason = networkReplyErrorString(*reply);
            }
            qCWarning(lcOauth) << "Error while refreshing the token:" << reply->error() << httpStatus << (oauthError.isEmpty() ? httpDetail : oauthError);
            Q_EMIT refreshFailed(reason, false, newRefreshToken);
        });
    };

    connect(this, &OAuth::fetchWellKnownFinished, this, refresh);

    fetchWellKnown();
}

QString OCC::toString(OAuth::PromptValuesSupportedFlags s)
{
    QStringList out;
    for (auto k : {OAuth::PromptValuesSupported::consent, OAuth::PromptValuesSupported::select_account})
        if (s & k) {
            out += Utility::enumToString(k);
        }
    return out.join(QLatin1Char(' '));
}
