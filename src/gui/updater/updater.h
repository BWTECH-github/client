/*
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
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

#ifndef UPDATER_H
#define UPDATER_H

#include "gui/owncloudguilib.h"

#include <QLoggingCategory>
#include <QObject>

class QUrl;
class QUrlQuery;

namespace OCC {

Q_DECLARE_LOGGING_CATEGORY(lcUpdater)

OWNCLOUDGUI_EXPORT QString updateChannel();

class OWNCLOUDGUI_EXPORT Updater : public QObject
{
    Q_OBJECT
public:
    static Updater *instance();
    static QUrl updateUrl();

    // Feed-Plattform, wie sie der Generator (admin/release/make-update-feed.py) kennt,
    // z. B. win-x64, linux-x86_64, macos-arm64
    static QString feedPlatform();
    // Dateiname der Feed-Datei je Plattform und Kanal
    static QString feedFileName(const QString &platform, const QString &channel);
    // Basisadresse (endet auf '/') + Feed-Datei; volle Adressen bleiben unverändert
    static QUrl feedUrl(const QUrl &base, const QString &platform, const QString &channel);

    // Kanal nach dem Versionssuffix: Vorabversionen (daily, nightly, alpha, rc, beta) -> "beta", sonst "stable"
    static QString defaultChannel(const QString &versionSuffix);
    // die gespeicherte Wahl, wenn sie "stable" oder "beta" ist, sonst defaultChannel()
    static QString channel(const QString &storedChoice, const QString &versionSuffix);
    /**
     * Wahl im Über-Fenster speichern, auch wenn sie der Vorgabe entspricht: Wer auf einer Beta „Beta“ wählt, bleibt
     * dort, wenn das finale Release kein Suffix mehr trägt. true, wenn sich der gültige Kanal dadurch ändert.
     */
    static bool setChannelChoice(const QString &channel);

    virtual void checkForUpdate() = 0;
    virtual void backgroundCheckForUpdate() = 0;
    virtual void validateUpdate();

    /***
     * This function will just restart on most platforms
    */
    virtual void applyUpdateAndRestart();

protected:
    Updater()
        : QObject(nullptr)
    {
    }

private:
    static QUrlQuery getQueryParams();
    static Updater *create();
    static Updater *_instance;
};

} // namespace OCC

#endif // UPDATER_H
