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
#pragma once

#include "gui/owncloudguilib.h"

#include <QUrl>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QShowEvent;

namespace OCC {

/**
 * Dialog „Anmeldung erforderlich“ in der Kontoansicht für ein Konto mit Benutzername und Kennwort: Der Benutzername
 * steht fest (er ist der Anmeldename des Kontos), nur das Kennwort wird eingegeben. Enter oder „Anmelden“ melden
 * loginRequested(); „Abgemeldet bleiben“ meldet stayLoggedOutClicked(). Eine Meldung (setErrorMessage()) steht unter dem
 * Kennwortfeld; ist die Kontoansicht niedriger als der Inhalt, rollt der Dialog zu ihr.
 */
class OWNCLOUDGUI_EXPORT BasicAuthenticationWidget : public QWidget
{
    Q_OBJECT
public:
    explicit BasicAuthenticationWidget(const QUrl &serverUrl, const QString &loginUser, QWidget *parent = nullptr);

    void setErrorMessage(const QString &error);
    QString password() const;

Q_SIGNALS:
    void loginRequested(const QString &password);
    void stayLoggedOutClicked();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void submit();
    void updateColors();
    // rollt die Meldung in den sichtbaren Bereich, sobald das Layout steht
    void revealErrorMessage();

    QLineEdit *_passwordField = nullptr;
    QLabel *_errorField = nullptr;
    QScrollArea *_scrollArea = nullptr;
    QPushButton *_cancelButton = nullptr;
    QPushButton *_signInButton = nullptr;
};
}
