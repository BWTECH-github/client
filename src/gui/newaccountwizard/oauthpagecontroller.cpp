/*
 * Copyright (C) Lisa Reese <lisa.reese@kiteworks.com>
 * Modified by BW-Tech GmbH for owncloud.online server compatibility.
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
#include "oauthpagecontroller.h"

#include "accessmanager.h"
#include "account.h"
#include "accountmanager.h"
#include "config/appconfig.h"
#include "iconresources.h"
#include "networkadapters/fetchcapabilitiesadapter.h"
#include "networkadapters/userinfoadapter.h"
#include "networkadapters/webfingerlookupadapter.h"
#include "template.h"
#include "mainwindow/colormanager.h"
#include "theme.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWizard>
#include <QWizardPage>

namespace OCC {

Q_LOGGING_CATEGORY(lcOAuthPage, "gui.newaccountwizard.oauthpage", QtInfoMsg)

OAuthPageController::OAuthPageController(QWizardPage *page, AccessManager *accessManager, QObject *parent)
    : QObject{parent}
    , _page(page)
    , _accessManager(accessManager)
    , _oauth(nullptr)
{
    buildPage();
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &OAuthPageController::clipboardChanged);
}

void OAuthPageController::buildPage()
{
    if (!_page)
        return;

    QString appName = Theme::instance()->appNameGUI();

    _logoLabel = new QLabel(_page);
    _logoLabel->setAlignment(Qt::AlignCenter);
    _logoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _logoLabel->setAccessibleName(tr("Application Name Logo %1").arg(appName));

    QLabel *instructionLabel =
        new QLabel(tr("Leave this screen open. A sign in prompt will appear in your web browser to connect you to the following address."), _page);
    QFont font = instructionLabel->font();
    font.setPixelSize(14);
    instructionLabel->setFont(font);
    instructionLabel->setWordWrap(true);
    instructionLabel->setAlignment(Qt::AlignCenter);
    instructionLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    _urlField = new QLabel(_page);
    _urlField->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    _urlField->setEnabled(false);
    _urlField->setAccessibleName(tr("Sign in URL")); // Prevent screen readers from reading the full URL!!!
    _urlField->setAccessibleDescription(tr("To copy the sign in URL to the clipboard, use the copy button"));

    _copyButton = new QPushButton(_page);
    _copyButton->setObjectName("CopyUrlToClipboardButton");
    _copyButton->setFlat(true);
    _copyButton->setContentsMargins(0, 0, 0, 0);
    _copyButton->setFixedSize(_urlField->height(), _urlField->height());
    _copyButton->setAccessibleName(tr("Copy sign in URL"));
    _copyButton->setAccessibleDescription(tr("Copy URL to sign in"));
    _copyButton->installEventFilter(this);
    connect(_copyButton, &QPushButton::clicked, this, &OAuthPageController::copyUrlClicked);

    _statusField = new QLabel(QString(), _page);
    _statusField->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _statusField->setWordWrap(true);
    _statusField->setAlignment(Qt::AlignCenter);

    _errorField = new QLabel(QString(), _page);
    // Fehlerrot aus dem Theme (WCAG 4,5:1); Qt::red erreichte auf Weiß nur 4:1 und wirkte auf QLabel gar nicht
    ColorManager::applyErrorColor(_errorField);
    _errorField->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _errorField->setWordWrap(true);
    _errorField->setAlignment(Qt::AlignLeft);
    _errorField->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);

    QLabel *footerLogoLabel = nullptr;
    if (!Theme::instance()->wizardFooterLogoName().isEmpty()) {
        footerLogoLabel = new QLabel({}, _page);
        footerLogoLabel->setPixmap(IconResources::getBrandingIcon(Theme::instance()->wizardFooterLogoName()).pixmap(100, 52));
        footerLogoLabel->setAlignment(Qt::AlignCenter);
        footerLogoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        footerLogoLabel->setAccessibleName(tr("Additional logo defined by the organization"));
    }

    QVBoxLayout *layout = new QVBoxLayout();
    layout->setContentsMargins(50, 0, 50, 0);
    layout->setSpacing(12);
    layout->addStretch(1);
    layout->addWidget(_logoLabel);
    layout->addSpacing(16);
    layout->addWidget(instructionLabel);

    QHBoxLayout *urlAreaLayout = new QHBoxLayout();
    urlAreaLayout->setContentsMargins(0, 0, 0, 0);
    urlAreaLayout->setSpacing(0);
    urlAreaLayout->addWidget(_urlField);
    urlAreaLayout->addWidget(_copyButton);
    layout->addLayout(urlAreaLayout);

    layout->addWidget(_statusField);
    layout->addWidget(_errorField);

    if (footerLogoLabel)
        layout->addWidget(footerLogoLabel);
    layout->addStretch(1);
    _page->setLayout(layout);

    updateIcons();
    // seed the copy button tooltip for the first go
    clipboardChanged();
}

void OAuthPageController::updateIcons()
{
    _logoLabel->setPixmap(IconResources::getBrandingIcon(Theme::instance()->wizardHeaderLogoName()).pixmap(200, 100));
    _copyButton->setIcon(copyIcon());
}

void OAuthPageController::handleError(const QString &error)
{
    _statusField->clear();
    _errorField->setText(error);
    _results.error = error;
    Q_EMIT failure(_results);
}

QIcon OAuthPageController::copyIcon()
{
    // this is required because the background color of the wizard pages can be set via theme and we can't pick an icon
    // color that "fits" based on the current
    // normally we would want to cache this but for this very special one off case I don't think it makes sense
    const QString iconPath = QStringLiteral(":/client/resources/core/copy.svg");
    QPalette pagePalette = qGuiApp->palette();
    const QString color = pagePalette.color(QPalette::Text).name();
    QByteArray data = Resources::Template::renderTemplateFromFile(iconPath, {{QStringLiteral("color"), color}}).toUtf8();
    QBuffer buffer(&data);
    QImageReader iconReader(&buffer, "svg");
    return QIcon(QPixmap::fromImageReader(&iconReader));
}

void OAuthPageController::setLookupWebfingerUrls(bool lookup)
{
    _lookupWebfingerUrls = lookup;
}

void OAuthPageController::setServerUrl(const QUrl &url)
{
    _serverUrl = url;
}

void OAuthPageController::setAuthenticationUrl(const QUrl &url)
{
    _authUrl = url;
    // Nach „Zurück“ und einer anderen Adresse die noch wartende Anmeldung beenden: Ihr Listener nähme sonst weiter den
    // Rückruf des vorigen Servers an. Wird ihr Ergebnis gerade geprüft, verwirft handleOauthResult() es am Ende selbst.
    if (_oauth && _oauthUrl != url && !_checkingResult) {
        qCInfo(lcOAuthPage) << "The address changed, ending the sign-in that is still waiting for the browser";
        endSignIn(_oauth);
    }
}

void OAuthPageController::endSignIn(OAuth *oauth)
{
    if (!oauth) {
        return;
    }
    // getrennt meldet sie nichts mehr; erst später löschen, weil das auch aus einem Signal von ihr heraus läuft
    oauth->disconnect(this);
    oauth->deleteLater();
    if (oauth == _oauth) {
        _oauth = nullptr;
    }
}

bool OAuthPageController::pageIsCurrent() const
{
    // QWizardPage::wizard() ist geschützt; die Seite hängt über einen inneren Rahmen im Assistenten
    const QWizard *wizard = nullptr;
    for (const QObject *object = _page; object && !wizard; object = object->parent()) {
        wizard = qobject_cast<const QWizard *>(object);
    }
    return wizard && wizard->currentPage() == _page;
}

bool OAuthPageController::resultStillWanted() const
{
    return pageIsCurrent() && _oauthUrl == _authUrl;
}

void OAuthPageController::discardResult()
{
    _results = {};
    // Steht die Anmeldeseite inzwischen für eine andere Adresse, hat validate() deren Anmeldung wegen der laufenden Prüfung
    // abgelehnt: nachholen, sobald diese abgeschlossen ist, sonst bliebe die Seite ohne Anmeldung stehen
    if (pageIsCurrent()) {
        QTimer::singleShot(0, this, [this] {
            if (pageIsCurrent() && !_oauth) {
                validate();
            }
        });
    }
}

void OAuthPageController::copyUrlClicked()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(_authEndpoint);
}

// I was going to implement an event filter for the tooltip but I think the positioning is risky given our previous issues with popup stuff
// on mac and/or linux. This is also the simplest, least error prone way to do it imo.
void OAuthPageController::clipboardChanged()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard->text() == _authEndpoint)
        _copyButton->setToolTip(tr("URL copied"));
    else
        _copyButton->setToolTip(tr("Copy URL"));
}

bool OAuthPageController::validate()
{
    // this is a little bit tricky. Because we "auto advance" the wizard to the next page when the oauth step has succeeded, validate
    // will be called *twice*
    // first when the page change triggers the authentication step in the browser, then a second time when we auto-advance after the main controller has
    // received the success signal.
    // In the first call to validate, we always return false because we don't yet know if the asynchronous oauth routine has succeeded.
    // the slot for handling the authentication result sets _oauthCompleted to true when all checks have passed,
    // so the second call to validate triggered by programmatically advancing the page, returns true.
    // we always reset the _oauthCompleted flag after success in case the user wants to go back to reauthenticate for whatever reason, they will start from
    // scratch.
    if (_oauthCompleted) {
        _oauthCompleted = false;
        return true;
    }

    // Der Browser ist zurückgekehrt, das Konto wird gerade geprüft (verschachtelte Event-Loops): kein zweiter Anmeldevorgang,
    // und handleOauthResult() darf am Ende nicht eine neue Anmeldung statt der eigenen löschen
    if (_checkingResult) {
        return false;
    }

    // Wartet die Anmeldung für dieselbe Adresse noch auf den Browser („Browser erneut öffnen“, Enter, zurück und erneut
    // anmelden), nur den Browser wieder öffnen: Port und state bleiben, damit ein schon geöffneter oder kopierter Link
    // gültig bleibt, bis die Anmeldung abgeschlossen oder der Assistent geschlossen ist. Ohne Link (die Endpunkte werden
    // noch abgefragt) kann niemand den Link haben: dann neu beginnen. Die Abfrage selbst bliebe sonst womöglich hängen,
    // weil die Adressprüfung nach „Zurück“ den AccessManager zurücksetzt.
    if (_oauth && _oauth->isListening() && _oauthUrl == _authUrl && !_authEndpoint.isEmpty()) {
        openBrowser();
        return false;
    }

    // this should have already been cleaned up after the auth routine but who knows. Never hurts to be sure.
    if (_oauth) {
        delete _oauth;
        _oauth = nullptr;
    }

    _results = {};
    _statusField->clear();
    _errorField->clear();
    _authEndpoint.clear();
    _urlField->clear();

    AppConfig systemConfig;
    _oauth = new OAuth(_authUrl, {}, systemConfig.openIdConfig(), _accessManager.get(), this);
    _oauthUrl = _authUrl;
    // if we ever need to split out the auth link calculation, it's coming from fetchWellKnown which is a subset of
    // the "full" authentication routine in the oauth impl
    connect(_oauth, &OAuth::authorisationLinkChanged, this, &OAuthPageController::authUrlReady);
    connect(_oauth, &OAuth::result, this, &OAuthPageController::handleOauthResult);
    _oauth->startAuthentication();
    return false;
}

void OAuthPageController::authUrlReady()
{
    _authEndpoint = _oauth->authorisationLink().toString(QUrl::FullyEncoded);

    // I hit this when demo was down - browser showed "We're having trouble connecting to the login service.
    // If the problem continues, please contact support."
    if (_authEndpoint.isEmpty()) {
        // ohne Link kann niemand den Listener erreichen; verwerfen, damit „Browser erneut öffnen“ neu beginnt
        endSignIn(_oauth);
        handleError(tr("Unable to connect to the login service. If the problem continues, please contact support."));
        return;
    }

    QFontMetrics metrics(_urlField->font());
    QString elidedText = metrics.elidedText(_authEndpoint, Qt::ElideRight, _urlField->width());
    _urlField->setText(elidedText);
    // try to force an immediate repaint before the browser pops, so there is no chance that the oauth page appears "incomplete"
    // for a moment
    // note in my experience mac does not always honor repaint()s
    _page->repaint();
    openBrowser();
}

void OAuthPageController::openBrowser()
{
    _errorField->clear();
    // ohne Knopfnamen: „Autorisieren“ zeigt nur die App „OAuth2“, und auch sie nicht für vertrauenswürdige Clients
    _statusField->setText(tr("Please sign in using the browser window that has opened and allow access if you are asked to."));
    // schlägt das Öffnen fehl, ersetzt handleOauthResult() den Hinweis
    _oauth->openBrowser();
}

void OAuthPageController::handleOauthResult(OAuth::Result result, const QString &token, const QString &refreshToken)
{
    _statusField->clear();
    QString extraHelp = tr("Please copy the authentication URL using the button above, and provide this information to support.");
    switch (result) {
    case OAuth::Result::LoggedIn: {
        // Nur übernehmen, solange die Anmeldeseite angezeigt wird und derselben Adresse gilt. Kam der Rückruf nach „Zurück“,
        // prüfte setCurrentId() sonst die Adressseite mit der inzwischen eingetragenen Adresse, und das Konto dafür bekäme
        // Token, Refresh-Token und Benutzer dieses Servers.
        if (!resultStillWanted()) {
            qCInfo(lcOAuthPage) << "Discarding the sign-in result: the sign-in page is no longer shown or the address changed";
            discardResult();
            break;
        }
        _checkingResult = true;
        const auto checkingDone = qScopeGuard([this] { _checkingResult = false; });
        // Die Prüfungen drehen eigene Event-Loops, „Zurück“ und eine andere Adresse sind währenddessen möglich: einen Fehler
        // nur melden, solange das Ergebnis noch gilt
        const auto checkFailed = [this](const QString &error) {
            if (resultStillWanted()) {
                handleError(error);
            } else {
                qCInfo(lcOAuthPage) << "Discarding the failed check: the sign-in page was left while the account was checked";
                discardResult();
            }
        };
        _results.token = token;
        _results.refreshToken = refreshToken;
        // special note: if the original server url does not support webfinger, the _authUrl will equal the _serverUrl
        // HOWEVER, we can't use this fact to determine whether webfinger is in play as some webfinger impls return the original
        // _serverUrl (eg demo) as the authentication url and some don't (eg infinite).
        if (_lookupWebfingerUrls) {
            // it is important to use the original server url for the webfinger lookup because the webfinger authentication url *only* knows how to authenticate
            // the webfinger service actually lives in the original server url so we have to use that to look up the webfinger user info endpoints.
            WebFingerLookupAdapter lookup(_accessManager, token, _serverUrl);
            const WebFingerLookupResult webfingerResult = lookup.getResult();
            if (!webfingerResult.success()) {
                checkFailed(tr("Failed to look up WebFinger instances: %1").arg(webfingerResult.error));
                break;
            } else {
                if (!webfingerResult.urls.isEmpty())
                    _results.webfingerUserUrl = webfingerResult.urls[0];
            }
        }

        // if we have successfully found a user info endpoint url using webfinger, that becomes our de facto url for further account related activities.
        // if webfinger is not in play we are just using the original server url.
        const QUrl userInfoUrl = _results.webfingerUserUrl.isEmpty() ? _serverUrl : _results.webfingerUserUrl;

        UserInfoAdapter infoAdapter(_accessManager, _results.token, userInfoUrl);
        UserInfoResult infoResult = infoAdapter.getResult();

        if (infoResult.success()) {
            if (AccountManager::instance()->accountForLoginExists(userInfoUrl, infoResult.userId)) {
                checkFailed(tr("You are already connected to an account with these credentials."));
                break;
            } else {
                _results.displayName = infoResult.displayName;
                _results.userId = infoResult.userId;
            }
        } else {
            checkFailed(infoResult.error);
            break;
        }

        // Finally, get the capabilities so unsupported legacy servers can still be blocked.
        FetchCapabilitiesAdapter fetchCapabilities(_accessManager, _results.token, userInfoUrl);
        FetchCapabilitiesResult capabilitiesResult = fetchCapabilities.getResult();
        if (!capabilitiesResult.success()) {
            // I don't think we want to display the core error message as it's stuff like json errors and not
            // useful to the user but we can change this after we have the discussion about error messages
            checkFailed(tr("Unable to retrieve capabilities from server."));
            break;
        }
        if (Account::supportLevelForCapabilities(capabilitiesResult.capabilities) != Account::ServerSupportLevel::Supported) {
            checkFailed(tr("The server is not supported by this client."));
            break;
        } else
            _results.capabilities = capabilitiesResult.capabilities;

        if (!resultStillWanted()) {
            qCInfo(lcOAuthPage) << "Discarding the sign-in result: the sign-in page was left while the account was checked";
            discardResult();
            break;
        }
        _oauthCompleted = true;
        Q_EMIT success(_results);
        break;
    }
    case OAuth::Result::Error: {
        handleError(tr("Error while trying to log in to OAuth2-enabled server. %1").arg(extraHelp));
        break;
    }
    case OAuth::Result::NotSupported: {
        // Der Browser ließ sich nicht öffnen. Die Anmeldung wartet weiter, damit der kopierte Link zum Client zurückführt.
        handleError(tr("The browser could not be opened. Copy the link with the button above and open it in your browser."));
        return;
    }
    case OAuth::Result::ErrorInsecureUrl: {
        handleError(tr("OAuth2 authentication requires a secured connection. %1").arg(extraHelp));
        break;
    }
    case OAuth::Result::ErrorIdPUnreachable: {
        handleError(tr("Authorization server unreachable. %1").arg(extraHelp));
        break;
    }
    };
    // Nur die Anmeldung beenden, die dieses Ergebnis geliefert hat, nicht eine inzwischen neu begonnene
    endSignIn(qobject_cast<OAuth *>(sender()));
}
}
