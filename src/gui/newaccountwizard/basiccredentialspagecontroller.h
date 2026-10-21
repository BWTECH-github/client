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

#include "capabilities.h"
#include "gui/owncloudguilib.h"
#include "wizardpagevalidator.h"

#include <QObject>
#include <QPointer>
#include <QUrl>

class QWizardPage;
class QLineEdit;
class QLabel;

namespace OCC {

class AccessManager;

/**
 * Ergebnis der Anmeldung mit Benutzername und Kennwort: loginUser und password gehen in die Zugangsdaten des Kontos,
 * userId (dav_user) und displayName kommen vom Server, ebenso die Capabilities.
 */
struct BasicCredentialsPageResults
{
    QString loginUser;
    QString password;
    QString userId;
    QString displayName;
    Capabilities capabilities = {QUrl{}, {}};

    QString error;
};

/**
 * @brief Seite des Assistenten für Server ohne die App „OAuth2“ (owncloud.online ohne oauth2): Benutzername und
 * Kennwort wie im 6.0.3-Client. validate() prüft die Eingabe synchron am Server (Benutzer, Capabilities) und meldet
 * success() oder failure(); ein abgelehntes Kennwort erscheint als klare Meldung auf der Seite.
 */
class OWNCLOUDGUI_EXPORT BasicCredentialsPageController : public QObject, public WizardPageValidator
{
    Q_OBJECT
public:
    /**
     * @param page is the page this controller will build and manage. Nullptr is acceptable eg for testing
     * @param accessManager is the access manager that will be used for all network requests
     */
    explicit BasicCredentialsPageController(QWizardPage *page, AccessManager *accessManager, QObject *parent);

    bool validate() override;

    /** die geprüfte Serveradresse von der Adressseite; sie erscheint im Hinweis der Seite */
    void setServerUrl(const QUrl &url);
    QUrl serverUrl() const { return _serverUrl; }

    /** Anmeldename vorbelegen (Umstieg vom offiziellen Client); eine Eingabe des Nutzers bleibt */
    void presetLoginUser(const QString &loginUser);
    /** nach „Zurück“ zur Adressseite: ein Kennwort gehört nicht zur nächsten Adresse */
    void clearPassword();

    /** Fokus ins erste leere Feld */
    void focusInput();

    void updateColorsAndIcons();

Q_SIGNALS:
    void success(const OCC::BasicCredentialsPageResults &results);
    void failure(const OCC::BasicCredentialsPageResults &results);

private:
    void buildPage();
    void handleError(const QString &error);
    /// wird die Seite noch angezeigt? Während der Prüfung lässt sich „Zurück“ klicken
    bool pageIsCurrent() const;

    QPointer<QWizardPage> _page;
    QPointer<AccessManager> _accessManager;
    QUrl _serverUrl;

    QLabel *_logoLabel = nullptr;
    QLabel *_instructionLabel = nullptr;
    QLineEdit *_userField = nullptr;
    QLineEdit *_passwordField = nullptr;
    QLabel *_errorField = nullptr;

    BasicCredentialsPageResults _results;
    // validate() läuft (verschachtelte Event-Loops der Prüfungen)
    bool _checking = false;
};
}
Q_DECLARE_METATYPE(OCC::BasicCredentialsPageResults)
// einmalige Registrierung des Typs (für QSignalSpy in den Tests)
static const int basicCredentialsPageResultsTypeId = qRegisterMetaType<OCC::BasicCredentialsPageResults>();
