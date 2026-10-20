/*
 * Copyright (C) 2026 by BW-Tech GmbH (owncloud.online)
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
#include "basiccredentialspagecontroller.h"

#include "accessmanager.h"
#include "account.h"
#include "accountmanager.h"
#include "creds/credentialssupport.h"
#include "iconresources.h"
#include "mainwindow/colormanager.h"
#include "networkadapters/fetchcapabilitiesadapter.h"
#include "networkadapters/userinfoadapter.h"
#include "theme.h"
#include "wizardpagelayout.h"

#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QVBoxLayout>
#include <QWizard>
#include <QWizardPage>

namespace OCC {

Q_LOGGING_CATEGORY(lcBasicCredentialsPage, "gui.newaccountwizard.basiccredentialspage", QtInfoMsg)

BasicCredentialsPageController::BasicCredentialsPageController(QWizardPage *page, AccessManager *accessManager, QObject *parent)
    : QObject{parent}
    , _page(page)
    , _accessManager(accessManager)
{
    buildPage();
}

void BasicCredentialsPageController::buildPage()
{
    if (!_page)
        return;

    const QString appName = Theme::instance()->appNameGUI();

    _logoLabel = new QLabel(_page);
    _logoLabel->setAlignment(Qt::AlignCenter);
    _logoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _logoLabel->setAccessibleName(tr("Application Name Logo %1").arg(appName));

    _instructionLabel = new QLabel(_page);
    QFont font = _instructionLabel->font();
    font.setPixelSize(14);
    _instructionLabel->setFont(font);
    _instructionLabel->setWordWrap(true);
    _instructionLabel->setAlignment(Qt::AlignCenter);
    _instructionLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // die Beschriftungen im Formular sind zugleich die Namen für Screenreader (Buddy)
    _userField = new QLineEdit(_page);
    _userField->setObjectName(QStringLiteral("LoginUserLineEdit"));
    _userField->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    _passwordField = new QLineEdit(_page);
    _passwordField->setObjectName(QStringLiteral("PasswordLineEdit"));
    _passwordField->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _passwordField->setEchoMode(QLineEdit::Password);

    QLabel *hintLabel = new QLabel(tr("Recommended: use an app password instead of your account password."), _page);
    hintLabel->setWordWrap(true);
    hintLabel->setAlignment(Qt::AlignCenter);
    hintLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    _errorField = new QLabel(QString(), _page);
    _errorField->setObjectName(QStringLiteral("ErrorLabel"));
    // Fehlerrot aus dem Theme (WCAG 4,5:1)
    ColorManager::applyErrorColor(_errorField);
    QFont errorFont = _errorField->font();
    errorFont.setPixelSize(14);
    _errorField->setFont(errorFont);
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
    layout->addWidget(_instructionLabel);

    QFormLayout *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow(tr("User name:"), _userField);
    form->addRow(tr("Password:"), _passwordField);
    layout->addLayout(form);

    layout->addWidget(hintLabel);
    layout->addWidget(_errorField);
    if (footerLogoLabel)
        layout->addWidget(footerLogoLabel);
    layout->addStretch(1);
    // wie die übrigen Seiten: eine Fehlermeldung oder größere Schrift rollt, statt unter der Knopfzeile zu verschwinden
    setScrollableWizardPageLayout(_page, layout);

    updateColorsAndIcons();
}

void BasicCredentialsPageController::updateColorsAndIcons()
{
    if (!_page)
        return;
    // wie das Adressfeld der ersten Seite
    const QPalette appPalette = qGuiApp->palette();
    for (QLineEdit *field : {_userField, _passwordField}) {
        QPalette fieldPalette = field->palette();
        fieldPalette.setColor(QPalette::Base, appPalette.color(QPalette::Button));
        fieldPalette.setColor(QPalette::Text, appPalette.color(QPalette::ButtonText));
        field->setPalette(fieldPalette);
    }
    _logoLabel->setPixmap(IconResources::getBrandingIcon(Theme::instance()->wizardHeaderLogoName()).pixmap(200, 100));
}

void BasicCredentialsPageController::setServerUrl(const QUrl &url)
{
    _serverUrl = url;
    if (_instructionLabel) {
        _instructionLabel->setText(url.isEmpty() ? QString() : tr("Sign in to %1 with your user name and password.").arg(url.toDisplayString()));
    }
}

void BasicCredentialsPageController::presetLoginUser(const QString &loginUser)
{
    if (_userField && _userField->text().isEmpty()) {
        _userField->setText(loginUser);
    }
}

void BasicCredentialsPageController::clearPassword()
{
    if (_passwordField) {
        _passwordField->clear();
    }
    if (_errorField) {
        _errorField->clear();
    }
}

void BasicCredentialsPageController::focusInput()
{
    if (!_userField || !_passwordField) {
        return;
    }
    QLineEdit *field = _userField->text().isEmpty() ? _userField : _passwordField;
    field->setFocus(Qt::OtherFocusReason);
}

bool BasicCredentialsPageController::pageIsCurrent() const
{
    if (!_page) {
        return true;
    }
    // QWizardPage::wizard() ist geschützt; die Seite hängt über einen inneren Rahmen im Assistenten
    const QWizard *wizard = nullptr;
    for (const QObject *object = _page; object && !wizard; object = object->parent()) {
        wizard = qobject_cast<const QWizard *>(object);
    }
    return !wizard || wizard->currentPage() == _page;
}

void BasicCredentialsPageController::handleError(const QString &error)
{
    if (_errorField) {
        _errorField->setText(error);
    }
    _results.error = error;
    Q_EMIT failure(_results);
}

bool BasicCredentialsPageController::validate()
{
    // Die Prüfungen drehen eigene Event-Loops: keine zweite Prüfung darin (Enter, „Zurück“ und wieder „Anmelden“)
    if (_checking) {
        return false;
    }
    _checking = true;
    const auto checkingDone = qScopeGuard([this] { _checking = false; });

    _results = {};
    if (_errorField) {
        _errorField->clear();
    }
    if (!_accessManager) {
        handleError(QStringLiteral("No valid access manager is available"));
        return false;
    }

    const QString loginUser = _userField ? _userField->text().trimmed() : QString();
    const QString password = _passwordField ? _passwordField->text() : QString();
    if (loginUser.isEmpty() || password.isEmpty()) {
        handleError(tr("Please enter your user name and password."));
        focusInput();
        return false;
    }
    const QByteArray authorization = basicAuthorizationHeader(loginUser, password);

    // Fehler nur melden, solange die Seite noch angezeigt wird (während der Prüfung ist „Zurück“ möglich)
    const auto checkFailed = [this](const QString &error) {
        if (pageIsCurrent()) {
            handleError(error);
        } else {
            qCInfo(lcBasicCredentialsPage) << "Discarding the failed check: the page was left while the account was checked";
            _results = {};
        }
        return false;
    };

    UserInfoAdapter infoAdapter(_accessManager, authorization, _serverUrl);
    const UserInfoResult infoResult = infoAdapter.getResult();
    if (!infoResult.success()) {
        if (infoResult.invalidCredentials) {
            qCInfo(lcBasicCredentialsPage) << "The server rejected the user name or password for" << _serverUrl.toDisplayString();
            if (_passwordField && pageIsCurrent()) {
                // gleich neu eingeben; der Benutzername bleibt
                _passwordField->clear();
                _passwordField->setFocus(Qt::OtherFocusReason);
            }
            return checkFailed(tr("The user name or password is not correct."));
        }
        return checkFailed(infoResult.error);
    }
    if (AccountManager::instance()->accountForLoginExists(_serverUrl, infoResult.userId)) {
        return checkFailed(tr("You are already connected to an account with these credentials."));
    }

    // die Capabilities, damit ein nicht unterstützter Server schon hier abgewiesen wird
    FetchCapabilitiesAdapter fetchCapabilities(_accessManager, authorization, _serverUrl);
    const FetchCapabilitiesResult capabilitiesResult = fetchCapabilities.getResult();
    if (!capabilitiesResult.success()) {
        return checkFailed(tr("Unable to retrieve capabilities from server."));
    }
    if (Account::supportLevelForCapabilities(capabilitiesResult.capabilities) != Account::ServerSupportLevel::Supported) {
        return checkFailed(tr("The server is not supported by this client."));
    }
    if (!pageIsCurrent()) {
        qCInfo(lcBasicCredentialsPage) << "Discarding the sign-in result: the page was left while the account was checked";
        return false;
    }

    _results.loginUser = loginUser;
    _results.password = password;
    _results.userId = infoResult.userId;
    _results.displayName = infoResult.displayName;
    _results.capabilities = capabilitiesResult.capabilities;
    Q_EMIT success(_results);
    // das Kennwort gehört jetzt dem Modell des Assistenten, nicht mehr dieser Seite
    _results.password.clear();
    return true;
}
}
