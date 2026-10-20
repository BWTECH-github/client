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
#include "basicauthenticationwidget.h"

#include "iconresources.h"
#include "mainwindow/colormanager.h"
#include "newaccountwizard/wizardpagelayout.h"
#include "theme.h"

#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace OCC {

BasicAuthenticationWidget::BasicAuthenticationWidget(const QUrl &serverUrl, const QString &loginUser, QWidget *parent)
    : QWidget{parent}
{
    updateColors();

    const QString appName = Theme::instance()->appNameGUI();

    QLabel *logoLabel = new QLabel({}, this);
    logoLabel->setPixmap(IconResources::getBrandingIcon(Theme::instance()->wizardHeaderLogoName()).pixmap(200, 100));
    logoLabel->setAlignment(Qt::AlignCenter);
    logoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    logoLabel->setAccessibleName(tr("Application Name Logo %1").arg(appName));

    QLabel *titleLabel = new QLabel(tr("Sign in required"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setWeight(QFont::DemiBold);
    titleFont.setPixelSize(20);
    titleLabel->setFont(titleFont);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QLabel *instructionLabel = new QLabel(tr("Please enter your password to sign in to %1.").arg(serverUrl.toDisplayString()), this);
    QFont font = instructionLabel->font();
    font.setPixelSize(14);
    instructionLabel->setFont(font);
    instructionLabel->setWordWrap(true);
    instructionLabel->setAlignment(Qt::AlignCenter);
    instructionLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // der Anmeldename steht fest: er gehört zum Konto
    QLabel *userLabel = new QLabel(loginUser, this);
    userLabel->setObjectName(QStringLiteral("LoginUserLabel"));
    userLabel->setTextFormat(Qt::PlainText);
    userLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    _passwordField = new QLineEdit(this);
    _passwordField->setObjectName(QStringLiteral("PasswordLineEdit"));
    _passwordField->setEchoMode(QLineEdit::Password);
    // wie das Adressfeld des Assistenten: auf dem Hintergrund des Themes als Eingabefeld erkennbar
    QPalette fieldPalette = _passwordField->palette();
    fieldPalette.setColor(QPalette::Base, qGuiApp->palette().color(QPalette::Button));
    fieldPalette.setColor(QPalette::Text, qGuiApp->palette().color(QPalette::ButtonText));
    _passwordField->setPalette(fieldPalette);
    connect(_passwordField, &QLineEdit::returnPressed, this, &BasicAuthenticationWidget::submit);
    connect(_passwordField, &QLineEdit::textChanged, this, [this](const QString &text) { _signInButton->setEnabled(!text.isEmpty()); });

    QLabel *hintLabel = new QLabel(tr("Recommended: use an app password instead of your account password."), this);
    hintLabel->setWordWrap(true);
    hintLabel->setAlignment(Qt::AlignCenter);
    hintLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    _errorField = new QLabel(QString(), this);
    _errorField->setObjectName(QStringLiteral("ErrorLabel"));
    ColorManager::applyErrorColor(_errorField);
    _errorField->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _errorField->setWordWrap(true);
    _errorField->setAlignment(Qt::AlignLeft);
    _errorField->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);

    QLabel *footerLogoLabel = nullptr;
    if (!Theme::instance()->wizardFooterLogoName().isEmpty()) {
        footerLogoLabel = new QLabel({}, this);
        footerLogoLabel->setPixmap(IconResources::getBrandingIcon(Theme::instance()->wizardFooterLogoName()).pixmap(100, 52));
        footerLogoLabel->setAlignment(Qt::AlignCenter);
        footerLogoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        footerLogoLabel->setAccessibleName(tr("Additional logo defined by the organization"));
    }

    _cancelButton = new QPushButton(tr("Stay logged out"), this);
    _cancelButton->setObjectName(QStringLiteral("StayLoggedOutButton"));
    connect(_cancelButton, &QPushButton::clicked, this, &BasicAuthenticationWidget::stayLoggedOutClicked);
    _signInButton = new QPushButton(tr("Sign in"), this);
    _signInButton->setObjectName(QStringLiteral("SignInButton"));
    _signInButton->setDefault(true);
    _signInButton->setEnabled(false);
    connect(_signInButton, &QPushButton::clicked, this, &BasicAuthenticationWidget::submit);

    QVBoxLayout *contentLayout = new QVBoxLayout();
    contentLayout->setContentsMargins(50, 24, 50, 0);
    contentLayout->setSpacing(12);
    contentLayout->addStretch(1);
    contentLayout->addWidget(logoLabel, 0, Qt::AlignCenter);
    contentLayout->addSpacing(16);
    contentLayout->addWidget(titleLabel);
    contentLayout->addWidget(instructionLabel);

    // die Beschriftungen sind zugleich die Namen der Felder für Screenreader (Buddy)
    QFormLayout *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow(tr("User name:"), userLabel);
    form->addRow(tr("Password:"), _passwordField);
    contentLayout->addLayout(form);

    contentLayout->addWidget(hintLabel);
    contentLayout->addWidget(_errorField);
    if (footerLogoLabel)
        contentLayout->addWidget(footerLogoLabel, 0, Qt::AlignCenter);
    contentLayout->addStretch(1);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(50, 0, 50, 24);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(_cancelButton);
    buttonLayout->addWidget(_signInButton);

    // Die Kontoansicht ist oft niedriger als der Inhalt (1024×768, große Schrift, Meldung): Ohne Bildlauf stauchte das Layout
    // das größte Element, das Logo, und schnitt „ONLINE“ ab. So rollt nur der Inhalt, die Knöpfe bleiben sichtbar.
    QVBoxLayout *layout = new QVBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->addWidget(createScrollableContent(this, contentLayout), 1);
    layout->addLayout(buttonLayout);

    setLayout(layout);
    setFocusProxy(_passwordField);
}

QString BasicAuthenticationWidget::password() const
{
    return _passwordField->text();
}

void BasicAuthenticationWidget::submit()
{
    const QString password = _passwordField->text();
    if (password.isEmpty()) {
        return;
    }
    Q_EMIT loginRequested(password);
}

void BasicAuthenticationWidget::setErrorMessage(const QString &error)
{
    _errorField->setText(error);
}

void BasicAuthenticationWidget::updateColors()
{
    QPalette palette = this->palette();
    QColor themeBackground = Theme::instance()->wizardHeaderBackgroundColor();
    QColor themeForeground = Theme::instance()->wizardHeaderTitleColor();
    if (themeBackground.isValid()) {
        palette.setColor(QPalette::Window, themeBackground);
        palette.setColor(QPalette::Base, themeBackground);
    }
    if (themeForeground.isValid()) {
        palette.setColor(QPalette::WindowText, themeForeground);
        palette.setColor(QPalette::Text, themeForeground);
    }
    setPalette(palette);
    setAutoFillBackground(true);
}

}
